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
#include <graphics/graphics.h>

/* R-BEH-8: seamless cuts. OBS's media source shows nothing from the moment it starts a new file until that file's
 * first frame is decoded (obs-ffmpeg-source.c, ffmpeg_source_start), about 7 frames at 60 fps for a local mp4. When a
 * load replaces an item whose picture is on screen, the source keeps drawing that picture, captured once just before
 * the load, until the new item has a picture of its own. This is the decision part; video_render owns the texture.
 * Not the still hold of R-DIR-9, which is a playback timer. */

#define PL_FRAME_HOLD_CAP_MS 1000 /* a load that brings no picture in this time is not waited for any longer */

struct pl_frame_hold {
	bool holding;     /* the held frame is drawn instead of the child */
	int64_t since_ms; /* when the held frame was captured */
	long after_epoch; /* the applied load when the hold was last asked for: a newer one replaces the held item */
};

/* A replacing load asked for the child's picture. captured: video_render drew the child's picture into the hold just
 * now; that picture is held from now on. Not captured (the child had no picture, as between a load and its first
 * frame): a hold in progress keeps its frame and its start, and now waits for a load newer than applied_epoch. */
void pl_frame_hold_requested(struct pl_frame_hold *hold, bool captured, long applied_epoch, int64_t now_ms);

/* Each render: whether the held frame is drawn instead of the child. The hold ends for good once a load newer than
 * the one it was asked for has applied and the child has a picture again (that load's first frame), or at the cap. */
bool pl_frame_hold_tend(struct pl_frame_hold *hold, long applied_epoch, bool child_has_picture, int64_t now_ms);

/* Stop, clear, an end with nothing next, a hide-stop, no item: the held frame is let go at once */
void pl_frame_hold_release(struct pl_frame_hold *hold);

/* What video_render saw this render */
struct pl_frame_hold_input {
	bool release;           /* a stop, clear or blank since the last render (perhaps from while hidden) */
	bool has_media;         /* an item is loaded */
	bool capture_requested; /* the player asks for the child's picture before a replacing load */
	bool child_has_picture; /* libobs has a frame of the child to draw */
	long applied_epoch;     /* the load whose child update applied last */
	int64_t now_ms;         /* monotonic time */
};

/* Draws the child's picture into the held frame; true when it did. Called at most once per render. */
typedef bool (*pl_frame_capture_fn)(void *param);

/* One render, in this order: a release (or no item) lets the held frame go; then a capture request is answered,
 * capturing only while an item is loaded and the child has a picture; then the hold is tended. A release and a
 * capture request seen in the same render are a stop followed by a load, so the new capture survives. Returns whether
 * the held frame is drawn instead of the child; once it returns false the held texture is no longer needed. */
bool pl_frame_hold_render(struct pl_frame_hold *hold, const struct pl_frame_hold_input *input,
			  pl_frame_capture_fn capture, void *param);

/* How a held frame captured in one color space is drawn into the current one, as libobs draws an async frame
 * (obs-source.c, obs_source_render_async_video); normally both spaces are the same and it is a plain Draw */
enum pl_hold_multiplier {
	PL_HOLD_MULTIPLIER_NONE,
	PL_HOLD_MULTIPLIER_TO_SCRGB,   /* SDR white level / 80 */
	PL_HOLD_MULTIPLIER_FROM_SCRGB, /* 80 / SDR white level */
};

struct pl_hold_draw {
	const char *tech; /* a technique of OBS's default effect */
	enum pl_hold_multiplier multiplier;
	bool linear_srgb; /* the value for gs_set_linear_srgb while drawing */
};

/* linear_srgb: the caller's gs_get_linear_srgb(), kept where libobs keeps it */
void pl_frame_hold_draw_choice(enum gs_color_space captured, enum gs_color_space current, bool linear_srgb,
			       struct pl_hold_draw *draw);
