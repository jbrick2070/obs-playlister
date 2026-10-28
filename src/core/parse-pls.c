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

#include "parse-pls.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>

#include "sort.h"

/* One File<N> or Title<N> line, a slice of the decoded text */
struct pls_line {
	uint32_t number;
	bool is_file;
	const char *value;
	size_t value_length;
};

static const char *trim_end(const char *start, const char *end)
{
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	return end;
}

static const char *trim_start(const char *start, const char *end)
{
	while (start < end && (*start == ' ' || *start == '\t'))
		start++;
	return start;
}

/* "File12" -> 12 for the prefix "File"; 0 when the name is not the prefix and a number from 1 to 999999999 */
static uint32_t numbered(const char *name, size_t length, const char *prefix)
{
	size_t prefix_length = strlen(prefix);
	if (length <= prefix_length || length > prefix_length + 9 || _strnicmp(name, prefix, prefix_length) != 0)
		return 0;
	uint32_t number = 0;
	for (size_t i = prefix_length; i < length; i++) {
		if (name[i] < '0' || name[i] > '9')
			return 0;
		number = number * 10 + (uint32_t)(name[i] - '0');
	}
	return number;
}

static bool looks_numbered(const char *name, size_t length, const char *prefix)
{
	size_t prefix_length = strlen(prefix);
	return length > prefix_length && _strnicmp(name, prefix, prefix_length) == 0 && name[prefix_length] >= '0' &&
	       name[prefix_length] <= '9';
}

static int compare_numbers(size_t ia, size_t ib, void *context)
{
	const struct pls_line *lines = context;
	return (lines[ia].number > lines[ib].number) - (lines[ia].number < lines[ib].number);
}

void pl_parse_pls(struct pl_list_result *result, const char *data, size_t size, const struct pl_list_source *source)
{
	struct pl_list_builder builder;
	pl_list_builder_init(&builder, source, result);

	size_t text_length = 0;
	char *text = pl_list_decode(data, size, &text_length);
	const char *cursor = text;
	const char *end = text + text_length;

	DARRAY(struct pls_line) lines;
	da_init(lines);
	bool in_playlist = true;
	const char *line;
	size_t length;
	while (pl_list_next_line(&cursor, end, &line, &length)) {
		if (!length || line[0] == ';' || line[0] == '#')
			continue;
		if (length > PL_LIST_MAX_LINE || memchr(line, '\0', length)) {
			if (in_playlist)
				pl_list_builder_unusable(&builder);
			continue;
		}
		if (line[0] == '[') {
			const char *close = memchr(line, ']', length);
			if (!close) {
				pl_list_builder_unusable(&builder);
				in_playlist = false;
				continue;
			}
			const char *name = trim_start(line + 1, close);
			const char *name_end = trim_end(name, close);
			in_playlist = name_end - name == 8 && _strnicmp(name, "playlist", 8) == 0;
			continue;
		}
		if (!in_playlist)
			continue;

		const char *equals = memchr(line, '=', length);
		if (!equals) {
			pl_list_builder_unusable(&builder);
			continue;
		}
		const char *name_end = trim_end(line, equals);
		size_t name_length = (size_t)(name_end - line);
		const char *value = trim_start(equals + 1, line + length);
		size_t value_length = (size_t)(line + length - value);

		bool is_file = looks_numbered(line, name_length, "File");
		bool is_title = looks_numbered(line, name_length, "Title");
		if (!is_file && !is_title)
			continue;
		uint32_t number = numbered(line, name_length, is_file ? "File" : "Title");
		if (!number) {
			pl_list_builder_unusable(&builder);
			continue;
		}

		struct pls_line *record = da_push_back_new(lines);
		record->number = number;
		record->is_file = is_file;
		record->value = value;
		record->value_length = value_length;
	}

	/* one stable sort, then each number's lines together in their text order (a scan per line would be quadratic
	 * in a hostile list) */
	if (lines.num) {
		size_t *order = bmalloc(lines.num * sizeof(*order));
		for (size_t i = 0; i < lines.num; i++)
			order[i] = i;
		pl_sort_indexes(order, lines.num, compare_numbers, lines.array);
		for (size_t i = 0; i < lines.num;) {
			uint32_t number = lines.array[order[i]].number;
			const struct pls_line *file = NULL, *title = NULL;
			for (; i < lines.num && lines.array[order[i]].number == number; i++) {
				const struct pls_line *record = &lines.array[order[i]];
				if (record->is_file && file)
					pl_list_builder_unusable(&builder);
				else if (record->is_file)
					file = record;
				else if (!title)
					title = record;
			}
			if (file)
				pl_list_builder_add(&builder, file->value, file->value_length,
						    title ? title->value : NULL, title ? title->value_length : 0);
		}
		bfree(order);
	}

	da_free(lines);
	pl_list_builder_finish(&builder);
	bfree(text);
}
