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

/* What a list change does to a source that is not playing (core/standstill.h; review findings Q-03 and Q-04): every
 * combination of state, kind of stop, visibility behaviour and visibility. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "test-common.h"

#include "core/standstill.h"

static enum pl_standstill decide(enum pl_playback playback, bool by_request, bool pause_when_hidden, bool active)
{
	struct pl_standstill_input input = {playback, by_request, pause_when_hidden, active};
	return pl_standstill_decide(&input);
}

int main(void)
{
	pl_test_no_crash_dialogs();

	for (int by_request = 0; by_request < 2; by_request++) {
		for (int pause_when_hidden = 0; pause_when_hidden < 2; pause_when_hidden++) {
			for (int active = 0; active < 2; active++) {
				/* a playing source goes on to what follows, whatever else is set */
				assert(decide(PL_PLAYBACK_PLAYING, by_request, pause_when_hidden, active) ==
				       PL_STANDSTILL_NONE);
				/* after the end of the list nothing starts by itself */
				assert(decide(PL_PLAYBACK_ENDED, by_request, pause_when_hidden, active) ==
				       PL_STANDSTILL_PARK);
				/* a paused source never goes on playing by itself; only one that pauses when hidden and is
				 * hidden plays again when it is shown */
				enum pl_standstill paused =
					decide(PL_PLAYBACK_PAUSED, by_request, pause_when_hidden, active);
				assert(paused == (pause_when_hidden && !active ? PL_STANDSTILL_PARK_TO_SHOW
									       : PL_STANDSTILL_PARK));
				/* stopped on request: nothing starts; stopped for want of anything to play: the change
				 * starts playback, as adding the first file to an empty list must */
				assert(decide(PL_PLAYBACK_STOPPED, by_request, pause_when_hidden, active) ==
				       (by_request ? PL_STANDSTILL_PARK : PL_STANDSTILL_NONE));
			}
		}
	}
	printf("test-standstill: all passed\n");
	return 0;
}
