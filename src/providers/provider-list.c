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

/* The list provider: one worker per playlist or remote row (R-THR-2). It reads a local playlist file again when it
 * changes (D-12 b), fetches a remote one on the refresh interval (R-REM-4), classifies an http(s) row by its first
 * response (R-LIST-1 steps 3 and 4), keeps the last good list after a failure and says so (R-BEH-3), and publishes
 * through the same filter, sort, mode and lock as folders (R-REM-7). Other threads only leave requests and wake it. */

#include "provider-kinds.h"

#include <windows.h>
#include <winhttp.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
/* after libobs's headers, as its blogva declaration repeats theirs */
#include <plugin-support.h>

#include "core/filter.h"
#include "core/parse-m3u.h"
#include "core/parse-pls.h"
#include "core/url.h"
#include "fold-key-win.h"
#include "publication.h"

#define WORKER_STACK_SIZE (256 * 1024) /* R-THR-2 */
#define LIST_CAP (16 * 1024 * 1024)    /* R-REM-5: list bodies only */
#define SNIFF_BYTES (64 * 1024)        /* R-LIST-1 step 4 */
#define FILETIME_UNIX_EPOCH 116444736000000000LL

static const struct pl_list_fetch_api winhttp_api = {pl_fetch_open, pl_fetch_get_info, pl_fetch_read, pl_fetch_close};
static const struct pl_list_fetch_api *fetch_api = &winhttp_api;

void pl_list_provider_use_fetch_api(const struct pl_list_fetch_api *api)
{
	fetch_api = api ? api : &winhttp_api;
}

enum list_format {
	FORMAT_M3U,
	FORMAT_PLS,
};

/* What the row turned out to be */
enum list_shape {
	SHAPE_UNKNOWN, /* nothing read yet, or every attempt failed before the row was classified */
	SHAPE_LIST,    /* a playlist: filtered, sorted, moded and read again (R-REM-4, D-12 b) */
	SHAPE_DIRECT,  /* one media URL the child plays; never downloaded and never fetched again (R-LIST-1) */
	SHAPE_STREAM,  /* one HLS stream the child plays; never fetched again (R-REM-1) */
};

struct pl_list_provider {
	/* fixed at create */
	char *location;
	bool remote;    /* an http(s) URL; else a local playlist file */
	bool classify;  /* a remote row: the first response decides what it is (R-LIST-1 steps 3 and 4) */
	char agent[64]; /* the User-Agent, "obs-playlister/<version>" (R-REM-5) */
	HANDLE thread;
	HANDLE stop_event; /* manual reset */
	HANDLE wake_event; /* auto reset: a request is waiting */

	/* requests, under request_mutex */
	pthread_mutex_t request_mutex;
	struct pl_container_options requested; /* extension_filter points at requested_filter */
	char *requested_filter;
	bool options_changed;
	bool refresh_requested;
	bool release_requested;

	/* worker only */
	struct pl_container_options options; /* extension_filter points at filter_text */
	char *filter_text;
	struct pl_filter filter;
	struct pl_publication publication;
	enum list_shape shape;
	enum list_format format;      /* by the extension, or as a remote row was classified */
	struct pl_list_result parsed; /* the last good list, as parsed */
	bool attempted;               /* a read or fetch has finished, well or not */
	bool has_list;                /* parsed holds a good list */
	char *error;                  /* why the last attempt failed; NULL when it did not */
	char *logged_error;           /* the last error logged, so a repeat is not */
	int64_t last_ok_unix_ms;
	int64_t last_attempt_ms; /* monotonic */
	bool fresh;              /* a list was taken since the last refresh logged it */
	bool empty_seen;         /* the last read found the local file empty and kept the list */
	bool have_stamp;         /* a local file's size and time when it was last read well */
	uint64_t stamp_size;
	int64_t stamp_write;
};

/* ---- time and text ---- */

static int64_t mono_now(void)
{
	return (int64_t)(os_gettime_ns() / 1000000);
}

static int64_t utc_now_ms(void)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER value = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
	return ((int64_t)value.QuadPart - FILETIME_UNIX_EPOCH) / 10000;
}

static bool stopping(struct pl_list_provider *p)
{
	return WaitForSingleObject(p->stop_event, 0) == WAIT_OBJECT_0;
}

static bool has_extension(const char *location, const char *extension)
{
	char *found = pl_url_path_extension(location);
	bool same = found && strcmp(found, extension) == 0;
	bfree(found);
	return same;
}

/* R-KEY-1: a local path folded as folder keys are, a URL as it is */
static char *entry_key(const char *location, void *context)
{
	UNUSED_PARAMETER(context);
	return pl_url_scheme_length(location) ? bstrdup(location) : pl_fold_key(location);
}

static void set_error(struct pl_list_provider *p, struct dstr *message)
{
	bfree(p->error);
	p->error = message->array;
	message->array = NULL;
	message->len = message->capacity = 0;
}

static void set_error_text(struct pl_list_provider *p, const char *text)
{
	bfree(p->error);
	p->error = bstrdup(text);
}

/* ---- the list itself ---- */

/* Takes a new good list: the old one goes, and the row is what the list says it is */
static void take_list(struct pl_list_provider *p, struct pl_list_result *result)
{
	pl_list_result_free(&p->parsed);
	p->parsed = *result;
	memset(result, 0, sizeof(*result));
	p->has_list = true;
	p->fresh = true;
	enum list_shape shape = p->parsed.stream ? SHAPE_STREAM : SHAPE_LIST;
	if (shape == SHAPE_STREAM && p->shape != SHAPE_STREAM)
		obs_log(LOG_INFO, "list %s: an HLS stream; played as one entry and not fetched again", p->location);
	p->shape = shape;
	bfree(p->error);
	p->error = NULL;
	p->last_ok_unix_ms = utc_now_ms();
}

static void parse(struct pl_list_result *result, enum list_format format, const char *data, size_t size,
		  const char *base)
{
	struct pl_list_source source = {base, entry_key, NULL};
	if (format == FORMAT_PLS)
		pl_parse_pls(result, data, size, &source);
	else
		pl_parse_m3u(result, data, size, &source);
}

static enum list_format format_of(const char *location)
{
	return has_extension(location, "pls") ? FORMAT_PLS : FORMAT_M3U;
}

/* ---- a local playlist file (D-12 b) ---- */

static void read_local(struct pl_list_provider *p)
{
	wchar_t *path = NULL;
	if (!os_utf8_to_wcs_ptr(p->location, 0, &path) || !path) {
		set_error_text(p, "the playlist path cannot be read as text");
		return;
	}
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
		DWORD error = GetLastError();
		bfree(path);
		struct dstr message = {0};
		if (error == ERROR_ACCESS_DENIED)
			dstr_copy(&message, "access denied (permissions or security software)");
		else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			dstr_copy(&message, "playlist file not found");
		else
			dstr_printf(&message, "cannot read the playlist file (Win32 error %lu)", (unsigned long)error);
		set_error(p, &message);
		return;
	}
	uint64_t size = ((uint64_t)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
	int64_t write = ((int64_t)attributes.ftLastWriteTime.dwHighDateTime << 32) |
			attributes.ftLastWriteTime.dwLowDateTime;
	/* unchanged since it was last read well: nothing to do, and a past error is over */
	if (p->have_stamp && p->has_list && size == p->stamp_size && write == p->stamp_write &&
	    (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		bfree(p->error);
		p->error = NULL;
		bfree(path);
		return;
	}
	if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		bfree(path);
		set_error_text(p, "not a playlist file (a folder)");
		return;
	}
	if (size > LIST_CAP) {
		bfree(path);
		set_error_text(p, "playlist file is over 16 MB");
		return;
	}

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bfree(path);
	if (file == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		struct dstr message = {0};
		if (error == ERROR_ACCESS_DENIED)
			dstr_copy(&message, "access denied (permissions or security software)");
		else if (error == ERROR_SHARING_VIOLATION)
			dstr_copy(&message, "the playlist file is in use; trying again");
		else
			dstr_printf(&message, "cannot read the playlist file (Win32 error %lu)", (unsigned long)error);
		set_error(p, &message);
		return;
	}
	char *data = bmalloc((size_t)size + 1);
	size_t got = 0;
	bool ok = true;
	while (got < size) {
		DWORD chunk = 0;
		DWORD want = (DWORD)(size - got > (1 << 20) ? (1 << 20) : size - got);
		if (!ReadFile(file, data + got, want, &chunk, NULL)) {
			ok = false;
			break;
		}
		if (!chunk)
			break;
		got += chunk;
	}
	DWORD error = GetLastError();
	CloseHandle(file);
	if (!ok) {
		bfree(data);
		struct dstr message = {0};
		dstr_printf(&message, "cannot read the playlist file (Win32 error %lu)", (unsigned long)error);
		set_error(p, &message);
		return;
	}

	/* an editor that empties the file and then writes it may be caught in between: an empty file replaces a good
	 * list only when the next read finds it empty too */
	if (!got && p->has_list && !p->empty_seen) {
		bfree(data);
		p->empty_seen = true;
		return;
	}
	p->empty_seen = false;

	struct pl_list_result result;
	parse(&result, format_of(p->location), data, got, p->location);
	bfree(data);
	take_list(p, &result);
	/* a file that changed size during the read is read again next time */
	p->have_stamp = got == size;
	p->stamp_size = size;
	p->stamp_write = write;
}

/* ---- a remote row ---- */

static void fetch_error(struct pl_list_provider *p, enum pl_fetch_result result, const struct pl_fetch_info *info)
{
	struct dstr message = {0};
	switch (result) {
	case PL_FETCH_TIMEOUT:
		dstr_copy(&message, "timed out");
		break;
	case PL_FETCH_TLS_ERROR:
		dstr_printf(&message, "secure connection failed (certificate or TLS error %u)", info->error);
		break;
	case PL_FETCH_TOO_MANY_REDIRECTS:
		dstr_copy(&message, "too many redirects");
		break;
	case PL_FETCH_HTTP_STATUS:
		dstr_printf(&message, "the server answered HTTP %u", info->status);
		break;
	case PL_FETCH_TRUNCATED:
		dstr_copy(&message, "the download was cut short");
		break;
	case PL_FETCH_TOO_LARGE:
		dstr_copy(&message, "the list is over 16 MB");
		break;
	case PL_FETCH_NETWORK_ERROR:
		if (info->error == ERROR_WINHTTP_NAME_NOT_RESOLVED)
			dstr_copy(&message, "the server name could not be found");
		else if (info->error == ERROR_WINHTTP_CANNOT_CONNECT)
			dstr_copy(&message, "the server could not be reached");
		else if (info->error == ERROR_WINHTTP_INVALID_URL || info->error == ERROR_WINHTTP_UNRECOGNIZED_SCHEME)
			dstr_copy(&message, "not a valid http(s) address");
		else if (info->error == ERROR_ACCESS_DENIED)
			dstr_copy(&message, "access denied (permissions or security software)");
		else
			dstr_printf(&message, "network error (WinHTTP error %u)", info->error);
		break;
	default:
		dstr_copy(&message, "network error");
		break;
	}
	set_error(p, &message);
}

/* What a response is, from its Content-Type (R-LIST-1 step 3) */
enum response_kind {
	RESPONSE_UNKNOWN,
	RESPONSE_M3U,
	RESPONSE_PLS,
	RESPONSE_FEED,
	RESPONSE_MEDIA,
	RESPONSE_INDEX,
};

static enum response_kind kind_of_type(const char *type)
{
	if (!type)
		return RESPONSE_UNKNOWN;
	if (strcmp(type, "application/vnd.apple.mpegurl") == 0 || strcmp(type, "application/x-mpegurl") == 0 ||
	    strcmp(type, "audio/x-mpegurl") == 0 || strcmp(type, "audio/mpegurl") == 0)
		return RESPONSE_M3U;
	if (strcmp(type, "audio/x-scpls") == 0)
		return RESPONSE_PLS;
	if (strcmp(type, "application/rss+xml") == 0 || strcmp(type, "application/atom+xml") == 0)
		return RESPONSE_FEED;
	if (strncmp(type, "audio/", 6) == 0 || strncmp(type, "video/", 6) == 0)
		return RESPONSE_MEDIA;
	return RESPONSE_UNKNOWN;
}

static bool starts_with_ci(const char *text, size_t length, const char *prefix)
{
	size_t prefix_length = strlen(prefix);
	return length >= prefix_length && _strnicmp(text, prefix, prefix_length) == 0;
}

static bool contains_ci(const char *text, size_t length, const char *needle)
{
	size_t needle_length = strlen(needle);
	for (size_t i = 0; i + needle_length <= length; i++) {
		if (_strnicmp(text + i, needle, needle_length) == 0)
			return true;
	}
	return false;
}

/* R-LIST-1 step 4: the first bytes, after a BOM and blank space */
static enum response_kind sniff(const char *data, size_t size)
{
	if (size >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
	    (unsigned char)data[2] == 0xBF) {
		data += 3;
		size -= 3;
	}
	while (size && (*data == ' ' || *data == '\t' || *data == '\r' || *data == '\n')) {
		data++;
		size--;
	}
	if (starts_with_ci(data, size, "#EXTM3U"))
		return RESPONSE_M3U;
	if (starts_with_ci(data, size, "[playlist]"))
		return RESPONSE_PLS;
	if (contains_ci(data, size, "<rss") || contains_ci(data, size, "<feed"))
		return RESPONSE_FEED;
	if (contains_ci(data, size, "<html"))
		return RESPONSE_INDEX;
	return RESPONSE_UNKNOWN;
}

static void fetch_options(struct pl_list_provider *p, struct pl_fetch_options *options)
{
	pl_fetch_default_options(options);
	options->user_agent = p->agent;
	options->stop_event = p->stop_event;
}

/* A parsed list taken as the row's new good list. A stream is played from the row's own URL, which a signed redirect
 * may not outlive. */
static void take_parsed(struct pl_list_provider *p, struct pl_list_result *list)
{
	if (list->stream) {
		struct pl_entry *stream = &list->entries[0];
		bfree(stream->location);
		bfree(stream->key);
		stream->location = bstrdup(p->location);
		stream->key = entry_key(p->location, NULL);
	}
	take_list(p, list);
}

static bool is_web_page(const char *type)
{
	return type && (strcmp(type, "text/html") == 0 || strcmp(type, "application/xhtml+xml") == 0);
}

/* The whole body of url, parsed as format against the final URL */
static void fetch_list(struct pl_list_provider *p, const char *url, enum list_format format)
{
	struct pl_fetch_options options;
	fetch_options(p, &options);
	enum pl_fetch_result result;
	struct pl_fetch *fetch = fetch_api->open(url, &options, &result);
	const struct pl_fetch_info *info = fetch_api->get_info(fetch);
	char *body = NULL;
	size_t size = 0;
	/* an error page sent with 200 is not read as a list of relative links */
	bool web_page = result == PL_FETCH_OK && is_web_page(info->content_type);
	/* nor is a media response read, which may be an endless stream */
	bool media = result == PL_FETCH_OK && kind_of_type(info->content_type) == RESPONSE_MEDIA;
	if (result == PL_FETCH_OK && !web_page && !media)
		result = fetch_api->read(fetch, LIST_CAP, false, &body, &size);

	if (media) {
		set_error_text(p, "the address now answers with media, not a list");
	} else if (web_page) {
		set_error_text(p, "the server sent a web page instead of the list");
	} else if (result != PL_FETCH_OK) {
		if (result != PL_FETCH_CANCELLED)
			fetch_error(p, result, info);
	} else {
		struct pl_list_result list;
		parse(&list, format, body ? body : "", size, info->final_url ? info->final_url : url);
		take_parsed(p, &list);
	}
	bfree(body);
	fetch_api->close(fetch);
}

/* The row's URL is one media file: the child plays it, the plugin never downloads it (R-LIST-1) */
static void take_direct(struct pl_list_provider *p)
{
	struct pl_list_result result = {0};
	result.entries = bzalloc(sizeof(struct pl_entry));
	result.count = 1;
	struct pl_entry *entry = &result.entries[0];
	entry->location = bstrdup(p->location);
	entry->key = entry_key(p->location, NULL);
	entry->occurrence = 1;
	char *extension = pl_url_path_extension(p->location);
	if (extension) {
		struct dstr name = {0};
		dstr_printf(&name, "x.%s", extension);
		entry->kind = pl_filter_kind(name.array);
		dstr_free(&name);
	}
	bfree(extension);
	take_list(p, &result);
	if (p->shape != SHAPE_DIRECT)
		obs_log(LOG_INFO, "list %s: a media URL; played directly and never downloaded", p->location);
	p->shape = SHAPE_DIRECT;
}

/* R-LIST-1 steps 3 and 4 for a row whose URL did not say what it is */
static void classify_remote(struct pl_list_provider *p)
{
	struct pl_fetch_options options;
	fetch_options(p, &options);
	enum pl_fetch_result result;
	struct pl_fetch *fetch = fetch_api->open(p->location, &options, &result);
	const struct pl_fetch_info *info = fetch_api->get_info(fetch);
	enum response_kind kind = result == PL_FETCH_OK ? kind_of_type(info->content_type) : RESPONSE_UNKNOWN;

	char *body = NULL;
	size_t size = 0;
	bool complete = false;
	if (result == PL_FETCH_OK && (kind == RESPONSE_M3U || kind == RESPONSE_PLS)) {
		result = fetch_api->read(fetch, LIST_CAP, false, &body, &size);
		complete = true;
	} else if (result == PL_FETCH_OK && kind == RESPONSE_UNKNOWN) {
		result = fetch_api->read(fetch, SNIFF_BYTES, true, &body, &size);
		/* a body shorter than the sniff is all there is */
		complete = size < SNIFF_BYTES;
		if (result == PL_FETCH_OK)
			kind = sniff(body ? body : "", size);
	}
	/* a media response is closed below with its body unread */

	if (result != PL_FETCH_OK) {
		if (result != PL_FETCH_CANCELLED)
			fetch_error(p, result, info);
	} else if (kind == RESPONSE_MEDIA) {
		take_direct(p);
	} else if (kind == RESPONSE_M3U || kind == RESPONSE_PLS) {
		p->format = kind == RESPONSE_PLS ? FORMAT_PLS : FORMAT_M3U;
		if (complete) {
			struct pl_list_result list;
			parse(&list, p->format, body ? body : "", size,
			      info->final_url ? info->final_url : p->location);
			take_parsed(p, &list);
		} else {
			/* the sniff read only the first part: the whole list is fetched once more */
			char *final_url = bstrdup(info->final_url ? info->final_url : p->location);
			fetch_api->close(fetch);
			fetch = NULL;
			fetch_list(p, final_url, p->format);
			bfree(final_url);
		}
	} else if (kind == RESPONSE_FEED) {
		set_error_text(p, "a podcast feed: feeds come in a later version");
	} else if (kind == RESPONSE_INDEX) {
		set_error_text(p, "a web page: folder listings on web servers come in a later version");
	} else {
		struct dstr message = {0};
		dstr_printf(&message, "not a playlist or media (content type %s)",
			    info->content_type ? info->content_type : "none");
		set_error(p, &message);
	}
	bfree(body);
	if (fetch)
		fetch_api->close(fetch);
}

/* ---- one refresh ---- */

static void refresh(struct pl_list_provider *p)
{
	if (!p->remote)
		read_local(p);
	else if (p->classify && p->shape == SHAPE_UNKNOWN)
		classify_remote(p);
	else if (p->classify && p->shape == SHAPE_DIRECT)
		; /* a media URL is never fetched again; Refresh changes nothing */
	else
		fetch_list(p, p->location, p->format);
	if (stopping(p))
		return;

	p->attempted = true;
	p->last_attempt_ms = mono_now();
	if (p->error && (!p->logged_error || strcmp(p->error, p->logged_error) != 0)) {
		obs_log(LOG_WARNING, "list %s: %s%s", p->location, p->error,
			p->has_list ? "; keeping the last good list" : "");
		bfree(p->logged_error);
		p->logged_error = bstrdup(p->error);
	} else if (!p->error && p->logged_error) {
		obs_log(LOG_INFO, "list %s: read again without error", p->location);
		bfree(p->logged_error);
		p->logged_error = NULL;
	}

	/* R-DIR-10: once for each list read, what date order and the newest modes mean without dates */
	bool fresh = p->fresh;
	p->fresh = false;
	bool dated = false;
	for (size_t i = 0; i < p->parsed.count && !dated; i++)
		dated = p->parsed.entries[i].has_date;
	if (fresh && p->shape == SHAPE_LIST && p->parsed.count && !dated) {
		if (p->options.mode != PL_MODE_FULL)
			obs_log(LOG_INFO, "list %s: no dates; the newest entries are the last in the list",
				p->location);
		else if (p->options.sort_key == PL_SORT_DATE)
			obs_log(LOG_INFO, "list %s: no dates; sorted by date it plays in list order", p->location);
	}
}

/* Whether the row is read again on its own, and how often (R-REM-4, D-12 b) */
static int64_t interval_ms(struct pl_list_provider *p)
{
	/* a local file, stream or not, is checked as folders are; a read that failed or was cut short is tried again on
	 * that interval even with the check off, so the row is not left on an error or a half-written file */
	if (!p->remote) {
		bool retry = p->error || p->empty_seen || (p->has_list && !p->have_stamp);
		return p->options.folder_check_enabled || retry ? (int64_t)p->options.folder_check_interval_s * 1000
								: 0;
	}
	if (p->shape == SHAPE_DIRECT || p->shape == SHAPE_STREAM)
		return 0;
	return (int64_t)(p->options.remote_refresh_s ? p->options.remote_refresh_s : 300) * 1000;
}

/* ---- publication ---- */

/* D-12 a: an entry whose path has an extension goes by the filter; one without, and a stream, is kept */
static bool admitted(const struct pl_list_provider *p, const struct pl_entry *entry)
{
	if (entry->kind == PL_ENTRY_STREAM)
		return true;
	char *extension = pl_url_path_extension(entry->location);
	if (!extension)
		return true;
	struct dstr name = {0};
	dstr_printf(&name, "x.%s", extension);
	bool matches = pl_filter_matches(&p->filter, name.array);
	dstr_free(&name);
	bfree(extension);
	return matches;
}

static void fill_status(struct pl_list_provider *p, const struct pl_entry *entries, size_t count,
			struct pl_publication_status *status, struct dstr *message)
{
	enum pl_status_level level = PL_STATUS_OK;
	if (p->error) {
		level = PL_STATUS_ERROR;
		dstr_copy(message, p->error);
		status->stale = p->has_list;
	}
	if (p->shape == SHAPE_LIST && p->has_list && (p->parsed.unusable_lines || p->parsed.nested_playlists)) {
		if (level == PL_STATUS_OK)
			level = PL_STATUS_WARNING;
		if (message->len)
			dstr_cat(message, "; ");
		if (p->parsed.unusable_lines)
			dstr_catf(message, "%zu lines could not be used", p->parsed.unusable_lines);
		if (p->parsed.unusable_lines && p->parsed.nested_playlists)
			dstr_cat(message, "; ");
		if (p->parsed.nested_playlists)
			dstr_catf(message, "%zu playlists inside the list skipped", p->parsed.nested_playlists);
	}
	/* R-DIR-10: with no dates, date order is list order, and "newest" means last in the list */
	bool dated = false;
	for (size_t i = 0; i < count && !dated; i++)
		dated = entries[i].has_date;
	if (p->shape == SHAPE_LIST && count && !dated &&
	    (p->options.sort_key == PL_SORT_DATE || p->options.mode != PL_MODE_FULL)) {
		bool newest = p->options.mode != PL_MODE_FULL;
		if (newest && level == PL_STATUS_OK)
			level = PL_STATUS_WARNING;
		if (message->len)
			dstr_cat(message, "; ");
		dstr_cat(message, newest ? "no dates: newest means last in the list" : "no dates; list order");
	}
	status->level = level;
	status->message = message->array;
	status->last_ok_unix_ms = p->last_ok_unix_ms;
}

static void update_publication(struct pl_list_provider *p, bool release)
{
	/* nothing to say before the first read or fetch has finished */
	if (!p->attempted)
		return;
	const struct pl_entry *parsed = p->has_list ? p->parsed.entries : NULL;
	size_t parsed_count = p->has_list ? p->parsed.count : 0;
	bool single = p->shape == SHAPE_DIRECT || p->shape == SHAPE_STREAM;
	struct pl_entry *raw = bzalloc((parsed_count ? parsed_count : 1) * sizeof(*raw));
	size_t count = 0;
	for (size_t i = 0; i < parsed_count; i++) {
		if (single || admitted(p, &parsed[i]))
			pl_entry_copy(&raw[count++], &parsed[i]);
	}

	struct dstr message = {0};
	struct pl_publication_status status = {0};
	fill_status(p, raw, count, &status, &message);
	status.listed = true;
	/* a list that failed before it was ever read is not the list taken at start */
	status.lock_point = p->has_list;
	pl_publication_update(&p->publication, &p->options, raw, count, &status, release);
	dstr_free(&message);
}

/* ---- requests ---- */

static void copy_options(struct pl_container_options *dst, char **filter_text, const struct pl_container_options *src)
{
	*dst = *src;
	bfree(*filter_text);
	*filter_text = bstrdup(src->extension_filter ? src->extension_filter : "");
	dst->extension_filter = *filter_text;
}

static void take_requests(struct pl_list_provider *p, bool *release, bool *refresh_now)
{
	struct pl_container_options options = {0};
	char *filter_text = NULL;
	pthread_mutex_lock(&p->request_mutex);
	bool options_changed = p->options_changed;
	*refresh_now = p->refresh_requested;
	*release = p->release_requested;
	if (options_changed)
		copy_options(&options, &filter_text, &p->requested);
	p->options_changed = false;
	p->refresh_requested = false;
	p->release_requested = false;
	pthread_mutex_unlock(&p->request_mutex);

	if (options_changed) {
		bool filter_changed = strcmp(options.extension_filter, p->filter_text) != 0;
		copy_options(&p->options, &p->filter_text, &options);
		if (filter_changed) {
			pl_filter_free(&p->filter);
			pl_filter_parse(&p->filter, p->filter_text);
		}
	}
	bfree(filter_text);
}

/* ---- the worker ---- */

static DWORD WINAPI worker_main(void *data)
{
	struct pl_list_provider *p = data;
	os_set_thread_name("obs-playlister: list");
	bool due = true; /* the first read or fetch, at once */

	while (!stopping(p)) {
		bool release = false;
		bool refresh_now = false;
		take_requests(p, &release, &refresh_now);
		int64_t interval = interval_ms(p);
		if (interval && p->attempted && mono_now() >= p->last_attempt_ms + interval)
			due = true;
		/* requests that came during a fetch are one refresh now (R-REM-4) */
		if (due || refresh_now) {
			due = false;
			refresh(p);
		}
		if (stopping(p))
			break;
		update_publication(p, release);

		interval = interval_ms(p);
		DWORD timeout = INFINITE;
		if (interval) {
			int64_t wait_ms = p->last_attempt_ms + interval - mono_now();
			timeout = wait_ms <= 0 ? 0 : wait_ms > 3600000 ? 3600000 : (DWORD)wait_ms;
		}
		HANDLE handles[2] = {p->stop_event, p->wake_event};
		if (WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0)
			break;
	}
	return 0;
}

/* ---- the interface ---- */

static void free_provider(struct pl_list_provider *p)
{
	if (p->thread)
		CloseHandle(p->thread);
	if (p->stop_event)
		CloseHandle(p->stop_event);
	if (p->wake_event)
		CloseHandle(p->wake_event);
	pthread_mutex_destroy(&p->request_mutex);
	bfree(p->requested_filter);
	bfree(p->filter_text);
	pl_filter_free(&p->filter);
	pl_publication_free(&p->publication);
	pl_list_result_free(&p->parsed);
	bfree(p->error);
	bfree(p->logged_error);
	bfree(p->location);
	bfree(p);
}

struct pl_list_provider *pl_list_provider_create(enum pl_row_kind kind, const char *location, uint64_t instance_id,
						 const struct pl_container_options *options, pl_publish_fn publish,
						 void *owner)
{
	if ((kind != PL_ROW_PLAYLIST && kind != PL_ROW_REMOTE) || !location || !options || !publish)
		return NULL;
	struct pl_list_provider *p = bzalloc(sizeof(*p));
	p->location = bstrdup(location);
	p->remote = pl_url_is_http(location);
	p->classify = kind == PL_ROW_REMOTE;
	p->format = format_of(location);
	snprintf(p->agent, sizeof(p->agent), "obs-playlister/%s", PLUGIN_VERSION);
	pl_publication_init(&p->publication, instance_id, publish, owner);
	pthread_mutex_init_value(&p->request_mutex);
	copy_options(&p->requested, &p->requested_filter, options);
	copy_options(&p->options, &p->filter_text, options);
	pl_filter_parse(&p->filter, p->filter_text);

	p->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	p->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (pthread_mutex_init(&p->request_mutex, NULL) != 0 || !p->stop_event || !p->wake_event) {
		free_provider(p);
		return NULL;
	}
	p->thread = CreateThread(NULL, WORKER_STACK_SIZE, worker_main, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (!p->thread) {
		obs_log(LOG_ERROR, "list %s: the worker thread could not be started (Win32 error %lu)", p->location,
			(unsigned long)GetLastError());
		free_provider(p);
		return NULL;
	}
	return p;
}

void pl_list_provider_refresh_now(struct pl_list_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->refresh_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

void pl_list_provider_set_options(struct pl_list_provider *provider, const struct pl_container_options *options)
{
	pthread_mutex_lock(&provider->request_mutex);
	copy_options(&provider->requested, &provider->requested_filter, options);
	provider->options_changed = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

void pl_list_provider_release_held(struct pl_list_provider *provider)
{
	pthread_mutex_lock(&provider->request_mutex);
	provider->release_requested = true;
	pthread_mutex_unlock(&provider->request_mutex);
	SetEvent(provider->wake_event);
}

size_t pl_list_provider_held_changes(struct pl_list_provider *provider)
{
	return pl_publication_held_changes(&provider->publication);
}

void pl_list_provider_destroy(struct pl_list_provider *provider)
{
	if (!provider)
		return;
	/* a fetch in progress returns at once on the stop event (R-REM-5); a local read stuck on a stalled share is
	 * interrupted, again and again, as the worker may not have entered the call yet when the first cancel comes
	 * (R-THR-3) */
	SetEvent(provider->stop_event);
	while (WaitForSingleObject(provider->thread, 100) == WAIT_TIMEOUT)
		CancelSynchronousIo(provider->thread);
	free_provider(provider);
}
