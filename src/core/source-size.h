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
#include <stdint.h>

/* The size a Playlister source reports (R-BEH-7, R-UP-81). Its child has no size until it decodes and loses it when
 * it stops, and OBS draws a source of no size as a box that cannot be placed. */

/* the largest texture side Direct3D 11 allows; a larger value, which only a damaged saved setting can hold, is no size */
#define PL_SOURCE_SIZE_MAX 16384u

struct pl_size {
	uint32_t width;
	uint32_t height;
};

/* true for a size with both sides in 1..PL_SOURCE_SIZE_MAX */
bool pl_size_known(struct pl_size size);

/* The child's size when it has one; else the size kept from the last time it had one; else the canvas size when the
 * list has rows; else 0x0. */
struct pl_size pl_source_size(struct pl_size child, struct pl_size kept, struct pl_size canvas, bool has_rows);
