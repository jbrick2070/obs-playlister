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

/* URLs and Windows paths as playlists give them (R-REM-1). Text only: nothing here touches the disk or the network.
 * Every returned string is owned by the caller (bmem). */

/* The length of the scheme that text starts with (RFC 3986: a letter, then letters, digits, "+", "-" or ".", then
 * ":"), or 0. A one-letter scheme is a drive letter ("C:"), so it is never a scheme here. */
size_t pl_url_scheme_length(const char *text);
/* Whether text is an http:// or https:// URL, in any case */
bool pl_url_is_http(const char *text);

/* The extension of the last path segment, lower-cased (ASCII), without its dot; NULL when there is none. For a URL
 * the query and fragment are removed first; a local path keeps "?" and "#", which file names may contain. */
char *pl_url_path_extension(const char *location);

/* RFC 3986 section 5.2: reference resolved against base, which must be an absolute URL; dot segments removed, scheme
 * and host lower-cased. A reference that is an absolute URL itself is normalized the same way. */
char *pl_url_resolve(const char *base, const char *reference);

/* "%XX" sequences decoded; a malformed one, and "%00", are kept as they are */
char *pl_url_percent_decode(const char *text);

/* A file: URL as a Windows path (file:///C:/a%20b.mp3 -> C:\a b.mp3, file://server/share/x -> \\server\share\x,
 * file://localhost/C:/x -> C:\x, file:/C:/x -> C:\x); NULL when text is not a file: URL or names no usable path */
char *pl_url_file_to_path(const char *text);

/* Windows path rules for an entry of a local playlist at base_path (an absolute path): an absolute reference (C:\x,
 * \\server\share\x) stands alone, \x is on base_path's drive or share, C:x is read as C:\x, anything else is relative
 * to base_path's folder. Separators become "\", repeated ones collapse, "." and ".." are resolved (never above the
 * root). A \\?\ or \\.\ reference is returned as it is. */
char *pl_path_resolve(const char *base_path, const char *reference);
