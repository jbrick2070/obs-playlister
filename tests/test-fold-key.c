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

/* R-KEY-1 key folding: invariant upper case in UTF-16, '/' separators, non-ASCII letters folded, nothing lost. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "providers/fold-key-win.h"

static void expect_fold(const char *text, const char *expected)
{
	char *folded = pl_fold_key(text);
	if (strcmp(folded, expected) != 0) {
		fprintf(stderr, "fold of \"%s\" gave \"%s\", expected \"%s\"\n", text, folded, expected);
		abort();
	}
	bfree(folded);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	expect_fold("clip.mp4", "CLIP.MP4");
	expect_fold("Sub\\Episode 2.MKV", "SUB/EPISODE 2.MKV");
	expect_fold("sub/ep10.mp4", "SUB/EP10.MP4");
	/* e with acute, and Cyrillic: letters outside ASCII fold too */
	expect_fold("caf\xc3\xa9.mp4", "CAF\xc3\x89.MP4");
	expect_fold("\xd0\xb0\xd0\xb1.mp4", "\xd0\x90\xd0\x91.MP4");
	/* digits, spaces and punctuation stay as they are */
	expect_fold("2026-10-24 render_01 (final).mp4", "2026-10-24 RENDER_01 (FINAL).MP4");
	/* beyond the BMP: an emoji passes through whole (a surrogate pair in UTF-16) */
	expect_fold("\xf0\x9f\x98\x80 clip.mp4", "\xf0\x9f\x98\x80 CLIP.MP4");
	/* a byte that is not UTF-8 becomes U+FFFD, so the name still has a key of its own */
	expect_fold("a\xff"
		    "b.mp4",
		    "A\xef\xbf\xbd"
		    "B.MP4");
	expect_fold("", "");
	assert(pl_fold_key(NULL) == NULL);
	assert(bnum_allocs() == allocations);
	printf("key folding tests: passed\n");
	return 0;
}
