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

#include "fold-key.h"

#include <limits.h>
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

/* The Windows code page of an IANA character set name, or 0. The names servers send in practice; UTF-8 and UTF-16
 * never come here (core/list-parse.c reads those itself). */
static UINT code_page_of(const char *charset)
{
	static const struct {
		const char *name;
		UINT code_page;
	} known[] = {
		{"windows-1250", 1250}, {"windows-1251", 1251}, {"windows-1252", 1252}, {"windows-1253", 1253},
		{"windows-1254", 1254}, {"windows-1255", 1255}, {"windows-1256", 1256}, {"windows-1257", 1257},
		{"windows-1258", 1258}, {"windows-874", 874},   {"tis-620", 874},       {"cp1250", 1250},
		{"cp1251", 1251},       {"cp1252", 1252},       {"cp1253", 1253},       {"cp1254", 1254},
		{"cp1255", 1255},       {"cp1256", 1256},       {"cp1257", 1257},       {"cp1258", 1258},
		{"iso-8859-1", 1252},   {"latin1", 1252},       {"us-ascii", 1252},     {"ascii", 1252},
		{"iso-8859-2", 28592},  {"iso-8859-3", 28593},  {"iso-8859-4", 28594},  {"iso-8859-5", 28595},
		{"iso-8859-6", 28596},  {"iso-8859-7", 28597},  {"iso-8859-8", 28598},  {"iso-8859-9", 1254},
		{"iso-8859-13", 28603}, {"iso-8859-15", 28605}, {"koi8-r", 20866},      {"koi8-u", 21866},
		{"shift_jis", 932},     {"shift-jis", 932},     {"sjis", 932},          {"windows-31j", 932},
		{"ms932", 932},         {"cp932", 932},         {"euc-jp", 20932},      {"iso-2022-jp", 50220},
		{"gbk", 936},           {"gb2312", 936},        {"cp936", 936},         {"gb18030", 54936},
		{"big5", 950},          {"cp950", 950},         {"euc-kr", 949},        {"ks_c_5601-1987", 949},
		{"cp949", 949},         {"windows-949", 949},   {"ibm866", 866},        {"cp866", 866},
	};
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (strcmp(charset, known[i].name) == 0)
			return known[i].code_page;
	}
	return 0;
}

char *pl_charset_to_utf8(const char *charset, const char *data, size_t size, size_t *length)
{
	UINT code_page = code_page_of(charset);
	if (!code_page || size > INT_MAX)
		return NULL;
	if (!size) {
		*length = 0;
		return bstrdup("");
	}
	/* a byte the code page does not have fails the conversion; the stateful code pages (ISO-2022-JP) do not take
	 * that flag */
	DWORD flags = MB_ERR_INVALID_CHARS;
	int wide_length = MultiByteToWideChar(code_page, flags, data, (int)size, NULL, 0);
	if (wide_length <= 0 && GetLastError() == ERROR_INVALID_FLAGS) {
		flags = 0;
		wide_length = MultiByteToWideChar(code_page, flags, data, (int)size, NULL, 0);
	}
	if (wide_length <= 0)
		return NULL;
	wchar_t *wide = bmalloc((size_t)wide_length * sizeof(*wide));
	MultiByteToWideChar(code_page, flags, data, (int)size, wide, wide_length);
	int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, NULL, 0, NULL, NULL);
	char *out = NULL;
	if (bytes > 0) {
		out = bmalloc((size_t)bytes + 1);
		WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, out, bytes, NULL, NULL);
		out[bytes] = '\0';
		*length = (size_t)bytes;
	}
	bfree(wide);
	return out;
}
