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

#include "filter.h"

#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>

static bool is_separator(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';';
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* ASCII case-insensitive equality */
static bool same_text(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (lower(*a) != lower(*b))
			return false;
	}
	return *a == *b;
}

/* The last path component */
static const char *base_name(const char *name)
{
	const char *base = name;
	for (const char *c = name; *c; c++) {
		if (*c == '/' || *c == '\\')
			base = c + 1;
	}
	return base;
}

/* The whole extension after the last dot of the last component, or NULL */
static const char *extension(const char *name)
{
	const char *dot = strrchr(base_name(name), '.');
	return dot && dot[1] ? dot + 1 : NULL;
}

void pl_filter_parse(struct pl_filter *filter, const char *text)
{
	DARRAY(char *) extensions;
	da_init(extensions);
	const char *c = text ? text : "";

	while (*c) {
		while (is_separator(*c))
			c++;
		const char *start = c;
		while (*c && !is_separator(*c))
			c++;
		const char *end = c;
		if (end - start >= 2 && start[0] == '*' && start[1] == '.')
			start += 2;
		else if (end > start && start[0] == '.')
			start += 1;
		if (end == start)
			continue;

		char *ext = bstrdup_n(start, (size_t)(end - start));
		for (char *e = ext; *e; e++)
			*e = lower(*e);
		bool duplicate = false;
		for (size_t i = 0; i < extensions.num && !duplicate; i++)
			duplicate = strcmp(extensions.array[i], ext) == 0;
		if (duplicate)
			bfree(ext);
		else
			da_push_back(extensions, &ext);
	}
	filter->extensions = extensions.array;
	filter->count = extensions.num;
}

void pl_filter_free(struct pl_filter *filter)
{
	for (size_t i = 0; i < filter->count; i++)
		bfree(filter->extensions[i]);
	bfree(filter->extensions);
	filter->extensions = NULL;
	filter->count = 0;
}

bool pl_filter_matches(const struct pl_filter *filter, const char *name)
{
	const char *ext = extension(name);
	if (!ext)
		return false;
	for (size_t i = 0; i < filter->count; i++) {
		if (same_text(filter->extensions[i], ext))
			return true;
	}
	return false;
}

bool pl_filter_always_skipped(const char *name)
{
	static const char *const skipped_names[] = {"desktop.ini", "Thumbs.db"};
	static const char *const unfinished_extensions[] = {"tmp", "part", "crdownload", "partial"};
	const char *base = base_name(name);

	if (base[0] == '~' && base[1] == '$')
		return true;
	for (size_t i = 0; i < sizeof(skipped_names) / sizeof(skipped_names[0]); i++) {
		if (same_text(base, skipped_names[i]))
			return true;
	}
	const char *ext = extension(name);
	for (size_t i = 0; ext && i < sizeof(unfinished_extensions) / sizeof(unfinished_extensions[0]); i++) {
		if (same_text(ext, unfinished_extensions[i]))
			return true;
	}
	return false;
}

enum pl_entry_kind pl_filter_kind(const char *name)
{
	static const char *const stills[] = {"jpg", "jpeg", "png", "webp", "bmp"};
	const char *ext = extension(name);
	for (size_t i = 0; ext && i < sizeof(stills) / sizeof(stills[0]); i++) {
		if (same_text(ext, stills[i]))
			return PL_ENTRY_STILL;
	}
	return PL_ENTRY_MEDIA;
}
