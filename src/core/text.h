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

/* User-visible text by locale key (R-I18N-1, R-I18N-2). Shared code asks for its messages here, so the plugin shows
 * them in the language OBS runs in, while the ctest programs, which load no locale, get the English given. Only text a
 * user reads goes through here; log lines stay English. */

#include <stddef.h>

#include <util/dstr.h>

/* The locale's text for key, or NULL when it has none. The plugin passes a function around obs_module_text. */
typedef const char *(*pl_text_lookup_fn)(const char *key);

/* Set once at module load, before any source exists; NULL goes back to English. */
void pl_text_set_lookup(pl_text_lookup_fn lookup);

/* The text for key in the current locale, else english. */
const char *pl_text(const char *key, const char *english);

/* Appends text to out with "%1" to "%9" replaced by values[0] to values[8]. A placeholder with no value (its number
 * past count, or a NULL value) is left out, and any other "%" stays as it is, so a translation can never make this read
 * past the values (R-I18N-2). */
void pl_text_cat(struct dstr *out, const char *text, const char *const *values, size_t count);
