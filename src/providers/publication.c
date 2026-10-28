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

/* Moved from provider-folder-win.c (Phase 3a step 3) so playlists publish exactly as folders do (R-REM-7) */

#include "publication.h"

#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "core/merge.h"
#include "core/modes.h"
#include "core/sort.h"

void pl_publication_init(struct pl_publication *publication, uint64_t instance_id, pl_publish_fn publish, void *owner)
{
	memset(publication, 0, sizeof(*publication));
	publication->instance_id = instance_id;
	publication->publish = publish;
	publication->owner = owner;
}

static void free_entry_array(struct pl_entry **entries, size_t *count)
{
	pl_entries_free(*entries, *count);
	*entries = NULL;
	*count = 0;
}

void pl_publication_free(struct pl_publication *publication)
{
	bfree(publication->last_signature);
	free_entry_array(&publication->published, &publication->published_count);
	bfree(publication->published_content);
	free_entry_array(&publication->held, &publication->held_count);
	bfree(publication->held_content);
	memset(publication, 0, sizeof(*publication));
}

struct pl_entry *pl_publication_copy_entries(const struct pl_entry *entries, size_t count, bool keep_renames)
{
	struct pl_entry *copy = bzalloc((count ? count : 1) * sizeof(*copy));
	for (size_t i = 0; i < count; i++) {
		pl_entry_copy(&copy[i], &entries[i]);
		if (!keep_renames) {
			bfree(copy[i].previous_key);
			copy[i].previous_key = NULL;
		}
	}
	return copy;
}

/* An entry as text: its identity (R-KEY-1) and its version */
struct identity {
	char *id;
	char *version;
};

static int compare_identities(const void *a, const void *b)
{
	return strcmp(((const struct identity *)a)->id, ((const struct identity *)b)->id);
}

/* Sorted by identity, so the order a listing happens to give (FAT, exFAT, shares) changes nothing */
static struct identity *identities(const struct pl_entry *entries, size_t count)
{
	struct identity *result = bmalloc((count ? count : 1) * sizeof(*result));
	for (size_t i = 0; i < count; i++) {
		struct dstr id = {0};
		struct dstr version = {0};
		dstr_printf(&id, "%s\x1f%u", entries[i].key, entries[i].occurrence);
		dstr_printf(&version, "%llu\x1f%lld\x1f%s", (unsigned long long)entries[i].size,
			    (long long)entries[i].date_unix_ms, entries[i].location);
		result[i].id = id.array;
		result[i].version = version.array;
	}
	qsort(result, count, sizeof(*result), compare_identities);
	return result;
}

static void free_identities(struct identity *identities, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		bfree(identities[i].id);
		bfree(identities[i].version);
	}
	bfree(identities);
}

/* What makes two memberships equal (R-DIR-8, R-BEH-5): the entries and their versions, not the view */
static char *content_digest(const struct pl_entry *entries, size_t count)
{
	struct identity *sorted = identities(entries, count);
	/* never NULL, as dstr_copy of "" leaves it, so an empty membership compares like any other */
	struct dstr digest = {0};
	dstr_copy(&digest, "membership\n");
	for (size_t i = 0; i < count; i++)
		dstr_catf(&digest, "%s\t%s\n", sorted[i].id, sorted[i].version);
	free_identities(sorted, count);
	return digest.array;
}

/* Entries one membership has and the other lacks, and entries whose version changed (a rewrite): the row status "N
 * changes waiting" (R-DIR-11) */
static size_t count_changes(const struct pl_entry *a, size_t a_count, const struct pl_entry *b, size_t b_count)
{
	struct identity *x = identities(a, a_count);
	struct identity *y = identities(b, b_count);
	size_t i = 0, j = 0, changes = 0;
	while (i < a_count || j < b_count) {
		int order = i == a_count ? 1 : j == b_count ? -1 : strcmp(x[i].id, y[j].id);
		changes += order != 0 || strcmp(x[i].version, y[j].version) != 0;
		i += order <= 0;
		j += order >= 0;
	}
	free_identities(x, a_count);
	free_identities(y, b_count);
	return changes;
}

/* Builds the published form of a membership: its own copy of the entries in display order (R-DIR-6), the mode's
 * visible list (R-DIR-7), the pending keys and the row status */
static struct pl_snapshot *build_snapshot(struct pl_publication *publication,
					  const struct pl_container_options *options, const struct pl_entry *raw,
					  size_t count, const struct pl_publication_status *status)
{
	struct pl_snapshot *snapshot = bzalloc(sizeof(*snapshot));
	snapshot->instance_id = publication->instance_id;
	snapshot->entries = pl_publication_copy_entries(raw, count, true);
	snapshot->count = count;
	pl_sort_entries(snapshot->entries, count, options->sort_key, options->sort_descending);
	snapshot->visible = bmalloc((count ? count : 1) * sizeof(size_t));
	snapshot->visible_count =
		pl_mode_select(snapshot->entries, count, options->mode, options->newest_count, snapshot->visible);
	snapshot->initial_scan_complete = status->listed;
	snapshot->pending_keys = bmalloc((status->pending_count ? status->pending_count : 1) * sizeof(char *));
	for (size_t i = 0; i < status->pending_count; i++)
		snapshot->pending_keys[i] = bstrdup(status->pending_keys[i]);
	snapshot->pending_count = status->pending_count;
	snapshot->level = status->level;
	snapshot->status_message = status->message ? bstrdup(status->message) : NULL;
	snapshot->stale = status->stale;
	snapshot->last_ok_unix_ms = status->last_ok_unix_ms;
	snapshot->held_changes = (size_t)os_atomic_load_long(&publication->held_changes);
	return snapshot;
}

/* Everything the receiver can see, to publish only what changed */
static char *snapshot_signature(const struct pl_snapshot *snapshot)
{
	struct dstr signature = {0};
	dstr_copy(&signature, "snapshot\n");
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct pl_entry *entry = &snapshot->entries[i];
		dstr_catf(&signature, "%s\t%s\t%u\t%llu\t%lld\t%s\t%s\t%d\n", entry->location, entry->key,
			  entry->occurrence, (unsigned long long)entry->size, (long long)entry->date_unix_ms,
			  entry->previous_key ? entry->previous_key : "", entry->title ? entry->title : "",
			  (int)entry->kind);
	}
	dstr_cat(&signature, "visible");
	for (size_t i = 0; i < snapshot->visible_count; i++)
		dstr_catf(&signature, " %zu", snapshot->visible[i]);
	dstr_catf(&signature, "\npending %zu\nheld %zu\nlevel %d %s\nstale %d initial %d\n", snapshot->pending_count,
		  snapshot->held_changes, (int)snapshot->level,
		  snapshot->status_message ? snapshot->status_message : "", snapshot->stale,
		  snapshot->initial_scan_complete);
	return signature.array;
}

/* Publishes this membership if anything the receiver sees changed (or when forced); it becomes the published one.
 * Takes raw and content. */
static bool publish_membership(struct pl_publication *publication, const struct pl_container_options *options,
			       struct pl_entry *raw, size_t count, char *content,
			       const struct pl_publication_status *status, bool force)
{
	struct pl_snapshot *snapshot = build_snapshot(publication, options, raw, count, status);
	char *signature = snapshot_signature(snapshot);
	if (!force && publication->last_signature && strcmp(signature, publication->last_signature) == 0) {
		bfree(signature);
		pl_snapshot_free(snapshot);
		pl_entries_free(raw, count);
		bfree(content);
		return false;
	}
	if (!publication->published_content || strcmp(content, publication->published_content) != 0)
		publication->content_revision++;
	snapshot->content_revision = publication->content_revision;
	snapshot->generation = ++publication->generation;
	bfree(publication->last_signature);
	publication->last_signature = signature;

	/* renames are published once; the kept membership has none */
	free_entry_array(&publication->published, &publication->published_count);
	publication->published = pl_publication_copy_entries(raw, count, false);
	publication->published_count = count;
	pl_entries_free(raw, count);
	bfree(publication->published_content);
	publication->published_content = content;

	publication->publish(publication->owner, snapshot);
	return true;
}

enum pl_publication_result pl_publication_update(struct pl_publication *publication,
						 const struct pl_container_options *options, struct pl_entry *raw,
						 size_t count, const struct pl_publication_status *status, bool release)
{
	enum pl_publication_result result;
	char *content = content_digest(raw, count);

	/* the list taken at start is the first listing once the files it found being written are in; the lock holds
	 * changes from then on (not an error published before any listing, nor a start with files still in the gate) */
	bool locked = options->lock_list_at_start && publication->lock_ready &&
		      strcmp(content, publication->published_content) != 0;
	if (locked && !release) {
		/* renames the kept membership carried and this one does not compose (A to B kept, then B to C) */
		if (publication->held)
			pl_merge_compose_renames(raw, count, publication->held, publication->held_count);
		free_entry_array(&publication->held, &publication->held_count);
		bfree(publication->held_content);
		publication->held = raw;
		publication->held_count = count;
		publication->held_content = content;
		os_atomic_set_long(&publication->held_changes,
				   (long)count_changes(publication->published, publication->published_count,
						       publication->held, publication->held_count));
		publish_membership(
			publication, options,
			pl_publication_copy_entries(publication->published, publication->published_count, false),
			publication->published_count, bstrdup(publication->published_content), status, false);
		result = PL_PUBLICATION_KEPT;
	} else {
		/* a release publishes even an unchanged view; with nothing kept there is nothing to force */
		bool released = release && publication->held;
		if (publication->held)
			pl_merge_compose_renames(raw, count, publication->held, publication->held_count);
		free_entry_array(&publication->held, &publication->held_count);
		bfree(publication->held_content);
		publication->held_content = NULL;
		os_atomic_set_long(&publication->held_changes, 0);
		result = publish_membership(publication, options, raw, count, content, status, released)
				 ? PL_PUBLICATION_PUBLISHED
				 : PL_PUBLICATION_UNCHANGED;
	}
	if (status->lock_point && !status->pending_count && publication->published_content)
		publication->lock_ready = true;
	return result;
}

size_t pl_publication_held_changes(struct pl_publication *publication)
{
	return (size_t)os_atomic_load_long(&publication->held_changes);
}
