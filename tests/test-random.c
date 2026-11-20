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

/* The shuffle's random numbers (core/random.h): a fixed seed gives a fixed sequence; a draw without a seed is seeded
 * by itself, differently each time; a draw stays below its bound and reaches every value, also far above 32767 (the
 * most the C library's rand() gives on Windows); and another thread goes on in the same sequence instead of starting
 * its own, as rand() does on Windows. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/threading.h>
#include "test-common.h"

#include "core/random.h"

#define DRAWS 8

static void draw(uint64_t out[DRAWS])
{
	for (size_t i = 0; i < DRAWS; i++)
		out[i] = pl_random_next();
}

static void *draw_on_thread(void *data)
{
	draw(data);
	return NULL;
}

static void test_seeds(void)
{
	uint64_t a[DRAWS], b[DRAWS], c[DRAWS];

	/* before any seed: seeded by itself, and not with a constant */
	draw(a);
	pl_random_seed();
	draw(b);
	assert(memcmp(a, b, sizeof(a)) != 0);
	pl_random_seed();
	draw(c);
	assert(memcmp(b, c, sizeof(b)) != 0);

	/* a fixed seed: the same numbers again, and other numbers for another seed */
	pl_random_seed_with(1234);
	draw(a);
	pl_random_seed_with(1234);
	draw(b);
	assert(memcmp(a, b, sizeof(a)) == 0);
	pl_random_seed_with(1235);
	draw(c);
	assert(memcmp(a, c, sizeof(a)) != 0);
	/* not stuck on one value */
	assert(a[0] != a[1] && a[1] != a[2]);
}

/* One generator for every thread: what a second thread draws is the continuation of the sequence, not a sequence of
 * its own from a seed of its own */
static void test_threads_share_the_generator(void)
{
	uint64_t one_thread[2 * DRAWS];
	pl_random_seed_with(99);
	draw(one_thread);
	draw(one_thread + DRAWS);

	uint64_t first[DRAWS], second[DRAWS];
	pl_random_seed_with(99);
	draw(first);
	pthread_t thread;
	assert(pthread_create(&thread, NULL, draw_on_thread, second) == 0);
	pthread_join(thread, NULL);
	assert(memcmp(first, one_thread, sizeof(first)) == 0);
	assert(memcmp(second, one_thread + DRAWS, sizeof(second)) == 0);
}

static void test_bounds(void)
{
	pl_random_seed_with(7);
	assert(pl_random_below(0) == 0);
	assert(pl_random_below(1) == 0);

	/* every value of a small range comes up, about equally often */
	size_t counts[6] = {0};
	for (size_t i = 0; i < 60000; i++) {
		size_t value = pl_random_below(6);
		assert(value < 6);
		counts[value]++;
	}
	for (size_t i = 0; i < 6; i++)
		assert(counts[i] > 9000 && counts[i] < 11000);

	/* a range far above 32767: the upper part is reached, and nothing is out of range */
	const size_t large = 1000000;
	size_t above = 0;
	for (size_t i = 0; i < 20000; i++) {
		size_t value = pl_random_below(large);
		assert(value < large);
		above += value >= 32768;
	}
	assert(above > 18000);

	/* a bound just over half the 64-bit range, where most draws are rejected by a biased method or loop for
	 * ever in a wrong one */
	if (sizeof(size_t) == 8) {
		size_t huge = (size_t)((UINT64_MAX / 2) + 2);
		for (size_t i = 0; i < 1000; i++)
			assert(pl_random_below(huge) < huge);
	}
}

int main(void)
{
	pl_test_no_crash_dialogs();
	test_seeds();
	test_threads_share_the_generator();
	test_bounds();
	printf("test-random: all passed\n");
	return 0;
}
