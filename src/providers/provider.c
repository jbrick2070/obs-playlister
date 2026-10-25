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

#include "provider.h"

#include <util/bmem.h>

void pl_snapshot_free(struct pl_snapshot *snapshot)
{
	if (!snapshot)
		return;
	pl_entries_free(snapshot->entries, snapshot->count);
	bfree(snapshot->visible);
	for (size_t i = 0; i < snapshot->pending_count; i++)
		bfree(snapshot->pending_keys[i]);
	bfree(snapshot->pending_keys);
	bfree(snapshot->status_message);
	bfree(snapshot);
}
