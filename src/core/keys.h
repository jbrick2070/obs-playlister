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

#include "entry.h"

/* R-KEY-1: entries with identical keys (true duplicates, or names that differ only by case in a case-sensitive
 * directory) are told apart by occurrence, 1, 2, ... in source order (source_ordinal), assigned before any display
 * sort. The key string itself is never changed. Keys arrive already folded (the folder provider upper-cases them
 * at the Win32 boundary); they compare byte for byte here, and a missing key counts as an empty one. */
void pl_assign_occurrences(struct pl_entry *entries, size_t count);
