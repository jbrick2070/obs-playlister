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

#include "modes.h"

#include <util/bmem.h>

#include "sort.h"

/* Newest first */
static int compare_newness(size_t ia, size_t ib, void *context)
{
	const struct pl_entry *entries = context;
	const struct pl_entry *a = &entries[ia];
	const struct pl_entry *b = &entries[ib];
	if (a->has_date != b->has_date)
		return a->has_date ? -1 : 1;
	if (a->has_date && a->date_unix_ms != b->date_unix_ms)
		return a->date_unix_ms > b->date_unix_ms ? -1 : 1;
	return (a->source_ordinal < b->source_ordinal) - (a->source_ordinal > b->source_ordinal);
}

size_t pl_mode_select(const struct pl_entry *entries, size_t count, enum pl_mode mode, uint32_t newest_count,
		      size_t *visible)
{
	size_t keep = count;
	if (mode == PL_MODE_NEWEST_ONLY)
		keep = 1;
	else if (mode == PL_MODE_NEWEST_N)
		keep = newest_count ? newest_count : 1;

	if (keep >= count) {
		for (size_t i = 0; i < count; i++)
			visible[i] = i;
		return count;
	}

	size_t *by_newness = bmalloc(count * sizeof(*by_newness));
	bool *kept = bzalloc(count * sizeof(*kept));
	for (size_t i = 0; i < count; i++)
		by_newness[i] = i;
	pl_sort_indexes(by_newness, count, compare_newness, (void *)entries);
	for (size_t i = 0; i < keep; i++)
		kept[by_newness[i]] = true;

	size_t visible_count = 0;
	for (size_t i = 0; i < count; i++) {
		if (kept[i])
			visible[visible_count++] = i;
	}
	bfree(kept);
	bfree(by_newness);
	return visible_count;
}
