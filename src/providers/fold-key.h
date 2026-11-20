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

/* R-KEY-1 key folding, one implementation per platform; the result is bmem, freed with bfree, and NULL for NULL.
 * Windows (fold-key-win.c): the UTF-8 text upper-cased in UTF-16 with the invariant locale (LCMapStringEx,
 * LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE), as NTFS compares names, and returned as UTF-8. Backslashes become '/'. A
 * byte that is not valid UTF-8 becomes U+FFFD, so such names still get distinct keys; characters beyond the BMP pass
 * through unchanged. If the mapping fails the text comes back unfolded. Linux: a copy of the text, unchanged, as its
 * file systems are case-sensitive and a backslash is a legal name character (R-LNX-5). */
char *pl_fold_key(const char *text);

/* Text in a named character set as UTF-8, through the operating system's own tables (Windows: MultiByteToWideChar;
 * macOS: CoreFoundation; Linux: iconv). charset is an IANA name as a Content-Type header gives it, lower-cased
 * (core/url.h, pl_content_type_split). Returns a bmem buffer of *length bytes plus a terminating NUL, or NULL when
 * the system does not know the name or the bytes are not valid in it. For a playlist a server sent with a charset
 * that is not UTF-8 (R-REM-1). */
char *pl_charset_to_utf8(const char *charset, const char *data, size_t size, size_t *length);
