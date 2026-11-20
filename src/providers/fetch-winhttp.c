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

/* R-REM-5: WinHTTP in asynchronous mode, because a synchronous request must not be cancelled by closing its handle.
 * The worker drives every step itself: it issues one call, then waits for that call's completion, the stop event or
 * the deadline. The status callback only records what completed and signals; it never calls WinHTTP. */

#include "fetch.h"
#include "os.h"

#include "core/url.h"

#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <windows.h>
#include <winhttp.h>

#define READ_CHUNK (64 * 1024)

static volatile LONG64 callback_count = 0;

struct pl_fetch {
	HINTERNET session, connect, request;
	HANDLE completed;           /* auto-reset: the pending call finished */
	HANDLE all_closed;          /* manual-reset: the last counted handle sent HANDLE_CLOSING */
	volatile LONG open_handles; /* handles whose HANDLE_CLOSING is still to come */
	volatile LONG redirects;
	bool tracked; /* some handle was counted: pl_fetch_close waits for all_closed */

	/* written by the callback, then completed is set; read by the worker after the wait */
	DWORD done_status;
	DWORD done_bytes;
	DWORD done_error;

	HANDLE stop_event;
	uint64_t deadline_ns;
	uint32_t max_redirects;
	bool opened; /* the headers came with a 2xx status */
	bool read;

	struct pl_fetch_info info;
	char buffer[READ_CHUNK]; /* WinHTTP writes here until the request handle has closed */
};

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
	return InterlockedCompareExchange64(&callback_count, 0, 0);
}

static void CALLBACK on_status(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID info, DWORD length)
{
	(void)handle;
	InterlockedIncrement64(&callback_count);
	struct pl_fetch *fetch = (struct pl_fetch *)context;
	if (!fetch)
		return;

	switch (status) {
	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		/* the event is read first: once the count reaches 0 the worker may free fetch, and it frees nothing before
		 * all_closed is set; this function still returns through plugin code after that, which only unloading the
		 * module at that very moment could catch */
		HANDLE all_closed = fetch->all_closed;
		if (InterlockedDecrement(&fetch->open_handles) == 0)
			SetEvent(all_closed);
		break;
	case WINHTTP_CALLBACK_STATUS_REDIRECT:
		InterlockedIncrement(&fetch->redirects);
		break;
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
		fetch->done_status = status;
		fetch->done_bytes = 0;
		fetch->done_error = 0;
		SetEvent(fetch->completed);
		break;
	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
		fetch->done_status = status;
		fetch->done_bytes = length;
		fetch->done_error = 0;
		SetEvent(fetch->completed);
		break;
	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
		const WINHTTP_ASYNC_RESULT *result = info;
		fetch->done_status = status;
		fetch->done_bytes = 0;
		fetch->done_error = result ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
		SetEvent(fetch->completed);
		break;
	}
	default:
		break;
	}
}

static bool tls_error(DWORD error)
{
	switch (error) {
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
	case ERROR_WINHTTP_SECURE_INVALID_CERT:
	case ERROR_WINHTTP_SECURE_CERT_REVOKED:
	case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
	case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		return true;
	default:
		return false;
	}
}

/* A WinHTTP or Win32 code sorted for the row status */
static enum pl_fetch_failure failure_kind(DWORD error)
{
	switch (error) {
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return PL_FETCH_FAILURE_NAME_NOT_FOUND;
	case ERROR_WINHTTP_CANNOT_CONNECT:
		return PL_FETCH_FAILURE_CANNOT_CONNECT;
	case ERROR_WINHTTP_INVALID_URL:
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
		return PL_FETCH_FAILURE_BAD_URL;
	case ERROR_ACCESS_DENIED:
		return PL_FETCH_FAILURE_ACCESS_DENIED;
	default:
		return PL_FETCH_FAILURE_OTHER;
	}
}

/* What a WinHTTP failure means; in_body when it came while reading the body */
static enum pl_fetch_result failure(struct pl_fetch *fetch, DWORD error, bool in_body)
{
	fetch->info.error = error;
	fetch->info.failure = failure_kind(error);
	if (error == ERROR_WINHTTP_TIMEOUT)
		return PL_FETCH_TIMEOUT;
	if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
		return PL_FETCH_CANCELLED;
	if (tls_error(error))
		return PL_FETCH_TLS_ERROR;
	if (error == ERROR_WINHTTP_REDIRECT_FAILED && (uint32_t)fetch->redirects >= fetch->max_redirects)
		return PL_FETCH_TOO_MANY_REDIRECTS;
	/* below the limit WinHTTP refuses a redirect only for where it leads: from https to http (the redirect policy
	 * set in start), or to a scheme that is not http(s), which it may also report as a scheme it does not know.
	 * The address the fetch was opened with never gives that code: start refuses such a one itself, as an invalid
	 * address, before WinHTTP sees it. */
	if (error == ERROR_WINHTTP_REDIRECT_FAILED || error == ERROR_WINHTTP_UNRECOGNIZED_SCHEME)
		fetch->info.failure = PL_FETCH_FAILURE_REDIRECT_REFUSED;
	return in_body ? PL_FETCH_TRUNCATED : PL_FETCH_NETWORK_ERROR;
}

/* Waits for the pending call: OK when it completed with expected, else what stopped it */
static enum pl_fetch_result wait_for(struct pl_fetch *fetch, DWORD expected, bool in_body)
{
	HANDLE handles[2] = {fetch->completed, fetch->stop_event};
	DWORD count = fetch->stop_event ? 2 : 1;
	/* a stop wins over a completion already signalled: WaitForMultipleObjects reports the lowest index */
	if (fetch->stop_event && WaitForSingleObject(fetch->stop_event, 0) == WAIT_OBJECT_0)
		return PL_FETCH_CANCELLED;
	uint64_t now = os_gettime_ns();
	if (now >= fetch->deadline_ns)
		return PL_FETCH_TIMEOUT;
	DWORD ms = (DWORD)((fetch->deadline_ns - now + 999999) / 1000000);

	DWORD signaled = WaitForMultipleObjects(count, handles, FALSE, ms);
	if (signaled == WAIT_OBJECT_0 + 1)
		return PL_FETCH_CANCELLED;
	if (signaled != WAIT_OBJECT_0)
		return PL_FETCH_TIMEOUT;
	if (fetch->done_status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
		return failure(fetch, fetch->done_error, in_body);
	if (fetch->done_status != expected)
		return failure(fetch, ERROR_WINHTTP_INTERNAL_ERROR, in_body);
	return PL_FETCH_OK;
}

static wchar_t *to_wide(const char *text)
{
	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(text, 0, &wide);
	return wide;
}

/* A response header as UTF-8, or NULL when absent */
static char *query_text(HINTERNET request, DWORD query)
{
	DWORD bytes = 0;
	WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
			    WINHTTP_NO_HEADER_INDEX);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes)
		return NULL;
	wchar_t *wide = bzalloc(bytes + sizeof(wchar_t));
	char *text = NULL;
	if (WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, wide, &bytes, WINHTTP_NO_HEADER_INDEX))
		os_wcs_to_utf8_ptr(wide, 0, &text);
	bfree(wide);
	return text;
}

static void read_headers(struct pl_fetch *fetch)
{
	struct pl_fetch_info *info = &fetch->info;
	DWORD status = 0;
	DWORD bytes = sizeof(status);
	if (WinHttpQueryHeaders(fetch->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
		info->status = status;

	char *type = query_text(fetch->request, WINHTTP_QUERY_CONTENT_TYPE);
	pl_content_type_split(type, &info->content_type, &info->charset);
	bfree(type);

	char *length = query_text(fetch->request, WINHTTP_QUERY_CONTENT_LENGTH);
	if (length && *length) {
		char *end = NULL;
		unsigned long long value = _strtoui64(length, &end, 10);
		if (end && *end == '\0') {
			info->has_length = true;
			info->content_length = value;
		}
	}
	bfree(length);

	char *encoding = query_text(fetch->request, WINHTTP_QUERY_CONTENT_ENCODING);
	info->encoded = encoding && *encoding;
	bfree(encoding);

	DWORD url_bytes = 0;
	WinHttpQueryOption(fetch->request, WINHTTP_OPTION_URL, NULL, &url_bytes);
	if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && url_bytes) {
		wchar_t *url = bzalloc(url_bytes + sizeof(wchar_t));
		if (WinHttpQueryOption(fetch->request, WINHTTP_OPTION_URL, url, &url_bytes))
			os_wcs_to_utf8_ptr(url, 0, &info->final_url);
		bfree(url);
	}
	info->redirects = (uint32_t)fetch->redirects;
}

/* Gives the handle the fetch as its context and counts it, so its HANDLE_CLOSING is waited for */
static bool track(struct pl_fetch *fetch, HINTERNET handle)
{
	DWORD_PTR context = (DWORD_PTR)fetch;
	if (WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
		InterlockedIncrement(&fetch->open_handles);
		fetch->tracked = true;
		return true;
	}
	/* a handle that took its parent's context still reports to fetch when it closes, so it is counted anyway */
	DWORD error = GetLastError();
	DWORD_PTR inherited = 0;
	DWORD size = sizeof(inherited);
	if (WinHttpQueryOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &inherited, &size) && inherited == context) {
		InterlockedIncrement(&fetch->open_handles);
		fetch->tracked = true;
	}
	SetLastError(error);
	return false;
}

/* One environment variable as UTF-8 (bmem), or NULL when it is not set or empty. Windows' own call, so a name is
 * found in any case, as the system compares them. */
static char *environment(const wchar_t *name)
{
	wchar_t value[2048];
	DWORD length = GetEnvironmentVariableW(name, value, (DWORD)(sizeof(value) / sizeof(value[0])));
	if (!length || length >= sizeof(value) / sizeof(value[0]))
		return NULL;
	char *text = NULL;
	os_wcs_to_utf8_ptr(value, 0, &text);
	return text;
}

/* Whether no_proxy names the host: entries separated by commas or spaces, "*" for every host, and an entry matches
 * the host itself and every host below it ("example.com", ".example.com" and "*.example.com" all match
 * "lists.example.com"), as FFmpeg and libcurl read it */
static bool host_in_no_proxy(const char *no_proxy, const char *host)
{
	size_t host_length = strlen(host);
	const char *c = no_proxy;
	while (*c) {
		c += strspn(c, ", \t");
		size_t length = strcspn(c, ", \t");
		const char *entry = c;
		c += length;
		if (length == 1 && entry[0] == '*')
			return true;
		if (length && entry[0] == '*') {
			entry++;
			length--;
		}
		if (length && entry[0] == '.') {
			entry++;
			length--;
		}
		if (!length || length > host_length)
			continue;
		const char *tail = host + host_length - length;
		if (astrcmpi_n(tail, entry, length) == 0 && (tail == host || tail[-1] == '.'))
			return true;
	}
	return false;
}

/* fetch.h: the proxy the environment names for this request, as the "host:port" WinHTTP takes, or NULL: none is
 * named, no_proxy names the host, or the value is not a plain http proxy without a user name (then the system's
 * settings decide, as they do when nothing is named). bmem. */
static wchar_t *environment_proxy(bool secure, const wchar_t *wide_host)
{
	char *value = secure ? environment(L"https_proxy") : environment(L"http_proxy");
	if (!value)
		value = environment(L"all_proxy");
	if (!value)
		return NULL;
	char *host = NULL;
	os_wcs_to_utf8_ptr(wide_host, 0, &host);
	char *no_proxy = environment(L"no_proxy");
	bool direct = no_proxy && host && host_in_no_proxy(no_proxy, host);
	bfree(no_proxy);
	bfree(host);

	const char *address = value;
	if (astrcmpi_n(address, "http://", 7) == 0)
		address += 7;
	size_t length = strcspn(address, "/");
	wchar_t *proxy = NULL;
	if (!direct && length && !strstr(address, "://") && !memchr(address, '@', length)) {
		char *plain = bstrdup_n(address, length);
		proxy = to_wide(plain);
		bfree(plain);
	}
	bfree(value);
	return proxy;
}

/* Whether two addresses have the same scheme, host and port, as they are written (a port left out and the same
 * port written out count as different, which errs on the side of not sending). User names, passwords, paths and
 * queries do not count. */
static bool same_origin(const char *a, const char *b)
{
	char *plain_a = pl_url_redacted(a);
	char *plain_b = pl_url_redacted(b);
	const char *host_a = strstr(plain_a, "://");
	const char *host_b = strstr(plain_b, "://");
	bool same = false;
	if (host_a && host_b) {
		size_t length_a = (size_t)(host_a + 3 - plain_a) + strcspn(host_a + 3, "/?#");
		size_t length_b = (size_t)(host_b + 3 - plain_b) + strcspn(host_b + 3, "/?#");
		same = length_a == length_b && astrcmpi_n(plain_a, plain_b, length_a) == 0;
	}
	bfree(plain_a);
	bfree(plain_b);
	return same;
}

/* Sends the request and waits for the response's headers */
static enum pl_fetch_result exchange(struct pl_fetch *fetch)
{
	if (!WinHttpSendRequest(fetch->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
				(DWORD_PTR)fetch))
		return failure(fetch, GetLastError(), false);
	enum pl_fetch_result result = wait_for(fetch, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, false);
	if (result != PL_FETCH_OK)
		return result;
	if (!WinHttpReceiveResponse(fetch->request, NULL))
		return failure(fetch, GetLastError(), false);
	return wait_for(fetch, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, false);
}

/* A user name or password out of a URL: its "%XX" sequences decoded, as libcurl decodes them (bmem) */
static wchar_t *url_part(const wchar_t *text, DWORD length)
{
	wchar_t *raw = bzalloc(((size_t)length + 1) * sizeof(wchar_t));
	memcpy(raw, text, (size_t)length * sizeof(wchar_t));
	char *utf8 = NULL;
	os_wcs_to_utf8_ptr(raw, 0, &utf8);
	bfree(raw);
	char *decoded = pl_url_percent_decode(utf8 ? utf8 : "");
	bfree(utf8);
	wchar_t *wide = to_wide(decoded);
	bfree(decoded);
	return wide;
}

static enum pl_fetch_result start(struct pl_fetch *fetch, const char *url, const struct pl_fetch_options *options)
{
	wchar_t *wide_url = to_wide(url);
	if (!wide_url)
		return failure(fetch, ERROR_WINHTTP_INVALID_URL, false);

	URL_COMPONENTS parts = {0};
	parts.dwStructSize = sizeof(parts);
	parts.dwSchemeLength = (DWORD)-1;
	parts.dwHostNameLength = (DWORD)-1;
	parts.dwUrlPathLength = (DWORD)-1;
	parts.dwExtraInfoLength = (DWORD)-1;
	parts.dwUserNameLength = (DWORD)-1;
	parts.dwPasswordLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) || !parts.dwHostNameLength ||
	    (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
		bfree(wide_url);
		return failure(fetch, ERROR_WINHTTP_INVALID_URL, false);
	}
	wchar_t *host = bzalloc((parts.dwHostNameLength + 1) * sizeof(wchar_t));
	memcpy(host, parts.lpszHostName, parts.dwHostNameLength * sizeof(wchar_t));
	/* the path and the query together; an empty path is "/" */
	size_t object_length = parts.dwUrlPathLength + parts.dwExtraInfoLength;
	wchar_t *object = bzalloc((object_length + 2) * sizeof(wchar_t));
	if (parts.dwUrlPathLength)
		memcpy(object, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof(wchar_t));
	else
		object[0] = L'/';
	if (parts.dwExtraInfoLength) {
		/* a fragment is not sent */
		size_t extra = 0;
		while (extra < parts.dwExtraInfoLength && parts.lpszExtraInfo[extra] != L'#')
			extra++;
		memcpy(object + wcslen(object), parts.lpszExtraInfo, extra * sizeof(wchar_t));
	}
	bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
	INTERNET_PORT port = parts.nPort;
	/* fetch.h: a user name and password in the address go to that server as Basic authentication */
	wchar_t *user = parts.dwUserNameLength ? url_part(parts.lpszUserName, parts.dwUserNameLength) : NULL;
	wchar_t *password = user ? url_part(parts.lpszPassword, parts.dwPasswordLength) : NULL;
	bfree(wide_url);

	enum pl_fetch_result result = PL_FETCH_OK;
	wchar_t *agent = to_wide(options->user_agent ? options->user_agent : "obs-playlister");
	/* fetch.h: the proxy the environment names, as the player of the items reads it; with none named, the system's
	 * own proxy settings, as before */
	wchar_t *proxy = environment_proxy(secure, host);
	fetch->session =
		WinHttpOpen(agent, proxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			    proxy ? proxy : WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	bfree(proxy);
	bfree(agent);
	DWORD flags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_REDIRECT |
		      WINHTTP_CALLBACK_FLAG_HANDLES;
	if (!fetch->session ||
	    WinHttpSetStatusCallback(fetch->session, on_status, flags, 0) == WINHTTP_INVALID_STATUS_CALLBACK ||
	    !track(fetch, fetch->session)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}
	int connect_ms = (int)options->connect_ms;
	if (!WinHttpSetTimeouts(fetch->session, connect_ms, connect_ms, connect_ms, (int)options->receive_ms)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}

	fetch->connect = WinHttpConnect(fetch->session, host, port, 0);
	if (!fetch->connect || !track(fetch, fetch->connect)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}
	fetch->request = WinHttpOpenRequest(fetch->connect, L"GET", object, NULL, WINHTTP_NO_REFERER,
					    WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
	if (!fetch->request || !track(fetch, fetch->request)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}

	DWORD redirects = fetch->max_redirects;
	DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
	/* fetch.h: never from https to http. WinHTTP's default, set here so that it is the plugin's rule and not a
	 * default that could change. */
	DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	if (!WinHttpSetOption(fetch->request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects,
			      sizeof(redirects)) ||
	    !WinHttpSetOption(fetch->request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
			      sizeof(redirect_policy)) ||
	    !WinHttpSetOption(fetch->request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression))) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}
	if (user && !WinHttpSetCredentials(fetch->request, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, user,
					   password ? password : L"", NULL)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}

	result = exchange(fetch);
	if (result != PL_FETCH_OK)
		goto done;
	read_headers(fetch);

	/* fetch.h: the user name and password also count after a redirect that stays with the origin they were given
	 * for. WinHTTP sends them with the first request and drops them at every redirect, so when such a redirect
	 * ends at a 401, the request, which now stands at the redirect's address, is sent once more with them. After
	 * a redirect to another origin they are never sent. */
	if (user && fetch->info.status == 401 && fetch->info.redirects > 0 && fetch->info.final_url &&
	    same_origin(url, fetch->info.final_url)) {
		if (!WinHttpSetCredentials(fetch->request, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, user,
					   password ? password : L"", NULL)) {
			result = failure(fetch, GetLastError(), false);
			goto done;
		}
		bfree(fetch->info.content_type);
		bfree(fetch->info.charset);
		bfree(fetch->info.final_url);
		uint32_t redirects_before = fetch->info.redirects;
		memset(&fetch->info, 0, sizeof(fetch->info));
		result = exchange(fetch);
		if (result != PL_FETCH_OK)
			goto done;
		read_headers(fetch);
		/* a further redirect of the second request is not followed with them either: only its own count shows */
		if (fetch->info.redirects < redirects_before)
			fetch->info.redirects = redirects_before;
	}
	/* WinHTTP hands back the redirect it would not follow; a 3xx without a Location is just a status */
	char *location = NULL;
	if (fetch->info.status >= 300 && fetch->info.status < 400 && fetch->info.redirects >= fetch->max_redirects)
		location = query_text(fetch->request, WINHTTP_QUERY_LOCATION);
	if (location && *location)
		result = PL_FETCH_TOO_MANY_REDIRECTS;
	else if (fetch->info.status < 200 || fetch->info.status > 299)
		result = PL_FETCH_HTTP_STATUS;
	else
		fetch->opened = true;
	bfree(location);

done:
	bfree(host);
	bfree(object);
	bfree(user);
	if (password)
		SecureZeroMemory(password, wcslen(password) * sizeof(wchar_t));
	bfree(password);
	return result;
}

struct pl_fetch *pl_fetch_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result)
{
	struct pl_fetch *fetch = bzalloc(sizeof(*fetch));
	fetch->completed = CreateEventW(NULL, FALSE, FALSE, NULL);
	fetch->all_closed = CreateEventW(NULL, TRUE, FALSE, NULL);
	fetch->stop_event = options->stop ? pl_stop_win32_event(options->stop) : NULL;
	fetch->max_redirects = options->max_redirects;
	fetch->deadline_ns = os_gettime_ns() + (uint64_t)options->deadline_ms * 1000000;

	if (!fetch->completed || !fetch->all_closed)
		*result = failure(fetch, GetLastError(), false);
	else
		*result = start(fetch, url, options);
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
	if (!fetch->opened || fetch->read)
		return failure(fetch, ERROR_WINHTTP_INCORRECT_HANDLE_STATE, true);
	fetch->read = true;

	struct dstr data = {0};
	enum pl_fetch_result result = PL_FETCH_OK;
	for (;;) {
		/* one byte past the cap is enough to know the body is over it */
		size_t room = prefix ? limit - data.len : limit + 1 - data.len;
		if (!room)
			break;
		DWORD chunk = (DWORD)(room < READ_CHUNK ? room : READ_CHUNK);
		if (!WinHttpReadData(fetch->request, fetch->buffer, chunk, NULL)) {
			result = failure(fetch, GetLastError(), true);
			break;
		}
		result = wait_for(fetch, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, true);
		if (result != PL_FETCH_OK)
			break;
		if (!fetch->done_bytes) {
			const struct pl_fetch_info *info = &fetch->info;
			if (info->has_length && !info->encoded && data.len < info->content_length)
				result = PL_FETCH_TRUNCATED;
			break;
		}
		/* not dstr_ncat, which drops a chunk that starts with a NUL byte (UTF-16 text has plenty) */
		dstr_ensure_capacity(&data, data.len + fetch->done_bytes + 1);
		memcpy(data.array + data.len, fetch->buffer, fetch->done_bytes);
		data.len += fetch->done_bytes;
		data.array[data.len] = '\0';
		if (!prefix && data.len > limit) {
			result = PL_FETCH_TOO_LARGE;
			break;
		}
	}

	if (result != PL_FETCH_OK) {
		dstr_free(&data);
		return result;
	}
	*body = data.array;
	*size = data.len;
	return PL_FETCH_OK;
}

void pl_fetch_close(struct pl_fetch *fetch)
{
	if (!fetch)
		return;
	/* closing the request cancels a pending call; its error and HANDLE_CLOSING still come through the callback */
	if (fetch->request)
		WinHttpCloseHandle(fetch->request);
	if (fetch->connect)
		WinHttpCloseHandle(fetch->connect);
	if (fetch->session)
		WinHttpCloseHandle(fetch->session);
	if (fetch->tracked)
		WaitForSingleObject(fetch->all_closed, INFINITE);

	if (fetch->completed)
		CloseHandle(fetch->completed);
	if (fetch->all_closed)
		CloseHandle(fetch->all_closed);
	bfree(fetch->info.content_type);
	bfree(fetch->info.charset);
	bfree(fetch->info.final_url);
	bfree(fetch);
}
