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

/* The folder provider's Windows calls (folder-os.h): listing with FindFirstFileExW, the deny-write probe, and the
 * watch with an overlapped ReadDirectoryChangesW (R-DIR-1..4). Moved unchanged from the folder provider in the Linux
 * port (PLAN step 2). */

#include "folder-os.h"

#include <windows.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "os.h"
#include "provider.h"

#define NOTIFY_BUFFER_BYTES (64 * 1024) /* the largest ReadDirectoryChangesW buffer a network share takes */
/* cloud placeholders: opening one starts a download (R-DIR-5) */
#define CLOUD_ATTRIBUTES (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)
#define FILETIME_UNIX_EPOCH 116444736000000000LL /* 1970-01-01 in 100 ns units since 1601 */

struct pl_folder_os {
	wchar_t *root_w; /* canonical, with its \\?\ prefix (R-THR-5); NULL when the path cannot be converted */
	bool include_subfolders;
	HANDLE dir;
	BY_HANDLE_FILE_INFORMATION dir_identity; /* of the folder dir is open on: volume serial and file index */
	OVERLAPPED overlapped;
	HANDLE overlapped_event;
	DWORD *buffer;
	bool armed;
	DWORD last_error;
	char *renamed_from; /* an old name whose new name has not come yet */
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
static wchar_t *below_root(const struct pl_folder_os *os, const char *relative, const wchar_t *suffix)
{
	wchar_t *relative_w = NULL;
	os_utf8_to_wcs_ptr(relative, 0, &relative_w);
	for (wchar_t *c = relative_w; *c; c++) {
		if (*c == L'/')
			*c = L'\\';
	}
	size_t root_length = wcslen(os->root_w);
	bool separator = root_length && os->root_w[root_length - 1] != L'\\' && *relative_w;
	wchar_t *joined = wide_concat(os->root_w, separator ? L"\\" : L"", relative_w);
	wchar_t *result = joined;
	if (*suffix) {
		bool sep = *relative_w || (root_length && os->root_w[root_length - 1] != L'\\');
		result = wide_concat(joined, sep ? L"\\" : L"", suffix);
		bfree(joined);
	}
	bfree(relative_w);
	return result;
}

/* A file's path for the probe and the sample: one in the \\?\ form (pl_folder_os_location, or a row typed with the
 * prefix) is not run through GetFullPathNameW, which would drop the dot or space a name ends with, but its '/' still
 * becomes '\' (R-THR-5), as "<row>/<relative>" has one; any other is canonicalized */
static wchar_t *file_path(const char *location)
{
	if (strncmp(location, "\\\\?\\", 4) != 0)
		return win32_path(location);
	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(location, 0, &wide);
	for (wchar_t *c = wide; c && *c; c++) {
		if (*c == L'/')
			*c = L'\\';
	}
	return wide;
}

/* A part of the relative path ends in a dot or a space, which Win32 path rules take away (the last part's, and a
 * single dot after any part), so the plain path names another file */
static bool trimmed_by_win32(const char *relative)
{
	for (const char *c = relative; *c; c++) {
		if ((*c == '.' || *c == ' ') && (c[1] == '/' || c[1] == '\0'))
			return true;
	}
	return false;
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

/* ---- create and destroy ---- */

struct pl_folder_os *pl_folder_os_create(const char *location, bool include_subfolders)
{
	HANDLE overlapped_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!overlapped_event)
		return NULL;
	struct pl_folder_os *os = bzalloc(sizeof(*os));
	os->root_w = win32_path(location);
	os->include_subfolders = include_subfolders;
	os->dir = INVALID_HANDLE_VALUE;
	os->overlapped_event = overlapped_event;
	os->buffer = bmalloc(NOTIFY_BUFFER_BYTES);
	return os;
}

void pl_folder_os_destroy(struct pl_folder_os *os)
{
	if (!os)
		return;
	pl_folder_os_close(os);
	if (os->overlapped_event)
		CloseHandle(os->overlapped_event);
	bfree(os->buffer);
	bfree(os->renamed_from);
	bfree(os->root_w);
	bfree(os);
}

void pl_folder_os_error_cat(const struct pl_folder_os *os, struct dstr *text)
{
	pl_os_error_cat(text, os->last_error);
}

char *pl_folder_os_location(const struct pl_folder_os *os, const char *row, const char *relative)
{
	struct dstr location = {0};
	if (os->root_w && trimmed_by_win32(relative)) {
		/* the \\?\ form keeps the name exactly, for the probe, the sample and the player alike (FFmpeg opens a
		 * \\?\ path as it is) */
		wchar_t *path = below_root(os, relative, L"");
		char *utf8 = NULL;
		os_wcs_to_utf8_ptr(path, 0, &utf8);
		bfree(path);
		if (utf8)
			return utf8;
	}
	dstr_printf(&location, "%s/%s", row, relative);
	return location.array;
}

/* ---- the gate's disk access ---- */

enum pl_probe_result pl_folder_os_probe(void *context, const char *location)
{
	UNUSED_PARAMETER(context);
	wchar_t *path = file_path(location);
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
bool pl_folder_os_sample(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			 int64_t *sampled_mono_ms)
{
	UNUSED_PARAMETER(context);
	wchar_t *path = file_path(location);
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

bool pl_folder_os_error_is_subfolders_own(unsigned long error)
{
	switch (error) {
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_DIRECTORY:
	case ERROR_FILENAME_EXCED_RANGE:
	case ERROR_CANT_ACCESS_FILE:
	case ERROR_CANT_RESOLVE_FILENAME:
	case ERROR_FILE_CORRUPT:
	case ERROR_DISK_CORRUPT:
		return true;
	default:
		return false;
	}
}

/* A subfolder that could not be opened for listing, or read to its end, with error. While the root still can be read,
 * an error that says this subfolder itself cannot be read (pl_folder_os_error_is_subfolders_own) leaves it out,
 * logged with its error, and the rest of the folder still plays; any other (a timeout, a share dropping, a handle
 * limit, a sharing violation, an I/O error) may pass, and the sink decides (folder-os.h): it fails the whole listing,
 * so the last good list stays instead of shrinking, until the subfolder keeps failing. A root that cannot be read any
 * more fails the listing at once. True when the subfolder is left out. */
static bool leave_out_subfolder(struct pl_folder_os *os, const struct pl_folder_listing_sink *sink,
				const char *relative, DWORD error)
{
	DWORD attributes = GetFileAttributesW(os->root_w);
	bool root_there = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
	if (!root_there ||
	    !(pl_folder_os_error_is_subfolders_own(error) || sink->failed(sink->context, relative, error)))
		return false;
	struct dstr why = {0};
	dstr_copy(&why, "cannot be read, ");
	pl_os_error_cat(&why, error);
	sink->skipped(sink->context, relative, why.array);
	dstr_free(&why);
	sink->left_out(sink->context, relative);
	return true;
}

/* Below the root, hidden and system folders are skipped and reparse points are never followed, so a link back up the
 * tree cannot make the scan loop (R-DIR-1); the root itself is opened even when it is a junction. */
enum pl_folder_scan pl_folder_os_list(struct pl_folder_os *os, struct pl_stop *stop,
				      const struct pl_folder_listing_sink *sink)
{
	DARRAY(char *) folders; /* relative folders still to list; "" is the root */
	da_init(folders);
	char *root = bstrdup("");
	da_push_back(folders, &root);
	enum pl_folder_scan result = os->root_w ? PL_FOLDER_SCAN_OK : PL_FOLDER_SCAN_LOST;
	if (!os->root_w)
		os->last_error = ERROR_INVALID_NAME;

	while (result == PL_FOLDER_SCAN_OK && folders.num) {
		char *relative = folders.array[folders.num - 1];
		da_pop_back(folders);
		bool is_root = !*relative;
		if (pl_stop_is_set(stop)) {
			bfree(relative);
			result = PL_FOLDER_SCAN_STOPPED;
			break;
		}

		wchar_t *pattern = below_root(os, relative, L"*");
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
					       FIND_FIRST_EX_LARGE_FETCH);
		bfree(pattern);
		if (find == INVALID_HANDLE_VALUE) {
			DWORD find_error = GetLastError();
			bool empty = find_error == ERROR_FILE_NOT_FOUND || find_error == ERROR_NO_MORE_FILES;
			if (is_root && !empty) {
				os->last_error = find_error;
				result = find_error == ERROR_ACCESS_DENIED ? PL_FOLDER_SCAN_DENIED
									   : PL_FOLDER_SCAN_LOST;
			} else if (find_error == ERROR_ACCESS_DENIED) {
				sink->skipped(sink->context, relative, "access denied");
			} else if (!empty && find_error != ERROR_PATH_NOT_FOUND) {
				/* a subfolder that vanished is left out; any other error, see leave_out_subfolder */
				if (!leave_out_subfolder(os, sink, relative, find_error)) {
					os->last_error = find_error;
					result = PL_FOLDER_SCAN_LOST;
				}
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
			/* hidden by attribute, or by a name starting with ".", as on Linux and macOS: a Mac leaves "._clip.mp4"
			 * and ".DS_Store" next to the files it writes to a USB drive or a share, mostly without the hidden
			 * attribute, and the same folder must give the same list on every system (R-DIR-5) */
			bool hidden = (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 ||
				      data.cFileName[0] == L'.';
			if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (os->include_subfolders && !hidden) {
					if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
						sink->skipped(sink->context, child,
							      "a junction, symbolic link or mount point");
					} else {
						da_push_back(subfolders, &child);
						child = NULL;
					}
				}
			} else if (!hidden) {
				uint64_t size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
				sink->file(sink->context, child, name, size, filetime_to_unix_ms(data.ftLastWriteTime),
					   (attributes & CLOUD_ATTRIBUTES) != 0);
			}
			bfree(child);
			bfree(name);
		} while (FindNextFileW(find, &data));
		DWORD next_error = GetLastError();
		FindClose(find);
		if (next_error != ERROR_NO_MORE_FILES) {
			if (!is_root && leave_out_subfolder(os, sink, relative, next_error)) {
				/* left out after it was read part way: none of it is listed (the sink dropped the files it
				 * gave), and none of its subfolders */
				for (size_t i = 0; i < subfolders.num; i++)
					bfree(subfolders.array[i]);
				da_resize(subfolders, 0);
			} else {
				/* the listing broke off, as when a share drops: the last good list stays */
				os->last_error = next_error;
				result = next_error == ERROR_ACCESS_DENIED ? PL_FOLDER_SCAN_DENIED
									   : PL_FOLDER_SCAN_LOST;
			}
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
	return result;
}

/* ---- the watch ---- */

void pl_folder_os_close(struct pl_folder_os *os)
{
	if (os->dir == INVALID_HANDLE_VALUE)
		return;
	if (os->armed) {
		CancelIoEx(os->dir, &os->overlapped);
		/* the buffer is the worker's again only once the call has finished (R-THR-3) */
		DWORD bytes = 0;
		GetOverlappedResult(os->dir, &os->overlapped, &bytes, TRUE);
		os->armed = false;
	}
	CloseHandle(os->dir);
	os->dir = INVALID_HANDLE_VALUE;
}

bool pl_folder_os_arm(struct pl_folder_os *os)
{
	if (os->dir == INVALID_HANDLE_VALUE)
		return false;
	if (os->armed)
		return true;
	ResetEvent(os->overlapped_event);
	memset(&os->overlapped, 0, sizeof(os->overlapped));
	os->overlapped.hEvent = os->overlapped_event;
	DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
		       FILE_NOTIFY_CHANGE_SIZE;
	if (!ReadDirectoryChangesW(os->dir, os->buffer, NOTIFY_BUFFER_BYTES, os->include_subfolders, filter, NULL,
				   &os->overlapped, NULL)) {
		os->last_error = GetLastError();
		return false;
	}
	os->armed = true;
	return true;
}

bool pl_folder_os_needs_rewatch(const struct pl_folder_os *os)
{
	/* ReadDirectoryChangesW covers the whole tree */
	UNUSED_PARAMETER(os);
	return false;
}

bool pl_folder_os_must_open_to_arm(const struct pl_folder_os *os)
{
	UNUSED_PARAMETER(os);
	return true;
}

enum pl_watch_event pl_folder_os_open(struct pl_folder_os *os)
{
	pl_folder_os_close(os);
	if (!os->root_w) {
		os->last_error = ERROR_INVALID_NAME;
		return PL_WATCH_OPEN_FAILED;
	}
	HANDLE dir = CreateFileW(os->root_w, FILE_LIST_DIRECTORY,
				 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	if (dir == INVALID_HANDLE_VALUE) {
		os->last_error = GetLastError();
		return os->last_error == ERROR_ACCESS_DENIED ? PL_WATCH_DENIED : PL_WATCH_OPEN_FAILED;
	}
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(dir, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		/* a file of that name: not a folder */
		CloseHandle(dir);
		os->last_error = ERROR_DIRECTORY;
		return PL_WATCH_OPEN_FAILED;
	}
	os->dir = dir;
	os->dir_identity = info;
	return pl_folder_os_arm(os) ? PL_WATCH_OPENED_ARMED : PL_WATCH_OPENED_ARM_FAILED;
}

/* The handle follows the folder it was opened on, through a rename or a move to the Recycle Bin, and the watch with
 * it; a new folder made at the path is another file, with another file index */
bool pl_folder_os_replaced(struct pl_folder_os *os)
{
	if (os->dir == INVALID_HANDLE_VALUE || !os->root_w)
		return false;
	/* some network file systems report no file index at all: nothing to compare */
	if (!os->dir_identity.nFileIndexHigh && !os->dir_identity.nFileIndexLow)
		return false;
	HANDLE at_path = CreateFileW(os->root_w, FILE_READ_ATTRIBUTES,
				     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				     FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (at_path == INVALID_HANDLE_VALUE)
		return false; /* nothing there, or not reachable: the listing says so */
	BY_HANDLE_FILE_INFORMATION info;
	bool known = GetFileInformationByHandle(at_path, &info) != 0;
	CloseHandle(at_path);
	return known && (info.dwVolumeSerialNumber != os->dir_identity.dwVolumeSerialNumber ||
			 info.nFileIndexHigh != os->dir_identity.nFileIndexHigh ||
			 info.nFileIndexLow != os->dir_identity.nFileIndexLow);
}

enum pl_folder_wait pl_folder_os_wait(struct pl_folder_os *os, struct pl_stop *stop, struct pl_wake *wake,
				      uint32_t timeout_ms)
{
	HANDLE handles[3] = {pl_stop_win32_event(stop), pl_wake_win32_event(wake), os->overlapped_event};
	DWORD timeout = timeout_ms == PL_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms;
	switch (WaitForMultipleObjects(os->armed ? 3 : 2, handles, FALSE, timeout)) {
	case WAIT_OBJECT_0:
		return PL_FOLDER_WAIT_STOP;
	case WAIT_OBJECT_0 + 1:
		return PL_FOLDER_WAIT_WAKE;
	case WAIT_OBJECT_0 + 2:
		return PL_FOLDER_WAIT_NOTIFIED;
	default:
		return PL_FOLDER_WAIT_TIMEOUT;
	}
}

/* Gives every paired rename in the buffer to the callback, with '/' between folders; the old name is kept across
 * completions, as a pair can be split between two */
static void collect_renames(struct pl_folder_os *os, DWORD bytes, pl_folder_rename_fn rename, void *context)
{
	const BYTE *cursor = (const BYTE *)os->buffer;
	const BYTE *end = cursor + bytes;
	const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
	while (cursor + header <= end) {
		const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)cursor;
		if (info->FileNameLength && cursor + header + info->FileNameLength <= end) {
			char *name = NULL;
			os_wcs_to_utf8_ptr(info->FileName, info->FileNameLength / sizeof(WCHAR), &name);
			for (char *c = name; c && *c; c++) {
				if (*c == '\\')
					*c = '/';
			}
			if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
				bfree(os->renamed_from);
				os->renamed_from = name;
				name = NULL;
			} else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && os->renamed_from) {
				rename(context, os->renamed_from, name);
				bfree(os->renamed_from);
				os->renamed_from = NULL;
			}
			bfree(name);
		}
		if (!info->NextEntryOffset)
			break;
		cursor += info->NextEntryOffset;
	}
}

bool pl_folder_os_take_completed_changes(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	if (!os->armed || !HasOverlappedIoCompleted(&os->overlapped))
		return false;
	DWORD bytes = 0;
	if (!GetOverlappedResult(os->dir, &os->overlapped, &bytes, FALSE) || bytes == 0)
		return false;
	ResetEvent(os->overlapped_event);
	os->armed = false;
	collect_renames(os, bytes, rename, context);
	return true;
}

enum pl_folder_note pl_folder_os_take_note(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	DWORD bytes = 0;
	BOOL ok = GetOverlappedResult(os->dir, &os->overlapped, &bytes, FALSE);
	DWORD error = ok ? ERROR_SUCCESS : GetLastError();
	if (!ok && error == ERROR_IO_INCOMPLETE)
		return PL_FOLDER_NOTE_NONE;
	ResetEvent(os->overlapped_event);
	os->armed = false;
	if (!ok) {
		/* not cancelled by us (close waits for its own cancel) */
		if (error == ERROR_OPERATION_ABORTED)
			return PL_FOLDER_NOTE_ABORTED;
		os->last_error = error;
		/* more changes than the buffer holds: ERROR_NOTIFY_ENUM_DIR locally, and through a share (SMB) also
		 * ERROR_MORE_DATA with nothing in the buffer, as seen on this machine's admin share (TESTING 2.11) */
		if (error == ERROR_NOTIFY_ENUM_DIR || error == ERROR_MORE_DATA)
			return PL_FOLDER_NOTE_OVERFLOW;
		if (error == ERROR_ACCESS_DENIED) {
			/* a deleted folder ends the call this way while our handle keeps it pending deletion: with the handle
			 * closed, a folder that is gone is lost, not a permissions problem */
			pl_folder_os_close(os);
			DWORD attributes = os->root_w ? GetFileAttributesW(os->root_w) : INVALID_FILE_ATTRIBUTES;
			DWORD lookup_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
			bool gone = lookup_error == ERROR_FILE_NOT_FOUND || lookup_error == ERROR_PATH_NOT_FOUND;
			return gone ? PL_FOLDER_NOTE_LOST : PL_FOLDER_NOTE_DENIED;
		}
		return PL_FOLDER_NOTE_LOST;
	}
	if (bytes == 0) {
		/* more changes than the buffer holds */
		os->last_error = ERROR_NOTIFY_ENUM_DIR;
		return PL_FOLDER_NOTE_OVERFLOW;
	}
	collect_renames(os, bytes, rename, context);
	return PL_FOLDER_NOTE_CHANGES;
}

/* ---- provider.h ---- */

bool pl_folder_is_directory(const char *path)
{
	wchar_t *path_w = path ? win32_path(path) : NULL;
	if (!path_w)
		return false;
	DWORD attributes = GetFileAttributesW(path_w);
	bfree(path_w);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool pl_path_on_network_drive(const char *path)
{
	/* \\?\Z:\ and \\.\Z:\ name the drive Z: too */
	if (path && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/') &&
	    (path[2] == '?' || path[2] == '.') && (path[3] == '\\' || path[3] == '/'))
		path += 4;
	if (!path || !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) || path[1] != ':')
		return false;
	/* the drive's kind comes from its definition, also while its share is offline */
	wchar_t root[] = {(wchar_t)path[0], L':', L'\\', L'\0'};
	return GetDriveTypeW(root) == DRIVE_REMOTE;
}
