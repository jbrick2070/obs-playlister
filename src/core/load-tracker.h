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

#include <stdbool.h>

/* Which load an end of the child belongs to, and whether that load may auto-advance (R-BEH-6). All fields are
 * written with os_atomic_* from any thread.
 *
 * Producers take a request number for every load, stop or clear (pl_command_queue_push_request, so numbers follow
 * queue order) and call pl_load_tracker_withdraw before and after taking it. The player thread calls
 * pl_load_tracker_begin_load when it executes a load; only the newest request's load becomes eligible. The child's
 * media_ended handler calls pl_load_tracker_child_ended, which records the current load unless ends are being
 * ignored (while the child tears media down). video_tick calls pl_load_tracker_take_end, which is true at most once
 * per eligible load. */
struct pl_load_tracker {
	volatile long request_sequence; /* load, stop and clear requests numbered so far */
	volatile long load_epoch;       /* loads executed so far; the current load's number */
	volatile long eligible_epoch;   /* the load allowed to auto-advance, 0 for none */
	volatile long ended_epoch;      /* the load the child's last end belonged to, 0 for none */
	volatile bool ignore_ends;      /* set while the child tears media down: ends raised then are its own */
};

/* The current load may no longer advance, and a pending end is dropped */
void pl_load_tracker_withdraw(struct pl_load_tracker *tracker);
/* Withdraws and also supersedes every request numbered so far, without a request of its own: for a hide whose
 * stop runs later as a queued media action */
void pl_load_tracker_supersede(struct pl_load_tracker *tracker);
/* True when a newer request than this one has been numbered: executing the load is pointless */
bool pl_load_tracker_superseded(struct pl_load_tracker *tracker, long request);
/* Starts a new load epoch; it is eligible only while its request is the newest. Returns the epoch. */
long pl_load_tracker_begin_load(struct pl_load_tracker *tracker, long request);
void pl_load_tracker_set_ignore_ends(struct pl_load_tracker *tracker, bool ignore);
/* The child reported an end */
void pl_load_tracker_child_ended(struct pl_load_tracker *tracker);
/* Consumes a pending end; true when it belongs to the eligible load, which then stops being eligible */
bool pl_load_tracker_take_end(struct pl_load_tracker *tracker);
