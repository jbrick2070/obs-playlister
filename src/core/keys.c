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

#include "keys.h"

#include <string.h>
#include <util/bmem.h>

#include "sort.h"

static const char *key_of(const struct pl_entry *entry)
{
	return entry->key ? entry->key : "";
}

/* By key, then source order */
static int compare_keys(size_t ia, size_t ib, void *context)
{
	const struct pl_entry *entries = context;
	int order = strcmp(key_of(&entries[ia]), key_of(&entries[ib]));
	if (order)
		return order;
	return (entries[ia].source_ordinal > entries[ib].source_ordinal) -
	       (entries[ia].source_ordinal < entries[ib].source_ordinal);
}

void pl_assign_occurrences(struct pl_entry *entries, size_t count)
{
	if (!count)
		return;
	size_t *order = bmalloc(count * sizeof(*order));
	for (size_t i = 0; i < count; i++)
		order[i] = i;
	pl_sort_indexes(order, count, compare_keys, entries);

	uint32_t occurrence = 0;
	for (size_t i = 0; i < count; i++) {
		bool same = i > 0 && strcmp(key_of(&entries[order[i]]), key_of(&entries[order[i - 1]])) == 0;
		occurrence = same ? occurrence + 1 : 1;
		entries[order[i]].occurrence = occurrence;
	}
	bfree(order);
}
