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
 * blocks the calling worker, which alone owns the fetch, and returns early when the stop is set or the overall
 * deadline passes. pl_fetch_close returns only when no callback of the backend can reach the plugin any more.
 *
 * Two backends, one behaviour (fetch-winhttp.c on Windows, fetch-curl.c with libcurl on Linux and macOS):
 * - http and https only, for the address and for every redirect, at most max_redirects of them. A redirect from
 *   https to http is refused: a list asked for over a secure connection is never taken from an open one.
 * - A user name and password in the address (https://user:password@host/list.m3u) are sent to that server as HTTP
 *   Basic authentication, with the first request. After a redirect they count only for the origin they were given
 *   for (the same scheme, host and port): libcurl sends them on there, WinHTTP when that address asks for them
 *   (a 401). They are never sent to another host, nor from http to https or back.
 * - The proxy the environment names is used: http_proxy for an http address, https_proxy for an https one, all_proxy
 *   for both, except for hosts no_proxy names. That is what a list behind a proxy needs to be fetched at all, and
 *   it is where the player of its items looks too: OBS's media source plays through FFmpeg, whose http and tls
 *   protocols read http_proxy and no_proxy from the environment (libavformat/http.c and tls.c; FFmpeg's source is
 *   not part of this repository's dependencies, so this is from FFmpeg itself, not checked against OBS's build).
 *   With no proxy named, WinHTTP uses the system's proxy settings, as it did before, and libcurl none. On Windows
 *   a proxy value that is not a plain http proxy (another scheme, or a user name in it) is passed over.
 * - Certificates are checked, always. */

/* Each fetch ends in exactly one of these (R-REM-5) */
enum pl_fetch_result {
	PL_FETCH_OK,
	PL_FETCH_CANCELLED,          /* the stop event */
	PL_FETCH_TIMEOUT,            /* a timeout of the backend or the overall deadline */
	PL_FETCH_TLS_ERROR,          /* a certificate or secure channel error */
	PL_FETCH_TOO_MANY_REDIRECTS, /* more than max_redirects */
	PL_FETCH_HTTP_STATUS,        /* a status outside 200-299; pl_fetch_info has it */
	PL_FETCH_TRUNCATED,          /* the connection failed during the body, or it ended short of Content-Length */
	PL_FETCH_TOO_LARGE,          /* the decompressed body went over the cap */
	PL_FETCH_NETWORK_ERROR, /* anything else, including a URL the backend cannot read; pl_fetch_info has the code */
};

#ifdef _WIN32
#define PL_FETCH_LIBRARY "WinHTTP"
#else
#define PL_FETCH_LIBRARY "libcurl"
#endif

/* A network error sorted for the row status; each backend sorts its own codes */
enum pl_fetch_failure {
	PL_FETCH_FAILURE_OTHER,
	PL_FETCH_FAILURE_NAME_NOT_FOUND,
	PL_FETCH_FAILURE_CANNOT_CONNECT,
	PL_FETCH_FAILURE_BAD_URL,
	PL_FETCH_FAILURE_ACCESS_DENIED,
	PL_FETCH_FAILURE_REDIRECT_REFUSED, /* a redirect from https to http, or to something that is not http(s) */
};

struct pl_stop;

struct pl_fetch_options {
	const char *user_agent; /* "obs-playlister/<version>" */
	struct pl_stop *stop;   /* the provider sets it to stop (os.h); NULL for none */
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
	char *charset;      /* the Content-Type's charset parameter, lower-cased; NULL when absent (R-REM-1) */
	bool has_length;
	uint64_t content_length;
	bool encoded;    /* a Content-Encoding header came with the response */
	char *final_url; /* the URL after redirects (the base for relative entries); NULL when no response came */
	uint32_t redirects;
	uint32_t error; /* the PL_FETCH_LIBRARY or operating-system error code behind a failure; 0 when none */
	enum pl_fetch_failure failure; /* that code sorted; OTHER when none */
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

/* Cancels whatever is pending and frees the fetch; NULL is nothing to close. On Windows it waits until WinHTTP has
 * closed every handle. With libcurl it does not wait for a name lookup in flight (CURLOPT_QUICK_EXIT, libcurl 7.87
 * and later): the lookup's thread ends by itself, inside libcurl. With an older libcurl it waits for the system's
 * resolver to give up, which the plugin cannot shorten. */
void pl_fetch_close(struct pl_fetch *fetch);

/* How many callbacks of the backend have run in this process (for the tests: none may run after pl_fetch_close) */
int64_t pl_fetch_callback_count(void);
