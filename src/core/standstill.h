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

/* What a list change does when it takes the loaded item away, or replaces it, while the source is not playing: the
 * item's row was deleted or edited in Properties, or the item left its folder or playlist row. While the source
 * plays, playback moves on to the item that follows, as before. While it stands still because someone asked it to
 * (paused, stopped by the user or by a hide, or at the end of the list), nothing may start by itself: the child lets
 * go of the item, and the position waits where Play or Restart will start. Without this a paused item that was
 * removed could be resumed with sound and no picture, and tidying the list after a stop started playback on air.
 * No OBS types here, so the rule can be tested by itself. */

enum pl_playback {
	PL_PLAYBACK_PLAYING,
	PL_PLAYBACK_PAUSED,
	PL_PLAYBACK_STOPPED,
	PL_PLAYBACK_ENDED, /* the end of the list was reached and nothing followed */
};

struct pl_standstill_input {
	enum pl_playback playback; /* the source's own state */
	/* STOPPED only: a stop was asked for (Stop, or a hide under "stop when not visible"), as opposed to the source
	 * having nothing it could play, where a list change is what starts it */
	bool stopped_by_request;
	bool pause_when_hidden; /* the visibility behaviour is pause / unpause */
	bool active;            /* the source is shown in Program */
};

enum pl_standstill {
	PL_STANDSTILL_NONE,         /* playing, or stopped for want of anything to play: a change loads as before */
	PL_STANDSTILL_PARK,         /* nothing loads: the position waits for Play or Restart */
	PL_STANDSTILL_PARK_TO_SHOW, /* as PARK, and showing the source plays from the position: it was paused only
	                             * because it was hidden, and showing it would have resumed the item that is gone */
};

enum pl_standstill pl_standstill_decide(const struct pl_standstill_input *input);
