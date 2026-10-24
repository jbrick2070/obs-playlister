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

#include "load-tracker.h"

#include <util/threading.h>

void pl_load_tracker_withdraw(struct pl_load_tracker *tracker)
{
	os_atomic_set_long(&tracker->eligible_epoch, 0);
	os_atomic_set_long(&tracker->ended_epoch, 0);
}

void pl_load_tracker_supersede(struct pl_load_tracker *tracker)
{
	pl_load_tracker_withdraw(tracker);
	os_atomic_inc_long(&tracker->request_sequence);
	pl_load_tracker_withdraw(tracker);
}

bool pl_load_tracker_superseded(struct pl_load_tracker *tracker, long request)
{
	return request != os_atomic_load_long(&tracker->request_sequence);
}

long pl_load_tracker_begin_load(struct pl_load_tracker *tracker, long request)
{
	long epoch = os_atomic_inc_long(&tracker->load_epoch);
	os_atomic_set_long(&tracker->ended_epoch, 0);
	/* Set, then check: a producer numbers its request and withdraws again afterwards, so in every interleaving
	 * a load superseded meanwhile ends up ineligible */
	os_atomic_set_long(&tracker->eligible_epoch, epoch);
	if (pl_load_tracker_superseded(tracker, request))
		os_atomic_set_long(&tracker->eligible_epoch, 0);
	return epoch;
}

void pl_load_tracker_set_ignore_ends(struct pl_load_tracker *tracker, bool ignore)
{
	os_atomic_set_bool(&tracker->ignore_ends, ignore);
}

void pl_load_tracker_child_ended(struct pl_load_tracker *tracker)
{
	if (os_atomic_load_bool(&tracker->ignore_ends))
		return;
	os_atomic_set_long(&tracker->ended_epoch, os_atomic_load_long(&tracker->load_epoch));
}

bool pl_load_tracker_take_end(struct pl_load_tracker *tracker)
{
	long ended = os_atomic_exchange_long(&tracker->ended_epoch, 0);
	/* consuming the eligibility makes a second end of the same load harmless */
	return ended != 0 && os_atomic_compare_swap_long(&tracker->eligible_epoch, ended, 0);
}
