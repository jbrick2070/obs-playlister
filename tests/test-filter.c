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

/* The extension filter (R-DIR-5): reading the option text, whole-extension matching, the always-skipped names and
 * the still-image kind (R-DIR-9). */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "core/filter.h"

static void test_parse_default(void)
{
	struct pl_filter filter;
	pl_filter_parse(&filter, PL_FILTER_DEFAULT);
	assert(filter.count == 24);
	assert(strcmp(filter.extensions[0], "mp4") == 0);
	assert(strcmp(filter.extensions[23], "bmp") == 0);
	pl_filter_free(&filter);
	assert(filter.count == 0 && filter.extensions == NULL);
}

static void test_parse_lenient(void)
{
	struct pl_filter filter;
	pl_filter_parse(&filter, "  .MP4,*.mkv;\tmp4  WebM . *. ");
	assert(filter.count == 3);
	assert(strcmp(filter.extensions[0], "mp4") == 0);
	assert(strcmp(filter.extensions[1], "mkv") == 0);
	assert(strcmp(filter.extensions[2], "webm") == 0);
	pl_filter_free(&filter);

	/* an empty filter admits nothing */
	pl_filter_parse(&filter, "");
	assert(filter.count == 0);
	assert(!pl_filter_matches(&filter, "a.mp4"));
	pl_filter_free(&filter);
	pl_filter_parse(&filter, NULL);
	assert(filter.count == 0);
	pl_filter_free(&filter);
	pl_filter_parse(&filter, " ,; 	");
	assert(filter.count == 0);
	pl_filter_free(&filter);

	/* kept as written, and never matching: only a whole last extension is compared */
	pl_filter_parse(&filter, ".. *.* tar.gz");
	assert(filter.count == 3);
	assert(strcmp(filter.extensions[0], ".") == 0);
	assert(strcmp(filter.extensions[1], "*") == 0);
	assert(!pl_filter_matches(&filter, "a.tar.gz"));
	assert(!pl_filter_matches(&filter, "a.mp4"));
	pl_filter_free(&filter);
}

static void test_matches(void)
{
	struct pl_filter filter;
	pl_filter_parse(&filter, "mp4 ts gz");
	assert(pl_filter_matches(&filter, "C:/media/Show.MP4"));
	assert(pl_filter_matches(&filter, "C:\\media\\clip.ts"));
	assert(pl_filter_matches(&filter, "archive.tar.gz"));
	/* the whole extension, never a part of one */
	assert(!pl_filter_matches(&filter, "clip.mts"));
	assert(!pl_filter_matches(&filter, "clip.mp44"));
	assert(!pl_filter_matches(&filter, "show.mp4.part"));
	/* the extension of the last component only */
	assert(!pl_filter_matches(&filter, "C:/media.mp4/readme"));
	assert(!pl_filter_matches(&filter, "noextension"));
	assert(!pl_filter_matches(&filter, "trailing."));
	pl_filter_free(&filter);
}

static void test_always_skipped(void)
{
	assert(pl_filter_always_skipped("desktop.ini"));
	assert(pl_filter_always_skipped("C:/media/DESKTOP.INI"));
	assert(pl_filter_always_skipped("C:\\media\\Thumbs.db"));
	assert(pl_filter_always_skipped("~$notes.docx"));
	assert(pl_filter_always_skipped("C:/media/render.mp4.crdownload"));
	assert(pl_filter_always_skipped("render.TMP"));
	assert(pl_filter_always_skipped("render.part"));
	assert(pl_filter_always_skipped("render.partial"));
	assert(!pl_filter_always_skipped("partial.mp4"));
	assert(!pl_filter_always_skipped("tmp.mp4"));
	assert(!pl_filter_always_skipped("~notes.mp4"));
	assert(!pl_filter_always_skipped("C:/~$dir/clip.mp4"));
}

static void test_kind(void)
{
	assert(pl_filter_kind("C:/a/photo.JPG") == PL_ENTRY_STILL);
	assert(pl_filter_kind("photo.jpeg") == PL_ENTRY_STILL);
	assert(pl_filter_kind("photo.png") == PL_ENTRY_STILL);
	assert(pl_filter_kind("photo.webp") == PL_ENTRY_STILL);
	assert(pl_filter_kind("photo.bmp") == PL_ENTRY_STILL);
	assert(pl_filter_kind("loop.gif") == PL_ENTRY_MEDIA);
	assert(pl_filter_kind("clip.mp4") == PL_ENTRY_MEDIA);
	assert(pl_filter_kind("noextension") == PL_ENTRY_MEDIA);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_parse_default();
	test_parse_lenient();
	test_matches();
	test_always_skipped();
	test_kind();
	printf("filter tests: passed\n");
	return 0;
}
