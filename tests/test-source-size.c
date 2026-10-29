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

/* The size a source reports while it is not playing (R-BEH-7): the child's, else the kept one, else the canvas when
 * the list has rows, else none. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/source-size.h"

static const struct pl_size none = {0, 0};
static const struct pl_size canvas = {1920, 1080};

static void expect(struct pl_size got, uint32_t width, uint32_t height, const char *what)
{
	if (got.width != width || got.height != height) {
		fprintf(stderr, "%s: got %ux%u, expected %ux%u\n", what, got.width, got.height, width, height);
		abort();
	}
}

int main(void)
{
	struct pl_size child = {1280, 720};
	struct pl_size kept = {640, 360};

	expect(pl_source_size(child, kept, canvas, true), 1280, 720, "a playing child wins over the kept size");
	expect(pl_source_size(child, none, none, false), 1280, 720, "a playing child needs no rows or canvas");
	expect(pl_source_size(none, kept, canvas, true), 640, 360, "a stopped child keeps the last size");
	expect(pl_source_size(none, kept, canvas, false), 640, 360, "the kept size stays when the rows are gone");
	expect(pl_source_size(none, none, canvas, true), 1920, 1080, "never played with rows: the canvas");
	expect(pl_source_size(none, none, canvas, false), 0, 0, "no rows: no size, as upstream");
	expect(pl_source_size(none, none, none, true), 0, 0, "no canvas known: no size");

	/* one side 0 is no size (a child half way through its first frame) */
	expect(pl_source_size((struct pl_size){1280, 0}, kept, canvas, true), 640, 360, "child width only");
	expect(pl_source_size((struct pl_size){0, 720}, none, canvas, true), 1920, 1080, "child height only");
	/* a saved size out of range is ignored */
	expect(pl_source_size(none, (struct pl_size){PL_SOURCE_SIZE_MAX + 1, 100}, canvas, true), 1920, 1080,
	       "a kept size too wide");
	expect(pl_source_size(none, (struct pl_size){100, 0}, canvas, true), 1920, 1080, "a kept size of no height");
	expect(pl_source_size(none, (struct pl_size){100, PL_SOURCE_SIZE_MAX + 1}, canvas, true), 1920, 1080,
	       "a kept size too tall");
	expect(pl_source_size((struct pl_size){PL_SOURCE_SIZE_MAX + 1, 720}, kept, canvas, true), 640, 360,
	       "a child too wide is no size");
	expect(pl_source_size(none, none, (struct pl_size){0, 1080}, true), 0, 0, "a canvas of no width");
	expect(pl_source_size(none, (struct pl_size){PL_SOURCE_SIZE_MAX, PL_SOURCE_SIZE_MAX}, canvas, true),
	       PL_SOURCE_SIZE_MAX, PL_SOURCE_SIZE_MAX, "the largest kept size");

	assert(pl_size_known((struct pl_size){1, 1}));
	assert(!pl_size_known(none));
	printf("test-source-size: all passed\n");
	return 0;
}
