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

/* M3U and M3U8 (R-REM-1, D-12): fixtures as players and servers write them, then encodings, line ends and malformed
 * input built here byte by byte */

#include "list-check.h"

#include <ctype.h>

#include "core/parse-m3u.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void parse_fixture(const char *name, const char *base, const struct expected_entry *expected, size_t count,
			  size_t nested, size_t unusable, bool stream)
{
	size_t size = 0;
	char *data = read_fixture(name, &size);
	struct pl_list_source source = {base, NULL, NULL};
	struct pl_list_result result;
	pl_parse_m3u(&result, data, size, &source);
	check_result(name, &result, expected, count, nested, unusable);
	assert(result.stream == stream);
	pl_list_result_free(&result);
	bfree(data);
}

static void parse_bytes(const char *what, const char *data, size_t size, const char *base,
			const struct expected_entry *expected, size_t count, size_t nested, size_t unusable)
{
	struct pl_list_source source = {base, NULL, NULL};
	struct pl_list_result result;
	pl_parse_m3u(&result, data, size, &source);
	check_result(what, &result, expected, count, nested, unusable);
	assert(!result.stream);
	pl_list_result_free(&result);
}

#define PARSE_TEXT(what, text, base, expected, nested, unusable) \
	parse_bytes(what, text, sizeof(text) - 1, base, expected, COUNT(expected), nested, unusable)

static void test_fixtures(void)
{
	/* VLC writes file: URLs, percent-encoded */
	const struct expected_entry vlc[] = {
		{"C:\\Music\\Song One.mp3", "Artist - Song One", PL_ENTRY_MEDIA},
		{"http://radio.example.com:8000/stream", "Live Radio", PL_ENTRY_MEDIA},
		{"D:\\Music\\Album\\Track 02.flac", "Song, With Comma", PL_ENTRY_MEDIA},
	};
	parse_fixture("vlc.m3u", "C:\\Lists\\vlc.m3u", vlc, COUNT(vlc), 0, 0, false);

	/* relative and absolute Windows paths, CRLF; nested lists skipped, an HLS entry kept as a stream (D-12 c) */
	const struct expected_entry local[] = {
		{"C:\\Lists\\Album\\01 - Intro.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\Other\\02.mp4", NULL, PL_ENTRY_MEDIA},
		{"C:\\Root\\03.mkv", NULL, PL_ENTRY_MEDIA},
		{"D:\\Absolute\\04.mp3", NULL, PL_ENTRY_MEDIA},
		{"\\\\server\\share\\05.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\Lists\\cover.jpg", NULL, PL_ENTRY_STILL},
		{"C:\\Lists\\live\\index.m3u8", NULL, PL_ENTRY_STREAM},
	};
	parse_fixture("relative.m3u", "C:\\Lists\\relative.m3u", local, COUNT(local), 2, 0, false);

	/* the same list on a server: RFC 3986 against the final URL, and no local paths */
	const struct expected_entry remote[] = {
		{"https://host.example.com/dir/Album/01%20-%20Intro.mp3", NULL, PL_ENTRY_MEDIA},
		{"https://host.example.com/Other/02.mp4", NULL, PL_ENTRY_MEDIA},
		{"https://host.example.com/Root/03.mkv", NULL, PL_ENTRY_MEDIA},
		{"https://host.example.com/dir/cover.jpg", NULL, PL_ENTRY_STILL},
		{"https://host.example.com/dir/live/index.m3u8", NULL, PL_ENTRY_STREAM},
	};
	parse_fixture("relative.m3u", "https://host.example.com/dir/relative.m3u", remote, COUNT(remote), 2, 2, false);

	/* IPTV: quoted attributes with commas, HLS entries */
	const struct expected_entry iptv[] = {
		{"https://cdn.example.com/news/index.m3u8", "News Channel", PL_ENTRY_STREAM},
		{"https://cdn.example.com/music/live.ts", "Music TV", PL_ENTRY_MEDIA},
	};
	parse_fixture("iptv.m3u", "https://lists.example.com/iptv.m3u", iptv, COUNT(iptv), 0, 0, false);

	/* HLS media and master playlists are one stream: the playlist itself */
	const struct expected_entry media[] = {
		{"https://video.example.com/show/hls-media.m3u8", NULL, PL_ENTRY_STREAM},
	};
	parse_fixture("hls-media.m3u8", "https://video.example.com/show/hls-media.m3u8", media, 1, 0, 0, true);
	const struct expected_entry master[] = {
		{"C:\\Video\\hls-master.m3u8", NULL, PL_ENTRY_STREAM},
	};
	parse_fixture("hls-master.m3u8", "C:\\Video\\hls-master.m3u8", master, 1, 0, 0, true);
}

static void test_line_ends(void)
{
	/* UTF-8 BOM, CRLF, a lone CR, blanks around lines and titles, no newline at the end */
	const struct expected_entry entries[] = {
		{"C:\\L\\a.mp3", "Caf\xC3\xA9", PL_ENTRY_MEDIA},
		{"C:\\L\\b.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\L\\c.mp3", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("line ends", "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:1,  Caf\xC3\xA9  \r\n  a.mp3\t \r\rb.mp3\nc.mp3",
		   "C:\\L\\x.m3u", entries, 0, 0);

	struct pl_list_source source = {"C:\\L\\x.m3u", NULL, NULL};
	struct pl_list_result result;
	pl_parse_m3u(&result, "", 0, &source);
	assert(result.count == 0 && result.unusable_lines == 0 && !result.stream);
	pl_list_result_free(&result);
	pl_parse_m3u(&result, "#EXTM3U\n\n   \n", 13, &source);
	assert(result.count == 0 && result.unusable_lines == 0);
	pl_list_result_free(&result);
}

static void test_encodings(void)
{
	/* not UTF-8: Windows-1252, as older Windows players wrote .m3u */
	const struct expected_entry cp1252[] = {
		{"C:\\L\\Caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D.mp3", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("cp1252", "Caf\xE9 \x93q\x94.mp3\n", "C:\\L\\x.m3u", cp1252, 0, 0);

	/* UTF-16 little endian with its BOM: "a.mp3", CRLF, then e-acute, U+1F3B5 as a surrogate pair, ".mp3" */
	const char le[] = {'\xFF', '\xFE', 'a',    0,      '.',    0,      'm', 0, 'p', 0, '3', 0, '\r', 0, '\n', 0,
			   '\xE9', 0,      '\x3C', '\xD8', '\xB5', '\xDF', '.', 0, 'm', 0, 'p', 0, '3',  0};
	const struct expected_entry utf16[] = {
		{"C:\\L\\a.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\L\\\xC3\xA9\xF0\x9F\x8E\xB5.mp3", NULL, PL_ENTRY_MEDIA},
	};
	parse_bytes("utf-16le", le, sizeof(le), "C:\\L\\x.m3u", utf16, COUNT(utf16), 0, 0);

	const char be[] = {'\xFE', '\xFF', 0, 'b', 0, '.', 0, 'm', 0, 'p', 0, '3'};
	const struct expected_entry utf16be[] = {{"C:\\L\\b.mp3", NULL, PL_ENTRY_MEDIA}};
	parse_bytes("utf-16be", be, sizeof(be), "C:\\L\\x.m3u", utf16be, 1, 0, 0);

	/* an unpaired surrogate becomes U+FFFD */
	const char lone[] = {'\xFF', '\xFE', '\x3C', '\xD8', 'x', 0};
	const struct expected_entry replaced[] = {{"C:\\L\\\xEF\xBF\xBDx", NULL, PL_ENTRY_MEDIA}};
	parse_bytes("lone surrogate", lone, sizeof(lone), "C:\\L\\x.m3u", replaced, 1, 0, 0);

	/* an odd byte at the end of UTF-16 text is dropped */
	const char odd[] = {'\xFF', '\xFE', 'b', 0, '.', 0, 'm', 0, 'p', 0, '3', 0, 'x'};
	parse_bytes("odd utf-16", odd, sizeof(odd), "C:\\L\\x.m3u", utf16be, 1, 0, 0);
}

static void test_malformed(void)
{
	/* a NUL inside a line makes it unusable; the lines around it still read */
	const struct expected_entry around[] = {
		{"C:\\L\\a.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\L\\d.mp3", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("nul", "a.mp3\nb\0c.mp3\nd.mp3\n", "C:\\L\\x.m3u", around, 0, 1);

	/* a 2 MB line is unusable, and the next line still reads */
	size_t long_length = 2 * 1024 * 1024;
	char *big = bmalloc(long_length + 32);
	memset(big, 'x', long_length);
	memcpy(big + long_length, ".mp3\nok.mp3\n", 12);
	const struct expected_entry after[] = {{"C:\\L\\ok.mp3", NULL, PL_ENTRY_MEDIA}};
	parse_bytes("long line", big, long_length + 12, "C:\\L\\x.m3u", after, 1, 0, 1);
	bfree(big);

	/* a truncated last line still reads; a tag that only starts like an HLS tag is not one */
	const struct expected_entry truncated[] = {
		{"C:\\L\\a.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\L\\par", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("truncated", "#EXT-X-TARGETDURATIONX:1\na.mp3\npar", "C:\\L\\x.m3u", truncated, 0, 0);
}

static void test_titles(void)
{
	/* a title belongs to the next reference only, through comments; no comma or an empty title is none */
	const struct expected_entry entries[] = {
		{"C:\\L\\no-title.mp3", NULL, PL_ENTRY_MEDIA},  {"C:\\L\\t.mp3", "T", PL_ENTRY_MEDIA},
		{"C:\\L\\reset.mp3", NULL, PL_ENTRY_MEDIA},     {"C:\\L\\empty.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\L\\quoted.mp3", "after", PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("titles",
		   "#EXTINF:5\nno-title.mp3\n#EXTINF:5,T\n# comment\nt.mp3\nreset.mp3\n#EXTINF:5,\nempty.mp3\n"
		   "#extinf:-1 a=\"x,y\",after\nquoted.mp3\n",
		   "C:\\L\\x.m3u", entries, 0, 0);
}

static void test_references(void)
{
	/* in a remote list: network stream schemes only (no file:, in any case, and no FFmpeg protocol that can wrap a
	 * local file), no drive paths, scheme and host lower-cased, network paths, spaces and non-ASCII encoded */
	const struct expected_entry remote[] = {
		{"rtmp://live.example.com/app", NULL, PL_ENTRY_MEDIA},
		{"http://other.com/Y.mp3?a=b", NULL, PL_ENTRY_MEDIA},
		{"https://cdn.example.com/z.mp3", NULL, PL_ENTRY_MEDIA},
		{"srt://host.example.com:9000", NULL, PL_ENTRY_MEDIA},
		{"https://host.example.com/dir/Caf%C3%A9%20x.mp3", NULL, PL_ENTRY_MEDIA},
		{"https://host.example.com/a%20b/c.mp3?q=1%202", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT(
		"remote references",
		"file:///C:/x.mp3\nrtmp://live.example.com/app\nHTTP://Other.COM/Y.mp3?a=b\n//cdn.example.com/z.mp3\n"
		"concat:C:\\secret.mp4|http://x/a.ts\nFILE:///C:/x.mp3\nC:x.mp3\ncache:http://x/a.mp4\npipe:0\n"
		"SRT://host.example.com:9000\nCaf\xC3\xA9 x.mp3\nhttps://host.example.com/a b/c.mp3?q=1 2\n",
		"https://host.example.com/dir/l.m3u", remote, 0, 6);

	/* in a local list a URL stays as written; a file: URL takes the path rules (one key for one file) */
	const struct expected_entry local[] = {
		{"rtmp://x/y", NULL, PL_ENTRY_MEDIA},
		{"HTTP://H/a.MP4", NULL, PL_ENTRY_MEDIA},
		{"C:\\a.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\a\\b.mp3", NULL, PL_ENTRY_MEDIA},
		{"\\\\server\\share\\x.mp3", NULL, PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("local references",
		   "rtmp://x/y\nHTTP://H/a.MP4\nfile:///etc/passwd\nfile:///C:/Music/../a.mp3\nfile:///C:/a//b.mp3\n"
		   "file:////server/share/x.mp3\n",
		   "C:\\L\\x.m3u", local, 0, 1);
}

static char *upper_key(const char *location, void *context)
{
	int *calls = context;
	(*calls)++;
	char *key = bstrdup(location);
	for (char *c = key; *c; c++)
		*c = (char)toupper((unsigned char)*c);
	return key;
}

static void test_keys(void)
{
	/* the key function's keys, with occurrences in list order (R-KEY-1) */
	int calls = 0;
	struct pl_list_source source = {"C:\\L\\x.m3u", upper_key, &calls};
	struct pl_list_result result;
	const char text[] = "a.mp3\nA.MP3\nb.mp3\na.mp3\n";
	pl_parse_m3u(&result, text, sizeof(text) - 1, &source);
	assert(calls == 4 && result.count == 4);
	const uint32_t occurrences[] = {1, 2, 1, 3};
	for (size_t i = 0; i < 4; i++) {
		assert(strcmp(result.entries[i].key, i == 2 ? "C:\\L\\B.MP3" : "C:\\L\\A.MP3") == 0);
		assert(result.entries[i].occurrence == occurrences[i]);
		assert(result.entries[i].source_ordinal == i);
	}
	assert(strcmp(result.entries[1].location, "C:\\L\\A.MP3") == 0);
	pl_list_result_free(&result);

	/* an HLS stream is keyed through the function too */
	calls = 0;
	const char hls[] = "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:4\nnested.m3u\nb\0d.ts\na.ts\n";
	pl_parse_m3u(&result, hls, sizeof(hls) - 1, &source);
	assert(result.stream && result.count == 1 && strcmp(result.entries[0].key, "C:\\L\\X.M3U") == 0);
	assert(result.entries[0].occurrence == 1);
	/* what the stream's own lines would have counted is not reported */
	assert(result.nested_playlists == 0 && result.unusable_lines == 0);
	pl_list_result_free(&result);
}

int main(void)
{
	long allocations = bnum_allocs();
	test_fixtures();
	test_line_ends();
	test_encodings();
	test_malformed();
	test_titles();
	test_references();
	test_keys();
	assert(bnum_allocs() == allocations);
	printf("test-parse-m3u: all passed\n");
	return 0;
}
