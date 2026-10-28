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

/* PLS (R-REM-1, D-12) */

#include "list-check.h"

#include <util/dstr.h>

#include "core/parse-pls.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void parse(const char *what, const char *data, size_t size, const char *base,
		  const struct expected_entry *expected, size_t count, size_t nested, size_t unusable)
{
	struct pl_list_source source = {base, NULL, NULL};
	struct pl_list_result result;
	pl_parse_pls(&result, data, size, &source);
	check_result(what, &result, expected, count, nested, unusable);
	assert(!result.stream);
	pl_list_result_free(&result);
}

static void parse_fixture(const char *name, const char *base, const struct expected_entry *expected, size_t count,
			  size_t nested, size_t unusable)
{
	size_t size = 0;
	char *data = read_fixture(name, &size);
	parse(name, data, size, base, expected, count, nested, unusable);
	bfree(data);
}

#define PARSE_TEXT(what, text, base, expected, nested, unusable) \
	parse(what, text, sizeof(text) - 1, base, expected, COUNT(expected), nested, unusable)

static void test_fixtures(void)
{
	/* a radio station's list: entries in ascending N whatever the line order, titles where given */
	const struct expected_entry radio[] = {
		{"http://radio.example.com/a", "First", PL_ENTRY_MEDIA},
		{"http://radio.example.com/b", NULL, PL_ENTRY_MEDIA},
		{"http://radio.example.com/c", "Third", PL_ENTRY_MEDIA},
	};
	parse_fixture("radio.pls", "https://radio.example.com/listen.pls", radio, COUNT(radio), 0, 0);

	/* no [playlist] line at the top, File0, a repeated File2 and a line without "=" are unusable; FileX is no entry
	 * key; an orphan title, another section and its garbage are ignored; a spaced, capitalized header reads */
	const struct expected_entry malformed[] = {
		{"C:\\Lists\\one.mp3", "One", PL_ENTRY_MEDIA},
		{"C:\\Lists\\two.mp3", NULL, PL_ENTRY_MEDIA},
		{"C:\\Lists\\four.mp3", NULL, PL_ENTRY_MEDIA},
	};
	parse_fixture("malformed.pls", "C:\\Lists\\malformed.pls", malformed, COUNT(malformed), 0, 3);
}

static void test_text(void)
{
	/* gaps in N, CRLF, blanks around "=", case-insensitive keys, comments, nested lists and a stream entry */
	const struct expected_entry entries[] = {
		{"C:\\L\\ten.mp3", "Ten", PL_ENTRY_MEDIA},
		{"C:\\L\\live.m3u8", NULL, PL_ENTRY_STREAM},
		{"C:\\L\\pic.png", NULL, PL_ENTRY_STILL},
	};
	PARSE_TEXT("text",
		   "[PlayList]\r\n; comment\r\n# comment\r\nfile10 = ten.mp3\r\nTITLE10=Ten\r\nFile20=inner.pls\r\n"
		   "File30=live.m3u8\r\nFile31=other.M3U\r\nFile40=pic.png\r\nNumberOfEntries=5\r\n",
		   "C:\\L\\x.pls", entries, 2, 0);

	/* a number too long for the counter, an unclosed section header, and an empty File line */
	const struct expected_entry few[] = {{"C:\\L\\a.mp3", NULL, PL_ENTRY_MEDIA}};
	PARSE_TEXT("numbers", "[playlist]\nFile1=a.mp3\nFile12345678901=b.mp3\nFile2=\n[broken\nFile3=c.mp3\n",
		   "C:\\L\\x.pls", few, 0, 2);

	struct pl_list_source source = {"C:\\L\\x.pls", NULL, NULL};
	struct pl_list_result result;
	pl_parse_pls(&result, "", 0, &source);
	assert(result.count == 0 && result.unusable_lines == 0);
	pl_list_result_free(&result);

	/* Windows-1252 text, a remote base with a relative entry */
	const struct expected_entry remote[] = {
		{"https://example.com/m/Caf%C3%A9.mp3", "Caf\xC3\xA9", PL_ENTRY_MEDIA},
	};
	PARSE_TEXT("remote", "[playlist]\nFile1=Caf\xE9.mp3\nTitle1=Caf\xE9\n", "https://example.com/m/list.pls",
		   remote, 0, 0);
}

static void test_numbers(void)
{
	/* File01 and File1 are one number: the second is a repeat */
	const struct expected_entry first[] = {{"C:\\L\\a.mp3", "A", PL_ENTRY_MEDIA}};
	PARSE_TEXT("same number", "[playlist]\nFile01=a.mp3\nFile1=b.mp3\nTitle1=A\nTitle01=B\n", "C:\\L\\x.pls", first,
		   0, 1);

	/* 200000 entries in reverse order come out in order, quickly (one sort, no scan per line) */
	const size_t count = 200000;
	struct dstr text = {0};
	dstr_reserve(&text, count * 24);
	dstr_cat(&text, "[playlist]\n");
	for (size_t i = count; i > 0; i--)
		dstr_catf(&text, "File%zu=%zu.mp3\n", i, i);
	struct pl_list_source source = {"C:\\L\\x.pls", NULL, NULL};
	struct pl_list_result result;
	uint64_t start = os_gettime_ns();
	pl_parse_pls(&result, text.array, text.len, &source);
	double seconds = (double)(os_gettime_ns() - start) / 1e9;
	assert(result.count == count && result.unusable_lines == 0);
	assert(strcmp(result.entries[0].location, "C:\\L\\1.mp3") == 0);
	assert(strcmp(result.entries[count - 1].location, "C:\\L\\200000.mp3") == 0);
	if (seconds > 5.0) {
		fprintf(stderr, "200000 entries took %.1f s\n", seconds);
		assert(false);
	}
	pl_list_result_free(&result);
	dstr_free(&text);
}

int main(void)
{
	long allocations = bnum_allocs();
	test_fixtures();
	test_text();
	test_numbers();
	assert(bnum_allocs() == allocations);
	printf("test-parse-pls: all passed\n");
	return 0;
}
