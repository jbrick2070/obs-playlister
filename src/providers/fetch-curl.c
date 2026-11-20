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

/* One HTTP(S) GET with libcurl (fetch.h, R-LNX-6), the Linux and macOS twin of fetch-winhttp.c. The worker drives one
 * easy handle through a multi handle itself: curl_multi_poll waits on the transfer and the provider's stop descriptor
 * together, so a stop never waits for a timeout of the transfer, and every libcurl callback runs inside these calls,
 * so none can run after pl_fetch_close. The headers phase pauses the transfer at the first body bytes; reading
 * resumes it.
 *
 * A name lookup is the one thing libcurl does on a thread of its own (getaddrinfo cannot be interrupted). The stop
 * ends the wait for it at once, and CURLOPT_QUICK_EXIT (libcurl 7.87 and later) lets pl_fetch_close return without
 * joining that thread, which then ends by itself inside libcurl. With an older libcurl the close waits until the
 * system's resolver gives up. */

#include "fetch.h"

#include <pthread.h>
#include <string.h>

#include <curl/curl.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "os.h"

#include "core/url.h"

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
	bool header_to_http;  /* ... which is an absolute http:// address */
	bool header_to_https; /* ... or an absolute https:// one */
	long header_status;
	bool secure;           /* the request whose response is coming went over https */
	bool redirect_refused; /* a redirect from https to http was refused (fetch.h) */
	bool headers_done;     /* the final response's headers are all in */
	bool paused;
	bool done;
	CURLcode done_result;
	char *url;   /* the address as it was given */
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
		fetch->header_to_http = false;
		fetch->header_to_https = false;
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
		size_t at = strlen("Location:");
		while (at < length && (line[at] == ' ' || line[at] == '\t'))
			at++;
		fetch->header_to_http = length - at >= 7 && astrcmpi_n(line + at, "http://", 7) == 0;
		fetch->header_to_https = length - at >= 8 && astrcmpi_n(line + at, "https://", 8) == 0;
	} else if ((length == 2 && line[0] == '\r' && line[1] == '\n') || (length == 1 && line[0] == '\n')) {
		/* the end of a response's headers: the final one unless it is informational or a redirect libcurl
		 * follows */
		bool informational = fetch->header_status >= 100 && fetch->header_status < 200;
		bool followed = fetch->header_status >= 300 && fetch->header_status < 400 && fetch->header_location;
		if (!informational && !followed)
			fetch->headers_done = true;
		/* fetch.h: a redirect from https to http is refused, wherever in the chain it comes, as WinHTTP's redirect
		 * policy refuses it. A short return ends the transfer. A relative Location keeps the scheme. */
		if (followed && fetch->secure && fetch->header_to_http) {
			fetch->redirect_refused = true;
			return 0;
		}
		if (followed && fetch->header_to_https)
			fetch->secure = true;
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
	default:
		return PL_FETCH_FAILURE_OTHER;
	}
}

/* What a libcurl failure means; in_body when it came while reading the body */
static enum pl_fetch_result failure(struct pl_fetch *fetch, CURLcode code, bool in_body)
{
	fetch->info.error = (uint32_t)code;
	fetch->info.failure = failure_kind(code);
	/* a redirect this code refused (https to http), or one libcurl refused for its scheme (not http or https) */
	if (fetch->redirect_refused || (code == CURLE_UNSUPPORTED_PROTOCOL && fetch->header_location)) {
		fetch->info.failure = PL_FETCH_FAILURE_REDIRECT_REFUSED;
		return PL_FETCH_NETWORK_ERROR;
	}
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
	pl_content_type_split(type, &fetch->info.content_type, &fetch->info.charset);
	curl_off_t length = -1;
	curl_easy_getinfo(fetch->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
	fetch->info.encoded = false;
	fetch->info.has_length = length >= 0 && !fetch->header_encoded;
	fetch->info.content_length = fetch->info.has_length ? (uint64_t)length : 0;
	long redirects = 0;
	curl_easy_getinfo(fetch->easy, CURLINFO_REDIRECT_COUNT, &redirects);
	fetch->info.redirects = (uint32_t)redirects;
	/* without a redirect the final address is the one given, user name and password included, which libcurl was
	 * given apart (pl_fetch_open): the entries of a list on a server that asks for them resolve against it */
	char *final_url = NULL;
	curl_easy_getinfo(fetch->easy, CURLINFO_EFFECTIVE_URL, &final_url);
	if (!redirects && fetch->url)
		fetch->info.final_url = bstrdup(fetch->url);
	else
		fetch->info.final_url = final_url ? bstrdup(final_url) : NULL;
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
	fetch->secure = astrcmpi_n(url, "https://", 8) == 0;
	fetch->url = bstrdup(url);
	/* fetch.h: a user name and password in the address are taken out of it and given to libcurl apart
	 * (CURLOPT_USERNAME, CURLOPT_PASSWORD), sent as Basic with the first request. Left inside the address they
	 * would be lost at the first redirect that names a whole address, the same server's too, because libcurl
	 * replaces the address then. Given apart, and with CURLOPT_UNRESTRICTED_AUTH left off, libcurl sends them
	 * only to the scheme, host and port of this first address, also after a redirect that stays there, and to no
	 * other (libcurl 7.83 and later compare all three; older ones the host name alone). */
	char *plain = NULL;
	char *user = NULL;
	char *password = NULL;
	pl_url_split_credentials(url, &plain, &user, &password);
	curl_easy_setopt(easy, CURLOPT_URL, plain);
	if (user) {
		curl_easy_setopt(easy, CURLOPT_USERNAME, user);
		curl_easy_setopt(easy, CURLOPT_PASSWORD, password ? password : "");
		curl_easy_setopt(easy, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
	}
	/* libcurl keeps its own copies */
	if (password)
		memset(password, 0, strlen(password));
	bfree(plain);
	bfree(user);
	bfree(password);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, options->user_agent);
	/* plain http and https only, for the first request and every redirect (R-REM-1) */
	if (curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") != CURLE_OK) {
		/* a libcurl older than 7.85 (the one in macOS 13.0, R-MAC-1) has only the bit-mask options */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		curl_easy_setopt(easy, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
		curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	}
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
	/* no signals: the lookup's time limit is the connect timeout above, not an alarm */
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075700
	/* a stop during a name lookup: closing does not wait for the lookup's thread (the top of this file). An older
	 * libcurl at run time does not know the option and refuses it, which changes nothing. */
	curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);
#endif
	/* fetch.h: the proxy the environment names (http_proxy, https_proxy, all_proxy, with no_proxy) is used, which
	 * is libcurl's default: no option is set for it */
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
	bfree(fetch->info.charset);
	bfree(fetch->info.final_url);
	bfree(fetch->url);
	bfree(fetch);
}
