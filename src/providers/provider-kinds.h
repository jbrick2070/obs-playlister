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

#include "fetch.h"
#include "provider.h"

/* The two provider kinds behind provider.h, which calls them by row kind. Each function means what its pl_provider_*
 * counterpart in provider.h says. */

/* A watched folder (provider-folder-win.c) */
struct pl_folder_provider;
struct pl_folder_provider *pl_folder_provider_create(const char *location, const char *row_uuid, uint64_t instance_id,
						     const struct pl_container_options *options, pl_publish_fn publish,
						     void *owner);
void pl_folder_provider_refresh_now(struct pl_folder_provider *provider);
void pl_folder_provider_set_options(struct pl_folder_provider *provider, const struct pl_container_options *options);
void pl_folder_provider_release_held(struct pl_folder_provider *provider);
size_t pl_folder_provider_held_changes(struct pl_folder_provider *provider);
void pl_folder_provider_destroy(struct pl_folder_provider *provider);

/* A local playlist file or an http(s) row (provider-list.c) */
struct pl_list_provider;
struct pl_list_provider *pl_list_provider_create(enum pl_row_kind kind, const char *location, uint64_t instance_id,
						 const struct pl_container_options *options, pl_publish_fn publish,
						 void *owner);
void pl_list_provider_refresh_now(struct pl_list_provider *provider);
void pl_list_provider_set_options(struct pl_list_provider *provider, const struct pl_container_options *options);
void pl_list_provider_release_held(struct pl_list_provider *provider);
size_t pl_list_provider_held_changes(struct pl_list_provider *provider);
void pl_list_provider_destroy(struct pl_list_provider *provider);

/* The fetch functions list providers call (fetch.h): WinHTTP's, or a test's stand-in */
struct pl_list_fetch_api {
	struct pl_fetch *(*open)(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result);
	const struct pl_fetch_info *(*get_info)(const struct pl_fetch *fetch);
	enum pl_fetch_result (*read)(struct pl_fetch *fetch, size_t limit, bool prefix, char **body, size_t *size);
	void (*close)(struct pl_fetch *fetch);
};

/* For the tests, before any list provider exists: the functions every list provider uses from now on; NULL goes back
 * to WinHTTP */
void pl_list_provider_use_fetch_api(const struct pl_list_fetch_api *api);
