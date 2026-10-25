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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <util/darray.h>

/* The folder provider's decisions, without Win32: the write-complete gate (R-DIR-4) and the watch modes (R-DIR-3).
 * The provider owns one of each per folder row on its worker thread, feeds them what it saw (listings, samples,
 * probe results, notification outcomes, the time) and does what they say. Nothing here touches the disk, sleeps or
 * logs.
 *
 * Two clocks: mono_ms is a monotonic clock (os_gettime_ns / 1000000), used for every window and deadline; utc_ms is
 * the wall clock in UTC milliseconds, used only to judge the age of a file's last-write time. */

/* ---- Write-complete gate (R-DIR-4) ---- */

#define PL_GATE_WINDOW_MS 2000 /* stability window and minimum age of the last write */

/* One file as a directory listing reports it, without opening it */
struct pl_gate_listing {
	const char *key;      /* R-KEY-1 identity */
	const char *location; /* the path to probe and sample */
	uint64_t size;
	int64_t write_ms; /* last-write time, UTC ms */
};

enum pl_probe_result {
	PL_PROBE_OK,     /* opened while denying writers: nobody holds it open for write */
	PL_PROBE_BUSY,   /* a sharing violation: still being written; try again later */
	PL_PROBE_DENIED, /* access denied: kept out and reported as a row error (R-PLAT-10), tried again later */
	PL_PROBE_GONE,   /* the file no longer exists */
};

/* Opens the file denying other writers and closes it at once */
typedef enum pl_probe_result (*pl_probe_fn)(void *context, const char *location);
/* Reads size and last-write time without opening the file and sets *sampled_mono_ms to the monotonic time read once
 * the sample is taken; false when the file no longer exists. The listing and the samples must read the same
 * attributes (the same API), or a file that is not changing looks as if it were. */
typedef bool (*pl_sample_fn)(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			     int64_t *sampled_mono_ms);

struct pl_gate_file {
	char *key;
	char *location;
	uint64_t size;
	int64_t write_ms;
	bool admitted;
	bool denied;         /* the last probe was refused: the row shows "access denied" */
	int64_t sampled_ms;  /* candidates: monotonic time the current size and time were first seen */
	int64_t deadline_ms; /* candidates: monotonic time they are next due */
	/* The last admitted version. A file rewritten in place (a render saved again under its name) goes back through
	 * the gate, but its row keeps this version, with its place, until the new one is admitted (PLAN step 5). */
	bool has_version;
	uint64_t version_size;
	int64_t version_write_ms;
};

/* Files are kept sorted by key */
struct pl_gate {
	DARRAY(struct pl_gate_file) files;
};

void pl_gate_init(struct pl_gate *gate);
void pl_gate_free(struct pl_gate *gate);

/* A full listing of the folder, with both clocks read after the listing finished. Files no longer listed leave the
 * gate. An admitted file whose size and time did not change stays admitted and is never probed again; one that
 * changed becomes a candidate again. In the initial scan every other file whose last write is at least the window
 * old is probed once and admitted at once if the probe succeeds; every other file, and every new file in a later
 * listing, becomes a candidate due one window later. Keys must be unique within a listing. */
void pl_gate_listing(struct pl_gate *gate, const struct pl_gate_listing *files, size_t count, bool initial,
		     int64_t mono_ms, int64_t utc_ms, pl_probe_fn probe, void *context);

/* The batched second pass: every candidate due by mono_ms is sampled once. One that vanished leaves; one whose size
 * or time changed starts its window again; one unchanged across two samples at least the window apart, with its
 * last write the window old, is probed and admitted if the probe succeeds, else due again one window later (a file
 * held open forever never holds up the others). Returns how many were admitted. */
size_t pl_gate_tick(struct pl_gate *gate, int64_t mono_ms, int64_t utc_ms, pl_sample_fn sample, pl_probe_fn probe,
		    void *context);

/* The earliest candidate deadline, or INT64_MAX with no candidates: when the worker must wake */
int64_t pl_gate_next_deadline(const struct pl_gate *gate);

/* A rename reported as a paired event: an admitted file whose size and time are unchanged moves to its new name and
 * stays admitted (no new wait); a candidate keeps its timing under the new name; a file the rename replaced leaves.
 * Returns true when the gate knew old_key, so the provider publishes the entry with previous_key. The strings must
 * not point into the gate. */
bool pl_gate_rename(struct pl_gate *gate, const char *old_key, const char *new_key, const char *new_location,
		    uint64_t size, int64_t write_ms, int64_t mono_ms);

/* The file with this key, or NULL */
const struct pl_gate_file *pl_gate_find(const struct pl_gate *gate, const char *key);
size_t pl_gate_pending_count(const struct pl_gate *gate);
size_t pl_gate_admitted_count(const struct pl_gate *gate);
/* Candidates whose last probe was refused; above 0 the row shows "access denied" (R-PLAT-10) */
size_t pl_gate_denied_count(const struct pl_gate *gate);

/* ---- Watch modes (R-DIR-3) ---- */

#define PL_WATCH_INTERVAL_MIN_S 2
#define PL_WATCH_INTERVAL_MAX_S 3600

enum pl_watch_mode {
	PL_WATCH_STARTING,    /* not opened yet */
	PL_WATCH_NOTIFY,      /* ReadDirectoryChangesW armed */
	PL_WATCH_RESCAN_ONLY, /* opened, but notifications cannot be armed on it (some shares) */
	PL_WATCH_MISSING,     /* cannot be opened, or lost; the last list is kept and marked stale */
};

/* What the provider observed */
enum pl_watch_event {
	PL_WATCH_OPENED_ARMED,      /* the folder opened (or was open) and notifications were armed */
	PL_WATCH_OPENED_ARM_FAILED, /* the folder opened (or was open) but arming failed */
	PL_WATCH_OPEN_FAILED,       /* cannot open it, or it is not a directory */
	PL_WATCH_DENIED,            /* opening, the armed call or a rescan failed with access denied (R-PLAT-10) */
	PL_WATCH_LOST,     /* the armed call or a rescan failed with the folder gone (deleted, share offline) */
	PL_WATCH_CHANGED,  /* a notification with changes (after the provider's debounce) */
	PL_WATCH_OVERFLOW, /* a notification with zero bytes or ERROR_NOTIFY_ENUM_DIR */
	PL_WATCH_TIMER,    /* the scheduled time came */
};

struct pl_watch {
	enum pl_watch_mode mode;
	bool check_enabled;        /* "Check folder every N seconds" */
	uint32_t check_interval_s; /* N, clamped to 2 to 3600 */
	int64_t next_ms;           /* monotonic time PL_WATCH_TIMER is due */
	uint32_t overflow_run; /* overflows in a row: each within PL_WATCH_OVERFLOW_RUN_MS of the last, none with changes */
	int64_t last_overflow_ms;
};

/* R-DIR-3: this many overflows in a run (a share can complete every new call at once that way) switch the folder to
 * rescan-only, so it is listed on the interval instead of after every call. The gap is measured between the times the
 * provider handles them, so it includes the listing each one starts. */
#define PL_WATCH_OVERFLOW_RUN 3
#define PL_WATCH_OVERFLOW_RUN_MS 10000

/* What to do now. When several are set: close first, then open or re-arm, then rescan, so a change made during the
 * scan is caught by the armed call. mode_changed asks for one log line: path, mode and reason, with the Win32 error
 * when the provider has one. */
struct pl_watch_actions {
	bool close;  /* close the handle */
	bool open;   /* (re)open the folder and arm notifications, then report the outcome */
	bool rearm;  /* arm notifications again on the open handle, then report the outcome */
	bool rescan; /* list the folder */
	bool mode_changed;
	enum pl_watch_mode mode; /* the mode after this event */
	const char *reason;      /* set when the mode changed */
};

void pl_watch_init(struct pl_watch *watch, bool check_enabled, uint32_t check_interval_s, int64_t mono_ms);
/* The interval option changed on the running provider: the next timer is re-armed from now. Values equal to the
 * current ones change nothing, so passing the options on every settings update does not delay the folder check. */
void pl_watch_set_interval(struct pl_watch *watch, bool check_enabled, uint32_t check_interval_s, int64_t mono_ms);

/* A call completed with changes (before their debounced PL_WATCH_CHANGED): a run of overflows ends */
void pl_watch_notified(struct pl_watch *watch);
struct pl_watch_actions pl_watch_handle(struct pl_watch *watch, enum pl_watch_event event, int64_t mono_ms);
const char *pl_watch_mode_name(enum pl_watch_mode mode);
