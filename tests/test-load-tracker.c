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

/* Trace tests for the load tracker (R-BEH-6): which end of the child may auto-advance. Each test replays the
 * calls the producers, the player thread, the child's handler and video_tick make, in one fixed order. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/threading.h>
#include <windows.h>

#include "core/load-tracker.h"

/* what push_request does for a load, stop or clear: withdraw, number the request, withdraw */
static long request(struct pl_load_tracker *tracker)
{
	pl_load_tracker_withdraw(tracker);
	long number = os_atomic_inc_long(&tracker->request_sequence);
	pl_load_tracker_withdraw(tracker);
	return number;
}

static void test_end_of_current_load_advances_once(void)
{
	struct pl_load_tracker tracker = {0};
	long load = request(&tracker);
	pl_load_tracker_begin_load(&tracker, load);
	pl_load_tracker_child_ended(&tracker);
	assert(pl_load_tracker_take_end(&tracker));
	/* nothing pending any more, and a second end of the same load cannot advance again */
	assert(!pl_load_tracker_take_end(&tracker));
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

static void test_end_before_any_load_is_ignored(void)
{
	struct pl_load_tracker tracker = {0};
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

/* Next pressed while item 1 plays: item 1's end, raised before the new load executes, must not advance */
static void test_end_of_replaced_load_is_ignored(void)
{
	struct pl_load_tracker tracker = {0};
	pl_load_tracker_begin_load(&tracker, request(&tracker));
	long second = request(&tracker);
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
	pl_load_tracker_begin_load(&tracker, second);
	pl_load_tracker_child_ended(&tracker);
	assert(pl_load_tracker_take_end(&tracker));
}

/* The child tears down media that was still opening and reports ENDED for it: nothing is recorded */
static void test_end_while_tearing_down_is_ignored(void)
{
	struct pl_load_tracker tracker = {0};
	pl_load_tracker_begin_load(&tracker, request(&tracker));
	long second = request(&tracker);
	pl_load_tracker_set_ignore_ends(&tracker, true);
	pl_load_tracker_child_ended(&tracker);
	assert(os_atomic_load_long(&tracker.ended_epoch) == 0);
	pl_load_tracker_set_ignore_ends(&tracker, false);
	pl_load_tracker_begin_load(&tracker, second);
	assert(!pl_load_tracker_take_end(&tracker));
}

/* A speed or decoder change makes the child reopen the current, eligible media; the end the teardown reports
 * while ends are ignored must not advance */
static void test_end_while_reopening_for_settings_is_ignored(void)
{
	struct pl_load_tracker tracker = {0};
	pl_load_tracker_begin_load(&tracker, request(&tracker));
	pl_load_tracker_set_ignore_ends(&tracker, true);
	pl_load_tracker_child_ended(&tracker);
	pl_load_tracker_set_ignore_ends(&tracker, false);
	assert(!pl_load_tracker_take_end(&tracker));
	/* the reopened media's own end still advances */
	pl_load_tracker_child_ended(&tracker);
	assert(pl_load_tracker_take_end(&tracker));
}

/* Two loads queued; the older one is superseded, executes ineligible (or is skipped), and only the newer advances */
static void test_superseded_load_is_not_eligible(void)
{
	struct pl_load_tracker tracker = {0};
	long first = request(&tracker);
	long second = request(&tracker);
	assert(pl_load_tracker_superseded(&tracker, first));
	assert(!pl_load_tracker_superseded(&tracker, second));
	pl_load_tracker_begin_load(&tracker, first);
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
	pl_load_tracker_begin_load(&tracker, second);
	pl_load_tracker_child_ended(&tracker);
	assert(pl_load_tracker_take_end(&tracker));
}

/* A load executed, then a stop requested: the ENDED that may follow the stop must not advance */
static void test_stop_after_load_does_not_advance(void)
{
	struct pl_load_tracker tracker = {0};
	pl_load_tracker_begin_load(&tracker, request(&tracker));
	request(&tracker); /* the stop */
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

/* A load queued, then a stop queued behind it, both before the player runs: the load executes after the stop was
 * requested and must not become eligible, so a quick end cannot override the stop */
static void test_load_executed_after_a_later_stop_is_not_eligible(void)
{
	struct pl_load_tracker tracker = {0};
	long load = request(&tracker);
	request(&tracker); /* the stop */
	pl_load_tracker_begin_load(&tracker, load);
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

/* A hide supersedes without a request of its own (pl_player_withdraw): a load queued before it becomes pointless,
 * and if it executes anyway it is not eligible */
static void test_hide_supersedes_queued_load(void)
{
	struct pl_load_tracker tracker = {0};
	long load = request(&tracker);
	pl_load_tracker_supersede(&tracker);
	assert(pl_load_tracker_superseded(&tracker, load));
	pl_load_tracker_begin_load(&tracker, load);
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

/* A request made right after a load began withdraws that load at once. (The other order, a request numbered
 * between begin_load's set and its check, is covered by the check itself; see
 * test_load_executed_after_a_later_stop_is_not_eligible.) */
static void test_request_after_begin_load_withdraws_it(void)
{
	struct pl_load_tracker tracker = {0};
	long load = request(&tracker);
	long epoch = pl_load_tracker_begin_load(&tracker, load);
	assert(os_atomic_load_long(&tracker.eligible_epoch) == epoch);
	request(&tracker);
	assert(os_atomic_load_long(&tracker.eligible_epoch) == 0);
	pl_load_tracker_child_ended(&tracker);
	assert(!pl_load_tracker_take_end(&tracker));
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_end_of_current_load_advances_once();
	test_end_before_any_load_is_ignored();
	test_end_of_replaced_load_is_ignored();
	test_end_while_tearing_down_is_ignored();
	test_end_while_reopening_for_settings_is_ignored();
	test_superseded_load_is_not_eligible();
	test_stop_after_load_does_not_advance();
	test_load_executed_after_a_later_stop_is_not_eligible();
	test_hide_supersedes_queued_load();
	test_request_after_begin_load_withdraws_it();
	printf("load tracker tests: passed\n");
	return 0;
}
