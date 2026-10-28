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

/* What test-parse-m3u.c and test-parse-pls.c share: reading a fixture and comparing a parse result */

#pragma once

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "core/list-parse.h"

struct expected_entry {
	const char *location;
	const char *title; /* NULL for none */
	enum pl_entry_kind kind;
};

/* tests/fixtures/playlists/<name>, as bytes; FIXTURE_DIR comes from tests/CMakeLists.txt */
static char *read_fixture(const char *name, size_t *size)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
	FILE *file = os_fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "cannot open %s\n", path);
		assert(false);
	}
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	char *data = bmalloc((size_t)length + 1);
	*size = fread(data, 1, (size_t)length, file);
	fclose(file);
	return data;
}

static bool same_text(const char *a, const char *b)
{
	return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/* The result holds exactly these entries in this order, each keyed by its location (no key function) with source
 * ordinals 0, 1, ... and the given counts */
static void check_result(const char *what, const struct pl_list_result *result, const struct expected_entry *expected,
			 size_t count, size_t nested, size_t unusable)
{
	bool good = result->count == count && result->nested_playlists == nested && result->unusable_lines == unusable;
	for (size_t i = 0; good && i < count; i++) {
		const struct pl_entry *e = &result->entries[i];
		good = same_text(e->location, expected[i].location) && same_text(e->title, expected[i].title) &&
		       e->kind == expected[i].kind && same_text(e->key, expected[i].location) &&
		       e->source_ordinal == (uint32_t)i && e->occurrence >= 1 && !e->has_date;
	}
	if (good)
		return;

	fprintf(stderr, "%s: %zu entries, %zu nested, %zu unusable; expected %zu, %zu, %zu\n", what, result->count,
		result->nested_playlists, result->unusable_lines, count, nested, unusable);
	for (size_t i = 0; i < result->count; i++) {
		const struct pl_entry *e = &result->entries[i];
		fprintf(stderr, "  [%zu] %s | %s | kind %d | key %s | ordinal %u | occurrence %u\n", i, e->location,
			e->title ? e->title : "(none)", e->kind, e->key ? e->key : "(null)", e->source_ordinal,
			e->occurrence);
	}
	assert(false);
}
