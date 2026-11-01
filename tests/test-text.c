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

/* R-I18N-1, R-I18N-2: pl_text gives the locale's text for a key, or the English given when no locale is loaded or
 * the locale has none; pl_text_cat fills %1 to %9 and never reads past the values. */

#undef NDEBUG
#include <assert.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include "test-common.h"

#include "core/text.h"

static const char *german(const char *key)
{
	if (strcmp(key, "Greeting") == 0)
		return "Hallo %1, %2 Dateien";
	if (strcmp(key, "Empty") == 0)
		return "";
	return NULL;
}

static void expect_cat(const char *text, const char *const *values, size_t count, const char *expected)
{
	struct dstr out = {0};
	dstr_copy(&out, "> ");
	pl_text_cat(&out, text, values, count);
	assert(strcmp(out.array, expected) == 0);
	dstr_free(&out);
}

static void test_lookup(void)
{
	/* no locale loaded: the English given */
	assert(strcmp(pl_text("Greeting", "Hello %1"), "Hello %1") == 0);
	pl_text_set_lookup(german);
	assert(strcmp(pl_text("Greeting", "Hello %1"), "Hallo %1, %2 Dateien") == 0);
	/* a key the locale lacks, and an empty text, fall back to English */
	assert(strcmp(pl_text("Missing", "fallback"), "fallback") == 0);
	assert(strcmp(pl_text("Empty", "fallback"), "fallback") == 0);
	assert(strcmp(pl_text(NULL, "no key"), "no key") == 0);
	pl_text_set_lookup(NULL);
	assert(strcmp(pl_text("Greeting", "Hello %1"), "Hello %1") == 0);
}

static void test_placeholders(void)
{
	const char *two[] = {"Ada", "3"};
	expect_cat("Hallo %1, %2 Dateien", two, 2, "> Hallo Ada, 3 Dateien");
	/* out of order and repeated */
	expect_cat("%2 then %1 and %1", two, 2, "> 3 then Ada and Ada");
	/* a placeholder with no value is left out, never read past the values */
	expect_cat("only %1 and %3", two, 2, "> only Ada and ");
	expect_cat("%9", two, 2, "> ");
	const char *none[] = {NULL};
	expect_cat("a %1 b", none, 1, "> a  b");
	expect_cat("a %1 b", NULL, 0, "> a  b");
	/* %10 is %1 followed by "0"; any other % stays */
	expect_cat("%10", two, 2, "> Ada0");
	expect_cat("100% sure, %s %d %%", two, 2, "> 100% sure, %s %d %%");
	expect_cat("ends with %", two, 2, "> ends with %");
	expect_cat("", two, 2, "> ");
	expect_cat(NULL, two, 2, "> ");
	/* UTF-8 passes through unchanged */
	const char *word[] = {"\xE5\x90\x8D"};
	expect_cat("\xE6\x96\x87%1\xE5\xAD\x97", word, 1, "> \xE6\x96\x87\xE5\x90\x8D\xE5\xAD\x97");
}

int main(void)
{
	pl_test_no_crash_dialogs();
	long allocations = bnum_allocs();
	test_lookup();
	test_placeholders();
	assert(bnum_allocs() == allocations);
	return 0;
}
