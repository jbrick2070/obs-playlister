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

#include "text.h"

#include <string.h>

static pl_text_lookup_fn lookup_text;

void pl_text_set_lookup(pl_text_lookup_fn lookup)
{
	lookup_text = lookup;
}

const char *pl_text(const char *key, const char *english)
{
	const char *found = lookup_text && key ? lookup_text(key) : NULL;
	return found && *found ? found : english;
}

void pl_text_cat(struct dstr *out, const char *text, const char *const *values, size_t count)
{
	if (!text)
		return;
	const char *c = text;
	while (*c) {
		const char *mark = strchr(c, '%');
		if (!mark) {
			dstr_cat(out, c);
			break;
		}
		if (mark > c)
			dstr_ncat(out, c, (size_t)(mark - c));
		if (mark[1] >= '1' && mark[1] <= '9') {
			size_t index = (size_t)(mark[1] - '1');
			if (index < count && values[index])
				dstr_cat(out, values[index]);
			c = mark + 2;
		} else {
			dstr_cat_ch(out, '%');
			c = mark + 1;
		}
	}
}
