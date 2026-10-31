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

/* R-KEY-1 keys on Linux: the text unchanged, as its file systems are case-sensitive and a backslash is a legal name
 * character (R-LNX-5). */

#include "fold-key.h"

#include <util/bmem.h>

char *pl_fold_key(const char *text)
{
	return text ? bstrdup(text) : NULL;
}
