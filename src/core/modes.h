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

#include <stddef.h>
#include <stdint.h>

#include "entry.h"

/* Which of a container's entries play (R-DIR-7). Values match the container_mode setting. */
enum pl_mode {
	PL_MODE_FULL = 0,
	PL_MODE_NEWEST_ONLY = 1,
	PL_MODE_NEWEST_N = 2,
};

/* Writes the indexes of the entries the mode keeps into visible (room for count), in the entries' order, which is
 * the display order; returns how many. Newest goes by date whatever the display order: a dateless entry is older
 * than any dated one, and between two entries with no date or the same date the later one in the container is
 * newer (R-DIR-10). newest_count below 1 counts as 1; the caller passes the setting clamped to its range
 * (newest_count, 1 to 1000, PLAN section 3.4), so a value read as negative never becomes a huge count here. */
size_t pl_mode_select(const struct pl_entry *entries, size_t count, enum pl_mode mode, uint32_t newest_count,
		      size_t *visible);
