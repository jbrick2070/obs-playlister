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

#include "provider-kinds.h"

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

/* ---- the two kinds behind one interface ---- */

struct pl_provider {
	struct pl_folder_provider *folder;
	struct pl_list_provider *list;
};

struct pl_provider *pl_provider_create(enum pl_row_kind kind, const char *location, const char *row_uuid,
				       uint64_t instance_id, const struct pl_container_options *options,
				       pl_publish_fn publish, void *owner)
{
	struct pl_provider *provider = bzalloc(sizeof(*provider));
	if (kind == PL_ROW_FOLDER)
		provider->folder = pl_folder_provider_create(location, row_uuid, instance_id, options, publish, owner);
	else if (kind == PL_ROW_PLAYLIST || kind == PL_ROW_REMOTE)
		provider->list = pl_list_provider_create(kind, location, instance_id, options, publish, owner);
	if (!provider->folder && !provider->list) {
		bfree(provider);
		return NULL;
	}
	return provider;
}

void pl_provider_refresh_now(struct pl_provider *provider)
{
	if (provider->folder)
		pl_folder_provider_refresh_now(provider->folder);
	else
		pl_list_provider_refresh_now(provider->list);
}

void pl_provider_set_options(struct pl_provider *provider, const struct pl_container_options *options)
{
	if (provider->folder)
		pl_folder_provider_set_options(provider->folder, options);
	else
		pl_list_provider_set_options(provider->list, options);
}

void pl_provider_release_held(struct pl_provider *provider)
{
	if (provider->folder)
		pl_folder_provider_release_held(provider->folder);
	else
		pl_list_provider_release_held(provider->list);
}

size_t pl_provider_held_changes(struct pl_provider *provider)
{
	return provider->folder ? pl_folder_provider_held_changes(provider->folder)
				: pl_list_provider_held_changes(provider->list);
}

void pl_provider_destroy(struct pl_provider *provider)
{
	if (!provider)
		return;
	if (provider->folder)
		pl_folder_provider_destroy(provider->folder);
	else
		pl_list_provider_destroy(provider->list);
	bfree(provider);
}
