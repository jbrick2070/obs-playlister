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

#include "entry.h"

#include <string.h>
#include <util/bmem.h>

void pl_entry_free(struct pl_entry *entry)
{
	bfree(entry->location);
	bfree(entry->title);
	bfree(entry->key);
	bfree(entry->previous_key);
	memset(entry, 0, sizeof(*entry));
}

void pl_entry_copy(struct pl_entry *dst, const struct pl_entry *src)
{
	*dst = *src;
	dst->location = bstrdup(src->location);
	dst->title = bstrdup(src->title);
	dst->key = bstrdup(src->key);
	dst->previous_key = bstrdup(src->previous_key);
}

void pl_entries_free(struct pl_entry *entries, size_t count)
{
	if (!entries)
		return;
	for (size_t i = 0; i < count; i++)
		pl_entry_free(&entries[i]);
	bfree(entries);
}
