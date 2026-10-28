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

#include "list-parse.h"

/* PLS (R-REM-1): an INI text whose [playlist] section holds File<N>=<reference> and Title<N>=<title>. Entries come in
 * ascending N whatever order the lines are in, and gaps in N are fine; the first File<N> for a number wins and a
 * repeat is unusable; a Title<N> without its File<N> is ignored, and so are NumberOfEntries, Length<N>, Version, ";"
 * and "#" comments and every other section. Lines before any section header count as the playlist section, so a file
 * missing its [playlist] line still reads. result is always filled; free it with pl_list_result_free. */
void pl_parse_pls(struct pl_list_result *result, const char *data, size_t size, const struct pl_list_source *source);
