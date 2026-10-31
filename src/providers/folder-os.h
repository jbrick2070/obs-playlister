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

#pragma once

/* What the folder provider (provider-folder.c) asks the operating system for, so that one file per platform holds
 * every call (R-LNX-2): listing a folder, sampling and probing one file for the write-complete gate (R-DIR-4), and
 * watching the folder for changes (R-DIR-2, R-DIR-3). Only the provider's worker calls these, and it alone opens and
 * closes the watch (R-THR-3). Windows: folder-os-win.c. */

#include <stdbool.h>
#include <stdint.h>

#include <util/dstr.h>

#include "core/folder-state.h"

struct pl_stop;
struct pl_wake;
struct pl_folder_os;

/* location is the row's path. NULL only when the system is out of resources; a path it cannot use fails at the
 * first open or listing. */
struct pl_folder_os *pl_folder_os_create(const char *location, bool include_subfolders);
/* Closes the watch (waiting for a pending call, R-THR-3) and frees. */
void pl_folder_os_destroy(struct pl_folder_os *os);

/* Appends the system's name for the error behind the last failure: "Win32 error 5". */
void pl_folder_os_error_cat(const struct pl_folder_os *os, struct dstr *text);

/* ---- listing (R-DIR-1) ---- */

enum pl_folder_scan {
	PL_FOLDER_SCAN_OK,
	PL_FOLDER_SCAN_LOST,   /* the root cannot be listed, or a listing broke off: the last good list stays */
	PL_FOLDER_SCAN_DENIED, /* access denied on the root */
	PL_FOLDER_SCAN_STOPPED,
};

/* Called for every file that is not hidden, in listing order: a folder's own files, then its subfolders depth first.
 * relative uses '/'; name is the last part. A cloud placeholder (R-DIR-5) is reported with cloud set and is never
 * opened. skipped is called for a subfolder left out below the root, with why. */
struct pl_folder_listing_sink {
	void *context;
	void (*file)(void *context, const char *relative, const char *name, uint64_t size, int64_t write_ms,
		     bool cloud);
	void (*skipped)(void *context, const char *relative, const char *why);
};

/* Lists the folder (and below it when include_subfolders is set) without opening a file. Returns early, STOPPED,
 * when stop is set. On any result but OK the sink may have been given part of a listing, which the caller drops. */
enum pl_folder_scan pl_folder_os_list(struct pl_folder_os *os, struct pl_stop *stop,
				      const struct pl_folder_listing_sink *sink);

/* ---- the write-complete gate's disk access (R-DIR-4); the context is unused ---- */

enum pl_probe_result pl_folder_os_probe(void *context, const char *location);
bool pl_folder_os_sample(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			 int64_t *sampled_mono_ms);

/* ---- the watch (R-DIR-2, R-DIR-3) ---- */

/* Opens the folder for watching and arms it: PL_WATCH_OPENED_ARMED, PL_WATCH_OPENED_ARM_FAILED, PL_WATCH_DENIED or
 * PL_WATCH_OPEN_FAILED (also for a file of that name). Any open watch is closed first. */
enum pl_watch_event pl_folder_os_open(struct pl_folder_os *os);
/* Arms the open watch again; false when it is not open or cannot be armed. Armed already is true. */
bool pl_folder_os_arm(struct pl_folder_os *os);
void pl_folder_os_close(struct pl_folder_os *os);

/* A paired rename seen by the watch, as relative paths with '/' */
typedef void (*pl_folder_rename_fn)(void *context, const char *old_relative, const char *new_relative);

enum pl_folder_wait {
	PL_FOLDER_WAIT_STOP,
	PL_FOLDER_WAIT_WAKE,
	PL_FOLDER_WAIT_TIMEOUT,
	PL_FOLDER_WAIT_NOTIFIED, /* the armed watch completed: call pl_folder_os_take_note */
};

/* Waits for the stop, the wake, the armed watch or the timeout; a set stop wins. */
enum pl_folder_wait pl_folder_os_wait(struct pl_folder_os *os, struct pl_stop *stop, struct pl_wake *wake,
				      uint32_t timeout_ms);

enum pl_folder_note {
	PL_FOLDER_NOTE_NONE,     /* nothing has completed yet */
	PL_FOLDER_NOTE_CHANGES,  /* changes; their renames were given to the callback; the watch is no longer armed */
	PL_FOLDER_NOTE_OVERFLOW, /* more changes than could be told; the watch is no longer armed */
	PL_FOLDER_NOTE_ABORTED,  /* the call was cancelled by someone else; the watch is no longer armed */
	PL_FOLDER_NOTE_LOST,     /* the folder went away; the watch is no longer armed */
	PL_FOLDER_NOTE_DENIED,   /* access to a folder that is still there was denied; the watch is closed */
};

/* What the armed watch reported. */
enum pl_folder_note pl_folder_os_take_note(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context);
/* As pl_folder_os_take_note, but only for changes that already completed: true when it took some (the watch is then
 * no longer armed); anything else stays for pl_folder_os_take_note. */
bool pl_folder_os_take_completed_changes(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context);
