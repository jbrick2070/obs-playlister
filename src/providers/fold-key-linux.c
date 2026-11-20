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

/* R-KEY-1 keys on Linux: the text unchanged, as its file systems are case-sensitive and a backslash is a legal name
 * character (R-LNX-5). */

#include "fold-key.h"

#include <iconv.h>
#include <stdint.h>

#include <util/bmem.h>

char *pl_fold_key(const char *text)
{
	return text ? bstrdup(text) : NULL;
}

char *pl_charset_to_utf8(const char *charset, const char *data, size_t size, size_t *length)
{
	iconv_t converter = iconv_open("UTF-8", charset);
	if (converter == (iconv_t)-1)
		return NULL;
	/* no character set gives more than four UTF-8 bytes for one of its own */
	size_t capacity = size * 4 + 4;
	char *out = bmalloc(capacity + 1);
	/* iconv takes a pointer it does not write through */
	char *in_cursor = (char *)(uintptr_t)data;
	size_t in_left = size;
	char *out_cursor = out;
	size_t out_left = capacity;
	size_t converted = iconv(converter, &in_cursor, &in_left, &out_cursor, &out_left);
	/* the end of the text: a set that keeps state (ISO-2022-JP) or holds a character back to see what follows it
	 * writes what it still has, and says so if the text stopped in the middle of something */
	if (converted != (size_t)-1 && !in_left)
		converted = iconv(converter, NULL, NULL, &out_cursor, &out_left);
	iconv_close(converter);
	if (converted == (size_t)-1 || in_left) {
		bfree(out);
		return NULL;
	}
	*length = capacity - out_left;
	out[*length] = '\0';
	return out;
}
