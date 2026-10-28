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

#include "parse-m3u.h"

#include <stdbool.h>
#include <string.h>
#include <util/bmem.h>

/* Whether the line is the tag, alone or followed by ":" */
static bool is_tag(const char *line, size_t length, const char *tag)
{
	size_t tag_length = strlen(tag);
	if (length < tag_length || _strnicmp(line, tag, tag_length) != 0)
		return false;
	return length == tag_length || line[tag_length] == ':';
}

static bool is_hls_tag(const char *line, size_t length)
{
	return is_tag(line, length, "#EXT-X-TARGETDURATION") || is_tag(line, length, "#EXT-X-STREAM-INF") ||
	       is_tag(line, length, "#EXT-X-MEDIA-SEQUENCE");
}

/* The title of "#EXTINF:<length> <attributes>,<title>": after the first comma outside double quotes, trimmed */
static void extinf_title(const char *line, size_t length, const char **title, size_t *title_length)
{
	*title = NULL;
	*title_length = 0;
	bool quoted = false;
	for (size_t i = 8; i < length; i++) {
		if (line[i] == '"') {
			quoted = !quoted;
		} else if (line[i] == ',' && !quoted) {
			const char *start = line + i + 1;
			const char *end = line + length;
			while (start < end && (*start == ' ' || *start == '\t'))
				start++;
			*title = start;
			*title_length = (size_t)(end - start);
			return;
		}
	}
}

void pl_parse_m3u(struct pl_list_result *result, const char *data, size_t size, const struct pl_list_source *source)
{
	struct pl_list_builder builder;
	pl_list_builder_init(&builder, source, result);

	size_t text_length = 0;
	char *text = pl_list_decode(data, size, &text_length);
	const char *cursor = text;
	const char *end = text + text_length;

	const char *title = NULL;
	size_t title_length = 0;
	bool hls = false;
	const char *line;
	size_t length;
	while (pl_list_next_line(&cursor, end, &line, &length)) {
		if (!length)
			continue;
		if (line[0] == '#') {
			if (is_tag(line, length, "#EXTINF"))
				extinf_title(line, length, &title, &title_length);
			else if (is_hls_tag(line, length))
				hls = true;
			continue;
		}
		pl_list_builder_add(&builder, line, length, title, title_length);
		title = NULL;
		title_length = 0;
	}

	if (hls)
		pl_list_builder_stream(&builder);
	pl_list_builder_finish(&builder);
	bfree(text);
}
