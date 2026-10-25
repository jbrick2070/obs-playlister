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
 * it with ReadDirectoryChangesW (R-DIR-2, R-DIR-3), admits files through the write-complete gate (R-DIR-4) and
 * publishes snapshots of plain entries. The worker alone opens and closes its directory handle (R-THR-3); other
 * threads only leave requests and wake it. */

#include "provider.h"

#include <windows.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#include "core/filter.h"
#include "core/folder-state.h"
#include "core/keys.h"
#include "core/merge.h"
#include "fold-key-win.h"

#define WORKER_STACK_SIZE (256 * 1024)  /* R-THR-2 */
#define NOTIFY_BUFFER_BYTES (64 * 1024) /* the largest ReadDirectoryChangesW buffer a network share takes */
#define DEBOUNCE_MS 500
/* cloud placeholders: opening one starts a download (R-DIR-5) */
#define CLOUD_ATTRIBUTES (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)
#define FILETIME_UNIX_EPOCH 116444736000000000LL /* 1970-01-01 in 100 ns units since 1601 */
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

enum scan_result {
	SCAN_OK,
	SCAN_LOST,
	SCAN_DENIED,
	SCAN_STOPPED,
};

struct pl_provider {
	/* fixed at create */
	char *location;  /* as the row gives it, without a trailing separator */
	wchar_t *root_w; /* canonical, with its \\?\ prefix (R-THR-5); NULL when the path cannot be converted */
	uint64_t instance_id;
	pl_publish_fn publish;
	void *owner;
	bool include_subfolders;
	HANDLE thread;
	HANDLE stop_event; /* manual reset */
	HANDLE wake_event; /* auto reset: a request is waiting */

	/* requests, under request_mutex */
	pthread_mutex_t request_mutex;
	struct pl_container_options requested; /* extension_filter points at requested_filter */
	char *requested_filter;
	bool options_changed;
	bool refresh_requested;
	bool release_requested;
	volatile long held_changes;

	/* worker only */
	struct pl_container_options options; /* extension_filter points at filter_text */
	char *filter_text;
	struct pl_filter filter;
	struct pl_watch watch;
	struct pl_gate gate;
	HANDLE dir;
	OVERLAPPED overlapped;
	HANDLE overlapped_event;
	DWORD *buffer;
	bool armed;
	bool arm_failed;     /* re-arming inside a rescan failed; the main loop reports it */
	int64_t debounce_ms; /* when the debounced rescan is due; 0 when none is */
	DWORD last_error;
	enum missing_cause missing;
	DARRAY(struct rename_pair) notified_renames; /* paired events since the last listing (relative names) */
	char *renamed_from;                          /* an old name whose new name has not come yet */
	DARRAY(struct rename_pair) renames;          /* carried to the next publication: key to key */
	DARRAY(struct listed_file) files;            /* the last good listing */
	DARRAY(char *) logged_paths;                 /* skipped subfolders already logged (R-DIR-1) */
	bool listed;
	bool gate_fed;
	size_t cloud_count;
	int64_t last_ok_unix_ms;
	uint64_t generation;
	uint64_t content_revision;
	char *last_signature;
	struct pl_entry *published; /* the published membership, without renames */
	size_t published_count;
	char *published_content;
	bool lock_ready;       /* R-DIR-11: the list taken at start is published; the lock applies from now */
	struct pl_entry *held; /* R-DIR-11: the newest membership, kept while the list is locked */
	size_t held_count;
	char *held_content;
};

/* ---- time and paths ---- */

static int64_t mono_now(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

static int64_t filetime_to_unix_ms(FILETIME time)
{
	ULARGE_INTEGER value;
	value.LowPart = time.dwLowDateTime;
	value.HighPart = time.dwHighDateTime;
	return ((int64_t)value.QuadPart - FILETIME_UNIX_EPOCH) / 10000;
}

static int64_t utc_now_ms(void)
{
	FILETIME now;
	GetSystemTimePreciseAsFileTime(&now);
	return filetime_to_unix_ms(now);
}

static wchar_t *wide_concat(const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
	size_t size = wcslen(a) + wcslen(b) + wcslen(c) + 1;
	wchar_t *result = bmalloc(size * sizeof(wchar_t));
	wcscpy_s(result, size, a);
	wcscat_s(result, size, b);
	wcscat_s(result, size, c);
	return result;
}

/* R-THR-5: canonicalized with GetFullPathNameW (backslashes, "." and ".." resolved, no trailing separator except at
 * a root), then given the \\?\ or \\?\UNC\ prefix; a path that already has a prefix is kept. NULL when it cannot be
 * converted. */
static wchar_t *win32_path(const char *utf8)
{
	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(utf8, 0, &wide);
	if (!wide || !*wide) {
		bfree(wide);
		return NULL;
	}
	DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
	wchar_t *full = needed ? bmalloc(needed * sizeof(wchar_t)) : NULL;
	DWORD length = full ? GetFullPathNameW(wide, needed, full, NULL) : 0;
	bfree(wide);
	if (!length || length >= needed) {
		bfree(full);
		return NULL;
	}
	if (wcsncmp(full, L"\\\\?\\", 4) == 0 || wcsncmp(full, L"\\\\.\\", 4) == 0) {
		/* the prefix turns off normalization, so the '/' of "<row>/<relative path>" becomes '\' here */
		for (wchar_t *c = full; *c; c++) {
			if (*c == L'/')
				*c = L'\\';
		}
		return full;
	}
	while (length > 3 && full[length - 1] == L'\\')
		full[--length] = L'\0';
	bool unc = full[0] == L'\\' && full[1] == L'\\';
	wchar_t *result = wide_concat(unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", unc ? full + 2 : full, L"");
	bfree(full);
	return result;
}

/* A path below the root: root, a separator unless the root ends in one, the relative path with backslashes, suffix */
static wchar_t *below_root(const struct pl_provider *p, const char *relative, const wchar_t *suffix)
{
	wchar_t *relative_w = NULL;
	os_utf8_to_wcs_ptr(relative, 0, &relative_w);
	for (wchar_t *c = relative_w; *c; c++) {
		if (*c == L'/')
			*c = L'\\';
	}
	size_t root_length = wcslen(p->root_w);
	bool separator = root_length && p->root_w[root_length - 1] != L'\\' && *relative_w;
	wchar_t *joined = wide_concat(p->root_w, separator ? L"\\" : L"", relative_w);
	wchar_t *result = joined;
	if (*suffix) {
		bool sep = *relative_w || (root_length && p->root_w[root_length - 1] != L'\\');
		result = wide_concat(joined, sep ? L"\\" : L"", suffix);
		bfree(joined);
	}
	bfree(relative_w);
	return result;
}

/* ---- the gate's disk access ---- */

static enum pl_probe_result probe_file(void *context, const char *location)
{
	UNUSED_PARAMETER(context);
	wchar_t *path = win32_path(location);
	if (!path)
		return PL_PROBE_GONE;
	/* reading while denying other writers: any open write handle makes this a sharing violation (R-DIR-4) */
	HANDLE file =
		CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
	bfree(path);
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		return PL_PROBE_OK;
	}
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return PL_PROBE_GONE;
	case ERROR_ACCESS_DENIED:
		return PL_PROBE_DENIED;
	default:
		/* sharing and lock violations, and anything that may pass: tried again one window later */
		return PL_PROBE_BUSY;
	}
}

/* The file's directory entry, as the listing reads it, so an unchanged file never looks changed (R-DIR-4) */
static bool sample_file(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			int64_t *sampled_mono_ms)
{
	UNUSED_PARAMETER(context);
	wchar_t *path = win32_path(location);
	WIN32_FIND_DATAW data;
	HANDLE find = path ? FindFirstFileExW(path, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0)
			   : INVALID_HANDLE_VALUE;
	*sampled_mono_ms = mono_now();
	bfree(path);
	if (find == INVALID_HANDLE_VALUE)
		return false;
	FindClose(find);
	if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;
	*size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	*write_ms = filetime_to_unix_ms(data.ftLastWriteTime);
	return true;
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

static bool stopping(struct pl_provider *p)
{
	return WaitForSingleObject(p->stop_event, 0) == WAIT_OBJECT_0;
}

/* Each skipped subfolder is logged once (R-DIR-1) */
static void log_skipped_once(struct pl_provider *p, const char *relative, const char *why)
{
	for (size_t i = 0; i < p->logged_paths.num; i++) {
		if (strcmp(p->logged_paths.array[i], relative) == 0)
			return;
	}
	char *copy = bstrdup(relative);
	da_push_back(p->logged_paths, &copy);
	obs_log(LOG_INFO, "folder %s: subfolder %s skipped (%s)", p->location, relative, why);
}

static char *join_relative(const char *parent, const char *name)
{
	struct dstr path = {0};
	if (*parent) {
		dstr_copy(&path, parent);
		dstr_cat_ch(&path, '/');
	}
	dstr_cat(&path, name);
	return path.array;
}

static void add_listed_file(struct pl_provider *p, struct darray *array, const char *relative, const char *name,
			    const WIN32_FIND_DATAW *data, uint32_t ordinal)
{
	DARRAY(struct listed_file) files;
	files.da = *array;
	struct listed_file *file = da_push_back_new(files);
	struct dstr location = {0};
	dstr_copy(&location, p->location);
	dstr_cat_ch(&location, '/');
	dstr_cat(&location, relative);
	file->location = location.array;
	file->key = pl_fold_key(relative);
	file->size = ((uint64_t)data->nFileSizeHigh << 32) | data->nFileSizeLow;
	file->write_ms = filetime_to_unix_ms(data->ftLastWriteTime);
	file->ordinal = ordinal;
	file->kind = pl_filter_kind(name);
	*array = files.da;
}

/* Lists the folder (and its subfolders when the option is on) without opening any file. Below the root, hidden and
 * system folders are skipped and reparse points are never followed, so a link back up the tree cannot make the scan
 * loop (R-DIR-1); the root itself is opened even when it is a junction. */
static enum scan_result scan_folder(struct pl_provider *p, struct darray *out, size_t *cloud_count, DWORD *error)
{
	DARRAY(struct listed_file) files;
	DARRAY(char *) folders; /* relative folders still to list; "" is the root */
	da_init(files);
	da_init(folders);
	char *root = bstrdup("");
	da_push_back(folders, &root);
	uint32_t ordinal = 0;
	size_t cloud = 0;
	enum scan_result result = p->root_w ? SCAN_OK : SCAN_LOST;
	if (!p->root_w)
		*error = ERROR_INVALID_NAME;

	while (result == SCAN_OK && folders.num) {
		char *relative = folders.array[folders.num - 1];
		da_pop_back(folders);
		bool is_root = !*relative;
		if (stopping(p)) {
			bfree(relative);
			result = SCAN_STOPPED;
			break;
		}

		wchar_t *pattern = below_root(p, relative, L"*");
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
					       FIND_FIRST_EX_LARGE_FETCH);
		bfree(pattern);
		if (find == INVALID_HANDLE_VALUE) {
			DWORD find_error = GetLastError();
			bool empty = find_error == ERROR_FILE_NOT_FOUND || find_error == ERROR_NO_MORE_FILES;
			if (is_root && !empty) {
				*error = find_error;
				result = find_error == ERROR_ACCESS_DENIED ? SCAN_DENIED : SCAN_LOST;
			} else if (find_error == ERROR_ACCESS_DENIED) {
				log_skipped_once(p, relative, "access denied");
			} else if (!empty && find_error != ERROR_PATH_NOT_FOUND) {
				/* a subfolder that vanished is left out; any other failure (a share dropping after the root
				 * was listed) fails the whole listing, so the last good list stays instead of shrinking */
				*error = find_error;
				result = SCAN_LOST;
			}
			bfree(relative);
			continue;
		}

		DARRAY(char *) subfolders;
		da_init(subfolders);
		do {
			if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
				continue;
			DWORD attributes = data.dwFileAttributes;
			char *name = NULL;
			os_wcs_to_utf8_ptr(data.cFileName, 0, &name);
			char *child = join_relative(relative, name);
			bool hidden = (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
			if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (p->include_subfolders && !hidden) {
					if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
						log_skipped_once(p, child, "a junction, symbolic link or mount point");
					} else {
						da_push_back(subfolders, &child);
						child = NULL;
					}
				}
			} else if (attributes & CLOUD_ATTRIBUTES) {
				/* counted for the warning only when it would otherwise play */
				if (!hidden && !pl_filter_always_skipped(name) && pl_filter_matches(&p->filter, name))
					cloud++;
			} else if (!hidden && !pl_filter_always_skipped(name)) {
				add_listed_file(p, &files.da, child, name, &data, ordinal++);
			}
			bfree(child);
			bfree(name);
		} while (FindNextFileW(find, &data));
		DWORD next_error = GetLastError();
		FindClose(find);
		if (next_error != ERROR_NO_MORE_FILES) {
			/* the listing broke off, as when a share drops: the last good list stays */
			*error = next_error;
			result = next_error == ERROR_ACCESS_DENIED ? SCAN_DENIED : SCAN_LOST;
		}
		/* depth first, in listing order */
		for (size_t i = subfolders.num; i-- > 0;)
			da_push_back(folders, &subfolders.array[i]);
		da_free(subfolders);
		bfree(relative);
	}

	for (size_t i = 0; i < folders.num; i++)
		bfree(folders.array[i]);
	da_free(folders);
	if (result != SCAN_OK) {
		free_listing(&files.da);
		return result;
	}
	free_listing(out);
	*out = files.da;
	*cloud_count = cloud;
	return SCAN_OK;
}

/* Hands the files the filter admits to the gate, with occurrences numbered in listing order (R-KEY-1) and gate keys
 * made unique */
static void feed_gate(struct pl_provider *p, bool initial)
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
	pl_gate_listing(&p->gate, listing, count, initial, mono_now(), utc_now_ms(), probe_file, p);
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
static void carry_rename(struct pl_provider *p, const char *old_key, const char *new_key)
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

static bool listing_has(const struct pl_provider *p, const char *key)
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
static void apply_notified_renames(struct pl_provider *p)
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
		/* the row's own path as it is, then the relative path with '/' (the notification's names use '\') */
		struct dstr relative = {0};
		dstr_copy(&relative, pair->new_key);
		dstr_replace(&relative, "\\", "/");
		struct dstr location = {0};
		dstr_printf(&location, "%s/%s", p->location, relative.array);
		dstr_free(&relative);
		uint64_t size = 0;
		int64_t write_ms = 0;
		int64_t sampled_ms = 0;
		if (pl_gate_find(&p->gate, old_key) && sample_file(p, location.array, &size, &write_ms, &sampled_ms) &&
		    pl_gate_rename(&p->gate, old_key, new_key, location.array, size, write_ms, mono_now()))
			carry_rename(p, old_key, new_key);
		dstr_free(&location);
		bfree(old_key);
		bfree(new_key);
	}
	free_renames(&p->notified_renames.da);
	p->notified_renames.da = waiting.da;
}

static void collect_notified_renames(struct pl_provider *p, DWORD bytes)
{
	const BYTE *cursor = (const BYTE *)p->buffer;
	const BYTE *end = cursor + bytes;
	const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
	while (cursor + header <= end) {
		const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)cursor;
		if (info->FileNameLength && cursor + header + info->FileNameLength <= end) {
			char *name = NULL;
			os_wcs_to_utf8_ptr(info->FileName, info->FileNameLength / sizeof(WCHAR), &name);
			/* the old name is kept across completions, as a pair can be split between two */
			if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
				bfree(p->renamed_from);
				p->renamed_from = name;
				name = NULL;
			} else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && p->renamed_from) {
				struct rename_pair pair = {p->renamed_from, name, 0};
				da_push_back(p->notified_renames, &pair);
				p->renamed_from = NULL;
				name = NULL;
			}
			bfree(name);
		}
		if (!info->NextEntryOffset)
			break;
		cursor += info->NextEntryOffset;
	}
}

/* ---- the directory handle and the watch modes (R-DIR-3) ---- */

static void close_dir(struct pl_provider *p)
{
	if (p->dir == INVALID_HANDLE_VALUE)
		return;
	if (p->armed) {
		CancelIoEx(p->dir, &p->overlapped);
		/* the buffer is the worker's again only once the call has finished (R-THR-3) */
		DWORD bytes = 0;
		GetOverlappedResult(p->dir, &p->overlapped, &bytes, TRUE);
		p->armed = false;
	}
	CloseHandle(p->dir);
	p->dir = INVALID_HANDLE_VALUE;
}

static bool arm_dir(struct pl_provider *p)
{
	if (p->dir == INVALID_HANDLE_VALUE)
		return false;
	if (p->armed)
		return true;
	ResetEvent(p->overlapped_event);
	memset(&p->overlapped, 0, sizeof(p->overlapped));
	p->overlapped.hEvent = p->overlapped_event;
	DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
		       FILE_NOTIFY_CHANGE_SIZE;
	if (!ReadDirectoryChangesW(p->dir, p->buffer, NOTIFY_BUFFER_BYTES, p->include_subfolders, filter, NULL,
				   &p->overlapped, NULL)) {
		p->last_error = GetLastError();
		return false;
	}
	p->armed = true;
	return true;
}

static enum pl_watch_event open_dir(struct pl_provider *p)
{
	close_dir(p);
	if (!p->root_w) {
		p->last_error = ERROR_INVALID_NAME;
		return PL_WATCH_OPEN_FAILED;
	}
	HANDLE dir = CreateFileW(p->root_w, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				 NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	if (dir == INVALID_HANDLE_VALUE) {
		p->last_error = GetLastError();
		return p->last_error == ERROR_ACCESS_DENIED ? PL_WATCH_DENIED : PL_WATCH_OPEN_FAILED;
	}
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(dir, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		/* a file of that name: not a folder */
		CloseHandle(dir);
		p->last_error = ERROR_DIRECTORY;
		return PL_WATCH_OPEN_FAILED;
	}
	p->dir = dir;
	return arm_dir(p) ? PL_WATCH_OPENED_ARMED : PL_WATCH_OPENED_ARM_FAILED;
}

/* Takes in a notification that completed with changes while the worker was busy, so the renames it reports are known
 * before the listing reaches the gate; anything else is left for the main loop */
static void take_completed_changes(struct pl_provider *p)
{
	if (!p->armed || !HasOverlappedIoCompleted(&p->overlapped))
		return;
	DWORD bytes = 0;
	if (!GetOverlappedResult(p->dir, &p->overlapped, &bytes, FALSE) || bytes == 0)
		return;
	ResetEvent(p->overlapped_event);
	p->armed = false;
	collect_notified_renames(p, bytes);
	/* the watch machine hears of a failed re-arm from the main loop */
	p->arm_failed = !arm_dir(p);
	if (!p->debounce_ms)
		p->debounce_ms = mono_now() + DEBOUNCE_MS;
}

static enum scan_result rescan(struct pl_provider *p)
{
	size_t cloud = 0;
	DWORD error = ERROR_SUCCESS;
	enum scan_result result = scan_folder(p, &p->files.da, &cloud, &error);
	if (result != SCAN_OK) {
		if (result != SCAN_STOPPED)
			p->last_error = error;
		return result;
	}
	p->cloud_count = cloud;
	p->listed = true;
	p->last_ok_unix_ms = utc_now_ms();
	/* a rename the listing already shows moves the admitted file before the gate sees the new name as new */
	take_completed_changes(p);
	apply_notified_renames(p);
	/* the first listing admits what is old and not held open at once (R-DIR-4) */
	feed_gate(p, !p->gate_fed);
	p->gate_fed = true;
	return SCAN_OK;
}

static void log_mode_change(struct pl_provider *p, const struct pl_watch_actions *actions)
{
	if (actions->mode == PL_WATCH_MISSING || actions->mode == PL_WATCH_RESCAN_ONLY)
		obs_log(LOG_WARNING, "folder %s: %s (%s; Win32 error %lu)", p->location,
			pl_watch_mode_name(actions->mode), actions->reason, (unsigned long)p->last_error);
	else
		obs_log(LOG_INFO, "folder %s: %s (%s)", p->location, pl_watch_mode_name(actions->mode),
			actions->reason);
}

/* Feeds an observation to the watch machine and does what it says, in its order (close, open or re-arm, rescan);
 * each outcome is the next observation */
static void run_event(struct pl_provider *p, enum pl_watch_event first)
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
			close_dir(p);
		if (actions.open && tail < MAX_CHAINED_EVENTS)
			events[tail++] = open_dir(p);
		else if (actions.rearm && tail < MAX_CHAINED_EVENTS)
			/* a handle that cannot be armed may be dead (the folder deleted and made again): recovery opens it
			 * again rather than re-arming it (R-THR-3) */
			events[tail++] = arm_dir(p) ? PL_WATCH_OPENED_ARMED : open_dir(p);
		if (actions.rescan) {
			p->debounce_ms = 0;
			enum scan_result result = rescan(p);
			if (result == SCAN_LOST && tail < MAX_CHAINED_EVENTS)
				events[tail++] = PL_WATCH_LOST;
			else if (result == SCAN_DENIED && tail < MAX_CHAINED_EVENTS)
				events[tail++] = PL_WATCH_DENIED;
		}
	}
}

/* The armed call completed */
static void on_notification(struct pl_provider *p)
{
	DWORD bytes = 0;
	BOOL ok = GetOverlappedResult(p->dir, &p->overlapped, &bytes, FALSE);
	DWORD error = ok ? ERROR_SUCCESS : GetLastError();
	if (!ok && error == ERROR_IO_INCOMPLETE)
		return;
	ResetEvent(p->overlapped_event);
	p->armed = false;
	if (!ok) {
		if (error == ERROR_OPERATION_ABORTED) {
			/* not cancelled by us (close_dir waits for its own cancel): keep watching */
			if (!arm_dir(p))
				run_event(p, PL_WATCH_OPENED_ARM_FAILED);
			return;
		}
		p->last_error = error;
		if (error == ERROR_NOTIFY_ENUM_DIR) {
			run_event(p, PL_WATCH_OVERFLOW);
		} else if (error == ERROR_ACCESS_DENIED) {
			/* a deleted folder ends the call this way while our handle keeps it pending deletion: with the handle
			 * closed, a folder that is gone is lost, not a permissions problem */
			close_dir(p);
			DWORD attributes = p->root_w ? GetFileAttributesW(p->root_w) : INVALID_FILE_ATTRIBUTES;
			DWORD lookup_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
			bool gone = lookup_error == ERROR_FILE_NOT_FOUND || lookup_error == ERROR_PATH_NOT_FOUND;
			run_event(p, gone ? PL_WATCH_LOST : PL_WATCH_DENIED);
		} else {
			run_event(p, PL_WATCH_LOST);
		}
		return;
	}
	if (bytes == 0) {
		/* more changes than the buffer holds */
		run_event(p, PL_WATCH_OVERFLOW);
		return;
	}
	collect_notified_renames(p, bytes);
	/* armed again before the debounced rescan, so a change made meanwhile is caught */
	if (!arm_dir(p))
		run_event(p, PL_WATCH_OPENED_ARM_FAILED);
	if (!p->debounce_ms)
		p->debounce_ms = mono_now() + DEBOUNCE_MS;
}

/* ---- snapshots ---- */

static void free_entry_array(struct pl_entry **entries, size_t *count)
{
	pl_entries_free(*entries, *count);
	*entries = NULL;
	*count = 0;
}

static struct pl_entry *copy_entries(const struct pl_entry *entries, size_t count, bool keep_renames)
{
	struct pl_entry *copy = bzalloc((count ? count : 1) * sizeof(*copy));
	for (size_t i = 0; i < count; i++) {
		pl_entry_copy(&copy[i], &entries[i]);
		if (!keep_renames) {
			bfree(copy[i].previous_key);
			copy[i].previous_key = NULL;
		}
	}
	return copy;
}

/* The admitted files the filter lets through, in listing order. A file rewritten in place keeps its last admitted
 * version (size, date and so its place) while the new one is gated (PLAN step 5); a file gated for the first time is
 * not listed yet. The keys still in the gate go to pending. */
static struct pl_entry *collect_entries(struct pl_provider *p, size_t *count, struct darray *pending_out)
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

/* An entry as text: its identity (R-KEY-1) and its version */
struct identity {
	char *id;
	char *version;
};

static int compare_identities(const void *a, const void *b)
{
	return strcmp(((const struct identity *)a)->id, ((const struct identity *)b)->id);
}

/* Sorted by identity, so the order a listing happens to give (FAT, exFAT, shares) changes nothing */
static struct identity *identities(const struct pl_entry *entries, size_t count)
{
	struct identity *result = bmalloc((count ? count : 1) * sizeof(*result));
	for (size_t i = 0; i < count; i++) {
		struct dstr id = {0};
		struct dstr version = {0};
		dstr_printf(&id, "%s\x1f%u", entries[i].key, entries[i].occurrence);
		dstr_printf(&version, "%llu\x1f%lld\x1f%s", (unsigned long long)entries[i].size,
			    (long long)entries[i].date_unix_ms, entries[i].location);
		result[i].id = id.array;
		result[i].version = version.array;
	}
	qsort(result, count, sizeof(*result), compare_identities);
	return result;
}

static void free_identities(struct identity *identities, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		bfree(identities[i].id);
		bfree(identities[i].version);
	}
	bfree(identities);
}

/* What makes two memberships equal (R-DIR-8, R-BEH-5): the entries and their versions, not the view */
static char *content_digest(const struct pl_entry *entries, size_t count)
{
	struct identity *sorted = identities(entries, count);
	/* never NULL, as dstr_copy of "" leaves it, so an empty membership compares like any other */
	struct dstr digest = {0};
	dstr_copy(&digest, "membership\n");
	for (size_t i = 0; i < count; i++)
		dstr_catf(&digest, "%s\t%s\n", sorted[i].id, sorted[i].version);
	free_identities(sorted, count);
	return digest.array;
}

/* Entries one membership has and the other lacks, and entries whose version changed (a rewrite): the row status "N
 * changes waiting" (R-DIR-11) */
static size_t count_changes(const struct pl_entry *a, size_t a_count, const struct pl_entry *b, size_t b_count)
{
	struct identity *x = identities(a, a_count);
	struct identity *y = identities(b, b_count);
	size_t i = 0, j = 0, changes = 0;
	while (i < a_count || j < b_count) {
		int order = i == a_count ? 1 : j == b_count ? -1 : strcmp(x[i].id, y[j].id);
		changes += order != 0 || strcmp(x[i].version, y[j].version) != 0;
		i += order <= 0;
		j += order >= 0;
	}
	free_identities(x, a_count);
	free_identities(y, b_count);
	return changes;
}

static void fill_status(struct pl_provider *p, struct pl_snapshot *snapshot)
{
	struct dstr message = {0};
	enum pl_status_level level = PL_STATUS_OK;
	if (p->watch.mode == PL_WATCH_MISSING) {
		level = PL_STATUS_ERROR;
		switch (p->missing) {
		case MISSING_DENIED:
			dstr_copy(&message, "access denied (permissions or security software)");
			break;
		case MISSING_LOST:
			dstr_copy(&message, "the folder went away (deleted, share offline or drive removed)");
			break;
		default:
			dstr_copy(&message, "folder not found, or not a folder");
			break;
		}
		snapshot->stale = p->listed;
	}
	size_t denied = pl_gate_denied_count(&p->gate);
	if (denied) {
		level = PL_STATUS_ERROR;
		if (message.len)
			dstr_cat(&message, "; ");
		dstr_catf(&message, "access denied (permissions or security software) to %zu files", denied);
	}
	if (p->cloud_count) {
		if (level == PL_STATUS_OK)
			level = PL_STATUS_WARNING;
		if (message.len)
			dstr_cat(&message, "; ");
		dstr_catf(&message, "%zu cloud-only files not downloaded; mark the folder Always keep on this device",
			  p->cloud_count);
	}
	snapshot->level = level;
	snapshot->status_message = message.array;
	snapshot->last_ok_unix_ms = p->last_ok_unix_ms;
}

/* Builds the published form of a membership: its own copy of the entries in display order (R-DIR-6), the mode's
 * visible list (R-DIR-7), the pending keys and the row status */
static struct pl_snapshot *build_snapshot(struct pl_provider *p, const struct pl_entry *raw, size_t count,
					  char **pending, size_t pending_count)
{
	struct pl_snapshot *snapshot = bzalloc(sizeof(*snapshot));
	snapshot->instance_id = p->instance_id;
	snapshot->entries = copy_entries(raw, count, true);
	snapshot->count = count;
	pl_sort_entries(snapshot->entries, count, p->options.sort_key, p->options.sort_descending);
	snapshot->visible = bmalloc((count ? count : 1) * sizeof(size_t));
	snapshot->visible_count =
		pl_mode_select(snapshot->entries, count, p->options.mode, p->options.newest_count, snapshot->visible);
	snapshot->initial_scan_complete = p->listed;
	snapshot->pending_keys = bmalloc((pending_count ? pending_count : 1) * sizeof(char *));
	for (size_t i = 0; i < pending_count; i++)
		snapshot->pending_keys[i] = bstrdup(pending[i]);
	snapshot->pending_count = pending_count;
	fill_status(p, snapshot);
	snapshot->held_changes = (size_t)os_atomic_load_long(&p->held_changes);
	return snapshot;
}

/* Everything the receiver can see, to publish only what changed */
static char *snapshot_signature(const struct pl_snapshot *snapshot)
{
	struct dstr signature = {0};
	dstr_copy(&signature, "snapshot\n");
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct pl_entry *entry = &snapshot->entries[i];
		dstr_catf(&signature, "%s\t%s\t%u\t%llu\t%lld\t%s\n", entry->location, entry->key, entry->occurrence,
			  (unsigned long long)entry->size, (long long)entry->date_unix_ms,
			  entry->previous_key ? entry->previous_key : "");
	}
	dstr_cat(&signature, "visible");
	for (size_t i = 0; i < snapshot->visible_count; i++)
		dstr_catf(&signature, " %zu", snapshot->visible[i]);
	dstr_catf(&signature, "\npending %zu\nheld %zu\nlevel %d %s\nstale %d initial %d\n", snapshot->pending_count,
		  snapshot->held_changes, (int)snapshot->level,
		  snapshot->status_message ? snapshot->status_message : "", snapshot->stale,
		  snapshot->initial_scan_complete);
	return signature.array;
}

/* Publishes this membership if anything the receiver sees changed (or when forced); it becomes the published one.
 * Takes raw and content. */
static void publish_membership(struct pl_provider *p, struct pl_entry *raw, size_t count, char *content, char **pending,
			       size_t pending_count, bool force)
{
	struct pl_snapshot *snapshot = build_snapshot(p, raw, count, pending, pending_count);
	char *signature = snapshot_signature(snapshot);
	if (!force && p->last_signature && strcmp(signature, p->last_signature) == 0) {
		bfree(signature);
		pl_snapshot_free(snapshot);
		pl_entries_free(raw, count);
		bfree(content);
		return;
	}
	if (!p->published_content || strcmp(content, p->published_content) != 0)
		p->content_revision++;
	snapshot->content_revision = p->content_revision;
	snapshot->generation = ++p->generation;
	bfree(p->last_signature);
	p->last_signature = signature;

	/* renames are published once; the kept membership has none */
	free_entry_array(&p->published, &p->published_count);
	p->published = copy_entries(raw, count, false);
	p->published_count = count;
	pl_entries_free(raw, count);
	bfree(p->published_content);
	p->published_content = content;
	free_renames(&p->renames.da);

	p->publish(p->owner, snapshot);
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
 * the list is locked (R-DIR-11) and publish the locked membership in the current view and status. release (or the
 * lock turned off) publishes the newest membership, carrying the renames the kept one had. */
static void update_publication(struct pl_provider *p, bool release)
{
	/* nothing to say before the first listing, unless the folder is missing */
	if (!p->listed && p->watch.mode != PL_WATCH_MISSING)
		return;
	size_t count = 0;
	DARRAY(char *) pending;
	struct pl_entry *raw = collect_entries(p, &count, &pending.da);
	char *content = content_digest(raw, count);

	/* the list taken at start is the first listing once the files it found being written are in; the lock holds
	 * changes from then on (not an error published before any listing, nor a start with files still in the gate) */
	bool locked = p->options.lock_list_at_start && p->lock_ready && strcmp(content, p->published_content) != 0;
	if (locked && !release) {
		/* renames the kept membership carried and this one does not compose (A to B kept, then B to C) */
		if (p->held)
			pl_merge_compose_renames(raw, count, p->held, p->held_count);
		free_entry_array(&p->held, &p->held_count);
		bfree(p->held_content);
		p->held = raw;
		p->held_count = count;
		p->held_content = content;
		free_renames(&p->renames.da);
		os_atomic_set_long(&p->held_changes,
				   (long)count_changes(p->published, p->published_count, p->held, p->held_count));
		publish_membership(p, copy_entries(p->published, p->published_count, false), p->published_count,
				   bstrdup(p->published_content), pending.array, pending.num, false);
	} else {
		/* a release publishes even an unchanged view; with nothing kept there is nothing to force */
		bool released = release && p->held;
		if (p->held)
			pl_merge_compose_renames(raw, count, p->held, p->held_count);
		free_entry_array(&p->held, &p->held_count);
		bfree(p->held_content);
		p->held_content = NULL;
		os_atomic_set_long(&p->held_changes, 0);
		publish_membership(p, raw, count, content, pending.array, pending.num, released);
	}
	if (p->listed && !pending.num && p->published_content)
		p->lock_ready = true;
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

static void apply_options(struct pl_provider *p, const struct pl_container_options *options)
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

static void take_requests(struct pl_provider *p, bool *release)
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

static DWORD WINAPI worker_main(void *data)
{
	struct pl_provider *p = data;
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
			pl_gate_tick(&p->gate, mono_now(), utc_now_ms(), sample_file, probe_file, p);
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
		DWORD timeout = wait_ms <= 0 ? 0 : wait_ms > 3600000 ? 3600000 : (DWORD)wait_ms;
		HANDLE handles[3] = {p->stop_event, p->wake_event, p->overlapped_event};
		DWORD signaled = WaitForMultipleObjects(p->armed ? 3 : 2, handles, FALSE, timeout);
		if (signaled == WAIT_OBJECT_0)
			break;
		if (signaled == WAIT_OBJECT_0 + 2)
			on_notification(p);
	}
	close_dir(p);
	return 0;
}

/* ---- the interface ---- */

static void free_provider(struct pl_provider *p)
{
	close_dir(p);
	if (p->thread)
		CloseHandle(p->thread);
	if (p->stop_event)
		CloseHandle(p->stop_event);
	if (p->wake_event)
		CloseHandle(p->wake_event);
	if (p->overlapped_event)
		CloseHandle(p->overlapped_event);
	pthread_mutex_destroy(&p->request_mutex);
	bfree(p->requested_filter);
	bfree(p->filter_text);
	pl_filter_free(&p->filter);
	pl_gate_free(&p->gate);
	bfree(p->buffer);
	free_renames(&p->notified_renames.da);
	bfree(p->renamed_from);
	free_renames(&p->renames.da);
	free_listing(&p->files.da);
	for (size_t i = 0; i < p->logged_paths.num; i++)
		bfree(p->logged_paths.array[i]);
	da_free(p->logged_paths);
	bfree(p->last_signature);
	free_entry_array(&p->published, &p->published_count);
	bfree(p->published_content);
	free_entry_array(&p->held, &p->held_count);
	bfree(p->held_content);
	bfree(p->root_w);
	bfree(p->location);
	bfree(p);
}

struct pl_provider *pl_provider_create(enum pl_row_kind kind, const char *location, const char *row_uuid,
				       uint64_t instance_id, const struct pl_container_options *options,
				       pl_publish_fn publish, void *owner)
{
	UNUSED_PARAMETER(row_uuid);
	if (kind != PL_ROW_FOLDER || !location || !options || !publish)
		return NULL;
	struct pl_provider *p = bzalloc(sizeof(*p));
	p->location = bstrdup(location);
	/* entries are "<location>/<relative path>", so the row's own trailing separators go (a drive root keeps
	 * nothing after its colon) */
	size_t length = strlen(p->location);
	while (length > 1 && (p->location[length - 1] == '/' || p->location[length - 1] == '\\'))
		p->location[--length] = '\0';
	p->root_w = win32_path(location);
	p->instance_id = instance_id;
	p->publish = publish;
	p->owner = owner;
	p->include_subfolders = options->include_subfolders;
	p->dir = INVALID_HANDLE_VALUE;
	p->buffer = bmalloc(NOTIFY_BUFFER_BYTES);
	pthread_mutex_init_value(&p->request_mutex);
	copy_options(&p->requested, &p->requested_filter, options);
	copy_options(&p->options, &p->filter_text, options);
	pl_filter_parse(&p->filter, p->filter_text);
	pl_gate_init(&p->gate);
	da_init(p->notified_renames);
	da_init(p->renames);
	da_init(p->files);
	da_init(p->logged_paths);

	p->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	p->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	p->overlapped_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (pthread_mutex_init(&p->request_mutex, NULL) != 0 || !p->stop_event || !p->wake_event ||
	    !p->overlapped_event) {
		free_provider(p);
		return NULL;
	}
	p->thread = CreateThread(NULL, WORKER_STACK_SIZE, worker_main, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (!p->thread) {
		obs_log(LOG_ERROR, "folder %s: the worker thread could not be started (Win32 error %lu)", p->location,
			(unsigned long)GetLastError());
		free_provider(p);
		return NULL;
	}
	return p;
}

void pl_provider_refresh_now(struct pl_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->refresh_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

void pl_provider_set_options(struct pl_provider *provider, const struct pl_container_options *options)
{
	pthread_mutex_lock(&provider->request_mutex);
	copy_options(&provider->requested, &provider->requested_filter, options);
	provider->options_changed = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

void pl_provider_release_held(struct pl_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->release_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

size_t pl_provider_held_changes(struct pl_provider *provider)
{
	return (size_t)os_atomic_load_long(&provider->held_changes);
}

void pl_provider_destroy(struct pl_provider *provider)
{
	if (!provider)
		return;
	SetEvent(provider->stop_event);
	/* a listing or probe stuck on a stalled share is interrupted; again and again, as the worker may not have
	 * entered the call yet when the first cancel comes (R-THR-3) */
	while (WaitForSingleObject(provider->thread, 100) == WAIT_TIMEOUT)
		CancelSynchronousIo(provider->thread);
	free_provider(provider);
}
