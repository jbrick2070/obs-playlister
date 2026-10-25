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

/* R-UP-74 as the plugin is built: with NDEBUG, shuffler_select's assert that the item is in the list is compiled
 * out, so only its guard stands between a missing item and array[DARRAY_INVALID]. test-shuffler.c keeps asserts on
 * and cannot reach the guard, so this file builds shuffler.c with NDEBUG and checks without assert(). */

#ifndef NDEBUG
#define NDEBUG
#endif
#include "shuffler.c"

#include <stdio.h>
#include <windows.h>

static int failures = 0;

static void expect(bool ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

int main(void)
{
	/* a crash exits non-zero instead of opening a dialog */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	DARRAY(struct media_file_data) items;
	da_init(items);
	const char *ids[] = {"1", "2", "3"};
	for (size_t i = 0; i < 3; i++) {
		struct media_file_data item = {0};
		item.id = bstrdup(ids[i]);
		da_init(item.folder_items);
		da_push_back(items, &item);
	}

	struct shuffler shuffler;
	shuffler_init(&shuffler);
	shuffler_set_loop(&shuffler, true);
	shuffler_update_files(&shuffler, &items.da);
	shuffler_next(&shuffler);
	shuffler_next(&shuffler);

	size_t head = shuffler.head, next = shuffler.next, history = shuffler.history;
	struct media_file_data *order[3];
	for (size_t i = 0; i < 3; i++)
		order[i] = shuffler.shuffled_files.array[i];

	/* an item of an older list, as a stale pointer would be */
	struct media_file_data stranger = {0};
	stranger.id = "stranger";
	shuffler_select(&shuffler, &stranger);

	expect(shuffler.head == head && shuffler.next == next && shuffler.history == history,
	       "selecting an item that is not in the list leaves head, next and history alone");
	expect(shuffler.shuffled_files.num == 3, "selecting an item that is not in the list keeps every item");
	for (size_t i = 0; i < 3; i++)
		expect(shuffler.shuffled_files.array[i] == order[i],
		       "selecting an item that is not in the list keeps the order");
	expect(shuffler_has_next(&shuffler), "the shuffler still has a next item");
	struct media_file_data *after = shuffler_next(&shuffler);
	expect(after >= items.array && after < items.array + items.num, "next still returns a list item");

	shuffler_destroy(&shuffler);
	for (size_t i = 0; i < items.num; i++)
		bfree(items.array[i].id);
	da_free(items);

	if (failures)
		return 1;
	printf("shuffler release-build tests: passed\n");
	return 0;
}
