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

#include "fold-key-win.h"

#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <windows.h>

char *pl_fold_key(const char *text)
{
	if (!text)
		return NULL;

	/* Windows' own conversion: an invalid byte becomes U+FFFD, so such names keep distinct keys, and characters
	 * beyond the BMP become surrogate pairs */
	int wide_length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (wide_length <= 0)
		return bstrdup(text);
	wchar_t *wide = bzalloc((size_t)wide_length * sizeof(*wide));
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wide_length);
	size_t length = (size_t)wide_length - 1;
	for (size_t i = 0; i < length; i++) {
		if (wide[i] == L'\\')
			wide[i] = L'/';
	}

	/* upper case maps one UTF-16 unit to one, so the length does not change */
	wchar_t *upper = bzalloc((length + 1) * sizeof(*upper));
	int written = length ? LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide, (int)length, upper,
					     (int)length, NULL, NULL, 0)
			     : 0;
	const wchar_t *result = written > 0 ? upper : wide;
	size_t result_length = written > 0 ? (size_t)written : length;

	int bytes = WideCharToMultiByte(CP_UTF8, 0, result, (int)result_length, NULL, 0, NULL, NULL);
	char *folded = bzalloc((size_t)(bytes > 0 ? bytes : 0) + 1);
	if (bytes > 0)
		WideCharToMultiByte(CP_UTF8, 0, result, (int)result_length, folded, bytes, NULL, NULL);
	bfree(upper);
	bfree(wide);
	return folded;
}
