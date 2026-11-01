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

/* One HTTP(S) GET with libcurl (fetch.h, R-LNX-6), the Linux twin of fetch-winhttp.c. The worker drives one easy
 * handle through a multi handle itself: curl_multi_poll waits on the transfer and the provider's stop descriptor
 * together, so a stop never waits for a timeout, and every libcurl callback runs inside these calls, so none can run
 * after pl_fetch_close. The headers phase pauses the transfer at the first body bytes; reading resumes it. */

#include "fetch.h"

#include <ctype.h>
#include <pthread.h>
#include <string.h>

#include <curl/curl.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "os.h"

#define POLL_SLICE_MS 1000

static volatile long callback_count;
static pthread_once_t global_once = PTHREAD_ONCE_INIT;
static CURLcode global_result = CURLE_FAILED_INIT;

enum body_mode {
	BODY_PAUSE,  /* the headers phase: the first body bytes pause the transfer */
	BODY_STORE,  /* reading: up to limit bytes */
	BODY_PREFIX, /* reading a prefix: limit bytes, then stop without error */
};

struct pl_fetch {
	CURL *easy;
	CURLM *multi;
	struct pl_stop *stop;
	uint64_t deadline_ns;
	struct pl_fetch_info info;
	bool header_encoded;  /* the response whose headers are coming has a Content-Encoding */
	bool header_location; /* ... and a Location, so libcurl follows it */
	long header_status;
	bool headers_done; /* the final response's headers are all in */
	bool paused;
	bool done;
	CURLcode done_result;
	bool opened; /* the open succeeded, so a read may follow */
	bool read_called;
	enum body_mode mode;
	size_t limit;
	char *body;
	size_t size;
	bool too_large;
	bool prefix_full;
};

static void global_init(void)
{
	global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

void pl_fetch_default_options(struct pl_fetch_options *options)
{
	memset(options, 0, sizeof(*options));
	options->user_agent = "obs-playlister";
	options->deadline_ms = 60000;
	options->connect_ms = 10000;
	options->receive_ms = 30000;
	options->max_redirects = 5;
}

int64_t pl_fetch_callback_count(void)
{
	return (int64_t)os_atomic_load_long(&callback_count);
}

/* ---- callbacks ---- */

static bool header_is(const char *line, size_t length, const char *name)
{
	size_t name_length = strlen(name);
	return length > name_length && astrcmpi_n(line, name, name_length) == 0 && line[name_length] == ':';
}

static size_t on_header(char *line, size_t size, size_t count, void *data)
{
	struct pl_fetch *fetch = data;
	os_atomic_inc_long(&callback_count);
	size_t length = size * count;
	/* every response, the redirects' too, starts with its status line */
	if (length >= 5 && strncmp(line, "HTTP/", 5) == 0) {
		fetch->header_encoded = false;
		fetch->header_location = false;
		/* "HTTP/1.1 200 OK": the digits after the first space (the line has no terminating NUL) */
		fetch->header_status = 0;
		size_t i = 5;
		while (i < length && line[i] != ' ')
			i++;
		for (i++; i < length && line[i] >= '0' && line[i] <= '9'; i++)
			fetch->header_status = fetch->header_status * 10 + (line[i] - '0');
	} else if (header_is(line, length, "Content-Encoding")) {
		fetch->header_encoded = true;
	} else if (header_is(line, length, "Location")) {
		fetch->header_location = true;
	} else if ((length == 2 && line[0] == '\r' && line[1] == '\n') || (length == 1 && line[0] == '\n')) {
		/* the end of a response's headers: the final one unless it is informational or a redirect libcurl
		 * follows */
		bool informational = fetch->header_status >= 100 && fetch->header_status < 200;
		bool followed = fetch->header_status >= 300 && fetch->header_status < 400 && fetch->header_location;
		if (!informational && !followed)
			fetch->headers_done = true;
	}
	return length;
}

static void append(struct pl_fetch *fetch, const char *bytes, size_t count)
{
	fetch->body = brealloc(fetch->body, fetch->size + count + 1);
	/* memcpy: a body may hold NUL bytes */
	memcpy(fetch->body + fetch->size, bytes, count);
	fetch->size += count;
	fetch->body[fetch->size] = '\0';
}

static size_t on_body(char *bytes, size_t size, size_t count, void *data)
{
	struct pl_fetch *fetch = data;
	os_atomic_inc_long(&callback_count);
	size_t length = size * count;
	switch (fetch->mode) {
	case BODY_PAUSE:
		/* libcurl keeps these bytes and gives them again once the transfer is resumed */
		fetch->paused = true;
		return CURL_WRITEFUNC_PAUSE;
	case BODY_PREFIX: {
		size_t room = fetch->limit - fetch->size;
		append(fetch, bytes, length < room ? length : room);
		if (fetch->size < fetch->limit)
			return length;
		/* enough: a short return ends the transfer, which is no error here */
		fetch->prefix_full = true;
		return 0;
	}
	case BODY_STORE:
	default:
		/* the cap counts decompressed bytes (R-REM-5) */
		if (fetch->size + length > fetch->limit) {
			fetch->too_large = true;
			return 0;
		}
		append(fetch, bytes, length);
		return length;
	}
}

/* ---- results ---- */

static bool tls_error(CURLcode code)
{
	switch (code) {
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
	case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
	case CURLE_SSL_INVALIDCERTSTATUS:
	case CURLE_SSL_ENGINE_NOTFOUND:
	case CURLE_SSL_ENGINE_SETFAILED:
	case CURLE_SSL_ENGINE_INITFAILED:
	case CURLE_SSL_CRL_BADFILE:
	case CURLE_SSL_SHUTDOWN_FAILED:
		return true;
	default:
		return false;
	}
}

static enum pl_fetch_failure failure_kind(CURLcode code)
{
	switch (code) {
	case CURLE_COULDNT_RESOLVE_HOST:
		return PL_FETCH_FAILURE_NAME_NOT_FOUND;
	case CURLE_COULDNT_CONNECT:
		return PL_FETCH_FAILURE_CANNOT_CONNECT;
	case CURLE_URL_MALFORMAT:
	case CURLE_UNSUPPORTED_PROTOCOL:
		return PL_FETCH_FAILURE_BAD_URL;
	case CURLE_REMOTE_ACCESS_DENIED:
		return PL_FETCH_FAILURE_ACCESS_DENIED;
	default:
		return PL_FETCH_FAILURE_OTHER;
	}
}

/* What a libcurl failure means; in_body when it came while reading the body */
static enum pl_fetch_result failure(struct pl_fetch *fetch, CURLcode code, bool in_body)
{
	fetch->info.error = (uint32_t)code;
	fetch->info.failure = failure_kind(code);
	if (code == CURLE_OPERATION_TIMEDOUT)
		return PL_FETCH_TIMEOUT;
	if (tls_error(code))
		return PL_FETCH_TLS_ERROR;
	if (code == CURLE_TOO_MANY_REDIRECTS)
		return PL_FETCH_TOO_MANY_REDIRECTS;
	return in_body ? PL_FETCH_TRUNCATED : PL_FETCH_NETWORK_ERROR;
}

/* ---- driving the transfer ---- */

/* Runs the transfer until it is paused (headers phase), done, stopped or past the deadline */
static enum pl_fetch_result run(struct pl_fetch *fetch, bool in_body)
{
	for (;;) {
		if (fetch->stop && pl_stop_is_set(fetch->stop))
			return PL_FETCH_CANCELLED;
		int running = 0;
		CURLMcode multi_result = curl_multi_perform(fetch->multi, &running);
		if (multi_result != CURLM_OK)
			return failure(fetch, CURLE_FAILED_INIT, in_body);
		CURLMsg *message;
		int left;
		while ((message = curl_multi_info_read(fetch->multi, &left))) {
			if (message->msg == CURLMSG_DONE) {
				fetch->done = true;
				fetch->done_result = message->data.result;
			}
		}
		if (fetch->done)
			return PL_FETCH_OK;
		/* the headers phase ends at the final headers, or at the first body bytes when both came at once */
		if (fetch->mode == BODY_PAUSE && (fetch->paused || fetch->headers_done))
			return PL_FETCH_OK;
		uint64_t now = os_gettime_ns();
		if (now >= fetch->deadline_ns)
			return failure(fetch, CURLE_OPERATION_TIMEDOUT, in_body);
		uint64_t left_ms = (fetch->deadline_ns - now) / 1000000;
		int timeout = left_ms < POLL_SLICE_MS ? (int)left_ms + 1 : POLL_SLICE_MS;
		struct curl_waitfd extra = {.fd = fetch->stop ? pl_stop_fd(fetch->stop) : -1,
					    .events = CURL_WAIT_POLLIN};
		curl_multi_poll(fetch->multi, fetch->stop ? &extra : NULL, fetch->stop ? 1 : 0, timeout, NULL);
	}
}

static char *normalized_type(const char *type)
{
	if (!type)
		return NULL;
	size_t length = strcspn(type, ";");
	while (length && isspace((unsigned char)type[length - 1]))
		length--;
	while (length && isspace((unsigned char)*type)) {
		type++;
		length--;
	}
	if (!length)
		return NULL;
	char *result = bstrdup_n(type, length);
	for (char *c = result; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	return result;
}

/* The headers of the final response, as WinHTTP reports them: a body libcurl decompresses has no Content-Encoding
 * and no Content-Length left, since the length counted the compressed bytes */
static void take_headers(struct pl_fetch *fetch)
{
	long status = 0;
	curl_easy_getinfo(fetch->easy, CURLINFO_RESPONSE_CODE, &status);
	fetch->info.status = (uint32_t)status;
	if (!status)
		return;
	char *type = NULL;
	curl_easy_getinfo(fetch->easy, CURLINFO_CONTENT_TYPE, &type);
	fetch->info.content_type = normalized_type(type);
	curl_off_t length = -1;
	curl_easy_getinfo(fetch->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
	fetch->info.encoded = false;
	fetch->info.has_length = length >= 0 && !fetch->header_encoded;
	fetch->info.content_length = fetch->info.has_length ? (uint64_t)length : 0;
	char *final_url = NULL;
	curl_easy_getinfo(fetch->easy, CURLINFO_EFFECTIVE_URL, &final_url);
	fetch->info.final_url = final_url ? bstrdup(final_url) : NULL;
	long redirects = 0;
	curl_easy_getinfo(fetch->easy, CURLINFO_REDIRECT_COUNT, &redirects);
	fetch->info.redirects = (uint32_t)redirects;
}

/* ---- fetch.h ---- */

struct pl_fetch *pl_fetch_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result)
{
	struct pl_fetch *fetch = bzalloc(sizeof(*fetch));
	fetch->stop = options->stop;
	fetch->deadline_ns = os_gettime_ns() + (uint64_t)options->deadline_ms * 1000000;
	fetch->mode = BODY_PAUSE;

	pthread_once(&global_once, global_init);
	if (global_result != CURLE_OK) {
		*result = failure(fetch, global_result, false);
		return fetch;
	}
	/* libcurl would guess "http" for a URL without a scheme; WinHTTP refuses one, and so does this */
	if (!url || !strstr(url, "://")) {
		*result = failure(fetch, CURLE_URL_MALFORMAT, false);
		return fetch;
	}
	fetch->easy = curl_easy_init();
	fetch->multi = curl_multi_init();
	if (!fetch->easy || !fetch->multi) {
		*result = failure(fetch, CURLE_OUT_OF_MEMORY, false);
		return fetch;
	}

	CURL *easy = fetch->easy;
	curl_easy_setopt(easy, CURLOPT_URL, url);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, options->user_agent);
	/* plain http and https only, for the first request and every redirect (R-REM-1) */
	curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)options->max_redirects);
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)options->connect_ms);
	/* the receive timeout: no byte for that long ends the transfer */
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	long receive_s = options->receive_ms / 1000 ? (long)(options->receive_ms / 1000) : 1;
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, receive_s);
	/* every encoding libcurl can decode is accepted and decoded */
	curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
	/* a proxy's answer to CONNECT is not the server's response */
	curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, fetch);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, fetch);
	if (curl_multi_add_handle(fetch->multi, easy) != CURLM_OK) {
		*result = failure(fetch, CURLE_FAILED_INIT, false);
		return fetch;
	}

	enum pl_fetch_result phase = run(fetch, false);
	if (phase != PL_FETCH_OK) {
		*result = phase;
		return fetch;
	}
	if (fetch->done && fetch->done_result != CURLE_OK) {
		*result = failure(fetch, fetch->done_result, false);
		take_headers(fetch);
		return fetch;
	}
	take_headers(fetch);
	if (fetch->info.status < 200 || fetch->info.status > 299) {
		*result = PL_FETCH_HTTP_STATUS;
		return fetch;
	}
	fetch->opened = true;
	*result = PL_FETCH_OK;
	return fetch;
}

const struct pl_fetch_info *pl_fetch_get_info(const struct pl_fetch *fetch)
{
	return &fetch->info;
}

enum pl_fetch_result pl_fetch_read(struct pl_fetch *fetch, size_t limit, bool prefix, char **body, size_t *size)
{
	*body = NULL;
	*size = 0;
	if (!fetch->opened || fetch->read_called)
		return PL_FETCH_NETWORK_ERROR;
	fetch->read_called = true;
	fetch->mode = prefix ? BODY_PREFIX : BODY_STORE;
	fetch->limit = limit;
	/* resuming hands over the bytes held at the pause at once, so the cap or the prefix may be reached here */
	CURLcode resumed = CURLE_OK;
	if (fetch->paused) {
		fetch->paused = false;
		resumed = curl_easy_pause(fetch->easy, CURLPAUSE_CONT);
	}

	enum pl_fetch_result result = PL_FETCH_OK;
	if (!fetch->too_large && !fetch->prefix_full) {
		if (resumed != CURLE_OK)
			result = failure(fetch, resumed, true);
		else if (!fetch->done)
			result = run(fetch, true);
	}
	/* the flags decide whatever libcurl made of the short return that ends the transfer */
	if (fetch->too_large)
		result = PL_FETCH_TOO_LARGE;
	else if (fetch->prefix_full)
		result = PL_FETCH_OK;
	else if (result == PL_FETCH_OK && fetch->done_result != CURLE_OK)
		result = failure(fetch, fetch->done_result, true);
	if (result != PL_FETCH_OK) {
		bfree(fetch->body);
		fetch->body = NULL;
		fetch->size = 0;
		return result;
	}
	if (!fetch->body)
		fetch->body = bzalloc(1);
	*body = fetch->body;
	*size = fetch->size;
	fetch->body = NULL;
	fetch->size = 0;
	return PL_FETCH_OK;
}

void pl_fetch_close(struct pl_fetch *fetch)
{
	if (!fetch)
		return;
	/* synchronous: once this returns no libcurl callback can run for this fetch */
	if (fetch->multi && fetch->easy)
		curl_multi_remove_handle(fetch->multi, fetch->easy);
	if (fetch->easy)
		curl_easy_cleanup(fetch->easy);
	if (fetch->multi)
		curl_multi_cleanup(fetch->multi);
	bfree(fetch->body);
	bfree(fetch->info.content_type);
	bfree(fetch->info.final_url);
	bfree(fetch);
}
