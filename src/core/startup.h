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
#include <stdint.h>

/* R-BEH-5: whether a load played. media_started proves nothing (the Phase 2 step 6 spike saw it for a missing file
 * too), so each load opens a startup window, checked every tick, and only progress counts: the media time past 0,
 * or audio delivered after the load. The video size does not count: the spike saw a size kept from an earlier still
 * come back at the start of a later load. */

#define PL_STARTUP_WINDOW_MS 5000       /* local files */
#define PL_STARTUP_WINDOW_LONG_MS 15000 /* URLs and UNC paths */

enum pl_startup_verdict {
	PL_STARTUP_WAITING,   /* no progress yet, the window still open */
	PL_STARTUP_SUCCEEDED, /* progress seen; reported once */
	PL_STARTUP_FAILED,    /* ended before progress, or the window ran out; reported once */
	PL_STARTUP_SETTLED,   /* decided earlier, or nothing watched */
};

struct pl_startup {
	long epoch;         /* the load watched, 0 for none */
	bool still;         /* a still image ends at once by nature, so its end is no failure (R-DIR-9) */
	int64_t window_ms;  /* the window's length */
	int64_t elapsed_ms; /* window time used: it runs only while the window may advance */
	int64_t last_ms;    /* monotonic time of the last evaluation */
	bool settled;
};

/* What the source saw of the watched load this tick */
struct pl_startup_evidence {
	int64_t media_time; /* the child's media time, ms */
	bool audio;         /* the child delivered audio after the load */
	bool ended;         /* the child ended this load */
	bool may_advance;   /* playing, and active or Always play without close-when-inactive: else the window waits */
};

void pl_startup_begin(struct pl_startup *startup, long epoch, bool long_window, bool still, int64_t now_ms);
void pl_startup_clear(struct pl_startup *startup);

/* Decides on the watched load. Progress wins over an end seen in the same tick (a short clip can end quickly); an
 * end without progress fails a media file but not a still, which then needs progress within the window. */
enum pl_startup_verdict pl_startup_evaluate(struct pl_startup *startup, const struct pl_startup_evidence *evidence,
					    int64_t now_ms);
