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

/* R-KEY-1 keys on macOS (R-MAC-4): APFS and HFS+ volumes are case-insensitive and normalization-insensitive unless
 * formatted otherwise, so "Clip.mp4", "clip.mp4" and a decomposed "café" name one file. The key is the text upper-cased
 * without a locale (CFStringUppercase) and composed (Unicode NFC), so it does not change when a rename only changes
 * case or a copy decomposes the name. A backslash is a legal name character here and stays. A byte that is not valid
 * UTF-8 becomes U+FFFD, as on Windows, so such names still get distinct keys. */

#include "fold-key.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>

#include <util/bmem.h>
#include <util/dstr.h>

/* Length of the valid UTF-8 sequence at text, or 0 when the byte there does not start one */
static size_t utf8_sequence(const unsigned char *text)
{
	unsigned char lead = text[0];
	if (lead < 0x80)
		return 1;
	size_t length;
	uint32_t code;
	if (lead >= 0xC2 && lead <= 0xDF) {
		length = 2;
		code = lead & 0x1F;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		length = 3;
		code = lead & 0x0F;
	} else if (lead >= 0xF0 && lead <= 0xF4) {
		length = 4;
		code = lead & 0x07;
	} else {
		return 0;
	}
	for (size_t i = 1; i < length; i++) {
		if ((text[i] & 0xC0) != 0x80)
			return 0;
		code = (code << 6) | (text[i] & 0x3F);
	}
	/* overlong forms, surrogates and code points past U+10FFFF are not valid UTF-8 */
	if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10FFFF ||
	    (code >= 0xD800 && code <= 0xDFFF))
		return 0;
	return length;
}

/* The text with each byte that is not valid UTF-8 replaced by U+FFFD (bmem) */
static char *valid_utf8(const char *text)
{
	if (!*text)
		return bstrdup("");
	struct dstr result = {0};
	dstr_reserve(&result, strlen(text) + 1);
	const unsigned char *cursor = (const unsigned char *)text;
	while (*cursor) {
		size_t length = utf8_sequence(cursor);
		if (length) {
			dstr_ncat(&result, (const char *)cursor, length);
			cursor += length;
		} else {
			dstr_cat(&result, "\xef\xbf\xbd");
			cursor++;
		}
	}
	return result.array;
}

char *pl_fold_key(const char *text)
{
	if (!text)
		return NULL;
	char *valid = valid_utf8(text);
	CFStringRef source = CFStringCreateWithCString(kCFAllocatorDefault, valid, kCFStringEncodingUTF8);
	CFMutableStringRef folded = source ? CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source) : NULL;
	if (source)
		CFRelease(source);
	if (!folded)
		return valid;
	/* NULL locale: the same mapping whatever the user's language, as LOCALE_NAME_INVARIANT on Windows */
	CFStringUppercase(folded, NULL);
	CFStringNormalize(folded, kCFStringNormalizationFormC);

	CFIndex length = CFStringGetLength(folded);
	CFIndex size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
	char *result = bzalloc((size_t)size);
	bool converted = CFStringGetCString(folded, result, size, kCFStringEncodingUTF8);
	CFRelease(folded);
	if (!converted) {
		bfree(result);
		return valid;
	}
	bfree(valid);
	return result;
}
