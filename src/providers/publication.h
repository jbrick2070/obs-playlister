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

#include "provider.h"

/* What every provider does with a membership once it has built one (R-REM-7, R-BEH-4): the same display sort
 * (R-DIR-6) and mode (R-DIR-7), the "Lock list at start" hold (R-DIR-11), and a publication only when something the
 * receiver sees changed. Worker-thread only, except held_changes. */

struct pl_publication {
	uint64_t instance_id;
	pl_publish_fn publish;
	void *owner;

	uint64_t generation;
	uint64_t content_revision;
	char *last_signature;
	struct pl_entry *published; /* the published membership, without renames */
	size_t published_count;
	char *published_content;
	bool lock_ready;       /* R-DIR-11: the list taken at start is published; the lock applies from now */
	struct pl_entry *held; /* R-DIR-11: the newest membership, kept while the list is locked */
	size_t held_count;
	char *held_content;
	volatile long held_changes; /* any thread */
};

/* What the provider says about the row along with a membership */
struct pl_publication_status {
	bool listed;     /* the first listing or fetch is done: settles an awaiting row (R-KEY-2) */
	bool lock_point; /* this membership may be the list taken at start (R-DIR-11): a real listing, not an error */
	char **pending_keys;
	size_t pending_count;
	enum pl_status_level level;
	const char *message; /* NULL when there is nothing to say */
	bool stale;          /* the last good list kept after a failure (R-BEH-3) */
	int64_t last_ok_unix_ms;
};

enum pl_publication_result {
	PL_PUBLICATION_UNCHANGED, /* nothing the receiver sees changed; nothing was published */
	PL_PUBLICATION_PUBLISHED, /* the membership was published */
	PL_PUBLICATION_KEPT, /* the list is locked: the membership was kept, the locked one republished if needed */
};

void pl_publication_init(struct pl_publication *publication, uint64_t instance_id, pl_publish_fn publish, void *owner);
void pl_publication_free(struct pl_publication *publication);

/* Publishes the membership raw (count entries, which it takes), or keeps it while the list is locked and publishes the
 * locked membership in the current view and status; release (or the lock turned off) publishes the newest membership,
 * carrying the renames the kept one had. Renames in raw (previous_key) are published once and never kept. */
enum pl_publication_result pl_publication_update(struct pl_publication *publication,
						 const struct pl_container_options *options, struct pl_entry *raw,
						 size_t count, const struct pl_publication_status *status,
						 bool release);

/* How many entries the kept membership adds, removes or rewrites; 0 when nothing is kept. Any thread. */
size_t pl_publication_held_changes(struct pl_publication *publication);

/* A copy of entries; previous_key dropped unless keep_renames */
struct pl_entry *pl_publication_copy_entries(const struct pl_entry *entries, size_t count, bool keep_renames);
