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

/* Occurrence numbering (R-KEY-1): identical keys are numbered in source order, not array order; distinct keys get
 * 1; a missing key is an empty one; the key strings are left alone. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/keys.h"

static void test_occurrences(void)
{
	/* the array is in display order; source order is the ordinal */
	char *keys[] = {"A", "B", "A", "C", "A", "B"};
	const uint32_t ordinals[] = {4, 5, 0, 1, 2, 3};
	const uint32_t expected[] = {3, 2, 1, 1, 2, 1};
	struct pl_entry entries[6] = {0};
	for (size_t i = 0; i < 6; i++) {
		entries[i].key = keys[i];
		entries[i].source_ordinal = ordinals[i];
		entries[i].occurrence = 99;
	}
	pl_assign_occurrences(entries, 6);
	for (size_t i = 0; i < 6; i++) {
		assert(entries[i].occurrence == expected[i]);
		assert(entries[i].key == keys[i]);
	}
	assert(strcmp(entries[0].key, "A") == 0);
}

static void test_special_keys(void)
{
	/* keys arrive folded, so differing case means differing keys; NULL and "" are the same key */
	char *keys[] = {"clip", "CLIP", NULL, "", NULL};
	const uint32_t expected[] = {1, 1, 1, 2, 3};
	struct pl_entry entries[5] = {0};
	for (size_t i = 0; i < 5; i++) {
		entries[i].key = keys[i];
		entries[i].source_ordinal = (uint32_t)i;
	}
	pl_assign_occurrences(entries, 5);
	for (size_t i = 0; i < 5; i++)
		assert(entries[i].occurrence == expected[i]);
	pl_assign_occurrences(NULL, 0);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_occurrences();
	test_special_keys();
	assert(bnum_allocs() == allocations);
	printf("key tests: passed\n");
	return 0;
}
