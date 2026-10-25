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

#include "filter.h"

/* R-LIST-1: a row's kind is detected, not chosen */
enum pl_row_kind {
	PL_ROW_FILE,   /* handed to the child: a local file, or any URL until Phase 3a classifies http(s) ones */
	PL_ROW_FOLDER, /* listed as a folder */
};

/* Whether path is an existing directory: the only disk access classification makes */
typedef bool (*pl_is_directory_fn)(const char *path, void *context);

/* A row with a path, in R-LIST-1's order. A URL (any "scheme://") is a file row. Otherwise a folder kind saved for
 * this row with an unchanged path wins, so a watched folder that is absent when OBS starts is still a folder; a name
 * whose extension is in the filter is a file; any other path is a folder when is_directory says so, else a file
 * (upstream, so a moved or mistyped file still reaches the missing-files dialog). Only that last rule touches the
 * disk. Playlist files (Phase 3a) and UNC folders decided on the provider's worker (Phase 2 step 5) come later. */
enum pl_row_kind pl_classify_row(const char *path, bool saved_folder, const struct pl_filter *filter,
				 pl_is_directory_fn is_directory, void *context);
