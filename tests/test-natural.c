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

/* Natural order (R-DIR-6): numbers by value however long, ASCII case ignored, equal names compare equal, and the
 * order is a consistent one (antisymmetric and transitive) over a mixed sample. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "core/natural.h"

static int sign(int value)
{
	return (value > 0) - (value < 0);
}

static void expect_order(const char *first, const char *second)
{
	if (pl_natural_compare(first, second) >= 0 || pl_natural_compare(second, first) <= 0) {
		fprintf(stderr, "expected \"%s\" before \"%s\"\n", first ? first : "(null)",
			second ? second : "(null)");
		abort();
	}
}

static void expect_equal(const char *a, const char *b)
{
	if (pl_natural_compare(a, b) != 0 || pl_natural_compare(b, a) != 0) {
		fprintf(stderr, "expected \"%s\" and \"%s\" to compare equal\n", a, b);
		abort();
	}
}

static void test_numbers(void)
{
	expect_order("ep2", "ep10");
	expect_order("ep9", "ep10");
	expect_order("track 2.mp3", "track 12.mp3");
	expect_order("s1e9", "s1e10");
	expect_order("s1e10", "s2e1");
	/* longer than any integer type: compared by length, then digits */
	expect_order("file99999999999999999999999", "file100000000000000000000000");
	expect_order("file100000000000000000000000", "file100000000000000000000001");
	expect_equal("ep01", "ep1");
	expect_equal("x0", "x000");
	expect_order("x0", "x1");
}

static void test_text(void)
{
	expect_order("a", "b");
	expect_order("ab", "abc");
	expect_order("abc", "abd");
	expect_equal("Episode", "episode");
	expect_equal("EP1.MP4", "ep1.mp4");
	/* other bytes by value: a UTF-8 lead byte sorts after ASCII */
	expect_order("z", "\xc3\xa9");
	expect_order("1a", "a");
	expect_order("", "a");
	expect_equal("", "");
}

static void test_null(void)
{
	expect_order(NULL, "a");
	expect_order(NULL, "");
	assert(pl_natural_compare(NULL, NULL) == 0);
}

/* sort algorithms rely on this: reversing the arguments reverses the result, and order carries over */
static void test_consistent_order(void)
{
	const char *names[] = {"ep1",  "Ep1",   "ep01",     "ep2",   "ep10",    "ep010",    "ep9",       "a",
			       "A",    "b10c2", "b10c10",   "b9c99", "b010c3",  "",         "x",         "x0",
			       "x00a", "x0a",   "\xc3\xa9", "z",     "track 2", "track 12", "track 012", "track2"};
	size_t count = sizeof(names) / sizeof(names[0]);
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < count; j++) {
			int ij = sign(pl_natural_compare(names[i], names[j]));
			assert(ij == -sign(pl_natural_compare(names[j], names[i])));
			for (size_t k = 0; k < count; k++) {
				int jk = sign(pl_natural_compare(names[j], names[k]));
				int ik = sign(pl_natural_compare(names[i], names[k]));
				if (ij <= 0 && jk <= 0)
					assert(ik <= 0);
				if (ij == 0 && jk == 0)
					assert(ik == 0);
			}
		}
	}
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_numbers();
	test_text();
	test_null();
	test_consistent_order();
	printf("natural order tests: passed\n");
	return 0;
}
