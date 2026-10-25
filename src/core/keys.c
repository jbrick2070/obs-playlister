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

#include <inttypes.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

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

char *pl_selection_format(const char *row_uuid, uint32_t occurrence, const char *item_key)
{
	struct dstr text = {0};
	dstr_printf(&text, "%s:%" PRIu32 ":%s", row_uuid, occurrence, item_key ? item_key : "");
	return text.array;
}

bool pl_selection_parse(struct pl_selection *selection, const char *text)
{
	memset(selection, 0, sizeof(*selection));
	const char *first = strchr(text, ':');
	if (!first || first == text)
		return false;
	const char *second = strchr(first + 1, ':');
	if (!second || second == first + 1)
		return false;

	uint64_t occurrence = 0;
	for (const char *c = first + 1; c < second; c++) {
		if (*c < '0' || *c > '9')
			return false;
		occurrence = occurrence * 10 + (uint64_t)(*c - '0');
		if (occurrence > UINT32_MAX)
			return false;
	}
	const char *key = second + 1;
	if ((occurrence == 0) != (*key == '\0'))
		return false;

	selection->row_uuid = bstrdup_n(text, (size_t)(first - text));
	selection->occurrence = (uint32_t)occurrence;
	selection->item_key = bstrdup(key);
	return true;
}

void pl_selection_free(struct pl_selection *selection)
{
	bfree(selection->row_uuid);
	bfree(selection->item_key);
	memset(selection, 0, sizeof(*selection));
}
