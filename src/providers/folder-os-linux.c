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

/* The folder provider's Linux calls (folder-os.h, R-LNX-3, R-LNX-4): listing with opendir and fstatat, the
 * read-lease probe, and an inotify watch with one watch per listed folder. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* F_SETLEASE */
#endif

#include "folder-os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#include "os.h"
#include "provider.h"

#define EVENT_BUFFER_BYTES (64 * 1024)
#define DIR_EVENTS (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB)
#define ROOT_EVENTS (DIR_EVENTS | IN_DELETE_SELF | IN_MOVE_SELF)

/* file systems whose changes made on another machine inotify never sees (R-LNX-3) */
#define MAGIC_NFS 0x6969
#define MAGIC_SMB 0x517B
#define MAGIC_CIFS 0xFF534D42
#define MAGIC_SMB2 0xFE534D42
#define MAGIC_FUSE 0x65735546

struct watched_folder {
	int wd;
	char *relative; /* "" for the root */
};

struct pl_folder_os {
	char *root; /* the row's path without trailing separators ("/" stays) */
	bool include_subfolders;
	int notify_fd; /* -1 when the watch is closed */
	bool armed;    /* every listed folder has its watch */
	int root_wd;
	DARRAY(struct watched_folder) folders;
	int last_error;
	const char *last_note; /* said instead of the error code when set */
	bool lease_logged;
	uint32_t moved_cookie; /* an IN_MOVED_FROM whose IN_MOVED_TO has not come yet */
	char *moved_from;
	enum pl_folder_note held_note; /* found by take_completed_changes, given by the next take_note */
	char *events;
};

/* ---- paths and time ---- */

static int64_t mono_now(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

static int64_t mtime_ms(const struct stat *status)
{
	return (int64_t)status->st_mtim.tv_sec * 1000 + status->st_mtim.tv_nsec / 1000000;
}

static char *join_path(const char *parent, const char *name)
{
	struct dstr path = {0};
	dstr_copy(&path, parent);
	if (*name) {
		if (path.len && path.array[path.len - 1] != '/')
			dstr_cat_ch(&path, '/');
		dstr_cat(&path, name);
	}
	return path.array;
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

static bool hidden_name(const char *name)
{
	return name[0] == '.';
}

/* ---- create and destroy ---- */

struct pl_folder_os *pl_folder_os_create(const char *location, bool include_subfolders)
{
	struct pl_folder_os *os = bzalloc(sizeof(*os));
	os->root = bstrdup(location ? location : "");
	size_t length = strlen(os->root);
	while (length > 1 && os->root[length - 1] == '/')
		os->root[--length] = '\0';
	os->include_subfolders = include_subfolders;
	os->notify_fd = -1;
	os->root_wd = -1;
	os->held_note = PL_FOLDER_NOTE_NONE;
	da_init(os->folders);
	return os;
}

static void forget_folders(struct pl_folder_os *os)
{
	for (size_t i = 0; i < os->folders.num; i++)
		bfree(os->folders.array[i].relative);
	da_free(os->folders);
	os->root_wd = -1;
}

void pl_folder_os_destroy(struct pl_folder_os *os)
{
	if (!os)
		return;
	pl_folder_os_close(os);
	bfree(os->events);
	bfree(os->moved_from);
	bfree(os->root);
	bfree(os);
}

void pl_folder_os_error_cat(const struct pl_folder_os *os, struct dstr *text)
{
	if (os->last_note) {
		dstr_cat(text, os->last_note);
		return;
	}
	pl_os_error_cat(text, (unsigned long)os->last_error);
	if (os->last_error == ENOSPC)
		dstr_cat(text, "; the inotify watch limit, fs.inotify.max_user_watches, is reached");
}

static void set_error(struct pl_folder_os *os, int error)
{
	os->last_error = error;
	os->last_note = NULL;
}

/* ---- the gate's disk access (R-LNX-4) ---- */

enum pl_probe_result pl_folder_os_probe(void *context, const char *location)
{
	struct pl_folder_os *os = context;
	int fd = open(location, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		switch (errno) {
		case ENOENT:
		case ENOTDIR:
			return PL_PROBE_GONE;
		case EACCES:
		case EPERM:
			return PL_PROBE_DENIED;
		default:
			return PL_PROBE_BUSY;
		}
	}
	/* a read lease is refused with EAGAIN while any process has the file open for writing. A writer that opens the
	 * file while the lease is held makes the kernel signal this process, SIGIO by default, which would end it: the
	 * lease signals SIGURG instead, which a process ignores unless it asks for it */
	fcntl(fd, F_SETSIG, SIGURG);
	int result = fcntl(fd, F_SETLEASE, F_RDLCK);
	int error = result == 0 ? 0 : errno;
	if (result == 0)
		fcntl(fd, F_SETLEASE, F_UNLCK);
	close(fd);
	if (result == 0)
		return PL_PROBE_OK;
	if (error == EAGAIN)
		return PL_PROBE_BUSY;
	/* no lease here (another user's file, a file system without leases): the stability rule alone admits it */
	if (os && !os->lease_logged) {
		os->lease_logged = true;
		struct dstr why = {0};
		pl_os_error_cat(&why, (unsigned long)error);
		obs_log(LOG_INFO,
			"folder %s: the open-for-writing check is not available here (%s); files are admitted once they "
			"stop changing",
			os->root, why.array);
		dstr_free(&why);
	}
	return PL_PROBE_OK;
}

bool pl_folder_os_sample(void *context, const char *location, uint64_t *size, int64_t *write_ms,
			 int64_t *sampled_mono_ms)
{
	UNUSED_PARAMETER(context);
	struct stat status;
	int result = stat(location, &status);
	*sampled_mono_ms = mono_now();
	if (result != 0 || !S_ISREG(status.st_mode))
		return false;
	*size = (uint64_t)status.st_size;
	*write_ms = mtime_ms(&status);
	return true;
}

/* ---- listing ---- */

/* Below the root, hidden folders (a name starting with '.') are skipped and symbolic links to folders are never
 * followed, so a link back up the tree cannot make the scan loop (R-DIR-1, R-LNX-3); the root itself may be a link. */
enum pl_folder_scan pl_folder_os_list(struct pl_folder_os *os, struct pl_stop *stop,
				      const struct pl_folder_listing_sink *sink)
{
	DARRAY(char *) folders; /* relative folders still to list; "" is the root */
	da_init(folders);
	char *root = bstrdup("");
	da_push_back(folders, &root);
	enum pl_folder_scan result = *os->root ? PL_FOLDER_SCAN_OK : PL_FOLDER_SCAN_LOST;
	if (!*os->root)
		set_error(os, ENOENT);

	while (result == PL_FOLDER_SCAN_OK && folders.num) {
		char *relative = folders.array[folders.num - 1];
		da_pop_back(folders);
		bool is_root = !*relative;
		if (pl_stop_is_set(stop)) {
			bfree(relative);
			result = PL_FOLDER_SCAN_STOPPED;
			break;
		}

		char *path = join_path(os->root, relative);
		DIR *dir = opendir(path);
		bfree(path);
		if (!dir) {
			int error = errno;
			if (is_root) {
				set_error(os, error);
				result = error == EACCES || error == EPERM ? PL_FOLDER_SCAN_DENIED
									   : PL_FOLDER_SCAN_LOST;
			} else if (error == EACCES || error == EPERM) {
				sink->skipped(sink->context, relative, "access denied");
			} else if (error != ENOENT && error != ENOTDIR) {
				/* a subfolder that vanished is left out; any other failure fails the whole listing, so the
				 * last good list stays instead of shrinking */
				set_error(os, error);
				result = PL_FOLDER_SCAN_LOST;
			}
			bfree(relative);
			continue;
		}

		DARRAY(char *) subfolders;
		da_init(subfolders);
		int dir_fd = dirfd(dir);
		for (;;) {
			errno = 0;
			struct dirent *item = readdir(dir);
			if (!item) {
				if (errno) {
					set_error(os, errno);
					result = PL_FOLDER_SCAN_LOST;
				}
				break;
			}
			const char *name = item->d_name;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
				continue;
			bool hidden = hidden_name(name);
			struct stat status;
			if (fstatat(dir_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0)
				continue; /* gone since the listing began */
			char *child = join_relative(relative, name);
			if (S_ISLNK(status.st_mode)) {
				struct stat target;
				if (fstatat(dir_fd, name, &target, 0) == 0) {
					if (S_ISDIR(target.st_mode)) {
						if (os->include_subfolders && !hidden)
							sink->skipped(sink->context, child, "a symbolic link");
					} else if (S_ISREG(target.st_mode) && !hidden) {
						sink->file(sink->context, child, name, (uint64_t)target.st_size,
							   mtime_ms(&target), false);
					}
				}
			} else if (S_ISDIR(status.st_mode)) {
				if (os->include_subfolders && !hidden) {
					da_push_back(subfolders, &child);
					child = NULL;
				}
			} else if (S_ISREG(status.st_mode) && !hidden) {
				sink->file(sink->context, child, name, (uint64_t)status.st_size, mtime_ms(&status),
					   false);
			}
			bfree(child);
		}
		closedir(dir);
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
	if (os->notify_fd >= 0)
		close(os->notify_fd);
	os->notify_fd = -1;
	os->armed = false;
	forget_folders(os);
	bfree(os->moved_from);
	os->moved_from = NULL;
	os->held_note = PL_FOLDER_NOTE_NONE;
}

static const char *relative_of(const struct pl_folder_os *os, int wd)
{
	for (size_t i = 0; i < os->folders.num; i++) {
		if (os->folders.array[i].wd == wd)
			return os->folders.array[i].relative;
	}
	return NULL;
}

/* Watches one folder and, with subfolders on, every folder below it that a listing would enter */
static bool watch_tree(struct pl_folder_os *os, const char *relative)
{
	char *path = join_path(os->root, relative);
	uint32_t mask = (*relative ? DIR_EVENTS : ROOT_EVENTS) | IN_ONLYDIR | IN_EXCL_UNLINK;
	int wd = inotify_add_watch(os->notify_fd, path, mask);
	if (wd < 0) {
		int error = errno;
		bfree(path);
		/* a subfolder that vanished or denies access is left out, as the listing leaves it out */
		if (*relative && (error == ENOENT || error == ENOTDIR || error == EACCES || error == EPERM))
			return true;
		set_error(os, error);
		return false;
	}
	if (!*relative)
		os->root_wd = wd;
	/* a folder already watched under another path (a bind mount of a folder above it) is not entered again */
	if (relative_of(os, wd)) {
		bfree(path);
		return true;
	}
	struct watched_folder folder = {wd, bstrdup(relative)};
	da_push_back(os->folders, &folder);
	bool ok = true;
	if (os->include_subfolders) {
		DIR *dir = opendir(path);
		if (dir) {
			int dir_fd = dirfd(dir);
			struct dirent *item;
			while (ok && (item = readdir(dir))) {
				const char *name = item->d_name;
				struct stat status;
				if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || hidden_name(name) ||
				    fstatat(dir_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
				    !S_ISDIR(status.st_mode))
					continue;
				char *child = join_relative(relative, name);
				ok = watch_tree(os, child);
				bfree(child);
			}
			closedir(dir);
		}
	}
	bfree(path);
	return ok;
}

static bool network_file_system(const char *path)
{
	struct statfs info;
	if (statfs(path, &info) != 0)
		return false;
	switch ((unsigned long)info.f_type) {
	case MAGIC_NFS:
	case MAGIC_SMB:
	case MAGIC_CIFS:
	case MAGIC_SMB2:
	case MAGIC_FUSE:
		return true;
	default:
		return false;
	}
}

bool pl_folder_os_arm(struct pl_folder_os *os)
{
	if (os->notify_fd < 0)
		return false;
	if (os->armed)
		return true;
	/* every watch is made again, so folders renamed or added since are watched under their current names */
	close(os->notify_fd);
	forget_folders(os);
	os->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (os->notify_fd < 0) {
		set_error(os, errno);
		return false;
	}
	if (network_file_system(os->root)) {
		/* inotify sees only this machine's changes there: the folder is listed on the interval instead */
		os->last_error = 0;
		os->last_note = "a network file system, where changes made on other machines are not reported";
		return false;
	}
	if (!watch_tree(os, ""))
		return false;
	os->armed = true;
	return true;
}

enum pl_watch_event pl_folder_os_open(struct pl_folder_os *os)
{
	pl_folder_os_close(os);
	struct stat status;
	if (!*os->root || stat(os->root, &status) != 0) {
		int error = *os->root ? errno : ENOENT;
		set_error(os, error);
		return error == EACCES || error == EPERM ? PL_WATCH_DENIED : PL_WATCH_OPEN_FAILED;
	}
	if (!S_ISDIR(status.st_mode)) {
		/* a file of that name: not a folder */
		set_error(os, ENOTDIR);
		return PL_WATCH_OPEN_FAILED;
	}
	DIR *probe = opendir(os->root);
	if (!probe) {
		int error = errno;
		set_error(os, error);
		return error == EACCES || error == EPERM ? PL_WATCH_DENIED : PL_WATCH_OPEN_FAILED;
	}
	closedir(probe);
	os->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (os->notify_fd < 0) {
		set_error(os, errno);
		return PL_WATCH_OPEN_FAILED;
	}
	return pl_folder_os_arm(os) ? PL_WATCH_OPENED_ARMED : PL_WATCH_OPENED_ARM_FAILED;
}

enum pl_folder_wait pl_folder_os_wait(struct pl_folder_os *os, struct pl_stop *stop, struct pl_wake *wake,
				      uint32_t timeout_ms)
{
	if (os->held_note != PL_FOLDER_NOTE_NONE)
		return pl_stop_is_set(stop) ? PL_FOLDER_WAIT_STOP : PL_FOLDER_WAIT_NOTIFIED;
	struct pollfd fds[3] = {
		{.fd = pl_stop_fd(stop), .events = POLLIN},
		{.fd = pl_wake_fd(wake), .events = POLLIN},
		{.fd = os->armed ? os->notify_fd : -1, .events = POLLIN},
	};
	int64_t deadline = timeout_ms == PL_WAIT_FOREVER ? 0 : mono_now() + timeout_ms;
	for (;;) {
		int wait_ms = -1;
		if (timeout_ms != PL_WAIT_FOREVER) {
			int64_t left = deadline - mono_now();
			wait_ms = left <= 0 ? 0 : left > INT32_MAX ? INT32_MAX : (int)left;
		}
		int ready = poll(fds, 3, wait_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready > 0 && (fds[0].revents & POLLIN))
			return PL_FOLDER_WAIT_STOP;
		if (ready > 0 && (fds[1].revents & POLLIN)) {
			uint64_t value;
			while (read(fds[1].fd, &value, sizeof(value)) < 0 && errno == EINTR)
				;
			return PL_FOLDER_WAIT_WAKE;
		}
		if (ready > 0 && (fds[2].revents & POLLIN))
			return PL_FOLDER_WAIT_NOTIFIED;
		if (ready == 0 && timeout_ms != PL_WAIT_FOREVER && mono_now() < deadline)
			continue;
		return PL_FOLDER_WAIT_TIMEOUT;
	}
}

/* Reads every event waiting. Paired moves of files within the watched folders go to the callback; a folder created,
 * moved or removed below the root makes the next arm watch the tree again. */
static enum pl_folder_note read_events(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	if (!os->events)
		os->events = bmalloc(EVENT_BUFFER_BYTES);
	enum pl_folder_note note = PL_FOLDER_NOTE_NONE;
	for (;;) {
		ssize_t length = read(os->notify_fd, os->events, EVENT_BUFFER_BYTES);
		if (length < 0 && errno == EINTR)
			continue;
		if (length <= 0)
			break;
		if (note == PL_FOLDER_NOTE_NONE)
			note = PL_FOLDER_NOTE_CHANGES;
		for (char *cursor = os->events; cursor < os->events + length;) {
			struct inotify_event *event = (struct inotify_event *)cursor;
			cursor += sizeof(*event) + event->len;
			if (event->mask & IN_Q_OVERFLOW) {
				set_error(os, EOVERFLOW);
				note = PL_FOLDER_NOTE_OVERFLOW;
				/* events were dropped, maybe a new subfolder's: the next arm watches the tree again, and until
				 * then the watch is not polled (folder-os.h) */
				os->armed = false;
				continue;
			}
			if (event->wd == os->root_wd &&
			    (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED))) {
				set_error(os, ENOENT);
				if (note != PL_FOLDER_NOTE_OVERFLOW)
					note = PL_FOLDER_NOTE_LOST;
				continue;
			}
			if (event->mask & IN_IGNORED) {
				/* a subfolder's watch ended with it */
				for (size_t i = 0; i < os->folders.num; i++) {
					if (os->folders.array[i].wd == event->wd) {
						bfree(os->folders.array[i].relative);
						da_erase(os->folders, i);
						break;
					}
				}
				continue;
			}
			const char *parent = relative_of(os, event->wd);
			if (!parent || !event->len)
				continue;
			if (event->mask & IN_ISDIR) {
				if (os->include_subfolders && (event->mask & (IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO)))
					os->armed = false;
				continue;
			}
			if (event->mask & IN_MOVED_FROM) {
				bfree(os->moved_from);
				os->moved_from = join_relative(parent, event->name);
				os->moved_cookie = event->cookie;
			} else if ((event->mask & IN_MOVED_TO) && os->moved_from && event->cookie == os->moved_cookie) {
				char *moved_to = join_relative(parent, event->name);
				rename(context, os->moved_from, moved_to);
				bfree(moved_to);
				bfree(os->moved_from);
				os->moved_from = NULL;
			}
		}
	}
	return note;
}

bool pl_folder_os_take_completed_changes(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	if (os->notify_fd < 0 || os->held_note != PL_FOLDER_NOTE_NONE)
		return false;
	enum pl_folder_note note = read_events(os, rename, context);
	if (note == PL_FOLDER_NOTE_CHANGES)
		return true;
	/* an overflow or a lost folder waits for the main loop, as on Windows */
	os->held_note = note;
	return false;
}

enum pl_folder_note pl_folder_os_take_note(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	if (os->held_note != PL_FOLDER_NOTE_NONE) {
		enum pl_folder_note held = os->held_note;
		os->held_note = PL_FOLDER_NOTE_NONE;
		return held;
	}
	if (os->notify_fd < 0)
		return PL_FOLDER_NOTE_NONE;
	return read_events(os, rename, context);
}

/* ---- provider.h ---- */

bool pl_folder_is_directory(const char *path)
{
	struct stat status;
	return path && *path && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}
