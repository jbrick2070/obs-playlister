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

/* One HTTP(S) GET (R-REM-5), in two phases: pl_fetch_open waits for the headers; then the caller closes at once (a
 * direct media URL is never downloaded), reads a bounded prefix (to sniff) or reads the body up to a cap. Every call
 * blocks the calling worker, which alone owns the fetch, and returns early when the stop event is set or the overall
 * deadline passes. pl_fetch_close returns only after WinHTTP has closed every handle, so no WinHTTP callback reaches
 * the plugin afterwards. */

/* Each fetch ends in exactly one of these (R-REM-5) */
enum pl_fetch_result {
	PL_FETCH_OK,
	PL_FETCH_CANCELLED,          /* the stop event */
	PL_FETCH_TIMEOUT,            /* a WinHTTP timeout or the overall deadline */
	PL_FETCH_TLS_ERROR,          /* a certificate or secure channel error */
	PL_FETCH_TOO_MANY_REDIRECTS, /* more than max_redirects */
	PL_FETCH_HTTP_STATUS,        /* a status outside 200-299; pl_fetch_info has it */
	PL_FETCH_TRUNCATED,          /* the connection failed during the body, or it ended short of Content-Length */
	PL_FETCH_TOO_LARGE,          /* the decompressed body went over the cap */
	PL_FETCH_NETWORK_ERROR, /* anything else, including a URL WinHTTP cannot read; pl_fetch_info has the code */
};

struct pl_fetch_options {
	const char *user_agent; /* "obs-playlister/<version>" */
	void *stop_event;       /* a Win32 event handle the provider sets to stop; NULL for none */
	uint32_t deadline_ms;   /* across redirects and body reads; 60000 */
	uint32_t connect_ms;    /* resolve, connect and send each; 10000 */
	uint32_t receive_ms;    /* 30000 */
	uint32_t max_redirects; /* 5 */
};

/* The R-REM-5 values */
void pl_fetch_default_options(struct pl_fetch_options *options);

struct pl_fetch_info {
	uint32_t status;    /* the final HTTP status, 0 when no response came */
	char *content_type; /* lower-cased, parameters removed; NULL when absent */
	bool has_length;
	uint64_t content_length;
	bool encoded;    /* a Content-Encoding header came with the response */
	char *final_url; /* the URL after redirects (the base for relative entries); NULL when no response came */
	uint32_t redirects;
	uint32_t error; /* the Win32 or WinHTTP error code behind a failure; 0 when none */
};

struct pl_fetch;

/* Starts the GET and waits for the response headers. Never NULL: a fetch that failed still has to be closed, and its
 * info says how far it got. A non-2xx status is PL_FETCH_HTTP_STATUS. */
struct pl_fetch *pl_fetch_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result);
const struct pl_fetch_info *pl_fetch_get_info(const struct pl_fetch *fetch);

/* Reads the body after a successful open. With prefix set it stops after limit bytes and that is no error; without,
 * a body over limit bytes (counted after decompression) is PL_FETCH_TOO_LARGE. *body is a bmem buffer of *size bytes
 * plus a terminating NUL, or NULL when nothing was read; the caller frees it. Call it at most once. */
enum pl_fetch_result pl_fetch_read(struct pl_fetch *fetch, size_t limit, bool prefix, char **body, size_t *size);

/* Cancels whatever is pending, closes every handle and waits until WinHTTP has closed them all, then frees the
 * fetch. NULL is nothing to close. */
void pl_fetch_close(struct pl_fetch *fetch);

/* How many WinHTTP status callbacks have run in this process (for the tests: none may run after pl_fetch_close) */
int64_t pl_fetch_callback_count(void);
