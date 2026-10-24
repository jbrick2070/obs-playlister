/*
obs-playlister (based on Media Playlist Source)
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

/* Runs the upstream shuffler self-tests (the TEST_SHUFFLER block in shuffler.c) and the
 * obs-playlister regression tests below as a ctest executable. shuffler.c is included so the
 * tests can reach its private helpers. NDEBUG is undefined first because every check is an
 * assert(), which release builds would otherwise compile away. */

#undef NDEBUG
#define TEST_SHUFFLER
#include "shuffler.c"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* The invariant shuffler_update_files documents (history never drops below head), plus the range of next */
static void assert_invariant(struct shuffler *shuffler)
{
	assert(shuffler->head <= shuffler->history);
	assert(shuffler->history <= shuffler->shuffled_files.num);
	assert(shuffler->next <= shuffler->shuffled_files.num);
}

static void play_next_items(struct shuffler *shuffler, int count)
{
	for (int i = 0; i < count; i++) {
		assert(shuffler_has_next(shuffler));
		struct media_file_data *item = shuffler_next(shuffler);
		assert(item);
		assert_invariant(shuffler);
	}
}

/* Loop on: plays every item once, then the first item of the second cycle. The other items are the last
 * cycle's history, still unplayed in the new cycle. */
static void setup_second_cycle(struct shuffler *shuffler, struct darray *items, size_t count)
{
	shuffler_init(shuffler);
	shuffler_set_loop(shuffler, true);
	ArrayInit(items, count);
	shuffler_update_files(shuffler, items);

	for (size_t i = 0; i <= count; i++) {
		assert(shuffler_has_next(shuffler));
		struct media_file_data *item = shuffler_next(shuffler);
		assert(item);
	}
	assert(shuffler->head == 1);
	assert(shuffler->next == 1);
	assert(shuffler->history == 1);
}

/* Removes the file that started the second cycle from the list. Only last-cycle history is left, so history
 * becomes 0. The copy shares the other items' strings, so it is freed with da_free. */
static void remove_playing_item(struct shuffler *shuffler, struct darray *items, struct darray *remaining)
{
	DARRAY(struct media_file_data) all;
	DARRAY(struct media_file_data) kept;
	all.da = *items;
	kept.da = *remaining;

	struct media_file_data *playing = shuffler->shuffled_files.array[0];
	for (size_t i = 0; i < all.num; i++) {
		if (&all.array[i] != playing)
			da_push_back(kept, &all.array[i]);
	}
	*remaining = kept.da;

	shuffler_update_files(shuffler, remaining);
	assert(shuffler->shuffled_files.num == all.num - 1);
	assert(shuffler->head == 0);
	assert(shuffler->next == 0);
	assert(shuffler->history == 0);
}

/* R-UP-75: Play Selected File on the only file the second cycle has not played yet empties the last cycle's
 * history (history == num), so the next item starts a new cycle instead of running past the end */
static void test_select_last_history_item(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;
	da_init(items);
	setup_second_cycle(&shuffler, &items.da, 2);

	shuffler_select(&shuffler, shuffler.shuffled_files.array[1]);
	assert(shuffler.head == 2);
	assert(shuffler.next == 2);
	assert(shuffler.history == 2);
	play_next_items(&shuffler, 4);

	shuffler_destroy(&shuffler);
	ArrayDestroy(&items.da);
}

/* R-UP-75: with history 0 every item is last-cycle history, so the selection mps_update makes after the list
 * update takes the item out of it, and head never passes history */
static void test_select_with_history_zero(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;
	DARRAY(struct media_file_data) remaining;
	da_init(items);
	da_init(remaining);
	setup_second_cycle(&shuffler, &items.da, 3);
	remove_playing_item(&shuffler, &items.da, &remaining.da);

	/* the removed file is not found, so mps_update selects the first file of the list */
	shuffler_select(&shuffler, &remaining.array[0]);
	assert(shuffler.head == 1);
	assert(shuffler.next == 1);
	assert(shuffler.history == 1);
	play_next_items(&shuffler, 6);

	shuffler_destroy(&shuffler);
	da_free(remaining);
	ArrayDestroy(&items.da);
}

/* R-UP-73: a further list update while history is 0 must stop its backward walk at index 0 */
static void test_update_files_with_history_zero(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;
	DARRAY(struct media_file_data) remaining;
	da_init(items);
	da_init(remaining);
	setup_second_cycle(&shuffler, &items.da, 3);
	remove_playing_item(&shuffler, &items.da, &remaining.da);

	shuffler_update_files(&shuffler, &remaining.da);
	assert(shuffler.shuffled_files.num == 2);
	assert(shuffler.history == 0);
	assert_invariant(&shuffler);
	play_next_items(&shuffler, 4);

	shuffler_destroy(&shuffler);
	da_free(remaining);
	ArrayDestroy(&items.da);
}

/* R-UP-73: before R-UP-75, shuffler_select_index could leave head == next == num with history == 0, which
 * puts every item in both ranges; removing an item then must not underflow next */
static void test_update_files_with_overlapping_ranges(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;
	DARRAY(struct media_file_data) remaining;
	da_init(items);
	da_init(remaining);
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);
	ArrayInit(&items.da, 2);
	shuffler_update_files(&shuffler, &items.da);
	shuffler.head = 2;
	shuffler.next = 2;
	shuffler.history = 0;

	/* keep only the first item; the copy shares its strings, so it is freed with da_free */
	da_push_back(remaining, &items.array[0]);
	shuffler_update_files(&shuffler, &remaining.da);
	assert(shuffler.shuffled_files.num == 1);
	assert_invariant(&shuffler);

	for (int i = 0; i < 3; i++) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item == &remaining.array[0]);
	}

	shuffler_destroy(&shuffler);
	da_free(remaining);
	ArrayDestroy(&items.da);
}

/* R-UP-73, R-UP-75: random edits of short lists (each followed by a loop setting and a selection, as in
 * mps_update), selections, next and previous keep the invariant, and next and previous return list items */
static void test_random_operations_keep_invariant(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) files;
	size_t last_id = 0;
	shuffler_init(&shuffler);
	da_init(files);
	srand(20261024);

	for (int step = 0; step < 50000; step++) {
		int op = rand() % 8;
		if (op < 2 || !files.num) {
			/* keep each item with probability 3/4, then add up to two new ones */
			DARRAY(struct media_file_data) edited;
			DARRAY(char *) dropped_ids;
			da_init(edited);
			da_init(dropped_ids);
			for (size_t i = 0; i < files.num; i++) {
				if (rand() % 4)
					da_push_back(edited, &files.array[i]);
				else
					da_push_back(dropped_ids, &files.array[i].id);
			}
			for (int added = rand() % 3; added > 0; added--) {
				struct media_file_data data = {0};
				struct dstr id = {0};
				dstr_printf(&id, "%zu", ++last_id);
				data.id = id.array;
				da_init(data.folder_items);
				da_push_back(edited, &data);
			}

			if (rand() % 8 == 0)
				shuffler_reshuffle(&shuffler); /* shuffle turned on again */
			shuffler_set_loop(&shuffler, rand() % 4 != 0);
			shuffler_update_files(&shuffler, &edited.da);
			/* the shuffler now points into the edited list; the dropped items' strings are unused */
			for (size_t i = 0; i < dropped_ids.num; i++)
				bfree(dropped_ids.array[i]);
			da_free(dropped_ids);
			da_free(files);
			files.da = edited.da;
			if (files.num)
				shuffler_select(&shuffler, &files.array[rand() % files.num]);
		} else if (op < 5) {
			if (shuffler_has_next(&shuffler)) {
				struct media_file_data *item = shuffler_next(&shuffler);
				assert(item >= files.array && item < files.array + files.num);
			}
		} else if (op < 7) {
			if (shuffler_has_prev(&shuffler)) {
				struct media_file_data *item = shuffler_prev(&shuffler);
				assert(item >= files.array && item < files.array + files.num);
			}
		} else {
			/* Play Selected File */
			shuffler_select(&shuffler, &files.array[rand() % files.num]);
		}
		assert_invariant(&shuffler);
	}

	shuffler_destroy(&shuffler);
	for (size_t i = 0; i < files.num; i++)
		bfree(files.array[i].id);
	da_free(files);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_shuffler();
	printf("upstream shuffler self-tests: passed\n");

	test_select_last_history_item();
	test_select_with_history_zero();
	test_update_files_with_history_zero();
	test_update_files_with_overlapping_ranges();
	test_random_operations_keep_invariant();
	printf("obs-playlister shuffler regression tests: passed\n");
	return 0;
}
