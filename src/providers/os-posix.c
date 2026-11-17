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

/* The providers' operating-system services on Linux and macOS (os.h, R-LNX-7, R-MAC-3). The signals are eventfds on
 * Linux and non-blocking pipes on macOS, which has no eventfd, so a wait can poll them together with an inotify,
 * FSEvents or libcurl descriptor. */

#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE /* pthread_timedjoin_np */
#endif

#include "os.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#ifndef __APPLE__
#include <sys/eventfd.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <util/bmem.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#define WORKER_STACK_SIZE (256 * 1024) /* R-THR-2 */
#define JOIN_LOG_MS 5000               /* R-LNX-7 */

/* ---- signals and waits ---- */

/* fd is what a wait polls and reads; write_fd is what a set writes to: the same eventfd on Linux, the pipe's other
 * end on macOS */
struct pl_stop {
	int fd; /* readable once set; never read, so it stays set */
	int write_fd;
};

struct pl_wake {
	int fd; /* readable while set; a wait that returns WAKE reads it */
	int write_fd;
};

#ifdef __APPLE__
static bool set_flags(int fd)
{
	int status = fcntl(fd, F_GETFL);
	return status >= 0 && fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static bool new_signal_fds(int *read_fd, int *write_fd)
{
	int fds[2];
	if (pipe(fds) != 0)
		return false;
	if (!set_flags(fds[0]) || !set_flags(fds[1])) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	*read_fd = fds[0];
	*write_fd = fds[1];
	return true;
}

static void close_signal_fds(int read_fd, int write_fd)
{
	close(read_fd);
	close(write_fd);
}

static void signal_fd(int write_fd)
{
	char one = 1;
	/* EAGAIN only when the pipe is full, which still leaves it set */
	while (write(write_fd, &one, 1) < 0 && errno == EINTR)
		;
}

static void drain_fd(int read_fd)
{
	char bytes[64];
	for (;;) {
		ssize_t got = read(read_fd, bytes, sizeof(bytes));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < (ssize_t)sizeof(bytes))
			break;
	}
}
#else
static bool new_signal_fds(int *read_fd, int *write_fd)
{
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0)
		return false;
	*read_fd = fd;
	*write_fd = fd;
	return true;
}

static void close_signal_fds(int read_fd, int write_fd)
{
	UNUSED_PARAMETER(write_fd);
	close(read_fd);
}

static void signal_fd(int write_fd)
{
	uint64_t one = 1;
	/* EAGAIN only when the counter is full, which still leaves it set */
	while (write(write_fd, &one, sizeof(one)) < 0 && errno == EINTR)
		;
}

static void drain_fd(int read_fd)
{
	uint64_t value;
	while (read(read_fd, &value, sizeof(value)) < 0 && errno == EINTR)
		;
}
#endif

static bool fd_is_set(int fd)
{
	struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
	return poll(&poll_fd, 1, 0) > 0 && (poll_fd.revents & POLLIN);
}

struct pl_stop *pl_stop_create(void)
{
	int fd, write_fd;
	if (!new_signal_fds(&fd, &write_fd))
		return NULL;
	struct pl_stop *stop = bzalloc(sizeof(*stop));
	stop->fd = fd;
	stop->write_fd = write_fd;
	return stop;
}

void pl_stop_destroy(struct pl_stop *stop)
{
	if (!stop)
		return;
	close_signal_fds(stop->fd, stop->write_fd);
	bfree(stop);
}

void pl_stop_set(struct pl_stop *stop)
{
	signal_fd(stop->write_fd);
}

void pl_stop_reset(struct pl_stop *stop)
{
	drain_fd(stop->fd);
}

bool pl_stop_is_set(struct pl_stop *stop)
{
	return fd_is_set(stop->fd);
}

int pl_stop_fd(struct pl_stop *stop)
{
	return stop->fd;
}

struct pl_wake *pl_wake_create(void)
{
	int fd, write_fd;
	if (!new_signal_fds(&fd, &write_fd))
		return NULL;
	struct pl_wake *wake = bzalloc(sizeof(*wake));
	wake->fd = fd;
	wake->write_fd = write_fd;
	return wake;
}

void pl_wake_destroy(struct pl_wake *wake)
{
	if (!wake)
		return;
	close_signal_fds(wake->fd, wake->write_fd);
	bfree(wake);
}

void pl_wake_set(struct pl_wake *wake)
{
	signal_fd(wake->write_fd);
}

void pl_wake_take(struct pl_wake *wake)
{
	drain_fd(wake->fd);
}

int pl_wake_fd(struct pl_wake *wake)
{
	return wake->fd;
}

static int64_t mono_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

enum pl_wait_result pl_wait(struct pl_stop *stop, struct pl_wake *wake, uint32_t timeout_ms)
{
	struct pollfd fds[2] = {{.fd = stop->fd, .events = POLLIN}, {.fd = wake ? wake->fd : -1, .events = POLLIN}};
	int64_t deadline = timeout_ms == PL_WAIT_FOREVER ? 0 : mono_ms() + timeout_ms;
	for (;;) {
		int wait_ms = -1;
		if (timeout_ms != PL_WAIT_FOREVER) {
			int64_t left = deadline - mono_ms();
			wait_ms = left <= 0 ? 0 : left > INT32_MAX ? INT32_MAX : (int)left;
		}
		int ready = poll(fds, wake ? 2 : 1, wait_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		/* a set stop wins */
		if (ready > 0 && (fds[0].revents & POLLIN))
			return PL_WAIT_STOP;
		if (ready > 0 && wake && (fds[1].revents & POLLIN)) {
			drain_fd(wake->fd);
			return PL_WAIT_WAKE;
		}
		/* a wait clamped to what poll takes goes on until the real deadline */
		if (ready == 0 && timeout_ms != PL_WAIT_FOREVER && mono_ms() < deadline)
			continue;
		return PL_WAIT_TIMEOUT;
	}
}

/* ---- worker threads ---- */

struct pl_thread {
	pthread_t handle;
	void (*main)(void *data);
	void *data;
#ifdef __APPLE__
	/* macOS has no pthread_timedjoin_np: the thread says it is done, and the join waits for that with a timeout */
	pthread_mutex_t mutex;
	pthread_cond_t done_cond;
	bool done;
#endif
};

static void *thread_main(void *param)
{
	struct pl_thread *thread = param;
	thread->main(thread->data);
#ifdef __APPLE__
	pthread_mutex_lock(&thread->mutex);
	thread->done = true;
	pthread_cond_broadcast(&thread->done_cond);
	pthread_mutex_unlock(&thread->mutex);
#endif
	return NULL;
}

struct pl_thread *pl_thread_start(void (*main)(void *data), void *data, unsigned long *error)
{
	struct pl_thread *thread = bzalloc(sizeof(*thread));
	thread->main = main;
	thread->data = data;
#ifdef __APPLE__
	pthread_mutex_init(&thread->mutex, NULL);
	pthread_cond_init(&thread->done_cond, NULL);
#endif
	pthread_attr_t attributes;
	int result = pthread_attr_init(&attributes);
	if (result == 0) {
		pthread_attr_setstacksize(&attributes, WORKER_STACK_SIZE);
		result = pthread_create(&thread->handle, &attributes, thread_main, thread);
		pthread_attr_destroy(&attributes);
	}
	if (result != 0) {
		if (error)
			*error = (unsigned long)result;
#ifdef __APPLE__
		pthread_cond_destroy(&thread->done_cond);
		pthread_mutex_destroy(&thread->mutex);
#endif
		bfree(thread);
		return NULL;
	}
	return thread;
}

void pl_thread_join(struct pl_thread *thread, const char *what)
{
	if (!thread)
		return;
	/* a call stuck on a hung network file system cannot be interrupted here (R-LNX-7): wait, and say so once */
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += JOIN_LOG_MS / 1000;
#ifdef __APPLE__
	/* the same on macOS (R-MAC-3): a call blocked on an unreachable SMB, NFS or AFP volume returns only when the
	 * volume's own timeout ends it */
	bool timed_out = false;
	pthread_mutex_lock(&thread->mutex);
	while (!thread->done && !timed_out) {
		if (pthread_cond_timedwait(&thread->done_cond, &thread->mutex, &deadline) == ETIMEDOUT)
			timed_out = !thread->done;
	}
	pthread_mutex_unlock(&thread->mutex);
	if (timed_out)
		obs_log(LOG_WARNING, "%s: still waiting for the worker to finish a call (a hung network share?)",
			what ? what : "worker");
	pthread_join(thread->handle, NULL);
	pthread_cond_destroy(&thread->done_cond);
	pthread_mutex_destroy(&thread->mutex);
#else
	if (pthread_timedjoin_np(thread->handle, NULL, &deadline) == ETIMEDOUT) {
		obs_log(LOG_WARNING, "%s: still waiting for the worker to finish a call (a hung network share?)",
			what ? what : "worker");
		pthread_join(thread->handle, NULL);
	}
#endif
	bfree(thread);
}

/* ---- time and errors ---- */

int64_t pl_utc_now_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void pl_os_error_cat(struct dstr *text, unsigned long code)
{
	char buffer[128];
	const char *message = buffer;
	/* the GNU strerror_r returns its message; the XSI one fills the buffer */
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
	message = strerror_r((int)code, buffer, sizeof(buffer));
#else
	if (strerror_r((int)code, buffer, sizeof(buffer)) != 0)
		buffer[0] = '\0';
#endif
	dstr_catf(text, "errno %lu, %s", code, message);
}

/* ---- one small file ---- */

static enum pl_file_result file_result(int error)
{
	switch (error) {
	case ENOENT:
	case ENOTDIR:
		return PL_FILE_NOT_FOUND;
	case EACCES:
	case EPERM:
		return PL_FILE_ACCESS_DENIED;
	default:
		return PL_FILE_ERROR;
	}
}

enum pl_file_result pl_file_get_info(const char *path, struct pl_file_info *info, unsigned long *os_error)
{
	*os_error = 0;
	if (!path || !*path)
		return PL_FILE_BAD_PATH;
	struct stat status;
	if (stat(path, &status) != 0) {
		*os_error = (unsigned long)errno;
		return file_result(errno);
	}
	info->size = (uint64_t)status.st_size;
#ifdef __APPLE__
	info->write_stamp = (int64_t)status.st_mtimespec.tv_sec * 1000000000 + status.st_mtimespec.tv_nsec;
#else
	info->write_stamp = (int64_t)status.st_mtim.tv_sec * 1000000000 + status.st_mtim.tv_nsec;
#endif
	info->directory = S_ISDIR(status.st_mode);
	return PL_FILE_OK;
}

enum pl_file_result pl_file_read(const char *path, uint64_t size, char **data, size_t *got, unsigned long *os_error)
{
	*data = NULL;
	*got = 0;
	*os_error = 0;
	if (!path || !*path)
		return PL_FILE_BAD_PATH;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		*os_error = (unsigned long)errno;
		return file_result(errno);
	}
	char *buffer = bmalloc((size_t)size + 1);
	size_t done = 0;
	while (done < size) {
		size_t want = size - done > (1 << 20) ? (1 << 20) : (size_t)(size - done);
		ssize_t chunk = read(fd, buffer + done, want);
		if (chunk < 0 && errno == EINTR)
			continue;
		if (chunk < 0) {
			*os_error = (unsigned long)errno;
			close(fd);
			bfree(buffer);
			return PL_FILE_ERROR;
		}
		if (chunk == 0)
			break;
		done += (size_t)chunk;
	}
	close(fd);
	*data = buffer;
	*got = done;
	return PL_FILE_OK;
}
