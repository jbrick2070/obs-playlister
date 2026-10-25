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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "entry.h"

/* The display order of a container's entries (R-DIR-6, R-DIR-10). Values match the sort_key setting. */
enum pl_sort_key {
	PL_SORT_LIST = 0, /* the order the container gave (source_ordinal) */
	PL_SORT_NAME = 1, /* natural order of the location, which within one container is its relative path */
	PL_SORT_DATE = 2, /* the default (R-DIR-12) */
};

/* Sorts in place. Every order is total: remaining ties go by source_ordinal, ascending in both directions, except
 * that list order descending is simply the reverse. Date order puts dated entries by date and dateless ones after
 * them ascending, before them descending, in list order either way (R-DIR-10). */
void pl_sort_entries(struct pl_entry *entries, size_t count, enum pl_sort_key key, bool descending);
/* Whether any entry has a date; without one, date order is list order and the row status says so (R-DIR-10) */
bool pl_entries_have_dates(const struct pl_entry *entries, size_t count);

/* A stable merge sort of indexes with a caller's comparison, for the core modules. compare returns a negative
 * number when the entry at index a goes first. */
typedef int (*pl_index_compare_fn)(size_t a, size_t b, void *context);
void pl_sort_indexes(size_t *indexes, size_t count, pl_index_compare_fn compare, void *context);
