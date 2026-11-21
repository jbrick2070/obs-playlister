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
#include <stdint.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "test-common.h"

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

static void redacted(const char *location, const char *expected)
{
	expect_text("redacted", location, pl_url_redacted(location), expected);
}

/* what a log line shows of a location: user information, query and fragment of a URL left out; a path as it is */
static void test_redacted(void)
{
	redacted("https://user:pw@lists.example.com/show.m3u?token=SECRET#f", "https://lists.example.com/show.m3u");
	redacted("https://a@b@host.example.com/x", "https://host.example.com/x"); /* the last '@' ends it */
	redacted("http://host.example.com", "http://host.example.com");
	redacted("https://host.example.com/media/clip@2x.mp4", "https://host.example.com/media/clip@2x.mp4");
	redacted("https://host.example.com/a?mail=me@example.org", "https://host.example.com/a");
	redacted("https://user:pw@host.example.com:8080/x", "https://host.example.com:8080/x");
	redacted("https://user@host.example.com/x", "https://host.example.com/x");
	redacted("https://user:p%2Fw%40x@host.example.com/x", "https://host.example.com/x");
	/* a valid address is never changed beyond its user information: an '@' or ':' in the path or the query, a
	 * port, an IPv6 host */
	redacted("http://nas:8080/media/clip@2x.mp4", "http://nas:8080/media/clip@2x.mp4");
	redacted("https://nas:8443/live?user=me@example.com", "https://nas:8443/live");
	redacted("https://cdn.example.com/show/ep1:2/clip@2x.mp4", "https://cdn.example.com/show/ep1:2/clip@2x.mp4");
	redacted("http://[::1]/a@b", "http://[::1]/a@b");
	redacted("http://[::1]:8080/a@b", "http://[::1]:8080/a@b");
	redacted("http://192.168.1.5:8096/list.m3u?t=a@b", "http://192.168.1.5:8096/list.m3u");
	redacted("https://user:pw@nas:8443/media/12:30@studio.mp4", "https://nas:8443/media/12:30@studio.mp4");
	redacted("https://host.example.com:/a@b", "https://host.example.com:/a@b"); /* an empty port is a port */
	redacted("file:///home/me/a@b.mp4", "file:///home/me/a@b.mp4");
	/* no address: a password with a '/', '?' or '#' that was not written as "%XX" cut the authority short, and
	 * what is left of it is no host and port. All of it is left out, up to the '@'. */
	redacted("https://user:ab/cd@host.example.com/x.mp4", "https://host.example.com/x.mp4");
	redacted("https://user:pa/ss@host.example.com/x?mail=a@b", "https://host.example.com/x");
	redacted("https://user:p?ss@host.example.com/x.mp4", "https://host.example.com/x.mp4");
	redacted("https://user:p#ss@host.example.com/x", "https://host.example.com/x");
	redacted("https://:pw/x@host.example.com/y", "https://host.example.com/y");
	redacted("https://us er:pw/x@host.example.com/y", "https://host.example.com/y");
	/* ... up to the last '@' before a '?' or '#': a password with an '@' as well shows what follows that */
	redacted("https://user:a/b@c#d?e@host.example.com/x?q", "https://c");
	/* where the cut-off authority reads as a host, or as a host and a port, the text is left as it is: these
	 * show (README: write %2F, %3F, %23 and %40) */
	redacted("https://user:12/34@host.example.com/x", "https://user:12/34@host.example.com/x");
	redacted("https://us/er:secret@host.example.com/x", "https://us/er:secret@host.example.com/x");
	redacted("https://dGVzdA/Zm9v==@host.example.com/x.mp4", "https://dGVzdA/Zm9v==@host.example.com/x.mp4");
	redacted("https://token?x@host.example.com/x.mp4", "https://token");
	redacted("C:\\Lists\\a.m3u", "C:\\Lists\\a.m3u");
	redacted("/home/me/a.m3u", "/home/me/a.m3u");
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

/* P-02: a reference of thousands of segments is resolved in the time its length takes. 16,000 "a/" then 15,999 "../"
 * leave one "a/"; taking segments off the front of a buffer made this cost seconds. */
static void test_resolve_long(void)
{
	struct dstr reference = {0};
	for (int i = 0; i < 16000; i++)
		dstr_cat(&reference, "a/");
	for (int i = 0; i < 15999; i++)
		dstr_cat(&reference, "../");
	dstr_cat(&reference, "x.mp3");
	uint64_t start = os_gettime_ns();
	for (int i = 0; i < 20; i++) {
		char *resolved = pl_url_resolve("http://example.com/l/list.m3u", reference.array);
		assert(strcmp(resolved, "http://example.com/l/a/x.mp3") == 0);
		bfree(resolved);
	}
	double seconds = (double)(os_gettime_ns() - start) / 1e9;
	if (seconds >= 2.0) {
		fprintf(stderr, "20 long references took %.2f s\n", seconds);
		assert(false);
	}
	dstr_free(&reference);

	/* what the cursor must not change: the ends of the RFC's cases */
	resolve("g/..", "http://a/b/c/");
	resolve("g/.", "http://a/b/c/g/");
	resolve("../../../..", "http://a/");
	resolve("/..", "http://a/");
	resolve("/.", "http://a/");
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

static void expect_type(const char *header, const char *type, const char *charset)
{
	char *got_type = (char *)"unset";
	char *got_charset = (char *)"unset";
	pl_content_type_split(header, &got_type, &got_charset);
	bool same = (type ? got_type && strcmp(got_type, type) == 0 : !got_type) &&
		    (charset ? got_charset && strcmp(got_charset, charset) == 0 : !got_charset);
	if (!same) {
		fprintf(stderr, "content type \"%s\": got \"%s\" and \"%s\"\n", header ? header : "(null)",
			got_type ? got_type : "(null)", got_charset ? got_charset : "(null)");
		assert(false);
	}
	bfree(got_type);
	bfree(got_charset);
}

/* R-REM-1: the media type, and the character set the server names for the body */
static void test_content_type(void)
{
	expect_type(NULL, NULL, NULL);
	expect_type("", NULL, NULL);
	expect_type("audio/x-mpegurl", "audio/x-mpegurl", NULL);
	expect_type("Audio/X-MpegURL; charset=UTF-8", "audio/x-mpegurl", "utf-8");
	expect_type(" \ttext/plain\t ;  Charset = \"Windows-1251\" ; q=1", "text/plain", "windows-1251");
	expect_type("text/plain;charset=\"Windows-1251\"", "text/plain", "windows-1251");
	expect_type("text/plain; format=flowed; charset=Shift_JIS ; x=y", "text/plain", "shift_jis");
	/* the first one counts; a name that is not plain is none */
	expect_type("text/plain; charset=koi8-r; charset=utf-8", "text/plain", "koi8-r");
	expect_type("text/plain; charset=../../etc", "text/plain", NULL);
	expect_type("text/plain; charset=", "text/plain", NULL);
	expect_type("text/plain; charset", "text/plain", NULL);
	expect_type("; charset=utf-8", NULL, "utf-8");
	expect_type("text/plain; charset=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "text/plain", NULL);
}

static void expect_split(const char *url, const char *plain, const char *user, const char *password)
{
	char *got_plain, *got_user, *got_password;
	pl_url_split_credentials(url, &got_plain, &got_user, &got_password);
	bool same = strcmp(got_plain, plain) == 0 && (user ? got_user && strcmp(got_user, user) == 0 : !got_user) &&
		    (password ? got_password && strcmp(got_password, password) == 0 : !got_password);
	if (!same) {
		fprintf(stderr, "split of \"%s\": \"%s\", \"%s\", \"%s\"\n", url, got_plain,
			got_user ? got_user : "(null)", got_password ? got_password : "(null)");
		assert(false);
	}
	bfree(got_plain);
	bfree(got_user);
	bfree(got_password);
}

/* R-REM-9: the user name and password leave the address, so the fetcher can keep them from other hosts */
static void test_split_credentials(void)
{
	expect_split("https://host/list.m3u", "https://host/list.m3u", NULL, NULL);
	expect_split("https://user:pw@host/list.m3u?x=1", "https://host/list.m3u?x=1", "user", "pw");
	expect_split("http://us%40er:p%3Ass@127.0.0.1:8080/a", "http://127.0.0.1:8080/a", "us@er", "p:ss");
	expect_split("https://user@host", "https://host", "user", "");
	expect_split("https://user:@host/", "https://host/", "user", "");
	expect_split("https://a:b:c@host/", "https://host/", "a", "b:c");
	/* the last "@" before the path ends the user information; one in the path or the query is not one */
	expect_split("https://a@b:pw@host/x", "https://host/x", "a@b", "pw");
	expect_split("https://host/x@y?z=@", "https://host/x@y?z=@", NULL, NULL);
	expect_split("https://@host/x", "https://host/x", NULL, NULL);
	expect_split("no scheme@here", "no scheme@here", NULL, NULL);
	expect_split("", "", NULL, NULL);
}

static void expect_carried(const char *given, const char *final, const char *expected)
{
	expect_text("carry credentials", final, pl_url_carry_credentials(given, final), expected);
}

/* R-REM-9: the address a fetch ended at gets the given address's user name and password back only on the same
 * scheme, host and port */
static void test_carry_credentials(void)
{
	assert(pl_url_same_origin("http://u:p@Host:80/a?x", "HTTP://host:80/b#c"));
	assert(!pl_url_same_origin("http://host/a", "https://host/a"));
	assert(!pl_url_same_origin("http://host/a", "http://host:8080/a"));
	assert(!pl_url_same_origin("http://host/a", "http://host.example/a"));
	assert(!pl_url_same_origin("no scheme", "no scheme"));

	expect_carried("https://us%40er:p%3Ass@host/list", "https://host/lists/2026.m3u",
		       "https://us%40er:p%3Ass@host/lists/2026.m3u");
	expect_carried("https://user@host:8443/list", "https://HOST:8443/x", "https://user@HOST:8443/x");
	/* another scheme, port or host: never */
	expect_carried("http://user:pw@host/list", "https://host/list", "https://host/list");
	expect_carried("https://user:pw@host/list", "https://host:8443/list", "https://host:8443/list");
	expect_carried("https://user:pw@host/list", "https://cdn.host/list", "https://cdn.host/list");
	/* nothing to carry, or the final address has its own */
	expect_carried("https://host/list", "https://host/x", "https://host/x");
	expect_carried("https://user:pw@host/list", "https://other:pw2@host/x", "https://other:pw2@host/x");
	expect_carried("https://user:pw@host/list", "https://user:pw@host/list", "https://user:pw@host/list");
	/* the same ones in libcurl's spelling, as it writes them after a relative redirect: the given spelling */
	expect_carried("http://us%40er:p%3Ass@127.0.0.1:8080/auth", "http://us%40er:p%3ass@127.0.0.1:8080/auth-needed",
		       "http://us%40er:p%3Ass@127.0.0.1:8080/auth-needed");
	expect_carried("https://user:pw@host/list", "https://user:p%77@host/x", "https://user:pw@host/x");
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

/* R-LNX-10: POSIX rules, for a list whose own path is a POSIX one; the text rules run on every platform */
static void posix_file_url(const char *text, const char *expected)
{
	expect_text("posix file url", text, pl_url_file_to_posix_path(text), expected);
}

static void posix_path(const char *base, const char *reference, const char *expected)
{
	expect_text("posix path", reference, pl_posix_path_resolve(base, reference), expected);
}

static void test_posix(void)
{
	assert(pl_path_is_posix("/home/me/a.m3u") && !pl_path_is_posix("//server/share/a.m3u") &&
	       !pl_path_is_posix("C:\\a.m3u") && !pl_path_is_posix("a.m3u") && !pl_path_is_posix(NULL));

	posix_file_url("file:///home/me/a%20b.mp3", "/home/me/a b.mp3");
	posix_file_url("FILE://localhost/srv/x.mp3", "/srv/x.mp3");
	posix_file_url("file:/srv/x.mp3", "/srv/x.mp3");
	posix_file_url("file:///srv/a.mp3?query#frag", "/srv/a.mp3");
	posix_file_url("file://server/share/x.mp3", NULL); /* another host */
	posix_file_url("file://", NULL);
	posix_file_url("http://example.com/a.mp3", NULL);

	const char *base = "/home/me/lists/a.m3u";
	posix_path(base, "b.mp3", "/home/me/lists/b.mp3");
	posix_path(base, "sub/c.mp3", "/home/me/lists/sub/c.mp3");
	posix_path(base, "./././w.mp3", "/home/me/lists/w.mp3");
	posix_path(base, "a///b.mp3", "/home/me/lists/a/b.mp3");
	posix_path(base, "../x.mp3", "/home/me/x.mp3");
	posix_path(base, "../../../../../x.mp3", "/x.mp3"); /* never above the root */
	posix_path(base, "/srv/y.mp3", "/srv/y.mp3");
	posix_path(base, "back\\slash.mp3", "/home/me/lists/back\\slash.mp3"); /* an ordinary character */
	posix_path("/a.m3u", "b.mp3", "/b.mp3");
}

static void location(const char *text, const char *name, const char *container)
{
	char *found_name;
	char *found_container;
	pl_location_split(text, &found_name, &found_container);
	expect_text("location name", text, found_name, name);
	expect_text("location container", text, found_container, container);
}

static void label(const char *title, const char *text, const char *expected)
{
	expect_text("location label", text, pl_location_label(title, text), expected);
}

/* R-UX-19: the name a person calls an item by comes first, what it is in after it */
static void test_location(void)
{
	location("D:/shows/friday/loops/clip-14.mp4", "clip-14.mp4", "D:/shows/friday/loops");
	location("D:\\shows\\clip 14.mp4", "clip 14.mp4", "D:\\shows");
	location("D:/clip.mp4", "clip.mp4", "D:");
	location("\\\\server\\share\\a.mp4", "a.mp4", "\\\\server\\share");
	location("/clip.mp4", "clip.mp4", "/");
	location("/home/me/a b.mov", "a b.mov", "/home/me");
	location("clip.mp4", "clip.mp4", NULL);
	location("D:/shows/loops/", "loops", "D:/shows");
	location("/", "/", NULL);
	location("", "", NULL);
	/* an address: never its user name, password or query, and its host is no file name */
	location("https://example.com/live/stream.m3u8?token=abc", "stream.m3u8", "https://example.com/live");
	location("https://user:pw@example.com/a.mp4", "a.mp4", "https://example.com");
	location("https://example.com/live/", "live", "https://example.com");
	location("https://example.com", "example.com", NULL);
	location("https://user:pw@example.com:8080/", "example.com:8080", NULL);
	location("rtmp://example.com?key=1", "example.com", NULL);
	/* a password with an unencoded '/' or '#' reaches neither part */
	location("https://user:ab/cd@example.com/x.mp4", "x.mp4", "https://example.com");
	location("https://user:p#ss@example.com/x", "x", "https://example.com");
	location("https://user:p#ss@example.com", "example.com", NULL);
	/* an address without a host, as local playlists write files */
	location("file:///D:/shows/a.mp4", "a.mp4", "file:///D:/shows");
	location("file:///home/me/a.mp4", "a.mp4", "file:///home/me");
	location("file:///a.mp4", "a.mp4", "file:///");
	location("file:///", "file:///", NULL);
	location("file://", "file://", NULL);
	location("file://server/share/a.mp4", "a.mp4", "file://server/share");
	location("file://localhost/a.mp4", "a.mp4", "file://localhost");
	location("https://", "https://", NULL);
	location("https:///a.mp4", "a.mp4", "https:///");

	label(NULL, "D:/shows/loops/clip-14.mp4", "clip-14.mp4 (D:/shows/loops)");
	label("", "D:/shows/loops/clip-14.mp4", "clip-14.mp4 (D:/shows/loops)");
	label("Intro", "D:/shows/loops/clip-14.mp4", "Intro - clip-14.mp4 (D:/shows/loops)");
	label(NULL, "clip.mp4", "clip.mp4");
	label("News", "https://user:pw@example.com/radio?x=1", "News - radio (https://example.com)");
	label(NULL, "https://example.com/", "example.com");
	label(NULL, "file:///a.mp4", "a.mp4 (file:///)");
	label(NULL, "file:///", "file:///");
	/* never NULL, and a title that is an address shows no password either */
	label(NULL, "", "");
	label("", "", "");
	label("https://user:pw@example.com/live", "https://user:pw@example.com/live/1.ts",
	      "https://example.com/live - 1.ts (https://example.com/live)");
	label("News https://user:p/w@example.com/a", "clip.mp4", "News https://example.com/a - clip.mp4");
}

int main(void)
{
	pl_test_no_crash_dialogs();
	long allocations = bnum_allocs();
	test_scheme();
	test_extension();
	test_redacted();
	test_location();
	test_resolve();
	test_resolve_long();
	test_decode();
	test_content_type();
	test_split_credentials();
	test_carry_credentials();
	test_file_url();
	test_path();
	test_posix();
	assert(bnum_allocs() == allocations);
	printf("test-url: all passed\n");
	return 0;
}
