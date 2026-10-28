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

/* What a WinHTTP failure means; in_body when it came while reading the body */
static enum pl_fetch_result failure(struct pl_fetch *fetch, DWORD error, bool in_body)
{
	fetch->info.error = error;
	if (error == ERROR_WINHTTP_TIMEOUT)
		return PL_FETCH_TIMEOUT;
	if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
		return PL_FETCH_CANCELLED;
	if (tls_error(error))
		return PL_FETCH_TLS_ERROR;
	if (error == ERROR_WINHTTP_REDIRECT_FAILED && (uint32_t)fetch->redirects >= fetch->max_redirects)
		return PL_FETCH_TOO_MANY_REDIRECTS;
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

/* "Audio/X-MpegURL; charset=utf-8" -> "audio/x-mpegurl" */
static char *normalize_content_type(char *text)
{
	if (!text)
		return NULL;
	text[strcspn(text, ";")] = '\0';
	char *start = text;
	while (*start == ' ' || *start == '\t')
		start++;
	size_t length = strlen(start);
	while (length && (start[length - 1] == ' ' || start[length - 1] == '\t'))
		length--;
	char *type = bstrdup_n(start, length);
	for (char *c = type; *c; c++) {
		if (*c >= 'A' && *c <= 'Z')
			*c = (char)(*c - 'A' + 'a');
	}
	bfree(text);
	if (!*type) {
		bfree(type);
		return NULL;
	}
	return type;
}

static void read_headers(struct pl_fetch *fetch)
{
	struct pl_fetch_info *info = &fetch->info;
	DWORD status = 0;
	DWORD bytes = sizeof(status);
	if (WinHttpQueryHeaders(fetch->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
		info->status = status;

	info->content_type = normalize_content_type(query_text(fetch->request, WINHTTP_QUERY_CONTENT_TYPE));

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
	bfree(wide_url);

	enum pl_fetch_result result = PL_FETCH_OK;
	wchar_t *agent = to_wide(options->user_agent ? options->user_agent : "obs-playlister");
	fetch->session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				     WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
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
	if (!WinHttpSetOption(fetch->request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects,
			      sizeof(redirects)) ||
	    !WinHttpSetOption(fetch->request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression))) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}

	if (!WinHttpSendRequest(fetch->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
				(DWORD_PTR)fetch)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}
	result = wait_for(fetch, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, false);
	if (result != PL_FETCH_OK)
		goto done;

	if (!WinHttpReceiveResponse(fetch->request, NULL)) {
		result = failure(fetch, GetLastError(), false);
		goto done;
	}
	result = wait_for(fetch, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, false);
	if (result != PL_FETCH_OK)
		goto done;

	read_headers(fetch);
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
	return result;
}

struct pl_fetch *pl_fetch_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result)
{
	struct pl_fetch *fetch = bzalloc(sizeof(*fetch));
	fetch->completed = CreateEventW(NULL, FALSE, FALSE, NULL);
	fetch->all_closed = CreateEventW(NULL, TRUE, FALSE, NULL);
	fetch->stop_event = options->stop_event;
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
	bfree(fetch->info.final_url);
	bfree(fetch);
}
