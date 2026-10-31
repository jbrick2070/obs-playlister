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

/* The providers' operating-system services on Windows (os.h). The calls are the ones provider-list.c made itself
 * before the Linux port, unchanged. */

#include "os.h"

#include <windows.h>

#include <util/bmem.h>
#include <util/platform.h>

#define WORKER_STACK_SIZE (256 * 1024) /* R-THR-2 */
#define FILETIME_UNIX_EPOCH 116444736000000000LL

/* ---- signals and waits ---- */

struct pl_stop {
	HANDLE event; /* manual reset */
};

struct pl_wake {
	HANDLE event; /* auto reset */
};

struct pl_stop *pl_stop_create(void)
{
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event)
		return NULL;
	struct pl_stop *stop = bzalloc(sizeof(*stop));
	stop->event = event;
	return stop;
}

void pl_stop_destroy(struct pl_stop *stop)
{
	if (!stop)
		return;
	CloseHandle(stop->event);
	bfree(stop);
}

void pl_stop_set(struct pl_stop *stop)
{
	SetEvent(stop->event);
}

void pl_stop_reset(struct pl_stop *stop)
{
	ResetEvent(stop->event);
}

bool pl_stop_is_set(struct pl_stop *stop)
{
	return WaitForSingleObject(stop->event, 0) == WAIT_OBJECT_0;
}

void *pl_stop_win32_event(struct pl_stop *stop)
{
	return stop->event;
}

struct pl_wake *pl_wake_create(void)
{
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!event)
		return NULL;
	struct pl_wake *wake = bzalloc(sizeof(*wake));
	wake->event = event;
	return wake;
}

void pl_wake_destroy(struct pl_wake *wake)
{
	if (!wake)
		return;
	CloseHandle(wake->event);
	bfree(wake);
}

void pl_wake_set(struct pl_wake *wake)
{
	SetEvent(wake->event);
}

enum pl_wait_result pl_wait(struct pl_stop *stop, struct pl_wake *wake, uint32_t timeout_ms)
{
	HANDLE handles[2] = {stop->event, wake ? wake->event : NULL};
	DWORD timeout = timeout_ms == PL_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms;
	switch (WaitForMultipleObjects(wake ? 2 : 1, handles, FALSE, timeout)) {
	case WAIT_OBJECT_0:
		return PL_WAIT_STOP;
	case WAIT_OBJECT_0 + 1:
		return PL_WAIT_WAKE;
	default:
		return PL_WAIT_TIMEOUT;
	}
}

/* ---- worker threads ---- */

struct pl_thread {
	HANDLE handle;
	void (*main)(void *data);
	void *data;
};

static DWORD WINAPI thread_main(void *param)
{
	struct pl_thread *thread = param;
	thread->main(thread->data);
	return 0;
}

struct pl_thread *pl_thread_start(void (*main)(void *data), void *data, unsigned long *error)
{
	struct pl_thread *thread = bzalloc(sizeof(*thread));
	thread->main = main;
	thread->data = data;
	thread->handle =
		CreateThread(NULL, WORKER_STACK_SIZE, thread_main, thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (!thread->handle) {
		if (error)
			*error = GetLastError();
		bfree(thread);
		return NULL;
	}
	return thread;
}

void pl_thread_join(struct pl_thread *thread, const char *what)
{
	UNUSED_PARAMETER(what);
	if (!thread)
		return;
	/* a call stuck on a stalled share is interrupted, again and again, as the thread may not have entered it yet
	 * when the first cancel comes (R-THR-3) */
	while (WaitForSingleObject(thread->handle, 100) == WAIT_TIMEOUT)
		CancelSynchronousIo(thread->handle);
	CloseHandle(thread->handle);
	bfree(thread);
}

/* ---- time and errors ---- */

int64_t pl_utc_now_ms(void)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER value = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
	return ((int64_t)value.QuadPart - FILETIME_UNIX_EPOCH) / 10000;
}

void pl_os_error_cat(struct dstr *text, unsigned long code)
{
	dstr_catf(text, "Win32 error %lu", code);
}

/* ---- one small file ---- */

static wchar_t *wide_path(const char *path)
{
	wchar_t *wide = NULL;
	if (!os_utf8_to_wcs_ptr(path, 0, &wide) || !wide) {
		bfree(wide);
		return NULL;
	}
	return wide;
}

static enum pl_file_result file_result(DWORD error)
{
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return PL_FILE_NOT_FOUND;
	case ERROR_ACCESS_DENIED:
		return PL_FILE_ACCESS_DENIED;
	case ERROR_SHARING_VIOLATION:
		return PL_FILE_IN_USE;
	default:
		return PL_FILE_ERROR;
	}
}

enum pl_file_result pl_file_get_info(const char *path, struct pl_file_info *info, unsigned long *os_error)
{
	*os_error = 0;
	wchar_t *wide = wide_path(path);
	if (!wide)
		return PL_FILE_BAD_PATH;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes);
	*os_error = ok ? 0 : GetLastError();
	bfree(wide);
	if (!ok)
		return file_result(*os_error);
	info->size = ((uint64_t)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
	info->write_stamp = ((int64_t)attributes.ftLastWriteTime.dwHighDateTime << 32) |
			    attributes.ftLastWriteTime.dwLowDateTime;
	info->directory = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	return PL_FILE_OK;
}

enum pl_file_result pl_file_read(const char *path, uint64_t size, char **data, size_t *got, unsigned long *os_error)
{
	*data = NULL;
	*got = 0;
	*os_error = 0;
	wchar_t *wide = wide_path(path);
	if (!wide)
		return PL_FILE_BAD_PATH;
	HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bfree(wide);
	if (file == INVALID_HANDLE_VALUE) {
		*os_error = GetLastError();
		return file_result(*os_error);
	}
	char *buffer = bmalloc((size_t)size + 1);
	size_t done = 0;
	bool ok = true;
	while (done < size) {
		DWORD chunk = 0;
		DWORD want = (DWORD)(size - done > (1 << 20) ? (1 << 20) : size - done);
		if (!ReadFile(file, buffer + done, want, &chunk, NULL)) {
			ok = false;
			break;
		}
		if (!chunk)
			break;
		done += chunk;
	}
	DWORD error = GetLastError();
	CloseHandle(file);
	if (!ok) {
		bfree(buffer);
		*os_error = error;
		return PL_FILE_ERROR;
	}
	*data = buffer;
	*got = done;
	return PL_FILE_OK;
}
