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

/* R-DIR-1: what a folder listing does with a subfolder that cannot be listed with an error that may pass (an I/O
 * error, a timeout, a sharing violation; folder-os.h). The listing fails, so the last good list stays, with two
 * exceptions: with no good list yet nothing can shrink, so the subfolder is left out at once and stays left out; and
 * once the same subfolder has failed PL_FAILING_LISTINGS listings in a row it is left out. A subfolder left out is
 * tried again at every listing, and one that did not fail in a complete listing starts over. One per folder row,
 * used by its worker only; no operating system call. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <util/darray.h>

#define PL_FAILING_LISTINGS 3
#define PL_MAX_FAILING 256 /* subfolders followed at once; the one followed longest goes first */

struct pl_failing_subfolder {
	char *relative;
	int listings;          /* failed listings in a row */
	uint64_t last_listing; /* the listing it last failed in */
};

struct pl_subfolder_failures {
	DARRAY(struct pl_failing_subfolder) items;
	uint64_t listing; /* listings begun */
};

void pl_subfolder_failures_init(struct pl_subfolder_failures *failures);
void pl_subfolder_failures_free(struct pl_subfolder_failures *failures);
/* A listing begins */
void pl_subfolder_failures_begin(struct pl_subfolder_failures *failures);
/* relative failed in the listing under way: true leaves it out, false fails the listing. has_good_list: the row has a
 * good list the failed listing would keep. */
bool pl_subfolder_failures_failed(struct pl_subfolder_failures *failures, const char *relative, bool has_good_list);
/* The listing under way completed: every subfolder that did not fail in it starts over */
void pl_subfolder_failures_complete(struct pl_subfolder_failures *failures);
