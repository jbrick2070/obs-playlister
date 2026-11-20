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
#include <stdlib.h>
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
	/* R-UP-85: only entries with a previous key can rename anything. Most snapshots carry none, so this returns at
	 * once; otherwise only those few are searched (a shallow copy: the strings stay the snapshot's). */
	size_t with_previous = 0;
	for (size_t r = 0; r < renamed_count; r++) {
		if (renamed[r].previous_key)
			with_previous++;
	}
	if (!with_previous)
		return 0;
	struct pl_entry *renames = bmalloc(with_previous * sizeof(*renames));
	size_t n = 0;
	for (size_t r = 0; r < renamed_count; r++) {
		if (renamed[r].previous_key)
			renames[n++] = renamed[r];
	}

	size_t rekeyed = 0;
	for (size_t o = 0; o < old_count; o++) {
		/* the old key decides, never one this pass already set, so renames of one snapshot apply at once */
		const char *old_key = old_entries[o].key;
		const struct pl_entry *rename = renamed_from(renames, n, old_key);
		if (rename) {
			char *new_key = bstrdup(rename->key);
			bfree(old_entries[o].key);
			old_entries[o].key = new_key;
			rekeyed++;
		} else if (old_key && is_rename_target(renames, n, old_key)) {
			/* overwritten by a rename: this file is gone */
			bfree(old_entries[o].key);
			old_entries[o].key = NULL;
		}
	}
	bfree(renames);
	return rekeyed;
}

/* A key and what it stands for, sorted for the look-ups of pl_merge_compose_renames */
struct key_ref {
	const char *key;   /* never NULL */
	const char *value; /* a held entry's previous_key; unused for the newer renames */
	size_t place;      /* in its list: among equal keys the first one wins, as a scan from the start finds it */
};

static int compare_key_refs(const void *a, const void *b)
{
	const struct key_ref *x = a, *y = b;
	int order = strcmp(x->key, y->key);
	return order ? order : (x->place > y->place) - (x->place < y->place);
}

/* The first reference with this key, or NULL */
static const struct key_ref *find_key_ref(const struct key_ref *refs, size_t count, const char *key)
{
	if (!key)
		key = "";
	size_t low = 0;
	size_t high = count;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (strcmp(refs[middle].key, key) < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return low < count && strcmp(refs[low].key, key) == 0 ? &refs[low] : NULL;
}

void pl_merge_compose_renames(struct pl_entry *newer, size_t newer_count, const struct pl_entry *held,
			      size_t held_count)
{
	if (!newer_count)
		return;
	/* Only entries with a previous key can change anything here. Mostly neither snapshot has one (a locked list
	 * that only gains and loses files), and this returns at once, as pl_merge_rekey does; it used to compare every
	 * newer entry with every newer and every held one, on each update of a locked list. */
	size_t held_renames = 0;
	for (size_t h = 0; h < held_count; h++)
		held_renames += held[h].previous_key != NULL;
	size_t newer_renames = 0;
	for (size_t n = 0; n < newer_count; n++)
		newer_renames += newer[n].previous_key != NULL;
	if (!held_renames && !newer_renames)
		return;

	/* the few renames, sorted once: held ones by the key they lead to, newer ones by the key they come from */
	struct key_ref *held_by_key = bmalloc((held_renames ? held_renames : 1) * sizeof(*held_by_key));
	struct key_ref *newer_by_previous = bmalloc((newer_renames ? newer_renames : 1) * sizeof(*newer_by_previous));
	size_t count = 0;
	for (size_t h = 0; h < held_count; h++) {
		if (held[h].previous_key)
			held_by_key[count++] =
				(struct key_ref){held[h].key ? held[h].key : "", held[h].previous_key, h};
	}
	count = 0;
	for (size_t n = 0; n < newer_count; n++) {
		if (newer[n].previous_key)
			newer_by_previous[count++] = (struct key_ref){newer[n].previous_key, NULL, n};
	}
	qsort(held_by_key, held_renames, sizeof(*held_by_key), compare_key_refs);
	qsort(newer_by_previous, newer_renames, sizeof(*newer_by_previous), compare_key_refs);

	/* decided from the newer snapshot as it came, before any previous_key below is rewritten */
	const char **published = bzalloc(newer_count * sizeof(*published));
	for (size_t n = 0; held_renames && n < newer_count; n++) {
		const struct pl_entry *entry = &newer[n];
		const struct key_ref *kept = NULL;
		if (entry->previous_key) {
			/* renamed again: the key it had at the last publication, if a held snapshot renamed it before */
			kept = find_key_ref(held_by_key, held_renames, entry->previous_key);
		} else if (!find_key_ref(newer_by_previous, newer_renames, entry->key)) {
			/* not renamed now: it keeps a held rename, unless the newer snapshot renamed that key away, in
			 * which case the file now under the key is new */
			kept = find_key_ref(held_by_key, held_renames, entry->key);
		}
		published[n] = kept ? kept->value : NULL;
	}
	bfree(held_by_key);
	bfree(newer_by_previous);
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
