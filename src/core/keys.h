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
#include <stdint.h>

#include "entry.h"

/* R-KEY-1: entries with identical keys (true duplicates, or names that differ only by case in a case-sensitive
 * directory) are told apart by occurrence, 1, 2, ... in source order (source_ordinal), assigned before any display
 * sort. The key string itself is never changed. Keys arrive already folded (the folder provider upper-cases them
 * at the Win32 boundary); they compare byte for byte here, and a missing key counts as an empty one. */
void pl_assign_occurrences(struct pl_entry *entries, size_t count);

/* R-UP-20: what Select File stores, "<row_uuid>:<occurrence>:<item_key>", so a list that changes while Properties is
 * open cannot make the button play another item. A file row is "<row_uuid>:0:"; an item of a container row has its
 * occurrence (1 or more) and key. The text splits at its first two colons, so a key keeps colons of its own; a row
 * uuid has none (OBS writes hex digits and hyphens). */
struct pl_selection {
	char *row_uuid;      /* owned (bmem) */
	uint32_t occurrence; /* 0 for a file row */
	char *item_key;      /* owned (bmem); "" for a file row */
};

/* The identity of a file row (occurrence 0, item_key NULL) or of a container item; owned (bmem) */
char *pl_selection_format(const char *row_uuid, uint32_t occurrence, const char *item_key);

/* False, with nothing allocated, when the text is no identity: fewer than two colons, an empty row uuid, an
 * occurrence that is not a decimal number from 0 to 4294967295, a file row with a key or an item without one */
bool pl_selection_parse(struct pl_selection *selection, const char *text);
void pl_selection_free(struct pl_selection *selection);
