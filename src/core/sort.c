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

#include "sort.h"

#include <string.h>
#include <util/bmem.h>

#include "natural.h"

static void merge_sort(size_t *indexes, size_t *scratch, size_t count, pl_index_compare_fn compare, void *context)
{
	if (count < 2)
		return;
	size_t half = count / 2;
	merge_sort(indexes, scratch, half, compare, context);
	merge_sort(indexes + half, scratch, count - half, compare, context);

	size_t left = 0;
	size_t right = half;
	size_t out = 0;
	while (left < half && right < count) {
		/* <= keeps equal elements in their order, so the sort is stable */
		if (compare(indexes[left], indexes[right], context) <= 0)
			scratch[out++] = indexes[left++];
		else
			scratch[out++] = indexes[right++];
	}
	while (left < half)
		scratch[out++] = indexes[left++];
	while (right < count)
		scratch[out++] = indexes[right++];
	memcpy(indexes, scratch, count * sizeof(*indexes));
}

void pl_sort_indexes(size_t *indexes, size_t count, pl_index_compare_fn compare, void *context)
{
	if (count < 2)
		return;
	size_t *scratch = bmalloc(count * sizeof(*scratch));
	merge_sort(indexes, scratch, count, compare, context);
	bfree(scratch);
}

struct sort_context {
	const struct pl_entry *entries;
	enum pl_sort_key key;
	bool descending;
};

static int compare_ordinals(const struct pl_entry *a, const struct pl_entry *b)
{
	return (a->source_ordinal > b->source_ordinal) - (a->source_ordinal < b->source_ordinal);
}

static int compare_entries(size_t ia, size_t ib, void *context)
{
	const struct sort_context *sort = context;
	const struct pl_entry *a = &sort->entries[ia];
	const struct pl_entry *b = &sort->entries[ib];
	int order = 0;

	switch (sort->key) {
	case PL_SORT_DATE:
		if (a->has_date != b->has_date) {
			/* dateless entries after the dated ones ascending, before them descending */
			order = a->has_date ? -1 : 1;
			return sort->descending ? -order : order;
		}
		if (a->has_date && a->date_unix_ms != b->date_unix_ms)
			order = a->date_unix_ms < b->date_unix_ms ? -1 : 1;
		break;
	case PL_SORT_NAME:
		order = pl_natural_compare(a->location, b->location);
		break;
	case PL_SORT_LIST:
		order = compare_ordinals(a, b);
		break;
	}
	if (order)
		return sort->descending ? -order : order;
	/* ties keep list order in both directions */
	return compare_ordinals(a, b);
}

void pl_sort_entries(struct pl_entry *entries, size_t count, enum pl_sort_key key, bool descending)
{
	if (count < 2)
		return;
	struct sort_context context = {entries, key, descending};
	size_t *order = bmalloc(count * sizeof(*order));
	for (size_t i = 0; i < count; i++)
		order[i] = i;
	pl_sort_indexes(order, count, compare_entries, &context);

	struct pl_entry *sorted = bmalloc(count * sizeof(*sorted));
	for (size_t i = 0; i < count; i++)
		sorted[i] = entries[order[i]];
	memcpy(entries, sorted, count * sizeof(*entries));
	bfree(sorted);
	bfree(order);
}

bool pl_entries_have_dates(const struct pl_entry *entries, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (entries[i].has_date)
			return true;
	}
	return false;
}
