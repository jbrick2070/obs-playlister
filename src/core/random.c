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

#ifdef _WIN32
#define _CRT_RAND_S /* rand_s, before stdlib.h */
#endif

#include "random.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <util/platform.h>
#include <util/threading.h>

static pthread_mutex_t random_mutex = PTHREAD_MUTEX_INITIALIZER;
/* under random_mutex */
static uint64_t state[4];
static bool seeded;

/* splitmix64: spreads one seed over the generator's state, which must not be all zero */
static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void seed_locked(uint64_t seed)
{
	for (size_t i = 0; i < 4; i++)
		state[i] = splitmix64(&seed);
	seeded = true;
}

/* 64 bits from the operating system's own generator; false when it gave none */
static bool os_entropy(uint64_t *value)
{
#ifdef _WIN32
	unsigned int high = 0;
	unsigned int low = 0;
	if (rand_s(&high) != 0 || rand_s(&low) != 0)
		return false;
	*value = ((uint64_t)high << 32) | low;
	return true;
#else
	FILE *file = fopen("/dev/urandom", "rb");
	if (!file)
		return false;
	bool ok = fread(value, sizeof(*value), 1, file) == 1;
	fclose(file);
	return ok;
#endif
}

static uint64_t fresh_seed(void)
{
	/* the clocks are mixed in as well, and are all there is should the system give nothing */
	uint64_t seed = os_gettime_ns() ^ ((uint64_t)time(NULL) << 32) ^ (uint64_t)(uintptr_t)&seed;
	uint64_t entropy = 0;
	if (os_entropy(&entropy))
		seed ^= entropy;
	return seed;
}

void pl_random_seed(void)
{
	uint64_t seed = fresh_seed();
	pthread_mutex_lock(&random_mutex);
	seed_locked(seed);
	pthread_mutex_unlock(&random_mutex);
}

void pl_random_seed_with(uint64_t seed)
{
	pthread_mutex_lock(&random_mutex);
	seed_locked(seed);
	pthread_mutex_unlock(&random_mutex);
}

static uint64_t rotate_left(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

/* xoshiro256** by David Blackman and Sebastiano Vigna (public domain) */
uint64_t pl_random_next(void)
{
	pthread_mutex_lock(&random_mutex);
	if (!seeded)
		seed_locked(fresh_seed());
	uint64_t result = rotate_left(state[1] * 5, 7) * 9;
	uint64_t t = state[1] << 17;
	state[2] ^= state[0];
	state[3] ^= state[1];
	state[1] ^= state[2];
	state[0] ^= state[3];
	state[2] ^= t;
	state[3] = rotate_left(state[3], 45);
	pthread_mutex_unlock(&random_mutex);
	return result;
}

size_t pl_random_below(size_t bound)
{
	if (bound < 2)
		return 0;
	/* the values below 2^64 mod bound are drawn again, so every remainder is as likely as the others */
	uint64_t range = bound;
	uint64_t threshold = (0 - range) % range;
	uint64_t value;
	do {
		value = pl_random_next();
	} while (value < threshold);
	return (size_t)(value % range);
}
