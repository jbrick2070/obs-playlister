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

/* R-REM-5 against an HTTP server inside this test on 127.0.0.1: no network, no admin rights, no PowerShell. HTTPS,
 * names that do not resolve and real servers are TESTING.md items (tests/manual/serve-fixtures.ps1). */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "providers/fetch.h"

/* ---- the server ---- */

static SOCKET listener = INVALID_SOCKET;
static uint16_t port;
static HANDLE release_hangs; /* manual-reset: hanging connections give up */
static volatile LONG connections;
static char *gzip_body;
static size_t gzip_size;

#define GZIP_PLAIN_SIZE (1 + 258 * 100)
#define BIG_SIZE 300000

static void send_all(SOCKET s, const char *data, size_t size)
{
	while (size) {
		int sent = send(s, data, size > 65536 ? 65536 : (int)size, 0);
		if (sent <= 0)
			return;
		data += sent;
		size -= (size_t)sent;
	}
}

static void send_text(SOCKET s, const char *text)
{
	send_all(s, text, strlen(text));
}

static void hang(void)
{
	WaitForSingleObject(release_hangs, 30000);
}

static DWORD WINAPI serve(void *param)
{
	SOCKET s = (SOCKET)(uintptr_t)param;
	char request[8192];
	size_t got = 0;
	while (got < sizeof(request) - 1) {
		int n = recv(s, request + got, (int)(sizeof(request) - 1 - got), 0);
		if (n <= 0)
			break;
		got += (size_t)n;
		request[got] = '\0';
		if (strstr(request, "\r\n\r\n"))
			break;
	}
	request[got] = '\0';

	char path[1024] = "";
	sscanf(request, "GET %1023s", path);
	struct dstr reply = {0};

	if (strcmp(path, "/ok") == 0 || strcmp(path, "/redirect/0") == 0) {
		const char *body = "#EXTM3U\na.mp3\n";
		dstr_printf(&reply,
			    "HTTP/1.1 200 OK\r\nContent-Type: Audio/X-MpegURL; charset=utf-8\r\nContent-Length: %zu\r\n"
			    "Connection: close\r\n\r\n%s",
			    strlen(body), body);
		send_text(s, reply.array);
	} else if (strcmp(path, "/binary") == 0) {
		/* UTF-16 text: every other byte is a NUL, and the body starts with one */
		send_text(s, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n");
		send_all(s, "\0a\0b\0c", 6);
	} else if (strcmp(path, "/nolength") == 0) {
		send_text(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhello");
	} else if (strncmp(path, "/redirect/", 10) == 0) {
		int n = atoi(path + 10);
		/* this server answers one request per connection and says so; without it WinHTTP may send the next
		 * request on a connection the server has already closed */
		dstr_printf(
			&reply,
			"HTTP/1.1 302 Found\r\nLocation: /redirect/%d\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
			n - 1);
		send_text(s, reply.array);
	} else if (strncmp(path, "/status/", 8) == 0) {
		dstr_printf(&reply, "HTTP/1.1 %s Status\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path + 8);
		send_text(s, reply.array);
	} else if (strcmp(path, "/gzip") == 0) {
		dstr_printf(&reply,
			    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: gzip\r\n"
			    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			    gzip_size);
		send_text(s, reply.array);
		send_all(s, gzip_body, gzip_size);
	} else if (strcmp(path, "/short") == 0) {
		send_text(s, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
		send_text(s, "0123456789012345678901234567890123456789012345678");
	} else if (strcmp(path, "/big") == 0) {
		dstr_printf(&reply, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", BIG_SIZE);
		send_text(s, reply.array);
		char *big = bmalloc(BIG_SIZE);
		memset(big, 'b', BIG_SIZE);
		send_all(s, big, BIG_SIZE);
		bfree(big);
	} else if (strcmp(path, "/hang-headers") == 0) {
		hang();
	} else if (strcmp(path, "/hang-body") == 0) {
		send_text(s, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n0123456789");
		hang();
	} else if (strcmp(path, "/media") == 0) {
		/* a 50 MB video: only the headers are sent, so a client that read the body would hang */
		send_text(s, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: 52428800\r\n\r\n");
		hang();
	} else {
		send_text(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	}

	dstr_free(&reply);
	shutdown(s, SD_SEND);
	closesocket(s);
	InterlockedDecrement(&connections);
	return 0;
}

static DWORD WINAPI accept_loop(void *param)
{
	(void)param;
	for (;;) {
		SOCKET s = accept(listener, NULL, NULL);
		if (s == INVALID_SOCKET)
			return 0;
		InterlockedIncrement(&connections);
		HANDLE thread = CreateThread(NULL, 0, serve, (void *)(uintptr_t)s, 0, NULL);
		if (thread)
			CloseHandle(thread);
		else
			InterlockedDecrement(&connections);
	}
}

static uint16_t bound_port(SOCKET s)
{
	struct sockaddr_in address;
	int length = sizeof(address);
	getsockname(s, (struct sockaddr *)&address, &length);
	return ntohs(address.sin_port);
}

static HANDLE start_server(void)
{
	WSADATA wsa;
	assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(listener, 16) == 0);
	port = bound_port(listener);
	release_hangs = CreateEventW(NULL, TRUE, FALSE, NULL);
	return CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
}

static void stop_server(HANDLE thread)
{
	SetEvent(release_hangs);
	closesocket(listener);
	WaitForSingleObject(thread, 10000);
	CloseHandle(thread);
	for (int i = 0; i < 200 && InterlockedCompareExchange(&connections, 0, 0); i++)
		Sleep(25);
	assert(connections == 0);
	CloseHandle(release_hangs);
	WSACleanup();
}

/* ---- a gzip body of 'a' * GZIP_PLAIN_SIZE: one literal and 100 copies of 258 bytes, fixed Huffman codes ---- */

/* dstr_ncat drops bytes that start with a NUL; a binary body needs every byte */
static void append_raw(struct dstr *out, const char *bytes, size_t count)
{
	dstr_ensure_capacity(out, out->len + count + 1);
	memcpy(out->array + out->len, bytes, count);
	out->len += count;
	out->array[out->len] = '\0';
}

struct bits {
	struct dstr out;
	uint32_t value;
	int count;
};

static void put_bits(struct bits *b, uint32_t value, int count)
{
	for (int i = 0; i < count; i++) {
		b->value |= ((value >> i) & 1) << b->count;
		if (++b->count == 8) {
			char byte = (char)b->value;
			append_raw(&b->out, &byte, 1);
			b->value = 0;
			b->count = 0;
		}
	}
}

/* Huffman codes go most significant bit first */
static void put_code(struct bits *b, uint32_t code, int count)
{
	for (int i = count - 1; i >= 0; i--)
		put_bits(b, (code >> i) & 1, 1);
}

static uint32_t crc32_of_run(char c, size_t count)
{
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < count; i++) {
		crc ^= (uint8_t)c;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
	}
	return ~crc;
}

static void build_gzip(void)
{
	struct bits b = {0};
	const unsigned char header[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
	append_raw(&b.out, (const char *)header, sizeof(header));
	put_bits(&b, 1, 1);          /* final block */
	put_bits(&b, 1, 2);          /* fixed Huffman */
	put_code(&b, 0x30 + 'a', 8); /* literal 'a' */
	for (int i = 0; i < 100; i++) {
		put_code(&b, 0xC5, 8); /* length 258 (code 285) */
		put_code(&b, 0, 5);    /* distance 1 */
	}
	put_code(&b, 0, 7); /* end of block */
	if (b.count)
		put_bits(&b, 0, 8 - b.count);
	uint32_t trailer[2] = {crc32_of_run('a', GZIP_PLAIN_SIZE), GZIP_PLAIN_SIZE};
	append_raw(&b.out, (const char *)trailer, sizeof(trailer));
	gzip_body = b.out.array;
	gzip_size = b.out.len;
}

/* ---- the tests ---- */

static char url_buffer[256];

static const char *url(const char *path)
{
	snprintf(url_buffer, sizeof(url_buffer), "http://127.0.0.1:%u%s", port, path);
	return url_buffer;
}

static void options_for_test(struct pl_fetch_options *options)
{
	pl_fetch_default_options(options);
	options->user_agent = "obs-playlister/test";
	options->deadline_ms = 5000;
}

/* No WinHTTP callback may run once pl_fetch_close has returned */
static void close_and_check(struct pl_fetch *fetch)
{
	pl_fetch_close(fetch);
	int64_t after = pl_fetch_callback_count();
	Sleep(150);
	assert(pl_fetch_callback_count() == after);
}

static struct pl_fetch *open_expect(const char *path, const struct pl_fetch_options *options,
				    enum pl_fetch_result expected)
{
	enum pl_fetch_result result;
	struct pl_fetch *fetch = pl_fetch_open(url(path), options, &result);
	assert(fetch);
	if (result != expected) {
		const struct pl_fetch_info *info = pl_fetch_get_info(fetch);
		fprintf(stderr, "%s: result %d (error %u, status %u), expected %d\n", path, result, info->error,
			info->status, expected);
		assert(false);
	}
	return fetch;
}

static void test_ok(void)
{
	struct pl_fetch_options options;
	options_for_test(&options);
	struct pl_fetch *fetch = open_expect("/ok", &options, PL_FETCH_OK);
	const struct pl_fetch_info *info = pl_fetch_get_info(fetch);
	assert(info->status == 200);
	assert(strcmp(info->content_type, "audio/x-mpegurl") == 0);
	assert(info->has_length && info->content_length == 14 && !info->encoded);
	assert(strcmp(info->final_url, url("/ok")) == 0 && info->redirects == 0);
	char *body;
	size_t size;
	assert(pl_fetch_read(fetch, 16 * 1024 * 1024, false, &body, &size) == PL_FETCH_OK);
	assert(size == 14 && strcmp(body, "#EXTM3U\na.mp3\n") == 0);
	bfree(body);
	close_and_check(fetch);

	fetch = open_expect("/binary", &options, PL_FETCH_OK);
	assert(pl_fetch_read(fetch, 1024, false, &body, &size) == PL_FETCH_OK);
	assert(size == 6 && memcmp(body, "\0a\0b\0c", 6) == 0 && body[6] == '\0');
	bfree(body);
	close_and_check(fetch);

	fetch = open_expect("/nolength", &options, PL_FETCH_OK);
	assert(!pl_fetch_get_info(fetch)->has_length);
	assert(pl_fetch_read(fetch, 1024, false, &body, &size) == PL_FETCH_OK);
	assert(size == 5 && strcmp(body, "hello") == 0);
	bfree(body);
	close_and_check(fetch);
}

static void test_redirects(void)
{
	struct pl_fetch_options options;
	options_for_test(&options);
	struct pl_fetch *fetch = open_expect("/redirect/5", &options, PL_FETCH_OK);
	const struct pl_fetch_info *info = pl_fetch_get_info(fetch);
	assert(info->redirects == 5);
	assert(strcmp(info->final_url, url("/redirect/0")) == 0);
	close_and_check(fetch);

	close_and_check(open_expect("/redirect/6", &options, PL_FETCH_TOO_MANY_REDIRECTS));
}

static void test_status(void)
{
	struct pl_fetch_options options;
	options_for_test(&options);
	struct pl_fetch *fetch = open_expect("/status/404", &options, PL_FETCH_HTTP_STATUS);
	assert(pl_fetch_get_info(fetch)->status == 404);
	char *body;
	size_t size;
	/* nothing to read after a failed open */
	assert(pl_fetch_read(fetch, 1024, false, &body, &size) != PL_FETCH_OK && !body && !size);
	close_and_check(fetch);

	fetch = open_expect("/status/429", &options, PL_FETCH_HTTP_STATUS);
	assert(pl_fetch_get_info(fetch)->status == 429);
	close_and_check(fetch);

	fetch = open_expect("/status/503", &options, PL_FETCH_HTTP_STATUS);
	assert(pl_fetch_get_info(fetch)->status == 503);
	close_and_check(fetch);
}

static void read_expect(const char *path, size_t limit, bool prefix, enum pl_fetch_result expected, size_t size,
			char fill)
{
	struct pl_fetch_options options;
	options_for_test(&options);
	struct pl_fetch *fetch = open_expect(path, &options, PL_FETCH_OK);
	char *body;
	size_t got;
	enum pl_fetch_result result = pl_fetch_read(fetch, limit, prefix, &body, &got);
	if (result != expected || got != size) {
		fprintf(stderr, "%s limit %zu prefix %d: result %d, %zu bytes (error %u); expected %d, %zu\n", path,
			limit, prefix, result, got, pl_fetch_get_info(fetch)->error, expected, size);
		for (size_t i = 0; body && i < got; i++) {
			if (body[i] != fill) {
				fprintf(stderr, "  first other byte at %zu: %02x\n", i, (unsigned char)body[i]);
				break;
			}
		}
		assert(false);
	}
	for (size_t i = 0; i < got; i++)
		assert(body[i] == fill);
	assert(!body || body[got] == '\0');
	bfree(body);
	close_and_check(fetch);
}

static void test_sizes(void)
{
	/* the cap counts decompressed bytes: about 180 compressed bytes are over a 10000 byte cap */
	read_expect("/gzip", 1 << 20, false, PL_FETCH_OK, GZIP_PLAIN_SIZE, 'a');
	read_expect("/gzip", GZIP_PLAIN_SIZE, false, PL_FETCH_OK, GZIP_PLAIN_SIZE, 'a');
	read_expect("/gzip", 10000, false, PL_FETCH_TOO_LARGE, 0, 'a');
	read_expect("/gzip", 64, true, PL_FETCH_OK, 64, 'a');

	read_expect("/big", 16 * 1024 * 1024, false, PL_FETCH_OK, BIG_SIZE, 'b');
	read_expect("/big", BIG_SIZE, false, PL_FETCH_OK, BIG_SIZE, 'b');
	read_expect("/big", BIG_SIZE - 1, false, PL_FETCH_TOO_LARGE, 0, 'b');
	read_expect("/big", 65536, true, PL_FETCH_OK, 65536, 'b');

	read_expect("/short", 1024, false, PL_FETCH_TRUNCATED, 0, 0);

	struct pl_fetch_options options;
	options_for_test(&options);
	struct pl_fetch *fetch = open_expect("/gzip", &options, PL_FETCH_OK);
	/* WinHTTP removes Content-Encoding and Content-Length once it decompresses, so the compressed length can
	 * never make a decompressed body look truncated */
	assert(!pl_fetch_get_info(fetch)->encoded && !pl_fetch_get_info(fetch)->has_length);
	close_and_check(fetch);
}

struct stopper {
	HANDLE event;
	DWORD delay_ms;
};

static DWORD WINAPI set_later(void *param)
{
	struct stopper *stopper = param;
	Sleep(stopper->delay_ms);
	SetEvent(stopper->event);
	return 0;
}

static double seconds_since(uint64_t start)
{
	return (double)(os_gettime_ns() - start) / 1e9;
}

static void test_stops(void)
{
	/* the bounds are loose so a busy machine passes: they only have to tell a prompt return from the 20 s deadline
	 * these waits would otherwise reach */
	struct pl_fetch_options options;
	options_for_test(&options);

	/* the overall deadline, while the server never answers */
	options.deadline_ms = 1500;
	uint64_t start = os_gettime_ns();
	struct pl_fetch *fetch = open_expect("/hang-headers", &options, PL_FETCH_TIMEOUT);
	double waited = seconds_since(start);
	assert(waited >= 1.4 && waited < 8.0);
	start = os_gettime_ns();
	close_and_check(fetch);
	assert(seconds_since(start) < 8.0);

	/* the stop event while waiting for headers, then while reading the body */
	options.deadline_ms = 20000;
	struct stopper stopper = {CreateEventW(NULL, TRUE, FALSE, NULL), 300};
	options.stop_event = stopper.event;
	HANDLE thread = CreateThread(NULL, 0, set_later, &stopper, 0, NULL);
	start = os_gettime_ns();
	fetch = open_expect("/hang-headers", &options, PL_FETCH_CANCELLED);
	assert(seconds_since(start) < 8.0);
	close_and_check(fetch);
	assert(seconds_since(start) < 8.0);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);

	ResetEvent(stopper.event);
	fetch = open_expect("/hang-body", &options, PL_FETCH_OK);
	thread = CreateThread(NULL, 0, set_later, &stopper, 0, NULL);
	start = os_gettime_ns();
	char *body;
	size_t size;
	assert(pl_fetch_read(fetch, 4096, false, &body, &size) == PL_FETCH_CANCELLED && !body);
	assert(seconds_since(start) < 8.0);
	close_and_check(fetch);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	CloseHandle(stopper.event);

	/* a direct media URL: closed after its headers, at once, without reading the body */
	options.stop_event = NULL;
	fetch = open_expect("/media", &options, PL_FETCH_OK);
	assert(strcmp(pl_fetch_get_info(fetch)->content_type, "video/mp4") == 0);
	start = os_gettime_ns();
	close_and_check(fetch);
	assert(seconds_since(start) < 8.0);
}

static void test_failures(void)
{
	struct pl_fetch_options options;
	options_for_test(&options);
	enum pl_fetch_result result;

	struct pl_fetch *fetch = pl_fetch_open("ftp://127.0.0.1/x", &options, &result);
	assert(result == PL_FETCH_NETWORK_ERROR && pl_fetch_get_info(fetch)->error == ERROR_WINHTTP_INVALID_URL);
	close_and_check(fetch);

	fetch = pl_fetch_open("not a url", &options, &result);
	assert(result == PL_FETCH_NETWORK_ERROR);
	close_and_check(fetch);

	/* a port nobody listens on */
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(s, (struct sockaddr *)&address, sizeof(address));
	uint16_t closed_port = bound_port(s);
	closesocket(s);
	char closed[64];
	snprintf(closed, sizeof(closed), "http://127.0.0.1:%u/x", closed_port);
	fetch = pl_fetch_open(closed, &options, &result);
	assert(result == PL_FETCH_NETWORK_ERROR && pl_fetch_get_info(fetch)->error != 0);
	close_and_check(fetch);

	pl_fetch_close(NULL);
}

int main(void)
{
	long allocations = bnum_allocs();
	build_gzip();
	HANDLE server = start_server();

	test_ok();
	test_redirects();
	test_status();
	test_sizes();
	test_stops();
	test_failures();

	stop_server(server);
	bfree(gzip_body);
	assert(bnum_allocs() == allocations);
	printf("test-fetch: all passed\n");
	return 0;
}
