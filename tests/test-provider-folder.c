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

/* The folder provider on a real folder in the temp directory: the initial listing and its filters, new files
 * through the write-complete gate, renames that keep their identity, deletions, a file rewritten in place keeping
 * its place, view and filter changes without a new listing, lock list at start, subfolders (a hidden one and a
 * junction back up the tree skipped), and a missing folder that comes back. Each wait has a generous limit, so a
 * slow machine only makes it slower. */

#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <windows.h>

#include "core/filter.h"
#include "providers/provider.h"

#define WAIT_MS 15000

/* ---- what the provider published ---- */

struct inbox {
	pthread_mutex_t mutex;
	HANDLE event;
	DARRAY(struct pl_snapshot *) snapshots;
	uint64_t last_generation;
	bool in_order;
};

static void on_publish(void *owner, struct pl_snapshot *snapshot)
{
	struct inbox *inbox = owner;
	pthread_mutex_lock(&inbox->mutex);
	if (snapshot->generation <= inbox->last_generation)
		inbox->in_order = false;
	inbox->last_generation = snapshot->generation;
	da_push_back(inbox->snapshots, &snapshot);
	pthread_mutex_unlock(&inbox->mutex);
	SetEvent(inbox->event);
}

static void inbox_init(struct inbox *inbox)
{
	memset(inbox, 0, sizeof(*inbox));
	pthread_mutex_init(&inbox->mutex, NULL);
	inbox->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	inbox->in_order = true;
}

static void inbox_free(struct inbox *inbox)
{
	assert(inbox->in_order);
	for (size_t i = 0; i < inbox->snapshots.num; i++)
		pl_snapshot_free(inbox->snapshots.array[i]);
	da_free(inbox->snapshots);
	CloseHandle(inbox->event);
	pthread_mutex_destroy(&inbox->mutex);
}

static size_t inbox_count(struct inbox *inbox)
{
	pthread_mutex_lock(&inbox->mutex);
	size_t count = inbox->snapshots.num;
	pthread_mutex_unlock(&inbox->mutex);
	return count;
}

typedef bool (*snapshot_test)(const struct pl_snapshot *snapshot, const void *context);

/* The first snapshot published from position since on that passes test, or NULL after the limit. The snapshot stays
 * owned by the inbox. */
static const struct pl_snapshot *wait_for(struct inbox *inbox, size_t since, snapshot_test test, const void *context,
					  const char *what)
{
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	for (;;) {
		pthread_mutex_lock(&inbox->mutex);
		for (size_t i = since; i < inbox->snapshots.num; i++) {
			if (test(inbox->snapshots.array[i], context)) {
				const struct pl_snapshot *found = inbox->snapshots.array[i];
				pthread_mutex_unlock(&inbox->mutex);
				return found;
			}
		}
		pthread_mutex_unlock(&inbox->mutex);
		if (os_gettime_ns() > limit) {
			fprintf(stderr, "timed out waiting for: %s\n", what);
			return NULL;
		}
		WaitForSingleObject(inbox->event, 100);
	}
}

static const struct pl_entry *entry(const struct pl_snapshot *snapshot, const char *key)
{
	for (size_t i = 0; i < snapshot->count; i++) {
		if (strcmp(snapshot->entries[i].key, key) == 0)
			return &snapshot->entries[i];
	}
	return NULL;
}

static bool pending(const struct pl_snapshot *snapshot, const char *key)
{
	for (size_t i = 0; i < snapshot->pending_count; i++) {
		if (strcmp(snapshot->pending_keys[i], key) == 0)
			return true;
	}
	return false;
}

/* The visible keys in order, joined by commas */
static char *visible_keys(const struct pl_snapshot *snapshot)
{
	struct dstr keys = {0};
	for (size_t i = 0; i < snapshot->visible_count; i++) {
		if (i)
			dstr_cat_ch(&keys, ',');
		dstr_cat(&keys, snapshot->entries[snapshot->visible[i]].key);
	}
	/* an empty list is "", not NULL */
	return keys.array ? keys.array : bstrdup("");
}

static bool shows_exactly(const struct pl_snapshot *snapshot, const void *context)
{
	char *keys = visible_keys(snapshot);
	bool same = snapshot->initial_scan_complete && strcmp(keys, context) == 0;
	bfree(keys);
	return same;
}

static bool lists(const struct pl_snapshot *snapshot, const void *context)
{
	return entry(snapshot, context) != NULL;
}

static bool lacks(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->initial_scan_complete && entry(snapshot, context) == NULL;
}

static bool waits_for(const struct pl_snapshot *snapshot, const void *context)
{
	return pending(snapshot, context);
}

struct expected_version {
	const char *key;
	uint64_t size;
	bool pending; /* the key is also in the gate */
};

static bool at_version(const struct pl_snapshot *snapshot, const void *context)
{
	const struct expected_version *version = context;
	const struct pl_entry *found = entry(snapshot, version->key);
	return found && found->size == version->size && pending(snapshot, version->key) == version->pending;
}

/* ---- the folder ---- */

static wchar_t *wide(const char *text)
{
	wchar_t *result = NULL;
	os_utf8_to_wcs_ptr(text, 0, &result);
	return result;
}

static char *path_in(const char *folder, const char *name)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s\\%s", folder, name);
	return path.array;
}

static void remember_folder(const char *folder);

static char *make_folder(const char *name)
{
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	char *temp_utf8 = NULL;
	os_wcs_to_utf8_ptr(temp, 0, &temp_utf8);
	struct dstr folder = {0};
	dstr_printf(&folder, "%splaylister-provider-%s-%lu-%llu", temp_utf8, name, GetCurrentProcessId(),
		    (unsigned long long)os_gettime_ns());
	bfree(temp_utf8);
	wchar_t *folder_w = wide(folder.array);
	assert(CreateDirectoryW(folder_w, NULL));
	bfree(folder_w);
	remember_folder(folder.array);
	return folder.array;
}

static void set_age(HANDLE file, int64_t age_ms)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER value = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
	value.QuadPart -= (ULONGLONG)age_ms * 10000;
	FILETIME then = {value.LowPart, value.HighPart};
	assert(SetFileTime(file, NULL, NULL, &then));
}

/* Writes size bytes; an age above 0 back-dates the last write, so the first listing admits the file at once. A
 * scanner (the provider's own probe, an antivirus or the indexer) may hold the file for a moment, so a busy file is
 * tried again for up to two seconds. */
static void write_file(const char *folder, const char *name, DWORD size, int64_t age_ms)
{
	char *path = path_in(folder, name);
	wchar_t *path_w = wide(path);
	HANDLE file = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 40 && file == INVALID_HANDLE_VALUE; attempt++) {
		file = CreateFileW(path_w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			Sleep(50);
	}
	assert(file != INVALID_HANDLE_VALUE);
	char *bytes = bzalloc(size ? size : 1);
	DWORD written = 0;
	assert(WriteFile(file, bytes, size, &written, NULL) && written == size);
	if (age_ms > 0)
		set_age(file, age_ms);
	CloseHandle(file);
	bfree(bytes);
	bfree(path_w);
	bfree(path);
}

static void make_subfolder(const char *folder, const char *name, DWORD attributes)
{
	char *path = path_in(folder, name);
	wchar_t *path_w = wide(path);
	assert(CreateDirectoryW(path_w, NULL));
	if (attributes)
		assert(SetFileAttributesW(path_w, attributes));
	bfree(path_w);
	bfree(path);
}

/* Removes the test folder and everything in it; a junction is removed as a link, never followed */
static void remove_tree(const char *folder)
{
	wchar_t *folder_w = wide(folder);
	wchar_t *pattern = bmalloc((wcslen(folder_w) + 3) * sizeof(wchar_t));
	wcscpy_s(pattern, wcslen(folder_w) + 3, folder_w);
	wcscat_s(pattern, wcslen(folder_w) + 3, L"\\*");
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(pattern, &data);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
				continue;
			char *name = NULL;
			os_wcs_to_utf8_ptr(data.cFileName, 0, &name);
			char *child = path_in(folder, name);
			wchar_t *child_w = wide(child);
			SetFileAttributesW(child_w, FILE_ATTRIBUTE_NORMAL);
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			    !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				remove_tree(child);
			else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				RemoveDirectoryW(child_w);
			else
				DeleteFileW(child_w);
			bfree(child_w);
			bfree(child);
			bfree(name);
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}
	RemoveDirectoryW(folder_w);
	bfree(pattern);
	bfree(folder_w);
}

/* The test folders, removed even when an assert fails (assert aborts, which skips atexit); plain malloc, so the bmem
 * leak check does not count them */
#define MAX_FOLDERS 8
static char *test_folders[MAX_FOLDERS];

static void remember_folder(const char *folder)
{
	for (int i = 0; i < MAX_FOLDERS; i++) {
		if (!test_folders[i]) {
			test_folders[i] = _strdup(folder);
			return;
		}
	}
}

static void remove_folder(const char *folder)
{
	remove_tree(folder);
	for (int i = 0; i < MAX_FOLDERS; i++) {
		if (test_folders[i] && strcmp(test_folders[i], folder) == 0) {
			free(test_folders[i]);
			test_folders[i] = NULL;
		}
	}
}

static void remove_folders_on_abort(int signal_number)
{
	UNUSED_PARAMETER(signal_number);
	for (int i = 0; i < MAX_FOLDERS; i++) {
		if (test_folders[i])
			remove_tree(test_folders[i]);
	}
}

static struct pl_container_options default_options(void)
{
	struct pl_container_options options = {0};
	options.extension_filter = PL_FILTER_DEFAULT;
	options.sort_key = PL_SORT_NAME;
	options.mode = PL_MODE_FULL;
	options.newest_count = 1;
	options.folder_check_enabled = true;
	options.folder_check_interval_s = 2;
	return options;
}

#define OLD (60 * 60 * 1000) /* an hour ago */

/* ---- tests ---- */

static void test_listing_and_changes(void)
{
	char *folder = make_folder("changes");
	write_file(folder, "a.mp4", 100, OLD);
	write_file(folder, "b.MKV", 200, OLD);
	write_file(folder, "notes.txt", 10, OLD);   /* not in the filter */
	write_file(folder, "desktop.ini", 10, OLD); /* always skipped */
	write_file(folder, "c.mp4.part", 10, OLD);  /* always skipped: still downloading */
	write_file(folder, "hidden.mp4", 10, OLD);
	char *hidden = path_in(folder, "hidden.mp4");
	wchar_t *hidden_w = wide(hidden);
	assert(SetFileAttributesW(hidden_w, FILE_ATTRIBUTE_HIDDEN));
	bfree(hidden_w);
	bfree(hidden);

	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_container_options options = default_options();
	struct pl_provider *provider =
		pl_provider_create(PL_ROW_FOLDER, folder, "row", 42, &options, on_publish, &inbox);
	assert(provider);

	/* the first listing: old files admitted at once, the rest left out */
	const struct pl_snapshot *first = wait_for(&inbox, 0, shows_exactly, "A.MP4,B.MKV", "the initial listing");
	assert(first && first->instance_id == 42 && first->pending_count == 0 && first->level == PL_STATUS_OK);
	const struct pl_entry *a = entry(first, "A.MP4");
	struct dstr expected = {0};
	dstr_printf(&expected, "%s/a.mp4", folder);
	assert(strcmp(a->location, expected.array) == 0 && a->size == 100 && a->has_date && a->occurrence == 1);
	dstr_free(&expected);
	assert(!entry(first, "NOTES.TXT") && !entry(first, "HIDDEN.MP4") && !entry(first, "DESKTOP.INI"));

	/* a new file waits in the gate, then joins */
	size_t mark = inbox_count(&inbox);
	write_file(folder, "d.mp4", 300, 0);
	assert(wait_for(&inbox, mark, waits_for, "D.MP4", "the new file pending"));
	const struct pl_snapshot *joined = wait_for(&inbox, mark, lists, "D.MP4", "the new file admitted");
	assert(joined && joined->pending_count == 0);

	/* a rename keeps the item: published under the new key with the old one, without a new wait */
	mark = inbox_count(&inbox);
	char *from = path_in(folder, "a.mp4");
	char *to = path_in(folder, "e.mp4");
	wchar_t *from_w = wide(from);
	wchar_t *to_w = wide(to);
	assert(MoveFileW(from_w, to_w));
	const struct pl_snapshot *renamed = wait_for(&inbox, mark, lists, "E.MP4", "the renamed file");
	assert(renamed && !entry(renamed, "A.MP4") && entry(renamed, "E.MP4")->previous_key &&
	       strcmp(entry(renamed, "E.MP4")->previous_key, "A.MP4") == 0);
	bfree(from_w);
	bfree(to_w);
	bfree(from);
	bfree(to);

	/* a deleted file leaves */
	mark = inbox_count(&inbox);
	char *b = path_in(folder, "b.MKV");
	wchar_t *b_w = wide(b);
	assert(DeleteFileW(b_w));
	assert(wait_for(&inbox, mark, lacks, "B.MKV", "the deleted file gone"));
	bfree(b_w);
	bfree(b);

	/* a file rewritten in place keeps its admitted version, and so its place, while the new one is gated (for at
	 * least the stability window), then takes the new one */
	mark = inbox_count(&inbox);
	write_file(folder, "d.mp4", 350, 0);
	struct expected_version old_version = {"D.MP4", 300, true};
	const struct pl_snapshot *rewriting = wait_for(&inbox, mark, at_version, &old_version, "the old version kept");
	assert(rewriting);
	struct expected_version new_version = {"D.MP4", 350, false};
	assert(wait_for(&inbox, mark, at_version, &new_version, "the new version"));

	/* a view change is published without a new listing and without a content change */
	pthread_mutex_lock(&inbox.mutex);
	uint64_t revision = inbox.snapshots.array[inbox.snapshots.num - 1]->content_revision;
	pthread_mutex_unlock(&inbox.mutex);
	mark = inbox_count(&inbox);
	options.sort_descending = true;
	pl_provider_set_options(provider, &options);
	const struct pl_snapshot *reversed = wait_for(&inbox, mark, shows_exactly, "E.MP4,D.MP4", "descending order");
	assert(reversed && reversed->content_revision == revision);

	/* a filter change admits old files from the last listing at once */
	mark = inbox_count(&inbox);
	options.extension_filter = "txt";
	pl_provider_set_options(provider, &options);
	assert(wait_for(&inbox, mark, shows_exactly, "NOTES.TXT", "the text filter"));
	mark = inbox_count(&inbox);
	options.extension_filter = PL_FILTER_DEFAULT;
	pl_provider_set_options(provider, &options);
	assert(wait_for(&inbox, mark, shows_exactly, "E.MP4,D.MP4", "the default filter again"));

	/* lock list at start: a change is kept, counted, and published only when released (R-DIR-11) */
	options.lock_list_at_start = true;
	pl_provider_set_options(provider, &options);
	Sleep(300);
	mark = inbox_count(&inbox);
	write_file(folder, "f.mp4", 10, 0);
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (pl_provider_held_changes(provider) != 1 && os_gettime_ns() < limit)
		Sleep(50);
	assert(pl_provider_held_changes(provider) == 1);
	pthread_mutex_lock(&inbox.mutex);
	for (size_t i = mark; i < inbox.snapshots.num; i++)
		assert(!entry(inbox.snapshots.array[i], "F.MP4"));
	pthread_mutex_unlock(&inbox.mutex);
	pl_provider_release_held(provider);
	assert(wait_for(&inbox, mark, lists, "F.MP4", "the released change"));
	assert(pl_provider_held_changes(provider) == 0);

	/* every file deleted while locked: an empty membership is kept and counted like any other */
	mark = inbox_count(&inbox);
	const char *names[] = {"d.mp4", "e.mp4", "f.mp4"};
	for (size_t i = 0; i < 3; i++) {
		char *path = path_in(folder, names[i]);
		wchar_t *path_w = wide(path);
		assert(DeleteFileW(path_w));
		bfree(path_w);
		bfree(path);
	}
	limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (pl_provider_held_changes(provider) != 3 && os_gettime_ns() < limit)
		Sleep(50);
	assert(pl_provider_held_changes(provider) == 3);
	pl_provider_release_held(provider);
	assert(wait_for(&inbox, mark, shows_exactly, "", "the emptied folder released"));

	/* nothing is published once destroyed */
	pl_provider_destroy(provider);
	size_t final_count = inbox_count(&inbox);
	Sleep(200);
	assert(inbox_count(&inbox) == final_count);
	inbox_free(&inbox);
	remove_folder(folder);
	bfree(folder);
}

static void test_subfolders(void)
{
	char *folder = make_folder("tree");
	write_file(folder, "top.mp4", 10, OLD);
	make_subfolder(folder, "sub", 0);
	write_file(folder, "sub\\inner.mp4", 10, OLD);
	make_subfolder(folder, "secret", FILE_ATTRIBUTE_HIDDEN);
	write_file(folder, "secret\\skipped.mp4", 10, OLD);
	/* a junction back to the folder itself: following it would never end */
	struct dstr command = {0};
	dstr_printf(&command, "cmd /c mklink /J \"%s\\loop\" \"%s\" >nul", folder, folder);
	wchar_t *command_w = wide(command.array);
	bool junction = _wsystem(command_w) == 0;
	bfree(command_w);
	dstr_free(&command);
	if (!junction)
		printf("note: no junction could be made here; the loop case is not covered\n");

	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_container_options options = default_options();
	options.include_subfolders = true;
	struct pl_provider *provider =
		pl_provider_create(PL_ROW_FOLDER, folder, "tree", 7, &options, on_publish, &inbox);
	assert(provider);
	const struct pl_snapshot *tree = wait_for(&inbox, 0, shows_exactly, "SUB/INNER.MP4,TOP.MP4", "the tree");
	assert(tree);
	struct dstr expected = {0};
	dstr_printf(&expected, "%s/sub/inner.mp4", folder);
	assert(strcmp(entry(tree, "SUB/INNER.MP4")->location, expected.array) == 0);
	dstr_free(&expected);
	/* list order: the folder's own files, then its subfolders */
	assert(entry(tree, "TOP.MP4")->source_ordinal < entry(tree, "SUB/INNER.MP4")->source_ordinal);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* without subfolders only the top level */
	inbox_init(&inbox);
	options.include_subfolders = false;
	provider = pl_provider_create(PL_ROW_FOLDER, folder, "tree", 8, &options, on_publish, &inbox);
	assert(wait_for(&inbox, 0, shows_exactly, "TOP.MP4", "the top level only"));
	pl_provider_destroy(provider);
	inbox_free(&inbox);
	remove_folder(folder);
	bfree(folder);
}

static bool missing_error(const struct pl_snapshot *snapshot, const void *context)
{
	UNUSED_PARAMETER(context);
	return snapshot->level == PL_STATUS_ERROR && !snapshot->initial_scan_complete && snapshot->count == 0 &&
	       snapshot->status_message && strstr(snapshot->status_message, "not found");
}

static bool kept_stale(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->level == PL_STATUS_ERROR && snapshot->stale && entry(snapshot, context) &&
	       snapshot->last_ok_unix_ms > 0;
}

static bool recovered(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->level == PL_STATUS_OK && !snapshot->stale && entry(snapshot, context);
}

static void test_missing_folder(void)
{
	char *parent = make_folder("missing");
	char *folder = path_in(parent, "watched");
	char *away = path_in(parent, "moved-away");

	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_container_options options = default_options();
	/* with the lock on: the list taken at start is the first real listing, not the error before it (R-DIR-11) */
	options.lock_list_at_start = true;
	struct pl_provider *provider =
		pl_provider_create(PL_ROW_FOLDER, folder, "gone", 9, &options, on_publish, &inbox);
	assert(provider);
	assert(wait_for(&inbox, 0, missing_error, NULL, "the missing folder's error"));

	/* it appears: opened again at the next check */
	size_t mark = inbox_count(&inbox);
	make_subfolder(parent, "watched", 0);
	write_file(folder, "clip.mp4", 10, OLD);
	assert(wait_for(&inbox, mark, recovered, "CLIP.MP4", "the folder found"));

	/* it goes away: the last list is kept, marked stale, with the last good time (R-BEH-3) */
	mark = inbox_count(&inbox);
	wchar_t *folder_w = wide(folder);
	wchar_t *away_w = wide(away);
	assert(MoveFileW(folder_w, away_w));
	assert(wait_for(&inbox, mark, kept_stale, "CLIP.MP4", "the stale list"));

	/* and comes back */
	mark = inbox_count(&inbox);
	assert(MoveFileW(away_w, folder_w));
	assert(wait_for(&inbox, mark, recovered, "CLIP.MP4", "the folder back"));
	bfree(folder_w);
	bfree(away_w);

	pl_provider_destroy(provider);
	inbox_free(&inbox);
	remove_folder(parent);
	bfree(away);
	bfree(folder);
	bfree(parent);
}

static void test_create_arguments(void)
{
	struct pl_container_options options = default_options();
	struct inbox inbox;
	inbox_init(&inbox);
	/* only folders have a provider so far */
	assert(!pl_provider_create(PL_ROW_FILE, "C:/x", "row", 1, &options, on_publish, &inbox));
	assert(!pl_provider_create(PL_ROW_FOLDER, NULL, "row", 1, &options, on_publish, &inbox));
	pl_provider_destroy(NULL);
	inbox_free(&inbox);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	signal(SIGABRT, remove_folders_on_abort);

	long allocations = bnum_allocs();
	test_create_arguments();
	printf("create arguments: passed\n");
	test_listing_and_changes();
	printf("listing and changes: passed\n");
	test_subfolders();
	printf("subfolders: passed\n");
	test_missing_folder();
	printf("missing folder: passed\n");
	assert(bnum_allocs() == allocations);
	printf("folder provider tests: passed\n");
	return 0;
}
