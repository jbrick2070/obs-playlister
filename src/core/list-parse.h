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

/* What the playlist parsers (parse-m3u.c, parse-pls.c) share: the text decoding, the line reader, and turning one
 * reference into an entry (R-REM-1, R-KEY-1, D-12). OBS-free apart from libobs util; nothing here touches the disk
 * or the network. */

/* The longest line a playlist may have; a longer one is unusable (Windows paths end at 32767 characters) */
#define PL_LIST_MAX_LINE 32768

/* An entry's key from its resolved location (R-KEY-1); returns a bmem string. The list provider folds local paths
 * at the Win32 boundary; NULL as the function makes the key the location itself. */
typedef char *(*pl_list_key_fn)(const char *location, void *context);

/* Text in a named character set as UTF-8 (bmem, *length bytes plus a NUL), or NULL when it cannot be converted. The
 * list provider passes the operating system's converter (providers/fold-key.h). */
typedef char *(*pl_list_convert_fn)(const char *charset, const char *data, size_t size, size_t *length);

struct pl_list_source {
	const char *base; /* the playlist's own location: an absolute local path, or the final URL after redirects */
	pl_list_key_fn key;
	void *key_context;
	/* the character set the server named for the body (lower case), and what converts from it; NULL for a local
	 * file, for a response without one, and in tests */
	const char *charset;
	pl_list_convert_fn convert;
};

struct pl_list_result {
	struct pl_entry *entries; /* in list order, with source_ordinal, key and occurrence set; bmem */
	size_t count;
	bool stream;             /* HLS tags: the list is one stream, entries holds just it (R-REM-1) */
	size_t nested_playlists; /* entries that are playlists themselves, skipped (D-12 c) */
	size_t unusable_lines;   /* too long, holding a NUL, malformed, or naming what this list may not (see below) */
	size_t over_limit;       /* references after the first PL_MAX_ENTRIES entries, left out */
};

void pl_list_result_free(struct pl_list_result *result);

/* The body as UTF-8: a UTF-8 BOM is dropped; a UTF-16 BOM (either byte order) converts; text that is valid UTF-8
 * stays, whatever the server called it (plain ASCII is the same in every character set a playlist comes in, and
 * servers that label UTF-8 as something else are common). Text that is not: UTF-16 without a BOM when charset says
 * "utf-16", "utf-16le" or "utf-16be"; else converted from charset by convert when both are given and the conversion
 * succeeds; else, and always without a charset, read as Windows-1252, the code page older Windows players wrote .m3u
 * files in. Returns a bmem buffer of *length bytes plus a terminating NUL; NULs inside the text are kept (the line
 * reader reports them). */
char *pl_list_decode_as(const char *data, size_t size, const char *charset, pl_list_convert_fn convert, size_t *length);
/* The same with no character set named */
char *pl_list_decode(const char *data, size_t size, size_t *length);

/* The next line from *cursor (LF, CRLF or a lone CR ends it), with spaces and tabs trimmed at both ends; false at
 * the end of the text */
bool pl_list_next_line(const char **cursor, const char *end, const char **line, size_t *length);

/* Builds a result one entry at a time */
struct pl_list_builder {
	const struct pl_list_source *source;
	struct pl_list_result *result;
	void *entries; /* the growing array, private */
};

void pl_list_builder_init(struct pl_list_builder *builder, const struct pl_list_source *source,
			  struct pl_list_result *result);
/* Adds the entry a reference names, with its title (NULL or empty for none). The reference resolves against the
 * source's base: in a local playlist by Windows path rules, a file: URL becoming its path and any other URL kept as
 * written; in a remote playlist by RFC 3986, with backslashes read as "/", spaces and non-ASCII bytes percent-encoded,
 * and a local path or any scheme but a network stream's (http, https, rtmp, rtsp, srt, udp, ...) unusable, so a list
 * on a server cannot name files on this computer. A reference whose extension is m3u or pls is counted as a nested
 * playlist and skipped (D-12 c); one ending in .m3u8 is an HLS stream the child plays, kept as a PL_ENTRY_STREAM and
 * never followed. Once the result has PL_MAX_ENTRIES entries, further references are only counted (over_limit). */
void pl_list_builder_add(struct pl_list_builder *builder, const char *reference, size_t reference_length,
			 const char *title, size_t title_length);
/* Counts a line the parser could not use */
void pl_list_builder_unusable(struct pl_list_builder *builder);
/* Counts a reference the parser itself left out because the list is over PL_MAX_ENTRIES */
void pl_list_builder_over_limit(struct pl_list_builder *builder);
/* The one entry of an HLS stream: the playlist itself (R-REM-1); drops anything added before */
void pl_list_builder_stream(struct pl_list_builder *builder);
/* Hands the entries to the result: source ordinals and occurrences assigned (R-KEY-1) */
void pl_list_builder_finish(struct pl_list_builder *builder);
