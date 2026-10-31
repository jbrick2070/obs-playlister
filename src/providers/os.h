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

/* The few operating-system services the providers need, so that only os-win.c (and later os-posix.c) call the
 * operating system directly (R-LNX-2): a stop signal, a wake signal, a wait for either, a worker thread, the wall
 * clock, and reading one small file. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <util/dstr.h>

#ifdef _WIN32
#define PL_ACCESS_DENIED_TEXT "access denied (permissions or security software)" /* R-PLAT-10 */
#else
#define PL_ACCESS_DENIED_TEXT "access denied (permissions)" /* R-LNX-4 */
#endif

/* ---- signals and waits ---- */

/* Manual reset: once set it stays set, and every wait on it returns at once. */
struct pl_stop;
struct pl_stop *pl_stop_create(void);
void pl_stop_destroy(struct pl_stop *stop);
void pl_stop_set(struct pl_stop *stop);
void pl_stop_reset(struct pl_stop *stop);
bool pl_stop_is_set(struct pl_stop *stop);
#ifdef _WIN32
/* The Win32 event behind the signal, for waits that also wait on other handles (WinHTTP, overlapped reads). */
void *pl_stop_win32_event(struct pl_stop *stop);
#endif

/* Auto reset: one wait takes it. */
struct pl_wake;
struct pl_wake *pl_wake_create(void);
void pl_wake_destroy(struct pl_wake *wake);
void pl_wake_set(struct pl_wake *wake);

enum pl_wait_result {
	PL_WAIT_STOP,
	PL_WAIT_WAKE,
	PL_WAIT_TIMEOUT,
};

#define PL_WAIT_FOREVER UINT32_MAX

/* Waits until the stop is set, the wake is set (wake may be NULL) or timeout_ms has passed. A set stop wins. */
enum pl_wait_result pl_wait(struct pl_stop *stop, struct pl_wake *wake, uint32_t timeout_ms);

/* ---- worker threads ---- */

/* A thread with the workers' 256 KB stack (R-THR-2). NULL when it could not be started; error then has the
 * operating system's code. */
struct pl_thread;
struct pl_thread *pl_thread_start(void (*main)(void *data), void *data, unsigned long *error);

/* Waits for the thread to end and frees it. On Windows a call the thread is blocked in (a stalled share) is
 * cancelled every 100 ms (R-THR-3); elsewhere that is not possible, and a wait over 5 s is logged once with
 * what (R-LNX-7). */
void pl_thread_join(struct pl_thread *thread, const char *what);

/* ---- time and errors ---- */

/* Now as UTC milliseconds since 1970 (R-DIR-10). */
int64_t pl_utc_now_ms(void);

/* Appends the operating system's name for an error code: "Win32 error 5" on Windows. */
void pl_os_error_cat(struct dstr *text, unsigned long code);

/* ---- one small file ---- */

enum pl_file_result {
	PL_FILE_OK,
	PL_FILE_BAD_PATH,      /* not valid UTF-8 */
	PL_FILE_NOT_FOUND,     /* the file or a folder on its path does not exist */
	PL_FILE_ACCESS_DENIED, /* R-PLAT-10, R-LNX-4 */
	PL_FILE_IN_USE,        /* another program holds it in a way that refuses this read */
	PL_FILE_ERROR,         /* anything else; os_error has the code */
};

struct pl_file_info {
	uint64_t size;
	int64_t write_stamp; /* compared for equality only; its unit differs by platform */
	bool directory;
};

enum pl_file_result pl_file_get_info(const char *path, struct pl_file_info *info, unsigned long *os_error);

/* Reads up to size bytes into a bmem buffer with one byte to spare (data[*got] is not set). On failure *data is
 * NULL. A file shorter than size gives the bytes there are. */
enum pl_file_result pl_file_read(const char *path, uint64_t size, char **data, size_t *got, unsigned long *os_error);
