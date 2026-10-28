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

#include "list-parse.h"

#include <stdint.h>
#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>

#include "filter.h"
#include "keys.h"
#include "url.h"

void pl_list_result_free(struct pl_list_result *result)
{
	pl_entries_free(result->entries, result->count);
	memset(result, 0, sizeof(*result));
}

/* Appends one code point as UTF-8 */
static void put_utf8(struct dstr *out, uint32_t cp)
{
	char bytes[4];
	size_t n;
	if (cp < 0x80) {
		bytes[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		bytes[0] = (char)(0xC0 | (cp >> 6));
		bytes[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		bytes[0] = (char)(0xE0 | (cp >> 12));
		bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		bytes[0] = (char)(0xF0 | (cp >> 18));
		bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	/* dstr_ncat stops at nothing, so a NUL code point stays in the text */
	dstr_ensure_capacity(out, out->len + n + 1);
	memcpy(out->array + out->len, bytes, n);
	out->len += n;
	out->array[out->len] = '\0';
}

/* RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF */
static bool valid_utf8(const unsigned char *s, size_t size)
{
	size_t i = 0;
	while (i < size) {
		unsigned char c = s[i];
		size_t n;
		uint32_t cp;
		if (c < 0x80) {
			i++;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			n = 1;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			n = 2;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			n = 3;
			cp = c & 0x07;
		} else {
			return false;
		}
		if (i + n >= size)
			return false;
		for (size_t k = 1; k <= n; k++) {
			if ((s[i + k] & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (s[i + k] & 0x3F);
		}
		static const uint32_t least[4] = {0, 0x80, 0x800, 0x10000};
		if (cp < least[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return false;
		i += n + 1;
	}
	return true;
}

/* Windows-1252 bytes 0x80-0x9F; the five it leaves undefined map to the C1 control of the same value, as Windows'
 * own conversion does */
static const uint16_t cp1252_high[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
	0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
	0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static void decode_utf16(struct dstr *out, const unsigned char *s, size_t size, bool big_endian)
{
	for (size_t i = 0; i + 1 < size; i += 2) {
		uint32_t unit = big_endian ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size) {
			uint32_t low = big_endian ? (uint32_t)(s[i + 2] << 8 | s[i + 3])
						  : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
			if (low >= 0xDC00 && low <= 0xDFFF) {
				put_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
				i += 2;
				continue;
			}
		}
		if (unit >= 0xD800 && unit <= 0xDFFF)
			unit = 0xFFFD;
		put_utf8(out, unit);
	}
}

char *pl_list_decode(const char *data, size_t size, size_t *length)
{
	const unsigned char *s = (const unsigned char *)data;
	struct dstr out = {0};

	if (size >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
		decode_utf16(&out, s + 2, size - 2, false);
	} else if (size >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
		decode_utf16(&out, s + 2, size - 2, true);
	} else {
		if (size >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
			s += 3;
			size -= 3;
		}
		if (valid_utf8(s, size)) {
			dstr_ensure_capacity(&out, size + 1);
			memcpy(out.array, s, size);
			out.len = size;
			out.array[size] = '\0';
		} else {
			for (size_t i = 0; i < size; i++) {
				uint32_t cp = s[i];
				if (cp >= 0x80 && cp < 0xA0)
					cp = cp1252_high[cp - 0x80];
				put_utf8(&out, cp);
			}
		}
	}

	*length = out.len;
	return out.array ? out.array : bstrdup("");
}

static bool blank(char c)
{
	return c == ' ' || c == '\t';
}

bool pl_list_next_line(const char **cursor, const char *end, const char **line, size_t *length)
{
	const char *c = *cursor;
	if (c >= end)
		return false;
	const char *start = c;
	while (c < end && *c != '\n' && *c != '\r')
		c++;
	const char *stop = c;
	if (c < end && *c == '\r')
		c++;
	if (c < end && *c == '\n')
		c++;
	*cursor = c;

	while (start < stop && blank(*start))
		start++;
	while (stop > start && blank(stop[-1]))
		stop--;
	*line = start;
	*length = (size_t)(stop - start);
	return true;
}

typedef DARRAY(struct pl_entry) entry_array_t;

void pl_list_builder_init(struct pl_list_builder *builder, const struct pl_list_source *source,
			  struct pl_list_result *result)
{
	memset(result, 0, sizeof(*result));
	builder->source = source;
	builder->result = result;
	entry_array_t *entries = bzalloc(sizeof(*entries));
	da_init(*entries);
	builder->entries = entries;
}

void pl_list_builder_unusable(struct pl_list_builder *builder)
{
	builder->result->unusable_lines++;
}

static bool is_url_base(const char *base)
{
	return pl_url_scheme_length(base) > 0;
}

static bool scheme_is(const char *text, const char *scheme)
{
	size_t length = pl_url_scheme_length(text);
	return length && length == strlen(scheme) && _strnicmp(text, scheme, length) == 0;
}

/* The schemes a remote list may name: network streams only. Anything else is refused, because the child hands a
 * location to FFmpeg, whose other protocols (concat:, cache:, crypto:, pipe:, ...) can wrap a local file. */
static bool network_scheme(const char *text)
{
	static const char *const allowed[] = {"http", "https", "rtmp", "rtmps", "rtsp", "rtsps", "rtp",
					      "srt",  "udp",   "rist", "mms",   "mmsh", "mmst"};
	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		if (scheme_is(text, allowed[i]))
			return true;
	}
	return false;
}

/* Spaces, control bytes and non-ASCII bytes percent-encoded, as a server expects them in a request line; everything
 * else, "%" included, as it is */
static char *encode_unsafe(char *url)
{
	static const char hex[] = "0123456789ABCDEF";
	struct dstr out = {0};
	for (const unsigned char *c = (const unsigned char *)url; *c; c++) {
		if (*c <= 0x20 || *c >= 0x7F) {
			char escaped[3] = {'%', hex[*c >> 4], hex[*c & 15]};
			dstr_ncat(&out, escaped, 3);
		} else {
			dstr_ncat(&out, (const char *)c, 1);
		}
	}
	bfree(url);
	return out.array ? out.array : bstrdup("");
}

/* NULL when the list may not name what the reference names */
static char *resolve(const char *base, const char *reference)
{
	if (is_url_base(base)) {
		if (pl_url_scheme_length(reference))
			return network_scheme(reference) ? encode_unsafe(pl_url_resolve(base, reference)) : NULL;
		bool drive = ((reference[0] >= 'a' && reference[0] <= 'z') ||
			      (reference[0] >= 'A' && reference[0] <= 'Z')) &&
			     reference[1] == ':';
		if (drive || (reference[0] == '\\' && reference[1] == '\\'))
			return NULL;
		char *slashed = bstrdup(reference);
		for (char *c = slashed; *c; c++) {
			if (*c == '\\')
				*c = '/';
		}
		char *resolved = pl_url_resolve(base, slashed);
		bfree(slashed);
		return encode_unsafe(resolved);
	}

	if (pl_url_scheme_length(reference)) {
		if (!scheme_is(reference, "file"))
			return bstrdup(reference);
		/* through the path rules too, so a file: URL and a plain path to one file get one key */
		char *path = pl_url_file_to_path(reference);
		char *resolved = path ? pl_path_resolve(base, path) : NULL;
		bfree(path);
		return resolved;
	}
	return pl_path_resolve(base, reference);
}

/* .m3u8 is left out: inside a list it names an HLS stream (every IPTV list is made of them), which the child plays
 * itself, so it is kept as a stream entry and never read here */
static bool is_playlist_extension(const char *extension)
{
	return strcmp(extension, "m3u") == 0 || strcmp(extension, "pls") == 0;
}

void pl_list_builder_add(struct pl_list_builder *builder, const char *reference, size_t reference_length,
			 const char *title, size_t title_length)
{
	if (!reference_length)
		return;
	if (reference_length > PL_LIST_MAX_LINE || memchr(reference, '\0', reference_length)) {
		builder->result->unusable_lines++;
		return;
	}

	char *text = bstrdup_n(reference, reference_length);
	char *location = resolve(builder->source->base, text);
	bfree(text);
	if (!location || !*location) {
		bfree(location);
		builder->result->unusable_lines++;
		return;
	}

	char *extension = pl_url_path_extension(location);
	if (extension && is_playlist_extension(extension)) {
		bfree(extension);
		bfree(location);
		builder->result->nested_playlists++;
		return;
	}

	struct pl_entry entry = {0};
	entry.kind = PL_ENTRY_MEDIA;
	if (extension && strcmp(extension, "m3u8") == 0) {
		entry.kind = PL_ENTRY_STREAM;
	} else if (extension) {
		/* the still list goes by name; a URL's query must not hide its extension */
		struct dstr name = {0};
		dstr_printf(&name, "x.%s", extension);
		entry.kind = pl_filter_kind(name.array);
		dstr_free(&name);
	}
	bfree(extension);

	entry.location = location;
	if (title && title_length && title_length <= PL_LIST_MAX_LINE && !memchr(title, '\0', title_length))
		entry.title = bstrdup_n(title, title_length);
	entry.key = builder->source->key ? builder->source->key(location, builder->source->key_context)
					 : bstrdup(location);

	entry_array_t *entries = builder->entries;
	da_push_back(*entries, &entry);
}

void pl_list_builder_stream(struct pl_list_builder *builder)
{
	entry_array_t *entries = builder->entries;
	for (size_t i = 0; i < entries->num; i++)
		pl_entry_free(&entries->array[i]);
	da_resize(*entries, 0);
	/* the lines it skipped were never entries of this result */
	builder->result->nested_playlists = 0;
	builder->result->unusable_lines = 0;

	const char *base = builder->source->base;
	struct pl_entry entry = {0};
	entry.kind = PL_ENTRY_STREAM;
	entry.location = bstrdup(base);
	entry.key = builder->source->key ? builder->source->key(base, builder->source->key_context) : bstrdup(base);
	da_push_back(*entries, &entry);
	builder->result->stream = true;
}

void pl_list_builder_finish(struct pl_list_builder *builder)
{
	entry_array_t *entries = builder->entries;
	for (size_t i = 0; i < entries->num; i++)
		entries->array[i].source_ordinal = (uint32_t)i;
	pl_assign_occurrences(entries->array, entries->num);

	builder->result->entries = entries->array;
	builder->result->count = entries->num;
	bfree(entries);
	builder->entries = NULL;
}
