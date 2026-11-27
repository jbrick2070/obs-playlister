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

#include "subfolder-failures.h"

#include <string.h>
#include <util/bmem.h>

void pl_subfolder_failures_init(struct pl_subfolder_failures *failures)
{
	da_init(failures->items);
	failures->listing = 0;
}

void pl_subfolder_failures_free(struct pl_subfolder_failures *failures)
{
	for (size_t i = 0; i < failures->items.num; i++)
		bfree(failures->items.array[i].relative);
	da_free(failures->items);
}

void pl_subfolder_failures_begin(struct pl_subfolder_failures *failures)
{
	failures->listing++;
}

bool pl_subfolder_failures_failed(struct pl_subfolder_failures *failures, const char *relative, bool has_good_list)
{
	struct pl_failing_subfolder *found = NULL;
	for (size_t i = 0; i < failures->items.num && !found; i++) {
		if (strcmp(failures->items.array[i].relative, relative) == 0)
			found = &failures->items.array[i];
	}
	if (!found) {
		if (failures->items.num >= PL_MAX_FAILING) {
			size_t oldest = 0;
			for (size_t i = 1; i < failures->items.num; i++) {
				if (failures->items.array[i].last_listing < failures->items.array[oldest].last_listing)
					oldest = i;
			}
			bfree(failures->items.array[oldest].relative);
			da_erase(failures->items, oldest);
		}
		found = da_push_back_new(failures->items);
		found->relative = bstrdup(relative);
	}
	if (found->last_listing != failures->listing) {
		found->listings++;
		found->last_listing = failures->listing;
	}
	/* left out with no good list, it stays left out until it can be read, so the next listing does not fail */
	if (!has_good_list && found->listings < PL_FAILING_LISTINGS)
		found->listings = PL_FAILING_LISTINGS;
	return found->listings >= PL_FAILING_LISTINGS;
}

void pl_subfolder_failures_complete(struct pl_subfolder_failures *failures)
{
	for (size_t i = failures->items.num; i-- > 0;) {
		if (failures->items.array[i].last_listing != failures->listing) {
			bfree(failures->items.array[i].relative);
			da_erase(failures->items, i);
		}
	}
}
