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

/* One item a container row offers (PLAN section 3.2): a file in a watched folder, and from Phase 3 a playlist,
 * feed or index entry. Plain data, built by providers on their workers and owned by whoever holds it. */

enum pl_entry_kind {
	PL_ENTRY_MEDIA,
	PL_ENTRY_STILL, /* held for the still-image duration (R-DIR-9) */
	PL_ENTRY_STREAM,
};

enum pl_status_level {
	PL_STATUS_OK,
	PL_STATUS_WARNING,
	PL_STATUS_ERROR,
};

struct pl_entry {
	char *location;          /* UTF-8 absolute path or URL */
	char *title;             /* may be NULL */
	char *key;               /* R-KEY-1 identity, never suffixed */
	uint32_t occurrence;     /* 1, 2, ... among identical keys, in source order */
	uint32_t source_ordinal; /* position as the container gave it; breaks every remaining tie */
	char *previous_key;      /* set when renamed since the last publication (R-DIR-8); may be NULL */
	enum pl_entry_kind kind;
	bool has_date;
	int64_t date_unix_ms; /* UTC; meaningful only with has_date */
	uint64_t size;        /* 0 when unknown */
};

/* Frees the entry's strings and zeroes it */
void pl_entry_free(struct pl_entry *entry);
/* A deep copy: dst gets its own strings */
void pl_entry_copy(struct pl_entry *dst, const struct pl_entry *src);
/* Frees every entry and the bmem array itself; a NULL array is nothing to free */
void pl_entries_free(struct pl_entry *entries, size_t count);
