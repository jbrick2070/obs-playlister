/*
obs-playlister
Copyright (C) 2026 Jeffrey A. Brick

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* The folder provider (PLAN section 3.2, Phase 2 step 5). One worker thread per folder row lists the folder, watches
 * it for changes (R-DIR-2, R-DIR-3), admits files through the write-complete gate (R-DIR-4) and publishes snapshots
 * of plain entries. The worker alone opens and closes its watch (R-THR-3); other threads only leave requests and wake
 * it. Every operating-system call is in folder-os-*.c (R-LNX-2). */

#include "provider-kinds.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#include "core/filter.h"
#include "core/keys.h"
#include "core/merge.h"
#include "fold-key.h"
#include "folder-os.h"
#include "os.h"
#include "publication.h"

#define DEBOUNCE_MS 500
#define MAX_CHAINED_EVENTS 8

/* One file as the last listing saw it. Every name the always-skip list and the attributes let through is kept, so a
 * filter change needs no new listing; the gate holds the ones the filter admits. */
struct listed_file {
	char *key;      /* the relative path folded (R-KEY-1): "SUB/CLIP.MP4" */
	char *location; /* the row path, '/', the relative path */
	char *gate_key; /* the key, made unique for the gate when a case-sensitive folder repeats it */
	uint64_t size;
	int64_t write_ms;
	uint32_t ordinal; /* listing order: a folder's files, then its subfolders depth first */
	uint32_t occurrence;
	enum pl_entry_kind kind;
	bool matched; /* the extension filter admits it */
};

struct rename_pair {
	char *old_key;
	char *new_key;
	int rescans; /* listings that did not show the new name yet */
};

enum missing_cause {
	MISSING_NONE,
	MISSING_OPEN_FAILED,
	MISSING_DENIED,
	MISSING_LOST,
};

struct pl_folder_provider {
	/* fixed at create */
	char *location; /* as the row gives it, without a trailing separator */
	bool include_subfolders;
	struct pl_thread *thread;
	struct pl_stop *stop;
	struct pl_wake *wake; /* a request is waiting */

	/* requests, under request_mutex */
	pthread_mutex_t request_mutex;
	struct pl_container_options requested; /* extension_filter points at requested_filter */
	char *requested_filter;
	bool options_changed;
	bool refresh_requested;
	bool release_requested;

	/* worker only */
	struct pl_container_options options; /* extension_filter points at filter_text */
	char *filter_text;
	struct pl_filter filter;
	struct pl_watch watch;
	struct pl_gate gate;
	struct pl_folder_os *os; /* the listing, the probe and the watch */
	bool arm_failed;         /* re-arming inside a rescan failed; the main loop reports it */
	int64_t debounce_ms;     /* when the debounced rescan is due; 0 when none is */
	enum missing_cause missing;
	DARRAY(struct rename_pair) notified_renames; /* paired events since the last listing (relative names) */
	DARRAY(struct rename_pair) renames;          /* carried to the next publication: key to key */
	DARRAY(struct listed_file) files;            /* the last good listing */
	DARRAY(char *) logged_paths;                 /* skipped subfolders already logged (R-DIR-1) */
	bool listed;
	bool gate_fed;
	size_t cloud_count;
	int64_t last_ok_unix_ms;
	struct pl_publication publication; /* what the receiver was given, and the lock (R-DIR-11) */
};

/* ---- time ---- */

static int64_t mono_now(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

/* ---- listing ---- */

static void free_listed_file(struct listed_file *file)
{
	bfree(file->key);
	bfree(file->location);
	bfree(file->gate_key);
}

static void free_listing(struct darray *array)
{
	DARRAY(struct listed_file) files;
	files.da = *array;
	for (size_t i = 0; i < files.num; i++)
		free_listed_file(&files.array[i]);
	da_free(files);
	*array = files.da;
}

static bool stopping(struct pl_folder_provider *p)
{
	return pl_stop_is_set(p->stop);
}

/* One listing in progress: the context of the listing sink */
struct scan {
	struct pl_folder_provider *provider;
	DARRAY(struct listed_file) files;
	uint32_t ordinal;
	size_t cloud;
};

/* Each skipped subfolder is logged once (R-DIR-1) */
static void log_skipped_once(void *context, const char *relative, const char *why)
{
	struct pl_folder_provider *p = ((struct scan *)context)->provider;
	for (size_t i = 0; i < p->logged_paths.num; i++) {
		if (strcmp(p->logged_paths.array[i], relative) == 0)
			return;
	}
	char *copy = bstrdup(relative);
	da_push_back(p->logged_paths, &copy);
	obs_log(LOG_INFO, "folder %s: subfolder %s skipped (%s)", p->location, relative, why);
}

static void add_listed_file(void *context, const char *relative, const char *name, uint64_t size, int64_t write_ms,
			    bool cloud)
{
	struct scan *scan = context;
	struct pl_folder_provider *p = scan->provider;
	if (pl_filter_always_skipped(name))
		return;
	if (cloud) {
		/* counted for the warning only when it would otherwise play */
		if (pl_filter_matches(&p->filter, name))
			scan->cloud++;
		return;
	}
	struct listed_file *file = da_push_back_new(scan->files);
	struct dstr location = {0};
	dstr_copy(&location, p->location);
	dstr_cat_ch(&location, '/');
	dstr_cat(&location, relative);
	file->location = location.array;
	file->key = pl_fold_key(relative);
	file->size = size;
	file->write_ms = write_ms;
	file->ordinal = scan->ordinal++;
	file->kind = pl_filter_kind(name);
}

/* Lists the folder (and its subfolders when the option is on) without opening any file (R-DIR-1); the last good
 * listing is replaced only by a complete one */
static enum pl_folder_scan scan_folder(struct pl_folder_provider *p, size_t *cloud_count)
{
	struct scan scan = {.provider = p};
	da_init(scan.files);
	struct pl_folder_listing_sink sink = {&scan, add_listed_file, log_skipped_once};
	enum pl_folder_scan result = pl_folder_os_list(p->os, p->stop, &sink);
	if (result != PL_FOLDER_SCAN_OK) {
		free_listing(&scan.files.da);
		return result;
	}
	free_listing(&p->files.da);
	p->files.da = scan.files.da;
	*cloud_count = scan.cloud;
	return PL_FOLDER_SCAN_OK;
}

/* Hands the files the filter admits to the gate, with occurrences numbered in listing order (R-KEY-1) and gate keys
 * made unique */
static void feed_gate(struct pl_folder_provider *p, bool initial)
{
	size_t count = 0;
	for (size_t i = 0; i < p->files.num; i++) {
		struct listed_file *file = &p->files.array[i];
		file->matched = pl_filter_matches(&p->filter, file->location);
		count += file->matched;
	}

	struct pl_entry *view = bzalloc((count ? count : 1) * sizeof(*view));
	size_t *index = bmalloc((count ? count : 1) * sizeof(*index));
	size_t n = 0;
	for (size_t i = 0; i < p->files.num; i++) {
		if (!p->files.array[i].matched)
			continue;
		view[n].key = p->files.array[i].key;
		view[n].source_ordinal = p->files.array[i].ordinal;
		index[n++] = i;
	}
	pl_assign_occurrences(view, count);

	struct pl_gate_listing *listing = bmalloc((count ? count : 1) * sizeof(*listing));
	for (size_t j = 0; j < count; j++) {
		struct listed_file *file = &p->files.array[index[j]];
		file->occurrence = view[j].occurrence;
		bfree(file->gate_key);
		if (file->occurrence == 1) {
			file->gate_key = bstrdup(file->key);
		} else {
			struct dstr unique = {0};
			dstr_printf(&unique, "%s\x1f%u", file->key, file->occurrence);
			file->gate_key = unique.array;
		}
		listing[j].key = file->gate_key;
		listing[j].location = file->location;
		listing[j].size = file->size;
		listing[j].write_ms = file->write_ms;
	}
	pl_gate_listing(&p->gate, listing, count, initial, mono_now(), pl_utc_now_ms(), pl_folder_os_probe, p->os);
	bfree(listing);
	bfree(index);
	bfree(view);
}

/* ---- renames (R-DIR-8) ---- */

static void free_renames(struct darray *array)
{
	DARRAY(struct rename_pair) renames;
	renames.da = *array;
	for (size_t i = 0; i < renames.num; i++) {
		bfree(renames.array[i].old_key);
		bfree(renames.array[i].new_key);
	}
	da_free(renames);
	*array = renames.da;
}

/* Keeps the key an item had at the last publication: A to B then B to C is one rename, A to C; back to A is none, and
 * so is a change of case only, as the key is the same */
static void carry_rename(struct pl_folder_provider *p, const char *old_key, const char *new_key)
{
	if (strcmp(old_key, new_key) == 0)
		return;
	for (size_t i = 0; i < p->renames.num; i++) {
		struct rename_pair *pair = &p->renames.array[i];
		if (strcmp(pair->new_key, old_key) != 0)
			continue;
		if (strcmp(pair->old_key, new_key) == 0) {
			bfree(pair->old_key);
			bfree(pair->new_key);
			da_erase(p->renames, i);
		} else {
			bfree(pair->new_key);
			pair->new_key = bstrdup(new_key);
		}
		return;
	}
	struct rename_pair pair = {bstrdup(old_key), bstrdup(new_key), 0};
	da_push_back(p->renames, &pair);
}

static bool listing_has(const struct pl_folder_provider *p, const char *key)
{
	for (size_t i = 0; i < p->files.num; i++) {
		if (strcmp(p->files.array[i].key, key) == 0)
			return true;
	}
	return false;
}

/* Paired rename events move an admitted file to its new name without a new wait (R-DIR-8), once a listing shows the
 * new name: one taken before the rename still shows the old one, and moving the file then would let that listing
 * gate the old name as new and drop the new one. Such a rename waits for the next listing, once. A folder rename is
 * not followed: the files below it are listed as new. */
static void apply_notified_renames(struct pl_folder_provider *p)
{
	DARRAY(struct rename_pair) waiting;
	da_init(waiting);
	for (size_t i = 0; i < p->notified_renames.num; i++) {
		struct rename_pair *pair = &p->notified_renames.array[i];
		char *old_key = pl_fold_key(pair->old_key);
		char *new_key = pl_fold_key(pair->new_key);
		if (!listing_has(p, new_key)) {
			if (++pair->rescans < 2) {
				da_push_back(waiting, pair);
				pair->old_key = NULL;
				pair->new_key = NULL;
			}
			bfree(old_key);
			bfree(new_key);
			continue;
		}
		/* the row's own path as it is, then the relative path (the watch gives it with '/') */
		struct dstr location = {0};
		dstr_printf(&location, "%s/%s", p->location, pair->new_key);
		uint64_t size = 0;
		int64_t write_ms = 0;
		int64_t sampled_ms = 0;
		if (pl_gate_find(&p->gate, old_key) &&
		    pl_folder_os_sample(p->os, location.array, &size, &write_ms, &sampled_ms) &&
		    pl_gate_rename(&p->gate, old_key, new_key, location.array, size, write_ms, mono_now()))
			carry_rename(p, old_key, new_key);
		dstr_free(&location);
		bfree(old_key);
		bfree(new_key);
	}
	free_renames(&p->notified_renames.da);
	p->notified_renames.da = waiting.da;
}

/* A paired rename the watch saw, kept until the next listing */
static void note_rename(void *context, const char *old_relative, const char *new_relative)
{
	struct pl_folder_provider *p = context;
	struct rename_pair pair = {bstrdup(old_relative), bstrdup(new_relative), 0};
	da_push_back(p->notified_renames, &pair);
}

/* ---- the watch modes (R-DIR-3) ---- */

/* Takes in a notification that completed with changes while the worker was busy, so the renames it reports are known
 * before the listing reaches the gate; anything else is left for the main loop */
static void take_completed_changes(struct pl_folder_provider *p)
{
	if (!pl_folder_os_take_completed_changes(p->os, note_rename, p))
		return;
	pl_watch_notified(&p->watch);
	/* the watch machine hears of a failed re-arm from the main loop */
	p->arm_failed = !pl_folder_os_arm(p->os);
	if (!p->debounce_ms)
		p->debounce_ms = mono_now() + DEBOUNCE_MS;
}

static enum pl_folder_scan rescan(struct pl_folder_provider *p)
{
	size_t cloud = 0;
	enum pl_folder_scan result = scan_folder(p, &cloud);
	if (result != PL_FOLDER_SCAN_OK)
		return result;
	p->cloud_count = cloud;
	p->listed = true;
	p->last_ok_unix_ms = pl_utc_now_ms();
	/* a rename the listing already shows moves the admitted file before the gate sees the new name as new */
	take_completed_changes(p);
	apply_notified_renames(p);
	/* the first listing admits what is old and not held open at once (R-DIR-4) */
	feed_gate(p, !p->gate_fed);
	p->gate_fed = true;
	return PL_FOLDER_SCAN_OK;
}

static void log_mode_change(struct pl_folder_provider *p, const struct pl_watch_actions *actions)
{
	if (actions->mode == PL_WATCH_MISSING || actions->mode == PL_WATCH_RESCAN_ONLY) {
		struct dstr error = {0};
		pl_folder_os_error_cat(p->os, &error);
		obs_log(LOG_WARNING, "folder %s: %s (%s; %s)", p->location, pl_watch_mode_name(actions->mode),
			actions->reason, error.array);
		dstr_free(&error);
	} else {
		obs_log(LOG_INFO, "folder %s: %s (%s)", p->location, pl_watch_mode_name(actions->mode),
			actions->reason);
	}
}

/* Feeds an observation to the watch machine and does what it says, in its order (close, open or re-arm, rescan);
 * each outcome is the next observation */
static void run_event(struct pl_folder_provider *p, enum pl_watch_event first)
{
	enum pl_watch_event events[MAX_CHAINED_EVENTS];
	size_t head = 0;
	size_t tail = 0;
	events[tail++] = first;
	while (head < tail && !stopping(p)) {
		enum pl_watch_event event = events[head++];
		if (event == PL_WATCH_OPEN_FAILED)
			p->missing = MISSING_OPEN_FAILED;
		else if (event == PL_WATCH_DENIED)
			p->missing = MISSING_DENIED;
		else if (event == PL_WATCH_LOST && p->watch.mode != PL_WATCH_MISSING &&
			 p->watch.mode != PL_WATCH_STARTING)
			p->missing = MISSING_LOST;

		struct pl_watch_actions actions = pl_watch_handle(&p->watch, event, mono_now());
		if (actions.mode_changed)
			log_mode_change(p, &actions);
		if (actions.mode != PL_WATCH_MISSING)
			p->missing = MISSING_NONE;
		if (actions.close)
			pl_folder_os_close(p->os);
		if (actions.open && tail < MAX_CHAINED_EVENTS)
			events[tail++] = pl_folder_os_open(p->os);
		else if (actions.rearm && tail < MAX_CHAINED_EVENTS)
			/* a handle that cannot be armed may be dead (the folder deleted and made again): recovery opens it
			 * again rather than re-arming it (R-THR-3) */
			events[tail++] = pl_folder_os_arm(p->os) ? PL_WATCH_OPENED_ARMED : pl_folder_os_open(p->os);
		if (actions.rescan) {
			p->debounce_ms = 0;
			enum pl_folder_scan result = rescan(p);
			if (result == PL_FOLDER_SCAN_LOST && tail < MAX_CHAINED_EVENTS)
				events[tail++] = PL_WATCH_LOST;
			else if (result == PL_FOLDER_SCAN_DENIED && tail < MAX_CHAINED_EVENTS)
				events[tail++] = PL_WATCH_DENIED;
		}
	}
}

/* More changes than the buffer holds: listed again (R-DIR-3), and said in the log, where a run of them that turns
 * the folder to rescan-only shows too */
static void on_overflow(struct pl_folder_provider *p)
{
	obs_log(LOG_INFO, "folder %s: more changes than the buffer holds; listing it again", p->location);
	run_event(p, PL_WATCH_OVERFLOW);
}

/* The armed watch completed */
static void on_notification(struct pl_folder_provider *p)
{
	switch (pl_folder_os_take_note(p->os, note_rename, p)) {
	case PL_FOLDER_NOTE_NONE:
		return;
	case PL_FOLDER_NOTE_ABORTED:
		/* not cancelled by us (close waits for its own cancel): keep watching */
		if (!pl_folder_os_arm(p->os))
			run_event(p, PL_WATCH_OPENED_ARM_FAILED);
		return;
	case PL_FOLDER_NOTE_OVERFLOW:
		on_overflow(p);
		return;
	case PL_FOLDER_NOTE_DENIED:
		run_event(p, PL_WATCH_DENIED);
		return;
	case PL_FOLDER_NOTE_LOST:
		run_event(p, PL_WATCH_LOST);
		return;
	case PL_FOLDER_NOTE_CHANGES:
		break;
	}
	pl_watch_notified(&p->watch);
	/* armed again before the debounced rescan, so a change made meanwhile is caught */
	if (!pl_folder_os_arm(p->os))
		run_event(p, PL_WATCH_OPENED_ARM_FAILED);
	if (!p->debounce_ms)
		p->debounce_ms = mono_now() + DEBOUNCE_MS;
}

/* ---- snapshots ---- */

/* The admitted files the filter lets through, in listing order. A file rewritten in place keeps its last admitted
 * version (size, date and so its place) while the new one is gated (PLAN step 5); a file gated for the first time is
 * not listed yet. The keys still in the gate go to pending. */
static struct pl_entry *collect_entries(struct pl_folder_provider *p, size_t *count, struct darray *pending_out)
{
	DARRAY(char *) pending;
	da_init(pending);
	struct pl_entry *entries = bzalloc((p->files.num ? p->files.num : 1) * sizeof(*entries));
	size_t n = 0;
	for (size_t i = 0; i < p->files.num; i++) {
		const struct listed_file *file = &p->files.array[i];
		if (!file->matched || !file->gate_key)
			continue;
		const struct pl_gate_file *gated = pl_gate_find(&p->gate, file->gate_key);
		if (!gated)
			continue;
		if (!gated->admitted) {
			char *key = bstrdup(file->key);
			da_push_back(pending, &key);
		}
		if (!gated->has_version)
			continue;
		struct pl_entry *entry = &entries[n++];
		entry->location = bstrdup(gated->location);
		entry->key = bstrdup(file->key);
		entry->occurrence = file->occurrence;
		entry->source_ordinal = file->ordinal;
		entry->kind = file->kind;
		entry->has_date = true;
		entry->date_unix_ms = gated->version_write_ms;
		entry->size = gated->version_size;
		for (size_t r = 0; r < p->renames.num; r++) {
			if (strcmp(p->renames.array[r].new_key, file->key) == 0) {
				entry->previous_key = bstrdup(p->renames.array[r].old_key);
				break;
			}
		}
	}
	*count = n;
	*pending_out = pending.da;
	return entries;
}

/* The row status of the folder (R-LIST-5); message holds the text status points at */
static void fill_status(struct pl_folder_provider *p, struct pl_publication_status *status, struct dstr *message)
{
	enum pl_status_level level = PL_STATUS_OK;
	if (p->watch.mode == PL_WATCH_MISSING) {
		level = PL_STATUS_ERROR;
		switch (p->missing) {
		case MISSING_DENIED:
			dstr_copy(message, PL_ACCESS_DENIED_TEXT);
			break;
		case MISSING_LOST:
			dstr_copy(message, "the folder went away (deleted, share offline or drive removed)");
			break;
		default:
			dstr_copy(message, "folder not found, or not a folder");
			break;
		}
		status->stale = p->listed;
	}
	size_t denied = pl_gate_denied_count(&p->gate);
	if (denied) {
		level = PL_STATUS_ERROR;
		if (message->len)
			dstr_cat(message, "; ");
		dstr_catf(message, PL_ACCESS_DENIED_TEXT " to %zu files", denied);
	}
	if (p->cloud_count) {
		if (level == PL_STATUS_OK)
			level = PL_STATUS_WARNING;
		if (message->len)
			dstr_cat(message, "; ");
		dstr_catf(message, "%zu cloud-only files not downloaded; mark the folder Always keep on this device",
			  p->cloud_count);
	}
	status->level = level;
	status->message = message->array;
	status->last_ok_unix_ms = p->last_ok_unix_ms;
}

static void free_pending(struct darray *array)
{
	DARRAY(char *) pending;
	pending.da = *array;
	for (size_t i = 0; i < pending.num; i++)
		bfree(pending.array[i]);
	da_free(pending);
	*array = pending.da;
}

/* After anything that can change what the row shows: build the current membership and publish it, or keep it while
 * the list is locked (R-DIR-11) */
static void update_publication(struct pl_folder_provider *p, bool release)
{
	/* nothing to say before the first listing, unless the folder is missing */
	if (!p->listed && p->watch.mode != PL_WATCH_MISSING)
		return;
	size_t count = 0;
	DARRAY(char *) pending;
	struct pl_entry *raw = collect_entries(p, &count, &pending.da);
	struct dstr message = {0};
	struct pl_publication_status status = {0};
	fill_status(p, &status, &message);
	status.listed = p->listed;
	status.lock_point = p->listed;
	status.pending_keys = pending.array;
	status.pending_count = pending.num;
	/* renames are published once, or dropped with a membership kept while the list is locked */
	if (pl_publication_update(&p->publication, &p->options, raw, count, &status, release) !=
	    PL_PUBLICATION_UNCHANGED)
		free_renames(&p->renames.da);
	dstr_free(&message);
	free_pending(&pending.da);
}

/* ---- requests ---- */

static void copy_options(struct pl_container_options *dst, char **filter_text, const struct pl_container_options *src)
{
	*dst = *src;
	bfree(*filter_text);
	*filter_text = bstrdup(src->extension_filter ? src->extension_filter : "");
	dst->extension_filter = *filter_text;
}

static void apply_options(struct pl_folder_provider *p, const struct pl_container_options *options)
{
	bool filter_changed = strcmp(options->extension_filter, p->filter_text) != 0;
	copy_options(&p->options, &p->filter_text, options);
	p->options.include_subfolders = p->include_subfolders;
	pl_watch_set_interval(&p->watch, p->options.folder_check_enabled, p->options.folder_check_interval_s,
			      mono_now());
	if (filter_changed) {
		pl_filter_free(&p->filter);
		pl_filter_parse(&p->filter, p->filter_text);
		/* no new listing: the files the new filter admits are gated from the last one */
		if (p->listed)
			feed_gate(p, true);
	}
	/* with the lock turned off, the next publication carries the waiting changes at once (R-DIR-11) */
}

static void take_requests(struct pl_folder_provider *p, bool *release)
{
	struct pl_container_options options = {0};
	char *filter_text = NULL;
	pthread_mutex_lock(&p->request_mutex);
	bool options_changed = p->options_changed;
	bool refresh = p->refresh_requested;
	*release = p->release_requested;
	if (options_changed)
		copy_options(&options, &filter_text, &p->requested);
	p->options_changed = false;
	p->refresh_requested = false;
	p->release_requested = false;
	pthread_mutex_unlock(&p->request_mutex);

	if (options_changed)
		apply_options(p, &options);
	bfree(filter_text);
	if (refresh)
		run_event(p, PL_WATCH_TIMER);
}

/* ---- the worker ---- */

static void worker_main(void *data)
{
	struct pl_folder_provider *p = data;
	os_set_thread_name("obs-playlister: folder");
	pl_watch_init(&p->watch, p->options.folder_check_enabled, p->options.folder_check_interval_s, mono_now());

	while (!stopping(p)) {
		bool release = false;
		take_requests(p, &release);
		int64_t now = mono_now();
		if (now >= p->watch.next_ms)
			run_event(p, PL_WATCH_TIMER);
		if (p->arm_failed) {
			p->arm_failed = false;
			run_event(p, PL_WATCH_OPENED_ARM_FAILED);
		}
		if (p->debounce_ms && mono_now() >= p->debounce_ms) {
			p->debounce_ms = 0;
			run_event(p, PL_WATCH_CHANGED);
		}
		if (pl_gate_next_deadline(&p->gate) <= mono_now())
			pl_gate_tick(&p->gate, mono_now(), pl_utc_now_ms(), pl_folder_os_sample, pl_folder_os_probe,
				     p->os);
		if (stopping(p))
			break;
		update_publication(p, release);

		int64_t next = p->watch.next_ms;
		if (p->debounce_ms && p->debounce_ms < next)
			next = p->debounce_ms;
		int64_t gate_next = pl_gate_next_deadline(&p->gate);
		if (gate_next < next)
			next = gate_next;
		int64_t wait_ms = next - mono_now();
		uint32_t timeout = wait_ms <= 0 ? 0 : wait_ms > 3600000 ? 3600000 : (uint32_t)wait_ms;
		enum pl_folder_wait signaled = pl_folder_os_wait(p->os, p->stop, p->wake, timeout);
		if (signaled == PL_FOLDER_WAIT_STOP)
			break;
		if (signaled == PL_FOLDER_WAIT_NOTIFIED)
			on_notification(p);
	}
	pl_folder_os_close(p->os);
}

/* ---- the interface ---- */

static void free_provider(struct pl_folder_provider *p)
{
	pl_folder_os_destroy(p->os);
	pl_stop_destroy(p->stop);
	pl_wake_destroy(p->wake);
	pthread_mutex_destroy(&p->request_mutex);
	bfree(p->requested_filter);
	bfree(p->filter_text);
	pl_filter_free(&p->filter);
	pl_gate_free(&p->gate);
	free_renames(&p->notified_renames.da);
	free_renames(&p->renames.da);
	free_listing(&p->files.da);
	for (size_t i = 0; i < p->logged_paths.num; i++)
		bfree(p->logged_paths.array[i]);
	da_free(p->logged_paths);
	pl_publication_free(&p->publication);
	bfree(p->location);
	bfree(p);
}

struct pl_folder_provider *pl_folder_provider_create(const char *location, const char *row_uuid, uint64_t instance_id,
						     const struct pl_container_options *options, pl_publish_fn publish,
						     void *owner)
{
	UNUSED_PARAMETER(row_uuid);
	if (!location || !options || !publish)
		return NULL;
	struct pl_folder_provider *p = bzalloc(sizeof(*p));
	p->location = bstrdup(location);
	/* entries are "<location>/<relative path>", so the row's own trailing separators go (a drive root keeps
	 * nothing after its colon) */
	size_t length = strlen(p->location);
	while (length > 1 && (p->location[length - 1] == '/' || p->location[length - 1] == '\\'))
		p->location[--length] = '\0';
	p->os = pl_folder_os_create(location, options->include_subfolders);
	pl_publication_init(&p->publication, instance_id, publish, owner);
	p->include_subfolders = options->include_subfolders;
	pthread_mutex_init_value(&p->request_mutex);
	copy_options(&p->requested, &p->requested_filter, options);
	copy_options(&p->options, &p->filter_text, options);
	pl_filter_parse(&p->filter, p->filter_text);
	pl_gate_init(&p->gate);
	da_init(p->notified_renames);
	da_init(p->renames);
	da_init(p->files);
	da_init(p->logged_paths);

	p->stop = pl_stop_create();
	p->wake = pl_wake_create();
	if (pthread_mutex_init(&p->request_mutex, NULL) != 0 || !p->stop || !p->wake || !p->os) {
		free_provider(p);
		return NULL;
	}
	unsigned long error = 0;
	p->thread = pl_thread_start(worker_main, p, &error);
	if (!p->thread) {
		struct dstr code = {0};
		pl_os_error_cat(&code, error);
		obs_log(LOG_ERROR, "folder %s: the worker thread could not be started (%s)", p->location, code.array);
		dstr_free(&code);
		free_provider(p);
		return NULL;
	}
	return p;
}

void pl_folder_provider_refresh_now(struct pl_folder_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->refresh_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	pl_wake_set(provider->wake);
}

void pl_folder_provider_set_options(struct pl_folder_provider *provider, const struct pl_container_options *options)
{
	pthread_mutex_lock(&provider->request_mutex);
	copy_options(&provider->requested, &provider->requested_filter, options);
	provider->options_changed = true;
	pthread_mutex_unlock(&provider->request_mutex);
	pl_wake_set(provider->wake);
}

void pl_folder_provider_release_held(struct pl_folder_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->release_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	pl_wake_set(provider->wake);
}

size_t pl_folder_provider_held_changes(struct pl_folder_provider *provider)
{
	return pl_publication_held_changes(&provider->publication);
}

void pl_folder_provider_destroy(struct pl_folder_provider *provider)
{
	if (!provider)
		return;
	/* a listing or probe stuck on a stalled share is interrupted where the platform can (R-THR-3, R-LNX-7) */
	pl_stop_set(provider->stop);
	pl_thread_join(provider->thread, provider->location);
	free_provider(provider);
}
