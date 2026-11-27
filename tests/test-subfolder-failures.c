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

/* R-DIR-1: a subfolder whose error may pass fails the listing, so the last good list stays, until it has failed three
 * listings in a row; with no good list yet it is left out at once and stays left out; one that did not fail in a
 * complete listing starts over; at most PL_MAX_FAILING subfolders are followed. Runs on every system (the Windows
 * test with a real folder is test-provider-folder's test_subfolder_that_fails). */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include "test-common.h"

#include "core/subfolder-failures.h"

/* One listing in which the given subfolder fails; returns what the listing was told. A listing that fails does not
 * complete; one where every failing subfolder was left out does. */
static bool listing(struct pl_subfolder_failures *failures, const char *relative, bool has_good_list)
{
	pl_subfolder_failures_begin(failures);
	bool left_out = pl_subfolder_failures_failed(failures, relative, has_good_list);
	if (left_out)
		pl_subfolder_failures_complete(failures);
	return left_out;
}

static void complete_listing(struct pl_subfolder_failures *failures)
{
	pl_subfolder_failures_begin(failures);
	pl_subfolder_failures_complete(failures);
}

static void test_with_a_good_list(void)
{
	struct pl_subfolder_failures failures;
	pl_subfolder_failures_init(&failures);
	/* two failed listings keep the list; the third leaves the subfolder out, and so does every later one */
	assert(!listing(&failures, "a", true));
	assert(!listing(&failures, "a", true));
	assert(listing(&failures, "a", true));
	assert(listing(&failures, "a", true));
	/* readable again: a complete listing it did not fail in, and it starts over */
	complete_listing(&failures);
	assert(failures.items.num == 0);
	assert(!listing(&failures, "a", true));
	/* a failure in between that is not in a row also starts over */
	complete_listing(&failures);
	assert(!listing(&failures, "a", true));
	assert(!listing(&failures, "a", true));
	/* asked twice in one listing counts once */
	pl_subfolder_failures_begin(&failures);
	assert(pl_subfolder_failures_failed(&failures, "a", true));
	assert(pl_subfolder_failures_failed(&failures, "a", true));
	assert(failures.items.array[0].listings == 3);
	pl_subfolder_failures_free(&failures);
}

static void test_without_a_good_list(void)
{
	struct pl_subfolder_failures failures;
	pl_subfolder_failures_init(&failures);
	/* the first listing leaves it out, and the listings after it (now with a good list) do too: the row never goes
	 * into an error and back */
	assert(listing(&failures, "a", false));
	assert(listing(&failures, "a", true));
	assert(listing(&failures, "a", true));
	/* readable again: starts over */
	complete_listing(&failures);
	assert(!listing(&failures, "a", true));
	pl_subfolder_failures_free(&failures);
}

static void test_bounded(void)
{
	struct pl_subfolder_failures failures;
	pl_subfolder_failures_init(&failures);
	char name[32];
	/* each fails first in a listing of its own, folder-0 earliest */
	for (int i = 0; i < PL_MAX_FAILING; i++) {
		snprintf(name, sizeof(name), "folder-%d", i);
		pl_subfolder_failures_begin(&failures);
		assert(pl_subfolder_failures_failed(&failures, name, false));
	}
	assert(failures.items.num == PL_MAX_FAILING);
	/* folder-0 fails again, so folder-1 is now the one followed longest without failing */
	pl_subfolder_failures_begin(&failures);
	assert(pl_subfolder_failures_failed(&failures, "folder-0", true));
	assert(pl_subfolder_failures_failed(&failures, "one more", true) == false);
	assert(failures.items.num == PL_MAX_FAILING);
	bool zero = false, one = false, more = false;
	for (size_t i = 0; i < failures.items.num; i++) {
		zero |= strcmp(failures.items.array[i].relative, "folder-0") == 0;
		one |= strcmp(failures.items.array[i].relative, "folder-1") == 0;
		more |= strcmp(failures.items.array[i].relative, "one more") == 0;
	}
	assert(zero && !one && more);
	pl_subfolder_failures_free(&failures);
}

int main(void)
{
	pl_test_no_crash_dialogs();
	long allocations = bnum_allocs();
	test_with_a_good_list();
	test_without_a_good_list();
	test_bounded();
	assert(bnum_allocs() == allocations);
	printf("subfolder failure tests: passed\n");
	return 0;
}
