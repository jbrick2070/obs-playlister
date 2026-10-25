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
 * 1; a missing key is an empty one; the key strings are left alone. Select File identities (R-UP-20) read back what
 * they write, and anything else is no identity. */

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

static void expect_identity(const char *text, const char *row_uuid, uint32_t occurrence, const char *item_key)
{
	struct pl_selection selection;
	assert(pl_selection_parse(&selection, text));
	assert(strcmp(selection.row_uuid, row_uuid) == 0);
	assert(selection.occurrence == occurrence);
	assert(strcmp(selection.item_key, item_key) == 0);
	pl_selection_free(&selection);
	assert(!selection.row_uuid && !selection.item_key);
}

static void expect_no_identity(const char *text)
{
	struct pl_selection selection = {(char *)"stale", 9, (char *)"stale"};
	if (pl_selection_parse(&selection, text)) {
		fprintf(stderr, "\"%s\" read as an identity\n", text);
		assert(false);
	}
	assert(!selection.row_uuid && !selection.item_key && !selection.occurrence);
}

static void test_selection_identities(void)
{
	/* what the dropdown writes reads back */
	char *file_row = pl_selection_format("1b4e28ba-2fa1-11d2-883f-0016d3cca427", 0, NULL);
	assert(strcmp(file_row, "1b4e28ba-2fa1-11d2-883f-0016d3cca427:0:") == 0);
	expect_identity(file_row, "1b4e28ba-2fa1-11d2-883f-0016d3cca427", 0, "");
	bfree(file_row);
	char *item = pl_selection_format("media", 2, "SEASON 1/EP1.MP4");
	assert(strcmp(item, "media:2:SEASON 1/EP1.MP4") == 0);
	expect_identity(item, "media", 2, "SEASON 1/EP1.MP4");
	bfree(item);
	char *largest = pl_selection_format("media", UINT32_MAX, "A");
	expect_identity(largest, "media", UINT32_MAX, "A");
	bfree(largest);

	/* the key is everything after the second colon, verbatim */
	expect_identity("feed:1:HTTPS://EXAMPLE.COM/A:B", "feed", 1, "HTTPS://EXAMPLE.COM/A:B");
	expect_identity("media:007:A", "media", 7, "A");

	/* upstream's positions, saved before Phase 2, and other text are no identity: the button reports a stale
	 * choice instead of guessing */
	expect_no_identity("1");
	expect_no_identity("2-3");
	expect_no_identity("0");
	expect_no_identity("");
	expect_no_identity("media:1");
	expect_no_identity(":1:A");
	expect_no_identity("media::A");
	expect_no_identity("media:x:A");
	expect_no_identity("media:-1:A");
	expect_no_identity("media:4294967296:A");
	expect_no_identity("media:0:A");
	expect_no_identity("media:1:");
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
	test_selection_identities();
	assert(bnum_allocs() == allocations);
	printf("key tests: passed\n");
	return 0;
}
