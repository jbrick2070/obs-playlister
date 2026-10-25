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

/* The startup window (R-BEH-5): progress decides, an end without progress fails a media file but not a still, and
 * the window runs only while it may advance */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/startup.h"

static enum pl_startup_verdict at(struct pl_startup *startup, int64_t now_ms, int64_t media_time, bool audio,
				  bool ended, bool may_advance)
{
	struct pl_startup_evidence evidence = {media_time, audio, ended, may_advance};
	return pl_startup_evaluate(startup, &evidence, now_ms);
}

static void test_progress(void)
{
	struct pl_startup startup;
	pl_startup_begin(&startup, 7, false, false, 1000);
	assert(at(&startup, 1050, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 1100, 92, false, false, true) == PL_STARTUP_SUCCEEDED);
	/* reported once; an end afterwards is a normal end */
	assert(at(&startup, 1150, 0, false, true, true) == PL_STARTUP_SETTLED);

	/* audio counts too (an audio-only file before its time is read) */
	pl_startup_begin(&startup, 8, false, false, 0);
	assert(at(&startup, 50, 0, true, false, true) == PL_STARTUP_SUCCEEDED);

	/* progress and an end in the same tick: a short clip that played */
	pl_startup_begin(&startup, 9, false, false, 0);
	assert(at(&startup, 50, 40, false, true, true) == PL_STARTUP_SUCCEEDED);
}

static void test_end_before_progress(void)
{
	/* a missing or corrupt file ends at once, its time still 0 (the spike) */
	struct pl_startup startup;
	pl_startup_begin(&startup, 1, false, false, 0);
	assert(at(&startup, 20, 0, false, true, true) == PL_STARTUP_FAILED);
	assert(at(&startup, 40, 0, false, true, true) == PL_STARTUP_SETTLED);

	/* a still ends at once by nature: it waits for its time instead (a valid jpg reads 40, a corrupt one 0) */
	pl_startup_begin(&startup, 2, false, true, 0);
	assert(at(&startup, 3, 0, false, true, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 30, 40, false, true, true) == PL_STARTUP_SUCCEEDED);
	pl_startup_begin(&startup, 3, false, true, 0);
	assert(at(&startup, 3, 0, false, true, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 4999, 0, false, true, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 5000, 0, false, true, true) == PL_STARTUP_FAILED);
}

static void test_window(void)
{
	/* local files get 5 s */
	struct pl_startup startup;
	pl_startup_begin(&startup, 1, false, false, 0);
	assert(at(&startup, 4999, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 5000, 0, false, false, true) == PL_STARTUP_FAILED);

	/* URLs and shares 15 s */
	pl_startup_begin(&startup, 2, true, false, 0);
	assert(at(&startup, 14999, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 15000, 0, false, false, true) == PL_STARTUP_FAILED);

	/* hidden or paused: the window waits, however long */
	pl_startup_begin(&startup, 3, false, false, 0);
	assert(at(&startup, 3000, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 60000, 0, false, false, false) == PL_STARTUP_WAITING);
	assert(at(&startup, 61999, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 62000, 0, false, false, true) == PL_STARTUP_FAILED);

	/* a clock that steps back adds nothing */
	pl_startup_begin(&startup, 4, false, false, 10000);
	assert(at(&startup, 9000, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 13999, 0, false, false, true) == PL_STARTUP_WAITING);
	assert(at(&startup, 14000, 0, false, false, true) == PL_STARTUP_FAILED);
}

static void test_nothing_watched(void)
{
	struct pl_startup startup;
	pl_startup_clear(&startup);
	assert(at(&startup, 100, 0, false, true, true) == PL_STARTUP_SETTLED);
	assert(at(&startup, 100, 50, false, false, true) == PL_STARTUP_SETTLED);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_progress();
	test_end_before_progress();
	test_window();
	test_nothing_watched();
	assert(bnum_allocs() == allocations);
	printf("startup tests: passed\n");
	return 0;
}
