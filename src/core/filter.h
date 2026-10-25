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

#include <stdbool.h>
#include <stddef.h>

#include "entry.h"

/* The extension filter of the shared container options (R-DIR-5): which file names a container admits. Names only;
 * hidden, system and cloud-only files are left out by the folder provider from their attributes. */

/* Upstream's video and audio lists plus still images (R-DIR-5) */
#define PL_FILTER_DEFAULT \
	"mp4 m4v mov mkv webm avi flv ts mpg mxf gif mp3 m4a mka aac ogg opus wav flac jpg jpeg png webp bmp"

struct pl_filter {
	char **extensions; /* lower case, without dots */
	size_t count;
};

/* Reads the option text: extensions separated by spaces (tabs, commas and semicolons are read as spaces too), each
 * lower-cased with a leading "." or "*." removed, duplicates dropped. An empty text gives an empty filter, which
 * admits nothing. */
void pl_filter_parse(struct pl_filter *filter, const char *text);
void pl_filter_free(struct pl_filter *filter);
/* Whether the name's whole extension (after the last dot of its last path component) is in the filter, ignoring
 * ASCII case. A name without an extension never matches, and neither does a filter entry with a dot or a wildcard
 * inside it (tar.gz, *.*). name is never NULL, here or below. */
bool pl_filter_matches(const struct pl_filter *filter, const char *name);
/* Names skipped whatever the filter says: desktop.ini, Thumbs.db, ~$ office lock files, and the .tmp, .part,
 * .crdownload and .partial extensions of files still being written */
bool pl_filter_always_skipped(const char *name);
/* A still image by its extension (jpg, jpeg, png, webp, bmp; R-DIR-9), else media; .gif is media, as upstream */
enum pl_entry_kind pl_filter_kind(const char *name);
