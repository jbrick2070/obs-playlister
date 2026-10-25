/*
Media Playlist Source
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com

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

#include <util/darray.h>

#include "core/entry.h"

struct media_file_data {
	char *path;
	char *filename; // filename with ext, ONLY for folder item checking
	char *id;
	bool is_url;
	bool is_folder;
	DARRAY(struct media_file_data) folder_items;
	struct media_file_data *parent;
	const char *parent_id; // for folder items
	size_t index;          // makes it easier to switch back to non-shuffle mode
	/* owned (bmem), freed by free_files: the item's identity and data (R-KEY-1); folder items have one from Phase 2
	 * step 4, rows and the upstream shuffler tests do not */
	struct pl_entry *entry;
	/* folder rows: not listed by the row's provider yet, so not empty either (R-KEY-2); the keys still in its
	 * write-complete gate (R-DIR-4) */
	bool awaiting;
	char **pending_keys;
	size_t pending_count;
};
