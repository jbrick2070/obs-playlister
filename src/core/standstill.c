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

#include "standstill.h"

enum pl_standstill pl_standstill_decide(const struct pl_standstill_input *input)
{
	switch (input->playback) {
	case PL_PLAYBACK_PLAYING:
		return PL_STANDSTILL_NONE;
	case PL_PLAYBACK_PAUSED:
		/* shown again, a source that pauses when hidden is unpaused whoever paused it */
		return input->pause_when_hidden && !input->active ? PL_STANDSTILL_PARK_TO_SHOW : PL_STANDSTILL_PARK;
	case PL_PLAYBACK_ENDED:
		return PL_STANDSTILL_PARK;
	case PL_PLAYBACK_STOPPED:
		return input->stopped_by_request ? PL_STANDSTILL_PARK : PL_STANDSTILL_NONE;
	}
	return PL_STANDSTILL_NONE;
}
