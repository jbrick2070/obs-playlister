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

/* Display order (R-DIR-6, R-DIR-10): list, natural name and date order in both directions, ties by source order,
 * dateless entries after or before the dated ones, a large shuffled list, and the stability of the index sort. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/sort.h"

#define NO_DATE INT64_MIN

struct spec {
	const char *location;
	int64_t date; /* NO_DATE for none */
};

static struct pl_entry *make_entries(const struct spec *specs, size_t count)
{
	struct pl_entry *entries = bzalloc(count * sizeof(*entries));
	for (size_t i = 0; i < count; i++) {
		entries[i].location = bstrdup(specs[i].location);
		entries[i].key = bstrdup(specs[i].location);
		entries[i].source_ordinal = (uint32_t)i;
		entries[i].has_date = specs[i].date != NO_DATE;
		entries[i].date_unix_ms = entries[i].has_date ? specs[i].date : 0;
	}
	return entries;
}

/* The source ordinals after sorting, compared with the expected ones */
static void expect_ordinals(const struct pl_entry *entries, size_t count, const uint32_t *expected)
{
	for (size_t i = 0; i < count; i++) {
		if (entries[i].source_ordinal != expected[i]) {
			fprintf(stderr, "position %zu: ordinal %u, expected %u\n", i, entries[i].source_ordinal,
				expected[i]);
			abort();
		}
		/* each entry keeps its own strings */
		assert(strcmp(entries[i].location, entries[i].key) == 0);
	}
}

static const struct spec mixed[] = {
	{"C:/f/ep10.mp4", 3000},    /* 0 */
	{"C:/f/ep2.mp4", NO_DATE},  /* 1 */
	{"C:/f/Ep1.mp4", 1000},     /* 2 */
	{"C:/f/ep1.mp4", 3000},     /* 3: same date as 0, same name as 2 */
	{"C:/f/clip.mp4", NO_DATE}, /* 4 */
	{"C:/f/b.mp4", 2000},       /* 5 */
};
#define MIXED_COUNT (sizeof(mixed) / sizeof(mixed[0]))

static void sort_and_expect(enum pl_sort_key key, bool descending, const uint32_t *expected)
{
	struct pl_entry *entries = make_entries(mixed, MIXED_COUNT);
	pl_sort_entries(entries, MIXED_COUNT, key, descending);
	expect_ordinals(entries, MIXED_COUNT, expected);
	pl_entries_free(entries, MIXED_COUNT);
}

static void test_date_order(void)
{
	/* ascending: by date, the tie at 3000 in list order, then the dateless ones in list order */
	const uint32_t ascending[] = {2, 5, 0, 3, 1, 4};
	sort_and_expect(PL_SORT_DATE, false, ascending);
	/* descending: the dateless ones first in list order, then newest first, the tie still in list order */
	const uint32_t descending[] = {1, 4, 0, 3, 5, 2};
	sort_and_expect(PL_SORT_DATE, true, descending);
}

static void test_name_order(void)
{
	/* natural, case ignored: b, clip, Ep1 and ep1 (equal, list order), ep2, ep10 */
	const uint32_t ascending[] = {5, 4, 2, 3, 1, 0};
	sort_and_expect(PL_SORT_NAME, false, ascending);
	const uint32_t descending[] = {0, 1, 2, 3, 4, 5};
	sort_and_expect(PL_SORT_NAME, true, descending);
}

static void test_list_order(void)
{
	const uint32_t ascending[] = {0, 1, 2, 3, 4, 5};
	sort_and_expect(PL_SORT_LIST, false, ascending);
	const uint32_t descending[] = {5, 4, 3, 2, 1, 0};
	sort_and_expect(PL_SORT_LIST, true, descending);
}

static void test_no_dates(void)
{
	static const struct spec undated[] = {{"c", NO_DATE}, {"a", NO_DATE}, {"b", NO_DATE}};
	struct pl_entry *entries = make_entries(undated, 3);
	assert(!pl_entries_have_dates(entries, 3));
	/* without dates, date order is list order in both directions */
	const uint32_t in_order[] = {0, 1, 2};
	pl_sort_entries(entries, 3, PL_SORT_DATE, false);
	expect_ordinals(entries, 3, in_order);
	pl_sort_entries(entries, 3, PL_SORT_DATE, true);
	expect_ordinals(entries, 3, in_order);
	pl_entries_free(entries, 3);

	struct pl_entry *dated = make_entries(mixed, MIXED_COUNT);
	assert(pl_entries_have_dates(dated, MIXED_COUNT));
	pl_entries_free(dated, MIXED_COUNT);
	pl_sort_entries(NULL, 0, PL_SORT_DATE, false);
}

/* 2,000 entries, few distinct dates: the result is a permutation in date order with ties in list order */
static void test_large_list(void)
{
	const size_t count = 2000;
	struct pl_entry *entries = bzalloc(count * sizeof(*entries));
	srand(20261024);
	for (size_t i = 0; i < count; i++) {
		char name[32];
		snprintf(name, sizeof(name), "item%zu", i);
		entries[i].location = bstrdup(name);
		entries[i].key = bstrdup(name);
		entries[i].source_ordinal = (uint32_t)i;
		entries[i].has_date = rand() % 10 != 0;
		entries[i].date_unix_ms = entries[i].has_date ? (int64_t)(rand() % 50) * 1000 : 0;
	}
	pl_sort_entries(entries, count, PL_SORT_DATE, false);
	bool *seen = bzalloc(count * sizeof(*seen));
	for (size_t i = 0; i < count; i++) {
		assert(!seen[entries[i].source_ordinal]);
		seen[entries[i].source_ordinal] = true;
		if (i == 0)
			continue;
		const struct pl_entry *a = &entries[i - 1];
		const struct pl_entry *b = &entries[i];
		if (a->has_date && b->has_date) {
			assert(a->date_unix_ms < b->date_unix_ms ||
			       (a->date_unix_ms == b->date_unix_ms && a->source_ordinal < b->source_ordinal));
		} else {
			assert(!(!a->has_date && b->has_date));
			if (!a->has_date && !b->has_date)
				assert(a->source_ordinal < b->source_ordinal);
		}
	}
	bfree(seen);
	pl_entries_free(entries, count);
}

static int compare_tens(size_t a, size_t b, void *context)
{
	(void)context;
	return (int)(a / 10) - (int)(b / 10);
}

/* equal elements keep their input order */
static void test_index_sort_is_stable(void)
{
	size_t indexes[100];
	for (size_t i = 0; i < 100; i++)
		indexes[i] = (i * 37) % 100;
	pl_sort_indexes(indexes, 100, compare_tens, NULL);
	for (size_t i = 1; i < 100; i++) {
		assert(indexes[i - 1] / 10 <= indexes[i] / 10);
		if (indexes[i - 1] / 10 == indexes[i] / 10) {
			/* the input order of equal elements: their positions in (i * 37) % 100 */
			size_t before = 0, after = 0;
			for (size_t p = 0; p < 100; p++) {
				if ((p * 37) % 100 == indexes[i - 1])
					before = p;
				if ((p * 37) % 100 == indexes[i])
					after = p;
			}
			assert(before < after);
		}
	}
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_date_order();
	test_name_order();
	test_list_order();
	test_no_dates();
	test_large_list();
	test_index_sort_is_stable();
	assert(bnum_allocs() == allocations);
	printf("sort tests: passed\n");
	return 0;
}
