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

#include "url.h"

#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

static bool ascii_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

static char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool separator(char c)
{
	return c == '\\' || c == '/';
}

size_t pl_url_scheme_length(const char *text)
{
	if (!ascii_alpha(text[0]))
		return 0;
	size_t length = 1;
	while (ascii_alpha(text[length]) || ascii_digit(text[length]) || text[length] == '+' || text[length] == '-' ||
	       text[length] == '.')
		length++;
	return (text[length] == ':' && length >= 2) ? length : 0;
}

static bool scheme_is(const char *text, const char *scheme)
{
	size_t length = pl_url_scheme_length(text);
	return length == strlen(scheme) && _strnicmp(text, scheme, length) == 0;
}

bool pl_url_is_http(const char *text)
{
	return (scheme_is(text, "http") || scheme_is(text, "https")) &&
	       strncmp(text + strcspn(text, ":"), "://", 3) == 0;
}

char *pl_url_path_extension(const char *location)
{
	bool url = pl_url_scheme_length(location) > 0;
	size_t end = url ? strcspn(location, "?#") : strlen(location);

	size_t segment = 0;
	for (size_t i = 0; i < end; i++) {
		if (location[i] == '/' || (!url && location[i] == '\\'))
			segment = i + 1;
	}
	/* in a URL the authority is no segment: "http://example.com" has no extension */
	if (url) {
		const char *authority = strstr(location, "://");
		if (authority && (size_t)(authority - location) + 3 >= segment)
			return NULL;
	}

	size_t dot = end;
	for (size_t i = end; i > segment; i--) {
		if (location[i - 1] == '.') {
			dot = i - 1;
			break;
		}
	}
	if (dot == end || dot + 1 == end)
		return NULL;

	char *extension = bstrdup_n(location + dot + 1, end - dot - 1);
	for (char *c = extension; *c; c++)
		*c = ascii_lower(*c);
	return extension;
}

/* RFC 3986 section 3, as the resolution in section 5 needs it: each part is a slice of the text, and a part that is
 * absent differs from one that is present but empty */
struct uri_parts {
	const char *scheme, *authority, *path, *query, *fragment;
	size_t scheme_len, authority_len, path_len, query_len, fragment_len;
	bool has_scheme, has_authority, has_query, has_fragment;
};

static void split_uri(struct uri_parts *parts, const char *text)
{
	memset(parts, 0, sizeof(*parts));
	size_t scheme = pl_url_scheme_length(text);
	if (scheme) {
		parts->has_scheme = true;
		parts->scheme = text;
		parts->scheme_len = scheme;
		text += scheme + 1;
	}
	if (text[0] == '/' && text[1] == '/') {
		parts->has_authority = true;
		parts->authority = text + 2;
		parts->authority_len = strcspn(parts->authority, "/?#");
		text = parts->authority + parts->authority_len;
	}
	parts->path = text;
	parts->path_len = strcspn(text, "?#");
	text += parts->path_len;
	if (*text == '?') {
		parts->has_query = true;
		parts->query = text + 1;
		parts->query_len = strcspn(parts->query, "#");
		text = parts->query + parts->query_len;
	}
	if (*text == '#') {
		parts->has_fragment = true;
		parts->fragment = text + 1;
		parts->fragment_len = strlen(parts->fragment);
	}
}

/* RFC 3986 section 5.2.4 */
static void remove_dot_segments(struct dstr *out, const char *path, size_t length)
{
	struct dstr input = {0};
	dstr_ncopy(&input, path, length);
	dstr_init(out);

	while (input.len) {
		if (strncmp(input.array, "../", 3) == 0) {
			dstr_remove(&input, 0, 3);
		} else if (strncmp(input.array, "./", 2) == 0) {
			dstr_remove(&input, 0, 2);
		} else if (strncmp(input.array, "/./", 3) == 0) {
			dstr_remove(&input, 0, 2);
		} else if (strcmp(input.array, "/.") == 0) {
			dstr_copy(&input, "/");
		} else if (strncmp(input.array, "/../", 4) == 0 || strcmp(input.array, "/..") == 0) {
			if (input.len == 3)
				dstr_copy(&input, "/");
			else
				dstr_remove(&input, 0, 3);
			char *last = out->array ? strrchr(out->array, '/') : NULL;
			if (last)
				dstr_resize(out, (size_t)(last - out->array));
			else
				dstr_resize(out, 0);
		} else if (strcmp(input.array, ".") == 0 || strcmp(input.array, "..") == 0) {
			break;
		} else {
			size_t segment = strcspn(input.array + 1, "/") + 1;
			if (input.array[0] != '/')
				segment = strcspn(input.array, "/");
			dstr_ncat(out, input.array, segment);
			dstr_remove(&input, 0, segment);
		}
	}
	dstr_free(&input);
}

static void append_lowered(struct dstr *out, const char *text, size_t length)
{
	size_t start = out->len;
	dstr_ncat(out, text, length);
	for (size_t i = start; i < out->len; i++)
		out->array[i] = ascii_lower(out->array[i]);
}

/* Host names are case-insensitive, user information is not */
static void append_authority(struct dstr *out, const char *authority, size_t length)
{
	size_t host = 0;
	for (size_t i = 0; i < length; i++) {
		if (authority[i] == '@')
			host = i + 1;
	}
	dstr_ncat(out, authority, host);
	append_lowered(out, authority + host, length - host);
}

char *pl_url_resolve(const char *base, const char *reference)
{
	struct uri_parts b, r;
	split_uri(&b, base);
	split_uri(&r, reference);

	const struct uri_parts *authority_from = &r;
	const char *query = r.query;
	size_t query_len = r.query_len;
	bool has_query = r.has_query;
	struct dstr path = {0};
	const struct uri_parts *scheme_from = &r;

	if (r.has_scheme) {
		remove_dot_segments(&path, r.path, r.path_len);
	} else {
		scheme_from = &b;
		if (r.has_authority) {
			remove_dot_segments(&path, r.path, r.path_len);
		} else {
			authority_from = &b;
			if (r.path_len == 0) {
				dstr_ncopy(&path, b.path, b.path_len);
				if (!r.has_query) {
					query = b.query;
					query_len = b.query_len;
					has_query = b.has_query;
				}
			} else if (r.path[0] == '/') {
				remove_dot_segments(&path, r.path, r.path_len);
			} else {
				/* section 5.2.3, merge */
				struct dstr merged = {0};
				if (b.has_authority && b.path_len == 0) {
					dstr_copy(&merged, "/");
				} else {
					size_t keep = 0;
					for (size_t i = 0; i < b.path_len; i++) {
						if (b.path[i] == '/')
							keep = i + 1;
					}
					dstr_ncopy(&merged, b.path, keep);
				}
				dstr_ncat(&merged, r.path, r.path_len);
				remove_dot_segments(&path, merged.array, merged.len);
				dstr_free(&merged);
			}
		}
	}

	struct dstr out = {0};
	if (scheme_from->has_scheme) {
		append_lowered(&out, scheme_from->scheme, scheme_from->scheme_len);
		dstr_cat(&out, ":");
	}
	if (authority_from->has_authority) {
		dstr_cat(&out, "//");
		append_authority(&out, authority_from->authority, authority_from->authority_len);
	}
	if (path.len)
		dstr_cat_dstr(&out, &path);
	if (has_query) {
		dstr_cat(&out, "?");
		dstr_ncat(&out, query, query_len);
	}
	if (r.has_fragment) {
		dstr_cat(&out, "#");
		dstr_ncat(&out, r.fragment, r.fragment_len);
	}
	dstr_free(&path);
	return out.array ? out.array : bstrdup("");
}

static int hex_value(char c)
{
	if (ascii_digit(c))
		return c - '0';
	c = ascii_lower(c);
	return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

char *pl_url_percent_decode(const char *text)
{
	size_t length = strlen(text);
	char *out = bmalloc(length + 1);
	size_t o = 0;
	for (size_t i = 0; i < length; i++) {
		if (text[i] == '%' && i + 2 < length) {
			int high = hex_value(text[i + 1]);
			int low = hex_value(text[i + 2]);
			if (high >= 0 && low >= 0 && (high || low)) {
				out[o++] = (char)(high * 16 + low);
				i += 2;
				continue;
			}
		}
		out[o++] = text[i];
	}
	out[o] = '\0';
	return out;
}

static bool drive_root(const char *path)
{
	return ascii_alpha(path[0]) && path[1] == ':' && separator(path[2]);
}

char *pl_url_file_to_path(const char *text)
{
	if (!scheme_is(text, "file"))
		return NULL;
	const char *rest = text + 5;
	const char *query = rest + strcspn(rest, "?#");

	struct dstr encoded = {0};
	if (rest[0] == '/' && rest[1] == '/') {
		const char *host = rest + 2;
		size_t host_len = strcspn(host, "/?#");
		const char *path = host + host_len;
		if (host_len == 0 || (host_len == 9 && _strnicmp(host, "localhost", 9) == 0)) {
			/* file:///C:/x or file://localhost/C:/x; file:////server/share/x keeps its share */
			if (*path == '/' && path[1] != '/')
				path++;
			dstr_ncopy(&encoded, path, (size_t)(query - path));
		} else {
			/* file://server/share/x is the share \\server\share\x */
			dstr_copy(&encoded, "//");
			dstr_ncat(&encoded, host, (size_t)(query - host));
		}
	} else {
		/* file:/C:/x */
		const char *path = rest;
		if (*path == '/')
			path++;
		dstr_ncopy(&encoded, path, (size_t)(query - path));
	}

	char *path = encoded.array ? pl_url_percent_decode(encoded.array) : NULL;
	dstr_free(&encoded);
	if (!path)
		return NULL;
	for (char *c = path; *c; c++) {
		if (*c == '/')
			*c = '\\';
	}
	bool usable = drive_root(path) || (path[0] == '\\' && path[1] == '\\' && path[2] && path[2] != '\\');
	if (!usable) {
		bfree(path);
		return NULL;
	}
	return path;
}

/* How long the root of an absolute Windows path is: "C:\" (3), or "\\server\share\" up to and including the
 * separator after the share (or the whole text when the share ends it); 0 when the path is not absolute */
static size_t root_length(const char *path)
{
	if (drive_root(path))
		return 3;
	if (separator(path[0]) && separator(path[1]) && path[2] && !separator(path[2])) {
		size_t i = 2;
		while (path[i] && !separator(path[i]))
			i++;
		if (!path[i])
			return i;
		i++;
		while (path[i] && !separator(path[i]))
			i++;
		return path[i] ? i + 1 : i;
	}
	return 0;
}

/* Separators to "\", repeated ones collapsed, "." and ".." resolved below the root */
static char *normalize_path(const char *path)
{
	size_t root = root_length(path);
	struct dstr out = {0};
	dstr_ncopy(&out, path, root);
	for (size_t i = 0; i < out.len; i++) {
		if (out.array[i] == '/')
			out.array[i] = '\\';
	}
	if (root && out.array[out.len - 1] != '\\')
		dstr_cat(&out, "\\");
	size_t floor = out.len;

	const char *c = path + root;
	while (*c) {
		while (separator(*c))
			c++;
		size_t length = strcspn(c, "\\/");
		if (!length)
			break;
		if (length == 1 && c[0] == '.') {
			/* nothing */
		} else if (length == 2 && c[0] == '.' && c[1] == '.') {
			if (out.len > floor) {
				size_t keep = out.len - 1;
				while (keep > floor && out.array[keep - 1] != '\\')
					keep--;
				dstr_resize(&out, keep);
			}
		} else {
			dstr_ncat(&out, c, length);
			dstr_cat(&out, "\\");
		}
		c += length;
	}
	/* no trailing separator after a name; the root keeps its own */
	if (out.len > floor && out.array[out.len - 1] == '\\')
		dstr_resize(&out, out.len - 1);
	return out.array ? out.array : bstrdup("");
}

char *pl_path_resolve(const char *base_path, const char *reference)
{
	if (separator(reference[0]) && separator(reference[1]) && (reference[2] == '?' || reference[2] == '.') &&
	    separator(reference[3]))
		return bstrdup(reference);

	struct dstr joined = {0};
	if (root_length(reference)) {
		dstr_copy(&joined, reference);
	} else if (ascii_alpha(reference[0]) && reference[1] == ':') {
		dstr_ncopy(&joined, reference, 2);
		dstr_cat(&joined, "\\");
		dstr_cat(&joined, reference + 2);
	} else if (separator(reference[0])) {
		size_t root = root_length(base_path);
		dstr_ncopy(&joined, base_path, root ? root : 0);
		dstr_cat(&joined, reference);
	} else {
		size_t folder = 0;
		for (size_t i = 0; base_path[i]; i++) {
			if (separator(base_path[i]))
				folder = i + 1;
		}
		dstr_ncopy(&joined, base_path, folder);
		dstr_cat(&joined, reference);
	}

	char *resolved = normalize_path(joined.array ? joined.array : "");
	dstr_free(&joined);
	return resolved;
}
