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

/* Merging a new snapshot while an item plays (R-DIR-8): visible, evicted or deleted; the surviving successor of a
 * deleted item; renames applied at once; held renames composed across snapshots (R-DIR-11). */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>
#include "test-common.h"

#include "core/merge.h"

/* key, occurrence, and previous_key (NULL for none) per entry; owned strings */
static void set_entry(struct pl_entry *entry, const char *key, uint32_t occurrence, const char *previous_key)
{
	memset(entry, 0, sizeof(*entry));
	entry->key = bstrdup(key);
	entry->occurrence = occurrence;
	entry->previous_key = bstrdup(previous_key);
}

static void free_entries(struct pl_entry *entries, size_t count)
{
	for (size_t i = 0; i < count; i++)
		pl_entry_free(&entries[i]);
}

static void test_fate(void)
{
	struct pl_entry entries[4];
	set_entry(&entries[0], "A", 1, NULL);
	set_entry(&entries[1], "B", 1, NULL);
	set_entry(&entries[2], "B", 2, NULL);
	set_entry(&entries[3], "C", 1, NULL);
	/* newest-N kept C and the second B */
	const size_t visible[] = {2, 3};
	size_t position = 0;

	struct pl_item_id second_b = {"B", 2};
	assert(pl_merge_current_fate(entries, 4, visible, 2, second_b, &position) == PL_CURRENT_VISIBLE);
	assert(position == 0);

	struct pl_item_id first_b = {"B", 1};
	assert(pl_merge_current_fate(entries, 4, visible, 2, first_b, &position) == PL_CURRENT_EVICTED);
	assert(position == PL_MERGE_NONE);

	struct pl_item_id gone = {"D", 1};
	assert(pl_merge_current_fate(entries, 4, visible, 2, gone, &position) == PL_CURRENT_DELETED);
	assert(position == PL_MERGE_NONE);
	assert(pl_merge_current_fate(entries, 4, visible, 2, second_b, NULL) == PL_CURRENT_VISIBLE);
	struct pl_item_id third_b = {"B", 3};
	assert(pl_merge_current_fate(entries, 4, visible, 2, third_b, &position) == PL_CURRENT_DELETED);
	free_entries(entries, 4);
}

static void test_successor(void)
{
	/* the previous visible order was A B C D E; B (position 1) was deleted */
	const struct pl_item_id previous[] = {{"A", 1}, {"B", 1}, {"C", 1}, {"D", 1}, {"E", 1}};
	struct pl_entry entries[4];
	set_entry(&entries[0], "E", 1, NULL);
	set_entry(&entries[1], "A", 1, NULL);
	set_entry(&entries[2], "D", 1, NULL);
	set_entry(&entries[3], "C", 1, NULL);

	/* the new list is sorted differently: C is visible at position 3 */
	const size_t all[] = {0, 1, 2, 3};
	assert(pl_merge_successor(previous, 5, 1, entries, all, 4) == 3);
	/* C was deleted too (not visible any more): D at position 2 */
	const size_t without_c[] = {0, 1, 2};
	assert(pl_merge_successor(previous, 5, 1, entries, without_c, 3) == 2);
	/* nothing after E survives: the caller wraps or ends */
	assert(pl_merge_successor(previous, 5, 4, entries, all, 4) == PL_MERGE_NONE);
	/* an earlier item that survives never counts as the successor */
	const size_t only_a[] = {1};
	assert(pl_merge_successor(previous, 5, 1, entries, only_a, 1) == PL_MERGE_NONE);
	/* a position outside the previous order, or nothing visible, gives none */
	assert(pl_merge_successor(previous, 5, PL_MERGE_NONE, entries, all, 4) == PL_MERGE_NONE);
	assert(pl_merge_successor(previous, 5, 5, entries, all, 4) == PL_MERGE_NONE);
	assert(pl_merge_successor(previous, 5, 1, entries, all, 0) == PL_MERGE_NONE);
	free_entries(entries, 4);
}

static void test_rekey(void)
{
	struct pl_entry old_entries[3];
	set_entry(&old_entries[0], "A", 1, NULL);
	set_entry(&old_entries[1], "B", 1, NULL);
	set_entry(&old_entries[2], "C", 1, NULL);

	/* one snapshot swaps A and B and renames C to D */
	struct pl_entry renamed[3];
	set_entry(&renamed[0], "B", 1, "A");
	set_entry(&renamed[1], "A", 1, "B");
	set_entry(&renamed[2], "D", 1, "C");
	assert(pl_merge_rekey(old_entries, 3, renamed, 3) == 3);
	assert(strcmp(old_entries[0].key, "B") == 0);
	assert(strcmp(old_entries[1].key, "A") == 0);
	assert(strcmp(old_entries[2].key, "D") == 0);

	/* entries without previous_key rename nothing */
	struct pl_entry plain[1];
	set_entry(&plain[0], "B", 1, NULL);
	assert(pl_merge_rekey(old_entries, 3, plain, 1) == 0);
	assert(strcmp(old_entries[0].key, "B") == 0);

	free_entries(old_entries, 3);
	free_entries(renamed, 3);
	free_entries(plain, 1);

	/* a chain in one snapshot moves both: A to B and B to C */
	struct pl_entry chain_old[2];
	set_entry(&chain_old[0], "A", 1, NULL);
	set_entry(&chain_old[1], "B", 1, NULL);
	struct pl_entry chain[2];
	set_entry(&chain[0], "B", 1, "A");
	set_entry(&chain[1], "C", 1, "B");
	assert(pl_merge_rekey(chain_old, 2, chain, 2) == 2);
	assert(strcmp(chain_old[0].key, "B") == 0 && strcmp(chain_old[1].key, "C") == 0);
	free_entries(chain_old, 2);
	free_entries(chain, 2);

	/* A renamed over B: the old B was overwritten and matches nothing now */
	struct pl_entry over_old[2];
	set_entry(&over_old[0], "A", 1, NULL);
	set_entry(&over_old[1], "B", 1, NULL);
	struct pl_entry over[1];
	set_entry(&over[0], "B", 1, "A");
	assert(pl_merge_rekey(over_old, 2, over, 1) == 1);
	assert(strcmp(over_old[0].key, "B") == 0 && over_old[1].key == NULL);
	free_entries(over_old, 2);
	free_entries(over, 1);
}

static void test_follow_rename(void)
{
	struct pl_entry renamed[2];
	set_entry(&renamed[0], "NEW.MP4", 2, "OLD.MP4");
	set_entry(&renamed[1], "OTHER.MP4", 1, NULL);
	struct pl_item_id current = {"OLD.MP4", 1};
	assert(pl_merge_follow_rename(&current, renamed, 2));
	assert(strcmp(current.key, "NEW.MP4") == 0 && current.occurrence == 2);
	/* followed, the item is still visible rather than deleted */
	const size_t visible[] = {0, 1};
	size_t position = 0;
	assert(pl_merge_current_fate(renamed, 2, visible, 2, current, &position) == PL_CURRENT_VISIBLE &&
	       position == 0);
	struct pl_item_id other = {"OTHER.MP4", 1};
	assert(!pl_merge_follow_rename(&other, renamed, 2));
	assert(strcmp(other.key, "OTHER.MP4") == 0);
	free_entries(renamed, 2);
}

static void test_compose(void)
{
	/* held: A renamed to B, and E renamed to F; X untouched */
	struct pl_entry held[3];
	set_entry(&held[0], "B", 1, "A");
	set_entry(&held[1], "F", 1, "E");
	set_entry(&held[2], "X", 1, NULL);

	/* newer: B renamed on to C, F renamed back to E, X renamed to Y, and a new item G */
	struct pl_entry newer[4];
	set_entry(&newer[0], "C", 1, "B");
	set_entry(&newer[1], "E", 1, "F");
	set_entry(&newer[2], "Y", 1, "X");
	set_entry(&newer[3], "G", 1, NULL);
	pl_merge_compose_renames(newer, 4, held, 3);
	assert(strcmp(newer[0].previous_key, "A") == 0); /* A -> B -> C keeps one identity */
	assert(newer[1].previous_key == NULL);           /* renamed back to its published key */
	assert(strcmp(newer[2].previous_key, "X") == 0); /* a new rename stays as it is */
	assert(newer[3].previous_key == NULL);           /* a new item has none */

	/* an item the newer snapshot did not rename keeps the held rename */
	struct pl_entry unchanged[1];
	set_entry(&unchanged[0], "B", 1, NULL);
	pl_merge_compose_renames(unchanged, 1, held, 3);
	assert(strcmp(unchanged[0].previous_key, "A") == 0);

	free_entries(held, 3);
	free_entries(newer, 4);
	free_entries(unchanged, 1);

	/* B (renamed from A, held) is renamed on to C while a new file takes the name B: only C keeps A */
	struct pl_entry held_b[1];
	set_entry(&held_b[0], "B", 1, "A");
	struct pl_entry vacated[2];
	set_entry(&vacated[0], "C", 1, "B");
	set_entry(&vacated[1], "B", 1, NULL);
	pl_merge_compose_renames(vacated, 2, held_b, 1);
	assert(strcmp(vacated[0].previous_key, "A") == 0);
	assert(vacated[1].previous_key == NULL);

	/* released, the composed snapshot moves the published item A to C, and B is a new item */
	struct pl_entry published[1];
	set_entry(&published[0], "A", 1, NULL);
	assert(pl_merge_rekey(published, 1, vacated, 2) == 1);
	assert(strcmp(published[0].key, "C") == 0);

	/* nothing held: the newer snapshot stays as it is */
	struct pl_entry alone[1];
	set_entry(&alone[0], "Q", 1, "P");
	pl_merge_compose_renames(alone, 1, NULL, 0);
	assert(strcmp(alone[0].previous_key, "P") == 0);
	pl_merge_compose_renames(NULL, 0, held_b, 1);

	free_entries(held_b, 1);
	free_entries(vacated, 2);
	free_entries(published, 1);
	free_entries(alone, 1);
}

/* P-08: what pl_merge_compose_renames did before it sorted its renames, a scan of both lists per entry, kept here
 * to compare with */
static void compose_by_scanning(struct pl_entry *newer, size_t newer_count, const struct pl_entry *held,
				size_t held_count)
{
	const char **published = bzalloc((newer_count ? newer_count : 1) * sizeof(*published));
	for (size_t n = 0; n < newer_count; n++) {
		const char *look_for = newer[n].previous_key;
		if (!look_for) {
			bool renamed_away = false;
			for (size_t r = 0; r < newer_count && !renamed_away; r++)
				renamed_away = newer[r].previous_key &&
					       strcmp(newer[r].previous_key, newer[n].key) == 0;
			if (renamed_away)
				continue;
			look_for = newer[n].key;
		}
		for (size_t h = 0; h < held_count && !published[n]; h++) {
			if (held[h].previous_key && strcmp(held[h].key, look_for) == 0)
				published[n] = held[h].previous_key;
		}
	}
	for (size_t n = 0; n < newer_count; n++) {
		if (published[n]) {
			char *copy = bstrdup(published[n]);
			bfree(newer[n].previous_key);
			newer[n].previous_key = copy;
		}
		if (newer[n].previous_key && strcmp(newer[n].previous_key, newer[n].key) == 0) {
			bfree(newer[n].previous_key);
			newer[n].previous_key = NULL;
		}
	}
	bfree((void *)published);
}

static bool same_or_both_null(const char *a, const char *b)
{
	return a && b ? strcmp(a, b) == 0 : a == b;
}

/* random held and newer snapshots over a few keys, so renames chain, swap, collide and repeat: the sorted look-ups
 * give what the scan gave, entry by entry */
static void test_compose_matches_the_scan(void)
{
	const char *keys[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
	srand(20261120);
	size_t composed = 0;
	for (int round = 0; round < 20000; round++) {
		struct pl_entry held[6], newer[6], expected[6];
		size_t held_count = (size_t)rand() % 7;
		size_t newer_count = (size_t)rand() % 7;
		for (size_t i = 0; i < held_count; i++)
			set_entry(&held[i], keys[rand() % 8], 1, rand() % 3 ? NULL : keys[rand() % 8]);
		for (size_t i = 0; i < newer_count; i++) {
			set_entry(&newer[i], keys[rand() % 8], 1, rand() % 3 ? NULL : keys[rand() % 8]);
			set_entry(&expected[i], newer[i].key, 1, newer[i].previous_key);
		}
		pl_merge_compose_renames(newer, newer_count, held, held_count);
		compose_by_scanning(expected, newer_count, held, held_count);
		for (size_t i = 0; i < newer_count; i++) {
			if (!same_or_both_null(newer[i].previous_key, expected[i].previous_key)) {
				fprintf(stderr, "round %d, entry %zu (%s): %s, the scan gave %s\n", round, i,
					newer[i].key, newer[i].previous_key ? newer[i].previous_key : "none",
					expected[i].previous_key ? expected[i].previous_key : "none");
				assert(false);
			}
			composed += newer[i].previous_key != NULL;
		}
		free_entries(held, held_count);
		free_entries(newer, newer_count);
		free_entries(expected, newer_count);
	}
	/* the rounds did compose renames */
	assert(composed > 1000);
}

/* Composes a locked list of count entries, timed, with the sorted look-ups or with the scan they replaced. Without
 * renames nothing is composed; renamed, every held entry was renamed from OLD-i to MID-i and every newer one from
 * MID-i to FILE-i, which composes to OLD-i. */
static double timed_compose(size_t count, bool renamed, bool sorted)
{
	struct pl_entry *held = bzalloc(count * sizeof(*held));
	struct pl_entry *newer = bzalloc(count * sizeof(*newer));
	char key[32], previous[32];
	for (size_t i = 0; i < count; i++) {
		snprintf(key, sizeof(key), "%s-%zu.MP4", renamed ? "MID" : "FILE", i);
		snprintf(previous, sizeof(previous), "OLD-%zu.MP4", i);
		set_entry(&held[i], key, 1, renamed ? previous : NULL);
		snprintf(previous, sizeof(previous), "FILE-%zu.MP4", i);
		set_entry(&newer[i], previous, 1, renamed ? key : NULL);
	}
	uint64_t start = os_gettime_ns();
	if (sorted)
		pl_merge_compose_renames(newer, count, held, count);
	else
		compose_by_scanning(newer, count, held, count);
	double seconds = (double)(os_gettime_ns() - start) / 1e9;
	for (size_t i = 0; i < count; i++) {
		snprintf(previous, sizeof(previous), "OLD-%zu.MP4", i);
		assert(renamed ? newer[i].previous_key && strcmp(newer[i].previous_key, previous) == 0
			       : !newer[i].previous_key);
	}
	free_entries(held, count);
	free_entries(newer, count);
	bfree(held);
	bfree(newer);
	return seconds;
}

/* A locked list: composing costs little, without renames and with every entry renamed in both snapshots. The scan
 * grows with the square of the count (20,000 entries took it about a second without renames and far longer with).
 * A wall-clock limit fails on a slow or busy machine, so composing is timed against the scan on the same machine:
 * with 10,000 entries it must be more than five times as quick (the best of three tries counts). 20,000 entries get
 * a limit only a broken compose reaches. */
static void test_compose_is_quick(void)
{
	for (int renamed = 0; renamed < 2; renamed++) {
		double sorted = 1e9;
		for (int i = 0; i < 3; i++) {
			double seconds = timed_compose(10000, renamed, true);
			sorted = seconds < sorted ? seconds : sorted;
		}
		double scanned = timed_compose(10000, renamed, false);
		printf("composing 10000 entries %s: %.4f s, with the scan %.4f s\n",
		       renamed ? "all renamed" : "without renames", sorted, scanned);
		if (sorted * 5 > scanned) {
			fprintf(stderr, "composing took %.4f s, the scan %.4f s\n", sorted, scanned);
			assert(false);
		}
		double many = timed_compose(20000, renamed, true);
		printf("composing 20000 entries: %.4f s\n", many);
		assert(many < 10.0);
	}
}

int main(void)
{
	pl_test_no_crash_dialogs();

	long allocations = bnum_allocs();
	test_fate();
	test_successor();
	test_rekey();
	test_follow_rename();
	test_compose();
	test_compose_matches_the_scan();
	test_compose_is_quick();
	assert(bnum_allocs() == allocations);
	printf("merge tests: passed\n");
	return 0;
}
