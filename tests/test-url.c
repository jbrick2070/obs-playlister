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

/* URLs and Windows paths as playlists give them (R-REM-1) */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>

#include "core/url.h"

static void expect_text(const char *what, const char *input, char *found, const char *expected)
{
	bool same = (!found && !expected) || (found && expected && strcmp(found, expected) == 0);
	if (!same) {
		fprintf(stderr, "%s(%s): \"%s\", expected \"%s\"\n", what, input, found ? found : "(null)",
			expected ? expected : "(null)");
		assert(false);
	}
	bfree(found);
}

static void test_scheme(void)
{
	assert(pl_url_scheme_length("http://example.com") == 4);
	assert(pl_url_scheme_length("rtmp://live") == 4);
	assert(pl_url_scheme_length("a+b-c.d:x") == 7);
	assert(pl_url_scheme_length("C:\\x") == 0); /* a drive letter */
	assert(pl_url_scheme_length("c:/x") == 0);
	assert(pl_url_scheme_length("1http://x") == 0);
	assert(pl_url_scheme_length("no scheme") == 0);
	assert(pl_url_scheme_length("") == 0);

	assert(pl_url_is_http("http://example.com/a"));
	assert(pl_url_is_http("HTTPS://example.com/a"));
	assert(!pl_url_is_http("http:relative"));
	assert(!pl_url_is_http("httpx://example.com"));
	assert(!pl_url_is_http("rtmp://example.com"));
	assert(!pl_url_is_http("C:\\http\\x.mp4"));
}

static void ext(const char *location, const char *expected)
{
	expect_text("extension", location, pl_url_path_extension(location), expected);
}

static void test_extension(void)
{
	ext("http://example.com/a/b.MP4?x=1.mkv#f.avi", "mp4");
	ext("https://example.com/list.m3u8", "m3u8");
	ext("http://example.com", NULL);
	ext("http://example.com/", NULL);
	ext("http://example.com/dir.v2/", NULL);
	ext("http://example.com/dir.v2/file", NULL);
	ext("http://example.com.au?q=a.mp3", NULL);
	ext("C:\\Music\\song.Flac", "flac");
	ext("C:\\dir.v2\\file", NULL);
	ext("C:\\a#b\\c.d?e.mp3", "mp3"); /* a local name may hold "?" and "#" */
	ext("C:\\dir\\file.", NULL);
	ext("relative/x.tar.gz", "gz");
}

static void resolve(const char *reference, const char *expected)
{
	expect_text("resolve", reference, pl_url_resolve("http://a/b/c/d;p?q", reference), expected);
}

/* RFC 3986 section 5.4, except "g:h": a one-letter scheme is a drive letter here */
static void test_resolve(void)
{
	resolve("g", "http://a/b/c/g");
	resolve("./g", "http://a/b/c/g");
	resolve("g/", "http://a/b/c/g/");
	resolve("/g", "http://a/g");
	resolve("//g", "http://g");
	resolve("?y", "http://a/b/c/d;p?y");
	resolve("g?y", "http://a/b/c/g?y");
	resolve("#s", "http://a/b/c/d;p?q#s");
	resolve("g#s", "http://a/b/c/g#s");
	resolve("g?y#s", "http://a/b/c/g?y#s");
	resolve(";x", "http://a/b/c/;x");
	resolve("g;x", "http://a/b/c/g;x");
	resolve("g;x?y#s", "http://a/b/c/g;x?y#s");
	resolve("", "http://a/b/c/d;p?q");
	resolve(".", "http://a/b/c/");
	resolve("./", "http://a/b/c/");
	resolve("..", "http://a/b/");
	resolve("../", "http://a/b/");
	resolve("../g", "http://a/b/g");
	resolve("../..", "http://a/");
	resolve("../../", "http://a/");
	resolve("../../g", "http://a/g");

	resolve("../../../g", "http://a/g");
	resolve("../../../../g", "http://a/g");
	resolve("/./g", "http://a/g");
	resolve("/../g", "http://a/g");
	resolve("g.", "http://a/b/c/g.");
	resolve(".g", "http://a/b/c/.g");
	resolve("g..", "http://a/b/c/g..");
	resolve("..g", "http://a/b/c/..g");
	resolve("./../g", "http://a/b/g");
	resolve("./g/.", "http://a/b/c/g/");
	resolve("g/./h", "http://a/b/c/g/h");
	resolve("g/../h", "http://a/b/c/h");
	resolve("g;x=1/./y", "http://a/b/c/g;x=1/y");
	resolve("g;x=1/../y", "http://a/b/c/y");
	resolve("g?y/./x", "http://a/b/c/g?y/./x");
	resolve("g?y/../x", "http://a/b/c/g?y/../x");
	resolve("g#s/./x", "http://a/b/c/g#s/./x");
	resolve("g#s/../x", "http://a/b/c/g#s/../x");
	resolve("http:g", "http:g");

	/* scheme and host lower-cased, user information and path kept */
	expect_text("resolve", "HTTP://User@Example.COM/A/./B",
		    pl_url_resolve("http://x/", "HTTP://User@Example.COM/A/./B"), "http://User@example.com/A/B");
	expect_text("resolve", "list on a bare host", pl_url_resolve("http://example.com", "a.mp3"),
		    "http://example.com/a.mp3");
	expect_text("resolve", "network path",
		    pl_url_resolve("https://example.com/x/list.m3u", "//cdn.example.com/z.mp3"),
		    "https://cdn.example.com/z.mp3");
}

static void decode(const char *text, const char *expected)
{
	expect_text("decode", text, pl_url_percent_decode(text), expected);
}

static void test_decode(void)
{
	decode("a%20b%2fc", "a b/c");
	decode("%C3%A9t%C3%A9", "\xC3\xA9t\xC3\xA9");
	decode("%zz%", "%zz%");
	decode("%00x", "%00x");
	decode("x%4", "x%4");
	decode("100%", "100%");
	decode("", "");
}

static void file_url(const char *text, const char *expected)
{
	expect_text("file url", text, pl_url_file_to_path(text), expected);
}

static void test_file_url(void)
{
	file_url("file:///C:/a%20b.mp3", "C:\\a b.mp3");
	file_url("FILE:///C:/X.MP3", "C:\\X.MP3");
	file_url("file://localhost/C:/x.mp3", "C:\\x.mp3");
	file_url("file:/C:/x.mp3", "C:\\x.mp3");
	file_url("file:///C:/a.mp3?query#frag", "C:\\a.mp3");
	file_url("file://server/share/x.mp3", "\\\\server\\share\\x.mp3");
	file_url("file:////server/share/x.mp3", "\\\\server\\share\\x.mp3");
	file_url("file:///etc/passwd", NULL); /* no drive and no share */
	file_url("file://", NULL);
	file_url("http://example.com/a.mp3", NULL);
	file_url("C:\\a.mp3", NULL);
}

static void path(const char *base, const char *reference, const char *expected)
{
	expect_text("path", reference, pl_path_resolve(base, reference), expected);
}

static void test_path(void)
{
	const char *base = "C:\\Lists\\a.m3u";
	path(base, "b.mp3", "C:\\Lists\\b.mp3");
	path(base, "sub/c.mp3", "C:\\Lists\\sub\\c.mp3");
	path(base, ".\\.\\w.mp3", "C:\\Lists\\w.mp3");
	path(base, "a\\\\\\b.mp3", "C:\\Lists\\a\\b.mp3");
	path(base, "..\\x.mp3", "C:\\x.mp3");
	path(base, "..\\..\\..\\x.mp3", "C:\\x.mp3"); /* never above the root */
	path(base, "\\Root\\y.mp3", "C:\\Root\\y.mp3");
	path(base, "D:/z.mp3", "D:\\z.mp3");
	path(base, "D:z.mp3", "D:\\z.mp3");
	path(base, "\\\\server\\share\\..\\x.mp3", "\\\\server\\share\\x.mp3");
	path(base, "//server/share/x.mp3", "\\\\server\\share\\x.mp3");
	path(base, "\\\\?\\C:\\long\\..\\name.mp3", "\\\\?\\C:\\long\\..\\name.mp3");
	path("C:/Lists/a.m3u", "b.mp3", "C:\\Lists\\b.mp3");

	const char *share = "\\\\srv\\sh\\lists\\l.m3u";
	path(share, "\\abs.mp3", "\\\\srv\\sh\\abs.mp3");
	path(share, "../up.mp3", "\\\\srv\\sh\\up.mp3");
	path(share, "../../../up.mp3", "\\\\srv\\sh\\up.mp3");
	path(share, "x.mp3", "\\\\srv\\sh\\lists\\x.mp3");
}

int main(void)
{
	long allocations = bnum_allocs();
	test_scheme();
	test_extension();
	test_resolve();
	test_decode();
	test_file_url();
	test_path();
	assert(bnum_allocs() == allocations);
	printf("test-url: all passed\n");
	return 0;
}
