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

/* Fetching on Linux until the libcurl backend (fetch-curl.c, R-LNX-6) lands in PLAN step 4: every fetch fails at once
 * as a network error, so a web row shows a row error and nothing else changes. */

#include "fetch.h"

#include <util/bmem.h>

struct pl_fetch {
	struct pl_fetch_info info;
};

void pl_fetch_default_options(struct pl_fetch_options *options)
{
	*options = (struct pl_fetch_options){
		.user_agent = "obs-playlister",
		.stop = NULL,
		.deadline_ms = 60000,
		.connect_ms = 10000,
		.receive_ms = 30000,
		.max_redirects = 5,
	};
}

struct pl_fetch *pl_fetch_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result)
{
	UNUSED_PARAMETER(url);
	UNUSED_PARAMETER(options);
	struct pl_fetch *fetch = bzalloc(sizeof(*fetch));
	fetch->info.failure = PL_FETCH_FAILURE_OTHER;
	*result = PL_FETCH_NETWORK_ERROR;
	return fetch;
}

const struct pl_fetch_info *pl_fetch_get_info(const struct pl_fetch *fetch)
{
	return &fetch->info;
}

enum pl_fetch_result pl_fetch_read(struct pl_fetch *fetch, size_t limit, bool prefix, char **body, size_t *size)
{
	UNUSED_PARAMETER(fetch);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(prefix);
	*body = NULL;
	*size = 0;
	return PL_FETCH_NETWORK_ERROR;
}

void pl_fetch_close(struct pl_fetch *fetch)
{
	bfree(fetch);
}

int64_t pl_fetch_callback_count(void)
{
	return 0;
}
