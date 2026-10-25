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

enum pl_row_kind pl_classify_row(const char *path, bool saved_folder, const struct pl_filter *filter,
				 pl_is_directory_fn is_directory, void *context)
{
	if (strstr(path, "://"))
		return PL_ROW_FILE;
	if (saved_folder)
		return PL_ROW_FOLDER;
	if (pl_filter_matches(filter, path))
		return PL_ROW_FILE;
	/* a share is never asked here, where an offline one would hold up the caller: its provider finds out on its
	 * worker, and one that is not a folder shows "not a folder" there (R-LIST-1, rule 4) */
	if (pl_path_is_unc(path))
		return PL_ROW_FOLDER;
	return is_directory(path, context) ? PL_ROW_FOLDER : PL_ROW_FILE;
}
