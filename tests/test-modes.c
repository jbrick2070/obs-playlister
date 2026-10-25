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

/* Container modes (R-DIR-7, R-DIR-10): full, newest only and newest N pick by date whatever the display order,
 * dateless entries count as oldest, ties go to the later entry, and the kept entries stay in display order. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/modes.h"

#define NO_DATE INT64_MIN

/* Entries in display order (say, by name); ordinal is the container's own order, date may be NO_DATE */
static void fill(struct pl_entry *entries, const uint32_t *ordinals, const int64_t *dates, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		struct pl_entry entry = {0};
		entry.source_ordinal = ordinals[i];
		entry.has_date = dates[i] != NO_DATE;
		entry.date_unix_ms = entry.has_date ? dates[i] : 0;
		entries[i] = entry;
	}
}

static void expect_visible(const size_t *visible, size_t visible_count, const size_t *expected, size_t expected_count)
{
	assert(visible_count == expected_count);
	for (size_t i = 0; i < expected_count; i++)
		assert(visible[i] == expected[i]);
}

static void test_modes(void)
{
	struct pl_entry entries[6];
	size_t visible[6];
	const uint32_t ordinals[] = {4, 0, 5, 1, 3, 2};
	const int64_t dates[] = {5000, NO_DATE, 9000, 9000, NO_DATE, 1000};
	fill(entries, ordinals, dates, 6);

	const size_t all[] = {0, 1, 2, 3, 4, 5};
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_FULL, 3, visible), all, 6);

	/* 9000 twice: the later one in the container (ordinal 5, display position 2) is the newest */
	const size_t newest[] = {2};
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_ONLY, 3, visible), newest, 1);

	/* the three newest are 9000, 9000 and 5000; they keep their display order */
	const size_t newest3[] = {0, 2, 3};
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, 3, visible), newest3, 3);

	/* all four dated ones, then the newer of the two dateless ones (later in the container: ordinal 3, position 4) */
	const size_t newest5[] = {0, 2, 3, 4, 5};
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, 5, visible), newest5, 5);

	/* N of 0 counts as 1; N of at least the count keeps everything */
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, 0, visible), newest, 1);
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, 6, visible), all, 6);
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, 1000, visible), all, 6);
	/* the mode takes the count as given; clamping the setting is the caller's (modes.h) */
	expect_visible(visible, pl_mode_select(entries, 6, PL_MODE_NEWEST_N, (uint32_t)-1, visible), all, 6);
}

/* no dates at all: newest means last in the container */
static void test_without_dates(void)
{
	struct pl_entry entries[4];
	size_t visible[4];
	const uint32_t ordinals[] = {2, 0, 3, 1};
	const int64_t dates[] = {NO_DATE, NO_DATE, NO_DATE, NO_DATE};
	fill(entries, ordinals, dates, 4);
	const size_t newest[] = {2};
	expect_visible(visible, pl_mode_select(entries, 4, PL_MODE_NEWEST_ONLY, 1, visible), newest, 1);
	const size_t newest2[] = {0, 2};
	expect_visible(visible, pl_mode_select(entries, 4, PL_MODE_NEWEST_N, 2, visible), newest2, 2);
	assert(pl_mode_select(entries, 0, PL_MODE_NEWEST_ONLY, 1, visible) == 0);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_modes();
	test_without_dates();
	assert(bnum_allocs() == allocations);
	printf("mode tests: passed\n");
	return 0;
}
