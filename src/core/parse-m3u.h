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

/* M3U and M3U8 (R-REM-1): one reference per line, "#" lines are comments or tags, "#EXTINF:<length>,<title>" gives
 * the next reference its title (a comma inside a quoted attribute does not end the attributes), and the #EXTM3U
 * header is optional. A list carrying #EXT-X-TARGETDURATION, #EXT-X-STREAM-INF or #EXT-X-MEDIA-SEQUENCE is an HLS
 * stream: the result is the one stream entry. result is always filled; free it with pl_list_result_free. */
void pl_parse_m3u(struct pl_list_result *result, const char *data, size_t size, const struct pl_list_source *source);
