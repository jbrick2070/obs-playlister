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

#include "merge.h"

#include <stdbool.h>
#include <string.h>
#include <util/bmem.h>

static bool same_key(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool is_item(const struct pl_entry *entry, struct pl_item_id id)
{
	return entry->occurrence == id.occurrence && same_key(entry->key, id.key);
}

bool pl_merge_follow_rename(struct pl_item_id *id, const struct pl_entry *renamed, size_t renamed_count)
{
	for (size_t r = 0; r < renamed_count; r++) {
		if (renamed[r].previous_key && same_key(renamed[r].previous_key, id->key)) {
			id->key = renamed[r].key;
			id->occurrence = renamed[r].occurrence;
			return true;
		}
	}
	return false;
}

enum pl_current_fate pl_merge_current_fate(const struct pl_entry *entries, size_t count, const size_t *visible,
					   size_t visible_count, struct pl_item_id current, size_t *visible_position)
{
	size_t unused_position;
	if (!visible_position)
		visible_position = &unused_position;
	*visible_position = PL_MERGE_NONE;
	for (size_t i = 0; i < visible_count; i++) {
		if (is_item(&entries[visible[i]], current)) {
			*visible_position = i;
			return PL_CURRENT_VISIBLE;
		}
	}
	for (size_t i = 0; i < count; i++) {
		if (is_item(&entries[i], current))
			return PL_CURRENT_EVICTED;
	}
	return PL_CURRENT_DELETED;
}

size_t pl_merge_successor(const struct pl_item_id *previous, size_t previous_count, size_t current_position,
			  const struct pl_entry *entries, const size_t *visible, size_t visible_count)
{
	if (current_position >= previous_count)
		return PL_MERGE_NONE;
	for (size_t p = current_position + 1; p < previous_count; p++) {
		for (size_t i = 0; i < visible_count; i++) {
			if (is_item(&entries[visible[i]], previous[p]))
				return i;
		}
	}
	return PL_MERGE_NONE;
}

/* The renamed entry whose previous_key is key, or NULL */
static const struct pl_entry *renamed_from(const struct pl_entry *renamed, size_t renamed_count, const char *key)
{
	for (size_t r = 0; r < renamed_count; r++) {
		if (renamed[r].previous_key && same_key(renamed[r].previous_key, key))
			return &renamed[r];
	}
	return NULL;
}

static bool is_rename_target(const struct pl_entry *renamed, size_t renamed_count, const char *key)
{
	for (size_t r = 0; r < renamed_count; r++) {
		if (renamed[r].previous_key && same_key(renamed[r].key, key))
			return true;
	}
	return false;
}

size_t pl_merge_rekey(struct pl_entry *old_entries, size_t old_count, const struct pl_entry *renamed,
		      size_t renamed_count)
{
	size_t rekeyed = 0;
	for (size_t o = 0; o < old_count; o++) {
		/* the old key decides, never one this pass already set, so renames of one snapshot apply at once */
		const char *old_key = old_entries[o].key;
		const struct pl_entry *rename = renamed_from(renamed, renamed_count, old_key);
		if (rename) {
			char *new_key = bstrdup(rename->key);
			bfree(old_entries[o].key);
			old_entries[o].key = new_key;
			rekeyed++;
		} else if (old_key && is_rename_target(renamed, renamed_count, old_key)) {
			/* overwritten by a rename: this file is gone */
			bfree(old_entries[o].key);
			old_entries[o].key = NULL;
		}
	}
	return rekeyed;
}

/* The previous_key a held entry carried for this key, or NULL */
static const char *held_previous_key(const struct pl_entry *held, size_t held_count, const char *key)
{
	for (size_t h = 0; h < held_count; h++) {
		if (held[h].previous_key && same_key(held[h].key, key))
			return held[h].previous_key;
	}
	return NULL;
}

void pl_merge_compose_renames(struct pl_entry *newer, size_t newer_count, const struct pl_entry *held,
			      size_t held_count)
{
	if (!newer_count)
		return;
	/* decided from the newer snapshot as it came, before any previous_key below is rewritten */
	const char **published = bzalloc(newer_count * sizeof(*published));
	for (size_t n = 0; n < newer_count; n++) {
		const struct pl_entry *entry = &newer[n];
		if (entry->previous_key) {
			/* renamed again: the key it had at the last publication, if a held snapshot renamed it before */
			published[n] = held_previous_key(held, held_count, entry->previous_key);
		} else if (!renamed_from(newer, newer_count, entry->key)) {
			/* not renamed now: it keeps a held rename, unless the newer snapshot renamed that key away, in
			 * which case the file now under the key is new */
			published[n] = held_previous_key(held, held_count, entry->key);
		}
	}
	for (size_t n = 0; n < newer_count; n++) {
		struct pl_entry *entry = &newer[n];
		if (published[n]) {
			char *copy = bstrdup(published[n]);
			bfree(entry->previous_key);
			entry->previous_key = copy;
		}
		if (entry->previous_key && same_key(entry->previous_key, entry->key)) {
			bfree(entry->previous_key);
			entry->previous_key = NULL;
		}
	}
	bfree((void *)published);
}
