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

#include "startup.h"

#include <string.h>

void pl_startup_begin(struct pl_startup *startup, long epoch, bool long_window, bool still, int64_t now_ms)
{
	startup->epoch = epoch;
	startup->still = still;
	startup->window_ms = long_window ? PL_STARTUP_WINDOW_LONG_MS : PL_STARTUP_WINDOW_MS;
	startup->elapsed_ms = 0;
	startup->last_ms = now_ms;
	startup->settled = false;
}

void pl_startup_clear(struct pl_startup *startup)
{
	memset(startup, 0, sizeof(*startup));
}

enum pl_startup_verdict pl_startup_evaluate(struct pl_startup *startup, const struct pl_startup_evidence *evidence,
					    int64_t now_ms)
{
	if (!startup->epoch || startup->settled)
		return PL_STARTUP_SETTLED;

	int64_t step = now_ms > startup->last_ms ? now_ms - startup->last_ms : 0;
	startup->last_ms = now_ms;

	if (evidence->media_time > 0 || evidence->audio) {
		startup->settled = true;
		return PL_STARTUP_SUCCEEDED;
	}
	if (evidence->ended && !startup->still) {
		startup->settled = true;
		return PL_STARTUP_FAILED;
	}
	/* a hidden or paused source never times out */
	if (evidence->may_advance)
		startup->elapsed_ms += step;
	if (startup->elapsed_ms >= startup->window_ms) {
		startup->settled = true;
		return PL_STARTUP_FAILED;
	}
	return PL_STARTUP_WAITING;
}
