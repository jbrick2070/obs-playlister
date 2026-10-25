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

#include "folder-state.h"

#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>

/* ---- Write-complete gate ---- */

void pl_gate_init(struct pl_gate *gate)
{
	da_init(gate->files);
}

static void free_file(struct pl_gate_file *file)
{
	bfree(file->key);
	bfree(file->location);
}

void pl_gate_free(struct pl_gate *gate)
{
	for (size_t i = 0; i < gate->files.num; i++)
		free_file(&gate->files.array[i]);
	da_free(gate->files);
}

/* Binary search over the sorted files: true when found; *position is its index, or where it would go */
static bool locate(const struct pl_gate *gate, const char *key, size_t *position)
{
	size_t low = 0;
	size_t high = gate->files.num;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		int order = strcmp(gate->files.array[middle].key, key);
		if (order == 0) {
			*position = middle;
			return true;
		}
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	*position = low;
	return false;
}

static void set_location(struct pl_gate_file *file, const char *location)
{
	if (strcmp(file->location, location) != 0) {
		bfree(file->location);
		file->location = bstrdup(location);
	}
}

/* A tuple seen for the first time (or changed) starts its window at the monotonic time it was seen */
static void start_window(struct pl_gate_file *file, uint64_t size, int64_t write_ms, int64_t seen_mono_ms)
{
	file->admitted = false;
	file->denied = false;
	file->size = size;
	file->write_ms = write_ms;
	file->sampled_ms = seen_mono_ms;
	file->deadline_ms = seen_mono_ms + PL_GATE_WINDOW_MS;
}

/* The last write is the window old. A last-write time in the future (a share with a skewed clock) cannot be
 * trusted; such a file counts as old once its size and time have held for the window, which the caller has
 * already checked. */
static bool write_settled(int64_t write_ms, int64_t utc_ms)
{
	return write_ms > utc_ms || utc_ms - write_ms >= PL_GATE_WINDOW_MS;
}

/* A probe result applied to a candidate; false when the file is gone */
static bool apply_probe(struct pl_gate_file *file, enum pl_probe_result result, int64_t mono_ms)
{
	switch (result) {
	case PL_PROBE_OK:
		file->admitted = true;
		file->denied = false;
		file->has_version = true;
		file->version_size = file->size;
		file->version_write_ms = file->write_ms;
		return true;
	case PL_PROBE_DENIED:
		file->denied = true;
		file->deadline_ms = mono_ms + PL_GATE_WINDOW_MS;
		return true;
	case PL_PROBE_BUSY:
		/* held open by a writer: due again later, without holding up the others */
		file->denied = false;
		file->deadline_ms = mono_ms + PL_GATE_WINDOW_MS;
		return true;
	case PL_PROBE_GONE:
		break;
	}
	return false;
}

static int compare_listings(const void *a, const void *b)
{
	const struct pl_gate_listing *const *first = a;
	const struct pl_gate_listing *const *second = b;
	return strcmp((*first)->key, (*second)->key);
}

void pl_gate_listing(struct pl_gate *gate, const struct pl_gate_listing *files, size_t count, bool initial,
		     int64_t mono_ms, int64_t utc_ms, pl_probe_fn probe, void *context)
{
	/* both lists sorted by key and walked once, so a scan of a large folder stays cheap */
	const struct pl_gate_listing **sorted = bmalloc((count ? count : 1) * sizeof(*sorted));
	for (size_t i = 0; i < count; i++)
		sorted[i] = &files[i];
	qsort((void *)sorted, count, sizeof(*sorted), compare_listings);

	DARRAY(struct pl_gate_file) merged;
	da_init(merged);
	da_reserve(merged, count);
	size_t known = 0;
	for (size_t i = 0; i < count; i++) {
		const struct pl_gate_listing *seen = sorted[i];
		/* known files sorting before this one are no longer listed */
		while (known < gate->files.num && strcmp(gate->files.array[known].key, seen->key) < 0)
			free_file(&gate->files.array[known++]);

		if (known < gate->files.num && strcmp(gate->files.array[known].key, seen->key) == 0) {
			struct pl_gate_file file = gate->files.array[known++];
			set_location(&file, seen->location);
			if (file.size != seen->size || file.write_ms != seen->write_ms)
				start_window(&file, seen->size, seen->write_ms, mono_ms);
			da_push_back(merged, &file);
			continue;
		}

		struct pl_gate_file file = {0};
		file.key = bstrdup(seen->key);
		file.location = bstrdup(seen->location);
		start_window(&file, seen->size, seen->write_ms, mono_ms);
		/* the initial scan admits what is old and not held open at once; younger files cannot pass yet */
		if (initial && seen->write_ms <= utc_ms && utc_ms - seen->write_ms >= PL_GATE_WINDOW_MS &&
		    !apply_probe(&file, probe(context, seen->location), mono_ms)) {
			free_file(&file);
			continue;
		}
		da_push_back(merged, &file);
	}
	while (known < gate->files.num)
		free_file(&gate->files.array[known++]);

	da_free(gate->files);
	gate->files.da = merged.da;
	bfree((void *)sorted);
}

static void remove_file(struct pl_gate *gate, size_t index)
{
	free_file(&gate->files.array[index]);
	da_erase(gate->files, index);
}

size_t pl_gate_tick(struct pl_gate *gate, int64_t mono_ms, int64_t utc_ms, pl_sample_fn sample, pl_probe_fn probe,
		    void *context)
{
	size_t admitted = 0;
	for (size_t i = gate->files.num; i-- > 0;) {
		struct pl_gate_file *file = &gate->files.array[i];
		if (file->admitted || file->deadline_ms > mono_ms)
			continue;

		uint64_t size = 0;
		int64_t write_ms = 0;
		int64_t sampled_ms = mono_ms;
		if (!sample(context, file->location, &size, &write_ms, &sampled_ms)) {
			remove_file(gate, i);
			continue;
		}
		if (size != file->size || write_ms != file->write_ms) {
			/* still being written: the window starts again from this sample */
			start_window(file, size, write_ms, sampled_ms);
			continue;
		}
		/* two samples at least the window apart, timed when each was actually taken */
		if (sampled_ms - file->sampled_ms < PL_GATE_WINDOW_MS) {
			file->deadline_ms = file->sampled_ms + PL_GATE_WINDOW_MS;
			continue;
		}
		if (!write_settled(file->write_ms, utc_ms)) {
			file->deadline_ms = mono_ms + (file->write_ms + PL_GATE_WINDOW_MS - utc_ms);
			continue;
		}
		if (!apply_probe(file, probe(context, file->location), mono_ms))
			remove_file(gate, i);
		else if (file->admitted)
			admitted++;
	}
	return admitted;
}

int64_t pl_gate_next_deadline(const struct pl_gate *gate)
{
	int64_t next = INT64_MAX;
	for (size_t i = 0; i < gate->files.num; i++) {
		const struct pl_gate_file *file = &gate->files.array[i];
		if (!file->admitted && file->deadline_ms < next)
			next = file->deadline_ms;
	}
	return next;
}

bool pl_gate_rename(struct pl_gate *gate, const char *old_key, const char *new_key, const char *new_location,
		    uint64_t size, int64_t write_ms, int64_t mono_ms)
{
	size_t index;
	if (!locate(gate, old_key, &index))
		return false;
	struct pl_gate_file file = gate->files.array[index];
	da_erase(gate->files, index);

	/* a file the rename replaced is gone */
	size_t position;
	if (locate(gate, new_key, &position))
		remove_file(gate, position);

	bfree(file.key);
	file.key = bstrdup(new_key);
	set_location(&file, new_location);
	if (file.size != size || file.write_ms != write_ms)
		start_window(&file, size, write_ms, mono_ms);

	locate(gate, new_key, &position);
	da_insert(gate->files, position, &file);
	return true;
}

const struct pl_gate_file *pl_gate_find(const struct pl_gate *gate, const char *key)
{
	size_t index;
	return locate(gate, key, &index) ? &gate->files.array[index] : NULL;
}

size_t pl_gate_pending_count(const struct pl_gate *gate)
{
	size_t pending = 0;
	for (size_t i = 0; i < gate->files.num; i++)
		pending += !gate->files.array[i].admitted;
	return pending;
}

size_t pl_gate_admitted_count(const struct pl_gate *gate)
{
	return gate->files.num - pl_gate_pending_count(gate);
}

size_t pl_gate_denied_count(const struct pl_gate *gate)
{
	size_t denied = 0;
	for (size_t i = 0; i < gate->files.num; i++)
		denied += !gate->files.array[i].admitted && gate->files.array[i].denied;
	return denied;
}

/* ---- Watch modes ---- */

#define SAFETY_RESCAN_MS 60000 /* notify mode with the check interval off */
#define RETRY_MS 10000         /* rescan-only and missing with the check interval off */

static uint32_t clamp_interval(uint32_t seconds)
{
	if (seconds < PL_WATCH_INTERVAL_MIN_S)
		return PL_WATCH_INTERVAL_MIN_S;
	if (seconds > PL_WATCH_INTERVAL_MAX_S)
		return PL_WATCH_INTERVAL_MAX_S;
	return seconds;
}

static int64_t period_ms(const struct pl_watch *watch, enum pl_watch_mode mode)
{
	if (watch->check_enabled)
		return (int64_t)watch->check_interval_s * 1000;
	return mode == PL_WATCH_NOTIFY ? SAFETY_RESCAN_MS : RETRY_MS;
}

void pl_watch_init(struct pl_watch *watch, bool check_enabled, uint32_t check_interval_s, int64_t mono_ms)
{
	watch->mode = PL_WATCH_STARTING;
	watch->check_enabled = check_enabled;
	watch->check_interval_s = clamp_interval(check_interval_s);
	/* the first timer opens the folder at once */
	watch->next_ms = mono_ms;
}

void pl_watch_set_interval(struct pl_watch *watch, bool check_enabled, uint32_t check_interval_s, int64_t mono_ms)
{
	uint32_t interval = clamp_interval(check_interval_s);
	if (watch->check_enabled == check_enabled && watch->check_interval_s == interval)
		return;
	watch->check_enabled = check_enabled;
	watch->check_interval_s = interval;
	if (watch->mode != PL_WATCH_STARTING)
		watch->next_ms = mono_ms + period_ms(watch, watch->mode);
}

static void enter(struct pl_watch *watch, struct pl_watch_actions *actions, enum pl_watch_mode mode, const char *reason,
		  int64_t mono_ms)
{
	if (watch->mode != mode) {
		actions->mode_changed = true;
		actions->reason = reason;
	}
	watch->mode = mode;
	watch->next_ms = mono_ms + period_ms(watch, mode);
}

struct pl_watch_actions pl_watch_handle(struct pl_watch *watch, enum pl_watch_event event, int64_t mono_ms)
{
	struct pl_watch_actions actions = {0};
	enum pl_watch_mode before = watch->mode;
	bool closed = before == PL_WATCH_STARTING || before == PL_WATCH_MISSING;

	switch (event) {
	case PL_WATCH_TIMER:
		if (closed) {
			/* a dead handle is never re-armed: open again */
			actions.open = true;
			watch->next_ms = mono_ms + period_ms(watch, PL_WATCH_MISSING);
		} else if (before == PL_WATCH_NOTIFY) {
			actions.rescan = true;
			watch->next_ms = mono_ms + period_ms(watch, before);
		} else {
			actions.rearm = true;
			actions.rescan = true;
			watch->next_ms = mono_ms + period_ms(watch, before);
		}
		break;
	case PL_WATCH_OPENED_ARMED:
		/* a folder just opened is listed; one that was open was listed with this attempt */
		actions.rescan = closed;
		enter(watch, &actions, PL_WATCH_NOTIFY, "notifications armed", mono_ms);
		break;
	case PL_WATCH_OPENED_ARM_FAILED:
		actions.rescan = closed;
		enter(watch, &actions, PL_WATCH_RESCAN_ONLY, "notifications cannot be armed; rescanning instead",
		      mono_ms);
		break;
	case PL_WATCH_OPEN_FAILED:
		actions.close = true;
		enter(watch, &actions, PL_WATCH_MISSING, "the folder cannot be opened", mono_ms);
		break;
	case PL_WATCH_DENIED:
		actions.close = true;
		enter(watch, &actions, PL_WATCH_MISSING, "access denied (permissions or security software)", mono_ms);
		break;
	case PL_WATCH_LOST:
		if (!closed) {
			actions.close = true;
			enter(watch, &actions, PL_WATCH_MISSING, "the folder went away", mono_ms);
		}
		break;
	case PL_WATCH_CHANGED:
		actions.rescan = before == PL_WATCH_NOTIFY;
		break;
	case PL_WATCH_OVERFLOW:
		if (before == PL_WATCH_NOTIFY) {
			/* too many changes to report: arm again, then list everything */
			actions.rearm = true;
			actions.rescan = true;
		}
		break;
	}
	actions.mode = watch->mode;
	return actions;
}

const char *pl_watch_mode_name(enum pl_watch_mode mode)
{
	switch (mode) {
	case PL_WATCH_STARTING:
		return "starting";
	case PL_WATCH_NOTIFY:
		return "notify";
	case PL_WATCH_RESCAN_ONLY:
		return "rescan-only";
	case PL_WATCH_MISSING:
		return "missing";
	}
	return "unknown";
}
