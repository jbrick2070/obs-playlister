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

static void print_state(const struct shuffler *shuffler)
{
	fprintf(stderr, "shuffler state: head %zu, next %zu, history %zu, num %zu\n", shuffler->head, shuffler->next,
		shuffler->history, shuffler->shuffled_files.num);
}

/* What every operation keeps: history never drops below head (as shuffler_update_files documents), and next points
 * past the array only when every item is determined, where shuffler_next starts a new cycle */
static void assert_invariant(struct shuffler *shuffler)
{
	bool ok = shuffler->head <= shuffler->history && shuffler->history <= shuffler->shuffled_files.num &&
		  (shuffler->next < shuffler->shuffled_files.num || shuffler->next == shuffler->head);
	if (!ok)
		print_state(shuffler);
	assert(ok);
}

/* What a list update leaves (R-UP-76): the current item, at next - 1, is a determined one or one of the last
 * cycle's history. Previous right after a reshuffle can leave it undetermined, as in VLC's randomizer; that is
 * harmless, and the next update puts it back in range. */
static void assert_next_in_cycle(struct shuffler *shuffler)
{
	bool ok = shuffler->next <= shuffler->head ||
		  (shuffler->next > shuffler->history && shuffler->next < shuffler->shuffled_files.num);
	if (!ok)
		print_state(shuffler);
	assert(ok);
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

/* R-UP-76: a finished pass, Previous twice (back into the last cycle's history), then one list update that removes
 * the item the previous pass ended with, and no select after it, as mps_update makes none for an edited current row
 * or an empty current folder. next used to be left at num, and the next shuffler_next read past the array. */
static void test_update_files_without_select_keeps_next_in_range(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;
	DARRAY(struct media_file_data) remaining;
	da_init(items);
	da_init(remaining);
	setup_second_cycle(&shuffler, &items.da, 4);

	struct media_file_data *pass_end = shuffler_prev(&shuffler);
	shuffler_prev(&shuffler);
	assert(shuffler.next == 3);
	assert_invariant(&shuffler);

	/* the copy shares the other items' strings, so it is freed with da_free */
	for (size_t i = 0; i < items.num; i++) {
		if (&items.array[i] != pass_end)
			da_push_back(remaining, &items.array[i]);
	}
	shuffler_update_files(&shuffler, &remaining.da);
	assert(shuffler.shuffled_files.num == 3);
	assert_next_in_cycle(&shuffler);
	assert_invariant(&shuffler);

	for (int i = 0; i < 8; i++) {
		assert(shuffler_has_next(&shuffler));
		struct media_file_data *item = shuffler_next(&shuffler);
		assert(item >= remaining.array && item < remaining.array + remaining.num);
		assert_invariant(&shuffler);
	}

	shuffler_destroy(&shuffler);
	da_free(remaining);
	ArrayDestroy(&items.da);
}

/* R-UP-76: the clamp leaves a valid next alone. An update with the same items in new storage keeps head, next and
 * history, with the current item determined (one step back) and with it in the last cycle's history (two back). */
static void test_identity_update_keeps_valid_next(void)
{
	for (int back = 1; back <= 2; back++) {
		struct shuffler shuffler;
		DARRAY(struct media_file_data) items;
		DARRAY(struct media_file_data) copy;
		da_init(items);
		da_init(copy);
		setup_second_cycle(&shuffler, &items.da, 4);
		for (int i = 0; i < back; i++)
			shuffler_prev(&shuffler);
		size_t head = shuffler.head, next = shuffler.next, history = shuffler.history;
		if (back == 1)
			assert(next <= head);
		else
			assert(next > history && next < shuffler.shuffled_files.num);

		/* the copy shares the items' strings, so it is freed with da_free */
		da_copy(copy, items);
		shuffler_update_files(&shuffler, &copy.da);
		assert(shuffler.head == head && shuffler.next == next && shuffler.history == history);

		shuffler_destroy(&shuffler);
		da_free(copy);
		ArrayDestroy(&items.da);
	}
}

/* R-KEY-1: folder items with entries match by key and occurrence across a rescan, whatever their file names */
static void test_update_files_matches_keys(void)
{
	struct pl_entry entries[2] = {{.key = "CLIP.MP4", .occurrence = 1}, {.key = "CLIP.MP4", .occurrence = 2}};
	struct pl_entry rescanned_entries[2] = {{.key = "CLIP.MP4", .occurrence = 1},
						{.key = "CLIP.MP4", .occurrence = 2}};
	struct media_file_data items[2] = {{.filename = "clip.mp4", .parent_id = "F", .entry = &entries[0]},
					   {.filename = "CLIP.mp4", .parent_id = "F", .entry = &entries[1]}};
	struct media_file_data rescanned[2] = {{.filename = "a.mp4", .parent_id = "F", .entry = &rescanned_entries[0]},
					       {.filename = "b.mp4", .parent_id = "F", .entry = &rescanned_entries[1]}};
	DARRAY(struct media_file_data) rows;
	DARRAY(struct media_file_data) rescanned_rows;
	da_init(rows);
	da_init(rescanned_rows);
	struct media_file_data row = {.id = "F", .is_folder = true};
	row.folder_items.array = items;
	row.folder_items.num = 2;
	da_push_back(rows, &row);
	struct media_file_data rescanned_row = row;
	rescanned_row.folder_items.array = rescanned;
	da_push_back(rescanned_rows, &rescanned_row);

	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);
	shuffler_update_files(&shuffler, &rows.da);
	shuffler_select(&shuffler, &items[1]);
	assert(shuffler.head == 1 && shuffler.shuffled_files.array[0] == &items[1]);

	/* the second CLIP.MP4 is still the selected one, by occurrence, though no file name matches */
	shuffler_update_files(&shuffler, &rescanned_rows.da);
	assert(shuffler.head == 1 && shuffler.shuffled_files.array[0] == &rescanned[1]);
	assert_next_in_cycle(&shuffler);

	/* an item without an entry on either side is matched by file name, as upstream */
	rescanned[1].entry = NULL;
	struct media_file_data by_name[2] = {{.filename = "a.mp4", .parent_id = "F"},
					     {.filename = "b.mp4", .parent_id = "F"}};
	DARRAY(struct media_file_data) by_name_rows;
	da_init(by_name_rows);
	struct media_file_data by_name_row = row;
	by_name_row.folder_items.array = by_name;
	da_push_back(by_name_rows, &by_name_row);
	shuffler_update_files(&shuffler, &by_name_rows.da);
	assert(shuffler.head == 1 && shuffler.shuffled_files.array[0] == &by_name[1]);
	da_free(by_name_rows);

	shuffler_destroy(&shuffler);
	da_free(rows);
	da_free(rescanned_rows);
}

/* A new row for the randomized test: a file row, or a folder row with up to three items */
static void add_random_row(struct darray *rows, size_t *last_id)
{
	DARRAY(struct media_file_data) list;
	struct media_file_data row = {0};
	struct dstr id = {0};
	list.da = *rows;
	dstr_printf(&id, "%zu", ++*last_id);
	row.id = id.array;
	da_init(row.folder_items);
	if (rand() % 3 == 0) {
		row.is_folder = true;
		for (int n = rand() % 4; n > 0; n--) {
			struct media_file_data item = {0};
			struct dstr filename = {0};
			dstr_printf(&filename, "item%zu", ++*last_id);
			item.filename = filename.array;
			item.parent_id = row.id;
			da_push_back(row.folder_items, &item);
		}
	}
	da_push_back(list, &row);
	*rows = list.da;
}

/* A kept row in a list edit, with new folder item storage as mps_update builds it; the folder may lose a file
 * or gain one */
static struct media_file_data copy_row(const struct media_file_data *row, size_t *last_id)
{
	struct media_file_data copy = *row;
	da_init(copy.folder_items);
	for (size_t j = 0; j < row->folder_items.num; j++) {
		if (rand() % 8 == 0)
			continue;
		struct media_file_data item = row->folder_items.array[j];
		item.filename = bstrdup(item.filename);
		da_push_back(copy.folder_items, &item);
	}
	if (row->is_folder && rand() % 8 == 0) {
		struct media_file_data item = {0};
		struct dstr filename = {0};
		dstr_printf(&filename, "item%zu", ++*last_id);
		item.filename = filename.array;
		item.parent_id = copy.id;
		da_push_back(copy.folder_items, &item);
	}
	return copy;
}

static void free_folder_items(struct media_file_data *row)
{
	for (size_t j = 0; j < row->folder_items.num; j++)
		bfree(row->folder_items.array[j].filename);
	da_free(row->folder_items);
}

/* The playable item at flattened position index: a file row or a folder item */
static struct media_file_data *flattened_item(struct darray *rows, size_t index)
{
	DARRAY(struct media_file_data) list;
	list.da = *rows;
	for (size_t i = 0; i < list.num; i++) {
		struct media_file_data *row = &list.array[i];
		size_t count = row->is_folder ? row->folder_items.num : 1;
		if (index < count)
			return row->is_folder ? &row->folder_items.array[index] : row;
		index -= count;
	}
	return NULL;
}

static size_t flattened_count(struct darray *rows)
{
	DARRAY(struct media_file_data) list;
	size_t count = 0;
	list.da = *rows;
	for (size_t i = 0; i < list.num; i++)
		count += list.array[i].is_folder ? list.array[i].folder_items.num : 1;
	return count;
}

static bool is_list_item(struct darray *rows, const struct media_file_data *item)
{
	for (size_t i = 0, count = flattened_count(rows); i < count; i++) {
		if (flattened_item(rows, i) == item)
			return true;
	}
	return false;
}

/* R-UP-73, R-UP-75, R-UP-76: random edits of short lists of file and folder rows (each followed by a loop setting
 * and, usually, a selection, as in mps_update), selections, next and previous keep the invariant, and next and
 * previous return list items */
static void test_random_operations_keep_invariant(void)
{
	struct shuffler shuffler;
	DARRAY(struct media_file_data) rows;
	size_t last_id = 0;
	shuffler_init(&shuffler);
	da_init(rows);
	srand(20261024);

	for (int step = 0; step < 50000; step++) {
		int op = rand() % 8;
		size_t playable = flattened_count(&rows.da);
		if (op < 2 || !playable) {
			/* keep each row with probability 3/4, then add up to two new ones */
			DARRAY(struct media_file_data) edited;
			da_init(edited);
			for (size_t i = 0; i < rows.num; i++) {
				if (rand() % 4) {
					struct media_file_data copy = copy_row(&rows.array[i], &last_id);
					da_push_back(edited, &copy);
				}
			}
			for (int added = rand() % 3; added > 0; added--)
				add_random_row(&edited.da, &last_id);

			if (rand() % 8 == 0)
				shuffler_reshuffle(&shuffler); /* shuffle turned on again */
			shuffler_set_loop(&shuffler, rand() % 4 != 0);
			shuffler_update_files(&shuffler, &edited.da);
			assert_next_in_cycle(&shuffler);
			/* the shuffler now points into the edited list; the old rows' folder items and the dropped rows'
			 * ids are unused */
			for (size_t i = 0; i < rows.num; i++) {
				bool kept = false;
				for (size_t j = 0; j < edited.num && !kept; j++)
					kept = edited.array[j].id == rows.array[i].id;
				if (!kept)
					bfree(rows.array[i].id);
				free_folder_items(&rows.array[i]);
			}
			da_free(rows);
			rows.da = edited.da;
			/* mps_update selects the current item, except for an edited current row or an empty current
			 * folder */
			playable = flattened_count(&rows.da);
			if (playable && rand() % 4)
				shuffler_select(&shuffler, flattened_item(&rows.da, rand() % playable));
		} else if (op < 5) {
			if (shuffler_has_next(&shuffler)) {
				struct media_file_data *item = shuffler_next(&shuffler);
				assert(is_list_item(&rows.da, item));
			}
		} else if (op < 7) {
			if (shuffler_has_prev(&shuffler)) {
				struct media_file_data *item = shuffler_prev(&shuffler);
				assert(is_list_item(&rows.da, item));
			}
		} else {
			/* Play Selected File */
			shuffler_select(&shuffler, flattened_item(&rows.da, rand() % playable));
		}
		assert_invariant(&shuffler);
	}

	shuffler_destroy(&shuffler);
	for (size_t i = 0; i < rows.num; i++) {
		bfree(rows.array[i].id);
		free_folder_items(&rows.array[i]);
	}
	da_free(rows);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_shuffler();
	printf("upstream shuffler self-tests: passed\n");

	printf("test_select_last_history_item\n");
	test_select_last_history_item();
	printf("test_select_with_history_zero\n");
	test_select_with_history_zero();
	printf("test_update_files_with_history_zero\n");
	test_update_files_with_history_zero();
	printf("test_update_files_with_overlapping_ranges\n");
	test_update_files_with_overlapping_ranges();
	printf("test_update_files_without_select_keeps_next_in_range\n");
	test_update_files_without_select_keeps_next_in_range();
	printf("test_update_files_matches_keys\n");
	test_update_files_matches_keys();
	printf("test_identity_update_keeps_valid_next\n");
	test_identity_update_keeps_valid_next();
	printf("test_random_operations_keep_invariant\n");
	test_random_operations_keep_invariant();
	printf("obs-playlister shuffler regression tests: passed\n");
	return 0;
}
