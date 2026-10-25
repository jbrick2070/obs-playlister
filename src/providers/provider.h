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

#include "core/entry.h"
#include "core/modes.h"
#include "core/row-kind.h"
#include "core/sort.h"

/* What a container row's provider publishes (PLAN section 3.2): an immutable list built on its worker, owned by the
 * receiver from the moment it is published */
struct pl_snapshot {
	uint64_t instance_id; /* the provider instance; the source accepts only its row's registered one (R-THR-2) */
	uint64_t generation;  /* orders snapshots within one instance only */
	uint64_t content_revision; /* bumps only when the entries changed; resets the full pass (R-BEH-5) */
	struct pl_entry *entries;  /* every filtered entry, in display order: the membership (R-DIR-8) */
	size_t count;
	size_t *visible; /* indexes into entries that the mode keeps (R-DIR-7), in display order */
	size_t visible_count;
	bool initial_scan_complete; /* settles an awaiting row (R-KEY-2) */
	char **pending_keys;        /* keys still in the write-complete gate (R-DIR-4) */
	size_t pending_count;
	enum pl_status_level level;
	char *status_message;    /* NULL when there is nothing to say */
	int64_t last_ok_unix_ms; /* the last successful listing, 0 = never (R-BEH-3) */
	bool stale;              /* the last good list kept after a failure (R-BEH-3) */
};

/* Frees the snapshot (bmem) and everything it holds; NULL is nothing to free */
void pl_snapshot_free(struct pl_snapshot *snapshot);

/* The shared container options (R-LIST-3, PLAN section 3.4) */
struct pl_container_options {
	bool include_subfolders;
	const char *extension_filter; /* R-DIR-5 format */
	enum pl_sort_key sort_key;
	bool sort_descending;
	enum pl_mode mode;
	uint32_t newest_count;
	bool folder_check_enabled;        /* R-DIR-3 */
	uint32_t folder_check_interval_s; /* R-DIR-3 */
	bool lock_list_at_start;          /* R-DIR-11 */
};

/* Called on the provider's worker thread with the list it just built; the receiver owns snapshot. Never called after
 * pl_provider_destroy returns. It must not create or destroy providers, nor wait for a lock that whoever calls
 * pl_provider_destroy holds meanwhile (destroy joins the worker, which may be inside this call). */
typedef void (*pl_publish_fn)(void *owner, struct pl_snapshot *snapshot);

struct pl_provider;

/* Starts a provider for a container row (only PL_ROW_FOLDER so far). Copies location, row_uuid, options and the
 * filter string before returning. The caller has already registered instance_id as the row's expected instance
 * (R-THR-2). NULL when the worker cannot be started. */
struct pl_provider *pl_provider_create(enum pl_row_kind kind, const char *location, const char *row_uuid,
				       uint64_t instance_id, const struct pl_container_options *options,
				       pl_publish_fn publish, void *owner);

/* Lists the container again now (the Properties Refresh button) */
void pl_provider_refresh_now(struct pl_provider *provider);

/* Filter, sort, direction, mode, N, the folder check and lock-list-at-start, applied to the running provider: the
 * view options are applied again to the last listing and republished, the interval re-armed; no rescan, and the
 * admitted-file cache is kept. include_subfolders is ignored here: changing it re-creates the provider. */
void pl_provider_set_options(struct pl_provider *provider, const struct pl_container_options *options);

/* R-DIR-11: publish the snapshot kept while the list is locked, through the normal path (no rescan) */
void pl_provider_release_held(struct pl_provider *provider);

/* How many entries the kept snapshot adds or removes (the row status "N changes waiting"); 0 when nothing is kept.
 * Any thread. */
size_t pl_provider_held_changes(struct pl_provider *provider);

/* Stops the worker (interrupting a scan or probe stuck on a stalled share), joins it and frees everything. NULL is
 * nothing to destroy. No publish happens after it returns. */
void pl_provider_destroy(struct pl_provider *provider);
