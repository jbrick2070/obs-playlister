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

#include "natural.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static unsigned char fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : (unsigned char)c;
}

int pl_natural_compare(const char *a, const char *b)
{
	if (!a || !b)
		return (a != NULL) - (b != NULL);

	while (*a && *b) {
		if (is_digit(*a) && is_digit(*b)) {
			while (*a == '0' && is_digit(a[1]))
				a++;
			while (*b == '0' && is_digit(b[1]))
				b++;
			size_t length_a = 0;
			size_t length_b = 0;
			while (is_digit(a[length_a]))
				length_a++;
			while (is_digit(b[length_b]))
				length_b++;
			if (length_a != length_b)
				return length_a < length_b ? -1 : 1;
			int digits = memcmp(a, b, length_a);
			if (digits)
				return digits < 0 ? -1 : 1;
			a += length_a;
			b += length_b;
			continue;
		}
		unsigned char ca = fold(*a);
		unsigned char cb = fold(*b);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	return (*a != '\0') - (*b != '\0');
}
