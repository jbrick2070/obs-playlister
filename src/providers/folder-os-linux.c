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

/* The folder provider's Linux calls (folder-os.h), an interim stand-in until the inotify watch and listing
 * (R-LNX-3, R-LNX-4) replace it in PLAN step 3: a folder row cannot be opened, so it shows "folder not found"
 * and its log line says why. pl_folder_is_directory is already real, so rows are classified as on Windows. */

#include "folder-os.h"

#include <sys/stat.h>

#include <util/bmem.h>

#include "os.h"
#include "provider.h"

struct pl_folder_os {
	bool unused;
};

struct pl_folder_os *pl_folder_os_create(const char *location, bool include_subfolders)
{
	UNUSED_PARAMETER(location);
	UNUSED_PARAMETER(include_subfolders);
	return bzalloc(sizeof(struct pl_folder_os));
}

void pl_folder_os_destroy(struct pl_folder_os *os)
{
	bfree(os);
}

void pl_folder_os_error_cat(const struct pl_folder_os *os, struct dstr *text)
{
	UNUSED_PARAMETER(os);
	dstr_cat(text, "folders are not supported on Linux yet");
}

enum pl_folder_scan pl_folder_os_list(struct pl_folder_os *os, struct pl_stop *stop,
				      const struct pl_folder_listing_sink *sink)
{
	UNUSED_PARAMETER(os);
	UNUSED_PARAMETER(sink);
	return pl_stop_is_set(stop) ? PL_FOLDER_SCAN_STOPPED : PL_FOLDER_SCAN_LOST;
}

enum pl_probe_result pl_folder_os_probe(void *context, const char *location)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(location);
	return PL_PROBE_GONE;
}

bool pl_folder_os_sample(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			 int64_t *sampled_mono_ms)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(location);
	*size = 0;
	*write_ms = 0;
	*sampled_mono_ms = 0;
	return false;
}

enum pl_watch_event pl_folder_os_open(struct pl_folder_os *os)
{
	UNUSED_PARAMETER(os);
	return PL_WATCH_OPEN_FAILED;
}

bool pl_folder_os_arm(struct pl_folder_os *os)
{
	UNUSED_PARAMETER(os);
	return false;
}

void pl_folder_os_close(struct pl_folder_os *os)
{
	UNUSED_PARAMETER(os);
}

enum pl_folder_wait pl_folder_os_wait(struct pl_folder_os *os, struct pl_stop *stop, struct pl_wake *wake,
				      uint32_t timeout_ms)
{
	UNUSED_PARAMETER(os);
	switch (pl_wait(stop, wake, timeout_ms)) {
	case PL_WAIT_STOP:
		return PL_FOLDER_WAIT_STOP;
	case PL_WAIT_WAKE:
		return PL_FOLDER_WAIT_WAKE;
	default:
		return PL_FOLDER_WAIT_TIMEOUT;
	}
}

enum pl_folder_note pl_folder_os_take_note(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	UNUSED_PARAMETER(os);
	UNUSED_PARAMETER(rename);
	UNUSED_PARAMETER(context);
	return PL_FOLDER_NOTE_NONE;
}

bool pl_folder_os_take_completed_changes(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	UNUSED_PARAMETER(os);
	UNUSED_PARAMETER(rename);
	UNUSED_PARAMETER(context);
	return false;
}

bool pl_folder_is_directory(const char *path)
{
	struct stat status;
	return path && *path && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}
