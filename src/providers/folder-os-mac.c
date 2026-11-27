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

/* The folder provider's macOS calls (folder-os.h, R-MAC-2, R-MAC-3): listing with opendir and fstatat, a shared-lock
 * probe, and an FSEvents stream with file-level events. macOS has no inotify, and kqueue reports only that a folder
 * changed, so the watch is FSEvents: one stream covers the root and everything below it, reports each file's
 * renames with its inode so a rename keeps the item (R-DIR-8), and runs on its own dispatch queue, which hands the
 * events to the worker through a pipe the worker polls with the stop and wake signals. */

#include "folder-os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#include "os.h"
#include "provider.h"

#ifndef SF_DATALESS
#define SF_DATALESS 0x40000000 /* an iCloud Drive file whose contents are not on this Mac (macOS 10.15+) */
#endif

/* seconds FSEvents gathers events before it hands them over; the provider debounces on top of this */
#define STREAM_LATENCY_S 0.05

struct fs_event {
	char *path; /* as FSEvents reports it: absolute, UTF-8 */
	uint64_t inode;
	bool has_inode;
	FSEventStreamEventFlags flags;
};

struct pl_folder_os {
	char *root;           /* the row's path without trailing separators ("/" stays) */
	char *canonical_root; /* realpath of the root when opened: FSEvents reports /private/var for /var */
	bool include_subfolders;
	bool open;  /* the watch is open: the pipe exists */
	bool armed; /* the stream runs and the worker polls it */
	int note_fds[2];
	FSEventStreamRef stream;
	dispatch_queue_t queue;
	pthread_mutex_t mutex; /* guards queued, which the stream's queue fills */
	DARRAY(struct fs_event) queued;
	int last_error;
	const char *last_note; /* said instead of the error code when set */
	bool probe_logged;
	char *rename_from; /* the last renamed file seen, waiting for the event that names where it went */
	uint64_t rename_inode;
	bool rename_has_inode;
	enum pl_folder_note held_note; /* found by take_completed_changes, given by the next take_note */
};

/* ---- paths and time ---- */

static int64_t mono_now(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

static int64_t mtime_ms(const struct stat *status)
{
	return (int64_t)status->st_mtimespec.tv_sec * 1000 + status->st_mtimespec.tv_nsec / 1000000;
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

/* A name starting with '.', or a file Finder hides (chflags hidden) */
static bool hidden_entry(const char *name, const struct stat *status)
{
	return name[0] == '.' || (status && (status->st_flags & UF_HIDDEN));
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
	os->note_fds[0] = -1;
	os->note_fds[1] = -1;
	os->held_note = PL_FOLDER_NOTE_NONE;
	pthread_mutex_init(&os->mutex, NULL);
	da_init(os->queued);
	return os;
}

static void free_events(struct fs_event *events, size_t count)
{
	for (size_t i = 0; i < count; i++)
		bfree(events[i].path);
}

void pl_folder_os_destroy(struct pl_folder_os *os)
{
	if (!os)
		return;
	pl_folder_os_close(os);
	pthread_mutex_destroy(&os->mutex);
	da_free(os->queued);
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
	/* macOS privacy protection refuses folders such as Desktop, Documents, Downloads, removable and network volumes
	 * to an app the user has not allowed (R-MAC-5) */
	if (os->last_error == EPERM || os->last_error == EACCES)
		dstr_cat(text,
			 "; check System Settings > Privacy & Security > Files and Folders (or Full Disk Access) for "
			 "OBS");
}

static void set_error(struct pl_folder_os *os, int error)
{
	os->last_error = error;
	os->last_note = NULL;
}

char *pl_folder_os_location(const struct pl_folder_os *os, const char *row, const char *relative)
{
	UNUSED_PARAMETER(os);
	struct dstr location = {0};
	dstr_printf(&location, "%s/%s", row, relative);
	return location.array;
}

/* ---- the gate's disk access (R-MAC-3) ---- */

enum pl_probe_result pl_folder_os_probe(void *context, const char *location)
{
	struct pl_folder_os *os = context;
	/* macOS has no read lease. A shared lock is refused while another program holds the file with an exclusive
	 * lock (and, on an SMB volume, while it is open with a share mode that denies writers); otherwise the stability
	 * rule alone admits it */
	int fd = open(location, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_SHLOCK);
	if (fd < 0 && errno == ENOTSUP)
		fd = open(location, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
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
	close(fd);
	if (os && !os->probe_logged) {
		os->probe_logged = true;
		obs_log(LOG_INFO,
			"folder %s: macOS cannot tell whether another program still has a file open for writing; files "
			"are admitted once they stop changing",
			os->root);
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

/* Below the root, hidden folders are skipped and symbolic links to folders are never followed, so a link back up the
 * tree cannot make the scan loop (R-DIR-1); the root itself may be a link. An iCloud Drive file not downloaded to
 * this Mac is reported as a cloud placeholder and never opened (R-DIR-5). */
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
			struct stat status;
			if (fstatat(dir_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0)
				continue; /* gone since the listing began */
			bool hidden = hidden_entry(name, &status);
			char *child = join_relative(relative, name);
			if (S_ISLNK(status.st_mode)) {
				struct stat target;
				if (fstatat(dir_fd, name, &target, 0) == 0) {
					if (S_ISDIR(target.st_mode)) {
						if (os->include_subfolders && !hidden)
							sink->skipped(sink->context, child, "a symbolic link");
					} else if (S_ISREG(target.st_mode) && !hidden) {
						sink->file(sink->context, child, name, (uint64_t)target.st_size,
							   mtime_ms(&target), (target.st_flags & SF_DATALESS) != 0);
					}
				}
			} else if (S_ISDIR(status.st_mode)) {
				if (os->include_subfolders && !hidden) {
					da_push_back(subfolders, &child);
					child = NULL;
				}
			} else if (S_ISREG(status.st_mode) && !hidden) {
				sink->file(sink->context, child, name, (uint64_t)status.st_size, mtime_ms(&status),
					   (status.st_flags & SF_DATALESS) != 0);
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

/* On the stream's dispatch queue: the events are queued for the worker, and the pipe says there are some */
static void stream_callback(ConstFSEventStreamRef stream, void *info, size_t count, void *paths,
			    const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[])
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(ids);
	struct pl_folder_os *os = info;
	CFArrayRef items = paths;
	pthread_mutex_lock(&os->mutex);
	for (size_t i = 0; i < count; i++) {
		struct fs_event event = {.flags = flags[i]};
		CFTypeRef item = CFArrayGetValueAtIndex(items, (CFIndex)i);
		CFStringRef path = NULL;
		if (item && CFGetTypeID(item) == CFDictionaryGetTypeID()) {
			path = CFDictionaryGetValue((CFDictionaryRef)item, kFSEventStreamEventExtendedDataPathKey);
			CFNumberRef inode =
				CFDictionaryGetValue((CFDictionaryRef)item, kFSEventStreamEventExtendedFileIDKey);
			long long value = 0;
			if (inode && CFGetTypeID(inode) == CFNumberGetTypeID() &&
			    CFNumberGetValue(inode, kCFNumberLongLongType, &value)) {
				event.inode = (uint64_t)value;
				event.has_inode = true;
			}
		} else if (item && CFGetTypeID(item) == CFStringGetTypeID()) {
			path = (CFStringRef)item;
		}
		if (path && CFGetTypeID(path) == CFStringGetTypeID()) {
			/* UTF-8 as stored, not CFStringGetFileSystemRepresentation, which would decompose it and no
			 * longer match what readdir lists */
			CFIndex size =
				CFStringGetMaximumSizeForEncoding(CFStringGetLength(path), kCFStringEncodingUTF8) + 1;
			event.path = bzalloc((size_t)size);
			if (!CFStringGetCString(path, event.path, size, kCFStringEncodingUTF8)) {
				bfree(event.path);
				event.path = NULL;
			}
		}
		da_push_back(os->queued, &event);
	}
	char one = 1;
	while (write(os->note_fds[1], &one, 1) < 0 && errno == EINTR)
		;
	pthread_mutex_unlock(&os->mutex);
}

static void nothing(void *context)
{
	UNUSED_PARAMETER(context);
}

static void stop_stream(struct pl_folder_os *os)
{
	if (os->stream) {
		FSEventStreamStop(os->stream);
		FSEventStreamInvalidate(os->stream);
		FSEventStreamRelease(os->stream);
		os->stream = NULL;
	}
	if (os->queue) {
		/* a callback already running on the queue finishes before the pipe and the queue go */
		dispatch_sync_f(os->queue, NULL, nothing);
		dispatch_release(os->queue);
		os->queue = NULL;
	}
}

void pl_folder_os_close(struct pl_folder_os *os)
{
	stop_stream(os);
	for (int i = 0; i < 2; i++) {
		if (os->note_fds[i] >= 0)
			close(os->note_fds[i]);
		os->note_fds[i] = -1;
	}
	pthread_mutex_lock(&os->mutex);
	free_events(os->queued.array, os->queued.num);
	da_resize(os->queued, 0);
	pthread_mutex_unlock(&os->mutex);
	os->open = false;
	os->armed = false;
	bfree(os->canonical_root);
	os->canonical_root = NULL;
	bfree(os->rename_from);
	os->rename_from = NULL;
	os->held_note = PL_FOLDER_NOTE_NONE;
}

/* A volume FSEvents watches only for this Mac's own changes: SMB, NFS, AFP, WebDAV and other network mounts */
static bool network_volume(const char *path)
{
	struct statfs info;
	if (statfs(path, &info) != 0)
		return false;
	return (info.f_flags & MNT_LOCAL) == 0;
}

static bool start_stream(struct pl_folder_os *os)
{
	CFStringRef root = CFStringCreateWithCString(kCFAllocatorDefault, os->root, kCFStringEncodingUTF8);
	if (!root) {
		set_error(os, EINVAL);
		return false;
	}
	CFArrayRef roots = CFArrayCreate(kCFAllocatorDefault, (const void **)&root, 1, &kCFTypeArrayCallBacks);
	CFRelease(root);
	FSEventStreamContext context = {.info = os};
	FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
					 kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagUseCFTypes |
					 kFSEventStreamCreateFlagUseExtendedData;
	os->stream = FSEventStreamCreate(kCFAllocatorDefault, stream_callback, &context, roots,
					 kFSEventStreamEventIdSinceNow, STREAM_LATENCY_S, flags);
	CFRelease(roots);
	if (!os->stream) {
		set_error(os, ENOMEM);
		return false;
	}
	os->queue = dispatch_queue_create("com.jbrick2070.obs-playlister.folder-watch", DISPATCH_QUEUE_SERIAL);
	FSEventStreamSetDispatchQueue(os->stream, os->queue);
	if (!FSEventStreamStart(os->stream)) {
		stop_stream(os);
		os->last_error = 0;
		os->last_note = "the folder's change notifications (FSEvents) could not be started";
		return false;
	}
	return true;
}

bool pl_folder_os_arm(struct pl_folder_os *os)
{
	if (!os->open)
		return false;
	if (os->armed)
		return true;
	if (network_volume(os->root)) {
		/* FSEvents sees only this Mac's changes there: the folder is listed on the interval instead */
		stop_stream(os);
		os->last_error = 0;
		os->last_note = "a network volume, where changes made on other machines are not reported";
		return false;
	}
	/* the stream covers every folder below the root by path, so folders renamed or added since need nothing new; it
	 * keeps running between arms, and what it reported meanwhile is still queued */
	if (!os->stream && !start_stream(os))
		return false;
	os->armed = true;
	return true;
}

bool pl_folder_os_replaced(struct pl_folder_os *os)
{
	/* the stream follows the path, not the folder that was there when it started */
	UNUSED_PARAMETER(os);
	return false;
}

static bool set_pipe_flags(int fd)
{
	int status = fcntl(fd, F_GETFL);
	return status >= 0 && fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
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
	if (pipe(os->note_fds) != 0) {
		set_error(os, errno);
		os->note_fds[0] = os->note_fds[1] = -1;
		return PL_WATCH_OPEN_FAILED;
	}
	if (!set_pipe_flags(os->note_fds[0]) || !set_pipe_flags(os->note_fds[1])) {
		set_error(os, errno);
		pl_folder_os_close(os);
		return PL_WATCH_OPEN_FAILED;
	}
	char canonical[PATH_MAX];
	if (realpath(os->root, canonical))
		os->canonical_root = bstrdup(canonical);
	os->open = true;
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
		{.fd = os->armed ? os->note_fds[0] : -1, .events = POLLIN},
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
			pl_wake_take(wake);
			return PL_FOLDER_WAIT_WAKE;
		}
		if (ready > 0 && (fds[2].revents & POLLIN))
			return PL_FOLDER_WAIT_NOTIFIED;
		if (ready == 0 && timeout_ms != PL_WAIT_FOREVER && mono_now() < deadline)
			continue;
		return PL_FOLDER_WAIT_TIMEOUT;
	}
}

/* The event's path relative to the root with '/', NULL when it is not below the root; "" is the root itself */
static const char *relative_of(const struct pl_folder_os *os, const char *path)
{
	const char *roots[2] = {os->canonical_root, os->root};
	for (int i = 0; i < 2; i++) {
		const char *root = roots[i];
		if (!root || !*root)
			continue;
		size_t length = strlen(root);
		if (length == 1 && root[0] == '/')
			return path[0] == '/' ? path + 1 : NULL;
		/* names on a default macOS volume compare without case */
		if (strncasecmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0'))
			return path[length] == '/' ? path + length + 1 : path + length;
	}
	return NULL;
}

static bool exists(const struct pl_folder_os *os, const char *relative)
{
	char *path = join_path(os->root, relative);
	struct stat status;
	bool found = lstat(path, &status) == 0;
	bfree(path);
	return found;
}

/* A renamed file: FSEvents reports the old name, then the new one, both with the file's inode. Paired by inode, a
 * chain of renames (A to B to C) gives each step in order. Without an inode, an old name that is gone followed by a
 * new one that exists is taken as the pair. */
static void note_rename(struct pl_folder_os *os, const struct fs_event *event, const char *relative,
			pl_folder_rename_fn rename, void *context)
{
	if (os->rename_from && strcmp(os->rename_from, relative) != 0) {
		bool pair = event->has_inode && os->rename_has_inode
				    ? event->inode == os->rename_inode
				    : !exists(os, os->rename_from) && exists(os, relative);
		if (pair)
			rename(context, os->rename_from, relative);
	} else if (os->rename_from) {
		return; /* the same name again: a coalesced event for the file that was renamed to it */
	}
	bfree(os->rename_from);
	os->rename_from = bstrdup(relative);
	os->rename_inode = event->inode;
	os->rename_has_inode = event->has_inode;
}

/* events were dropped: everything must be listed again */
#define DROPPED_EVENTS                                                                 \
	(kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | \
	 kFSEventStreamEventFlagKernelDropped)
#define ROOT_GONE (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagUnmount)

/* Takes every queued event. Renames of files below the root go to the callback; an event in a subfolder is ignored
 * when subfolders are off. */
static enum pl_folder_note read_events(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	pthread_mutex_lock(&os->mutex);
	size_t count = os->queued.num;
	struct fs_event *events = count ? bmemdup(os->queued.array, count * sizeof(*events)) : NULL;
	da_resize(os->queued, 0);
	char bytes[64];
	for (;;) {
		ssize_t got = read(os->note_fds[0], bytes, sizeof(bytes));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
	}
	pthread_mutex_unlock(&os->mutex);

	enum pl_folder_note note = PL_FOLDER_NOTE_NONE;
	for (size_t i = 0; i < count; i++) {
		const struct fs_event *event = &events[i];
		if (event->flags & DROPPED_EVENTS) {
			set_error(os, EOVERFLOW);
			note = PL_FOLDER_NOTE_OVERFLOW;
			/* as on Linux, the watch is not polled until the next arm (folder-os.h) */
			os->armed = false;
			continue;
		}
		if (event->flags & ROOT_GONE) {
			set_error(os, ENOENT);
			if (note != PL_FOLDER_NOTE_OVERFLOW)
				note = PL_FOLDER_NOTE_LOST;
			continue;
		}
		const char *relative = event->path ? relative_of(os, event->path) : "";
		if (!relative)
			continue;
		if (!os->include_subfolders && strchr(relative, '/'))
			continue;
		if (note == PL_FOLDER_NOTE_NONE)
			note = PL_FOLDER_NOTE_CHANGES;
		if (*relative && (event->flags & kFSEventStreamEventFlagItemRenamed) &&
		    (event->flags & kFSEventStreamEventFlagItemIsFile))
			note_rename(os, event, relative, rename, context);
	}
	free_events(events, count);
	bfree(events);
	return note;
}

bool pl_folder_os_take_completed_changes(struct pl_folder_os *os, pl_folder_rename_fn rename, void *context)
{
	if (!os->open || os->held_note != PL_FOLDER_NOTE_NONE)
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
	if (!os->open)
		return PL_FOLDER_NOTE_NONE;
	return read_events(os, rename, context);
}

/* ---- provider.h ---- */

bool pl_folder_is_directory(const char *path)
{
	struct stat status;
	return path && *path && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}
