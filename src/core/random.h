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

#include <stddef.h>
#include <stdint.h>

/* Playlister's own random numbers, for the shuffle. The C library's rand() is not used: on Windows its state is kept
 * per thread and starts at the same seed on every thread, and libobs seeds only its graphics thread, so a Next from
 * a hotkey or an obs-websocket request drew the same numbers in every OBS session; and its largest value there is
 * 32767, so a longer list could not be drawn from evenly. One generator (xoshiro256**) serves every source and every
 * thread, under its own lock. Not for anything that must be unpredictable. */

/* Seeds the generator from the operating system (and the clocks, should that fail). Called once when the module
 * loads; a draw made before any seed seeds it the same way. */
void pl_random_seed(void);

/* For the tests: a fixed seed, so the numbers that follow are the same on every run */
void pl_random_seed_with(uint64_t seed);

/* The next 64 random bits. Any thread. */
uint64_t pl_random_next(void);

/* A number from 0 to bound - 1, each as likely as the others (no modulo bias); 0 when bound is 0. Any thread. */
size_t pl_random_below(size_t bound);
