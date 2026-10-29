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

#include "source-size.h"

bool pl_size_known(struct pl_size size)
{
	return size.width > 0 && size.height > 0 && size.width <= PL_SOURCE_SIZE_MAX &&
	       size.height <= PL_SOURCE_SIZE_MAX;
}

struct pl_size pl_source_size(struct pl_size child, struct pl_size kept, struct pl_size canvas, bool has_rows)
{
	if (pl_size_known(child))
		return child;
	if (pl_size_known(kept))
		return kept;
	if (has_rows && pl_size_known(canvas))
		return canvas;
	return (struct pl_size){0, 0};
}
