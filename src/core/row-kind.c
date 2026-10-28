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

#include "row-kind.h"

#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include "url.h"

static bool separator(char c)
{
	return c == '\\' || c == '/';
}

bool pl_path_is_unc(const char *path)
{
	if (!separator(path[0]) || !separator(path[1]))
		return false;
	/* \\?\ and \\.\ prefix local paths too; only \\?\UNC\ is a share */
	if ((path[2] == '?' || path[2] == '.') && separator(path[3]))
		return _strnicmp(path + 4, "UNC", 3) == 0 && separator(path[7]);
	return path[2] != '\0' && !separator(path[2]);
}

/* .m3u, .m3u8 or .pls, for a path or a URL (query and fragment removed) */
static bool playlist_extension(const char *location)
{
	char *extension = pl_url_path_extension(location);
	bool playlist = extension && (strcmp(extension, "m3u") == 0 || strcmp(extension, "m3u8") == 0 ||
				      strcmp(extension, "pls") == 0);
	bfree(extension);
	return playlist;
}

/* A media or still extension in the filter, for a path or a URL */
static bool media_extension(const char *location, const struct pl_filter *filter)
{
	char *extension = pl_url_path_extension(location);
	bool media = false;
	if (extension) {
		struct dstr name = {0};
		dstr_printf(&name, "x.%s", extension);
		media = pl_filter_matches(filter, name.array);
		dstr_free(&name);
	}
	bfree(extension);
	return media;
}

enum pl_row_kind pl_classify_row(const char *path, bool saved_folder, const struct pl_filter *filter,
				 pl_is_directory_fn is_directory, void *context)
{
	if (strstr(path, "://")) {
		/* rtmp://, srt:// and the like go to the child as they are; an http(s) URL by its extension, else by
		 * its first response (R-LIST-1) */
		if (!pl_url_is_http(path))
			return PL_ROW_FILE;
		if (playlist_extension(path))
			return PL_ROW_PLAYLIST;
		return media_extension(path, filter) ? PL_ROW_FILE : PL_ROW_REMOTE;
	}
	if (saved_folder)
		return PL_ROW_FOLDER;
	if (playlist_extension(path))
		return PL_ROW_PLAYLIST;
	if (pl_filter_matches(filter, path))
		return PL_ROW_FILE;
	/* a share is never asked here, where an offline one would hold up the caller: its provider finds out on its
	 * worker, and one that is not a folder shows "not a folder" there (R-LIST-1, rule 4) */
	if (pl_path_is_unc(path))
		return PL_ROW_FOLDER;
	return is_directory(path, context) ? PL_ROW_FOLDER : PL_ROW_FILE;
}
