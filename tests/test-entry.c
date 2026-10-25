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

/* Entries: a copy owns its own strings, missing strings stay missing, and freeing leaves nothing behind (the
 * plugin's bmem leak count is checked by the runtime harness; here bnum_allocs). */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/entry.h"

static void test_copy_and_free(void)
{
	long allocations = bnum_allocs();
	struct pl_entry original = {0};
	original.location = bstrdup("C:/media/clip.mp4");
	original.key = bstrdup("CLIP.MP4");
	original.title = bstrdup("Clip");
	original.previous_key = bstrdup("OLD.MP4");
	original.occurrence = 2;
	original.source_ordinal = 7;
	original.kind = PL_ENTRY_STILL;
	original.has_date = true;
	original.date_unix_ms = 1761300000000;
	original.size = 123456789;

	struct pl_entry copy;
	pl_entry_copy(&copy, &original);
	assert(copy.location != original.location && strcmp(copy.location, original.location) == 0);
	assert(copy.key != original.key && strcmp(copy.key, original.key) == 0);
	assert(copy.title != original.title && strcmp(copy.title, "Clip") == 0);
	assert(copy.previous_key != original.previous_key && strcmp(copy.previous_key, "OLD.MP4") == 0);
	assert(copy.occurrence == 2 && copy.source_ordinal == 7 && copy.kind == PL_ENTRY_STILL);
	assert(copy.has_date && copy.date_unix_ms == 1761300000000 && copy.size == 123456789);

	copy.location[0] = 'D';
	assert(original.location[0] == 'C');
	pl_entry_free(&copy);
	assert(copy.location == NULL && copy.key == NULL && copy.title == NULL && copy.occurrence == 0);

	/* missing strings stay missing */
	struct pl_entry bare = {0};
	pl_entry_copy(&copy, &bare);
	assert(copy.location == NULL && copy.title == NULL && copy.key == NULL && copy.previous_key == NULL);
	pl_entry_free(&original);
	assert(bnum_allocs() == allocations);
}

static void test_entries_free(void)
{
	long allocations = bnum_allocs();
	struct pl_entry *entries = bzalloc(3 * sizeof(*entries));
	for (size_t i = 0; i < 3; i++) {
		entries[i].location = bstrdup("C:/media/x.mp4");
		entries[i].title = bstrdup("x");
		entries[i].previous_key = bstrdup("OLD.MP4");
	}
	pl_entries_free(entries, 3);
	pl_entries_free(NULL, 0);
	assert(bnum_allocs() == allocations);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_copy_and_free();
	test_entries_free();
	printf("entry tests: passed\n");
	return 0;
}
