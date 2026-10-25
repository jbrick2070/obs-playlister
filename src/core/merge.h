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

/* What a container row does with a new snapshot while an item plays (R-DIR-8): where the playing item stands, which
 * item follows a deleted one, and how renames carry an item's identity. The caller holds the list lock; these
 * functions only look at plain data. */

/* An item's identity within a row (R-KEY-1) */
struct pl_item_id {
	const char *key;
	uint32_t occurrence;
};

enum pl_current_fate {
	PL_CURRENT_VISIBLE, /* still in the visible list */
	PL_CURRENT_EVICTED, /* still a member (it passed the filter) but the mode left it out, as newest-only does */
	PL_CURRENT_DELETED, /* no longer a member: deleted, moved away or filtered out */
};

#define PL_MERGE_NONE ((size_t)-1)

/* A rename carried to an identity the source keeps (the current item, the resume point): when an entry of the
 * snapshot was renamed from id's key, id takes that entry's key and occurrence and the function returns true. The
 * key is borrowed from renamed. Call it before pl_merge_current_fate, which would see the old key as deleted. */
bool pl_merge_follow_rename(struct pl_item_id *id, const struct pl_entry *renamed, size_t renamed_count);

/* Where the current item stands in a snapshot: entries are every filtered entry (the membership), visible the
 * indexes the mode kept. With PL_CURRENT_VISIBLE, *visible_position (may be NULL) is its index in visible, else
 * PL_MERGE_NONE. */
enum pl_current_fate pl_merge_current_fate(const struct pl_entry *entries, size_t count, const size_t *visible,
					   size_t visible_count, struct pl_item_id current, size_t *visible_position);

/* After the current item was deleted: the first item that followed it in the previous visible order and is still
 * visible, as an index into the new visible list. previous is the previous visible order and current_position the
 * deleted item's place in it (out of range gives PL_MERGE_NONE). previous holds keys as they are after this
 * snapshot's renames (built after pl_merge_rekey, its keys borrowed from the re-keyed entries), so a renamed
 * successor is found. PL_MERGE_NONE when nothing after it survives; the caller then wraps (loop on) or ends the
 * list, as at the end of a list. */
size_t pl_merge_successor(const struct pl_item_id *previous, size_t previous_count, size_t current_position,
			  const struct pl_entry *entries, const size_t *visible, size_t visible_count);

/* Renames, before the shuffler matches old items with new ones: every old entry whose key is the previous_key of a
 * renamed entry takes that entry's key. All renames of one snapshot apply at once, so a swap (A to B and B to A)
 * swaps and a chain (A to B, B to C) moves both. An old entry that a rename overwrote (its key is a rename's new
 * key, and it was not renamed itself) loses its key, so it matches nothing and leaves like a deleted one. Returns
 * how many old entries were re-keyed. */
size_t pl_merge_rekey(struct pl_entry *old_entries, size_t old_count, const struct pl_entry *renamed,
		      size_t renamed_count);

/* R-DIR-11: a held snapshot is replaced by a newer one, and the renames the held one carried were never published.
 * Each newer entry's previous_key becomes the key the item had at the last publication: A to B (held) then B to C
 * gives C with previous_key A, an item the newer snapshot did not rename keeps the held rename (unless the newer
 * snapshot renamed that key away, in which case a file now under it is new), and an item renamed back to its
 * published key has no previous_key. */
void pl_merge_compose_renames(struct pl_entry *newer, size_t newer_count, const struct pl_entry *held,
			      size_t held_count);
