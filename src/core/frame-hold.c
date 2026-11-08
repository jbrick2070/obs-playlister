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

#include "frame-hold.h"

void pl_frame_hold_requested(struct pl_frame_hold *hold, bool captured, long applied_epoch, int64_t now_ms)
{
	if (captured) {
		hold->holding = true;
		hold->since_ms = now_ms;
		hold->after_epoch = applied_epoch;
	} else if (hold->holding) {
		/* the load before this one has not shown a picture yet: the held frame now waits for this one */
		hold->after_epoch = applied_epoch;
	}
}

bool pl_frame_hold_tend(struct pl_frame_hold *hold, long applied_epoch, bool child_has_picture, int64_t now_ms)
{
	if (!hold->holding)
		return false;
	int64_t held_ms = now_ms > hold->since_ms ? now_ms - hold->since_ms : 0;
	/* Until a newer load has applied, a picture the child has is still the replaced item's. After it, a picture is
	 * the new item's: the child shows nothing from the start of a new file until its first frame. */
	bool replaced = applied_epoch != hold->after_epoch && child_has_picture;
	if (replaced || held_ms >= PL_FRAME_HOLD_CAP_MS)
		hold->holding = false;
	return hold->holding;
}

void pl_frame_hold_release(struct pl_frame_hold *hold)
{
	hold->holding = false;
}

bool pl_frame_hold_render(struct pl_frame_hold *hold, const struct pl_frame_hold_input *input,
			  pl_frame_capture_fn capture, void *param)
{
	if (input->release || !input->has_media)
		pl_frame_hold_release(hold);
	if (input->capture_requested) {
		bool captured = input->has_media && input->child_has_picture && capture(param);
		pl_frame_hold_requested(hold, captured, input->applied_epoch, input->now_ms);
	}
	return pl_frame_hold_tend(hold, input->applied_epoch, input->child_has_picture, input->now_ms);
}

void pl_frame_hold_draw_choice(enum gs_color_space captured, enum gs_color_space current, bool linear_srgb,
			       struct pl_hold_draw *draw)
{
	draw->tech = "Draw";
	draw->multiplier = PL_HOLD_MULTIPLIER_NONE;
	draw->linear_srgb = linear_srgb;

	switch (captured) {
	case GS_CS_SRGB:
		/* 8-bit, sRGB-encoded: decoded to linear wherever the target is not plain sRGB */
		draw->linear_srgb = linear_srgb || current != GS_CS_SRGB;
		if (current == GS_CS_709_SCRGB) {
			draw->tech = "DrawMultiply";
			draw->multiplier = PL_HOLD_MULTIPLIER_TO_SCRGB;
		}
		break;
	case GS_CS_SRGB_16F:
		if (current == GS_CS_709_SCRGB) {
			draw->tech = "DrawMultiply";
			draw->multiplier = PL_HOLD_MULTIPLIER_TO_SCRGB;
		}
		break;
	case GS_CS_709_EXTENDED:
		if (current == GS_CS_SRGB || current == GS_CS_SRGB_16F) {
			draw->tech = "DrawTonemap";
			draw->linear_srgb = true;
		} else if (current == GS_CS_709_SCRGB) {
			draw->tech = "DrawMultiply";
			draw->multiplier = PL_HOLD_MULTIPLIER_TO_SCRGB;
		}
		break;
	case GS_CS_709_SCRGB:
		if (current == GS_CS_SRGB || current == GS_CS_SRGB_16F) {
			draw->tech = "DrawMultiplyTonemap";
			draw->multiplier = PL_HOLD_MULTIPLIER_FROM_SCRGB;
			draw->linear_srgb = true;
		} else if (current == GS_CS_709_EXTENDED) {
			draw->tech = "DrawMultiply";
			draw->multiplier = PL_HOLD_MULTIPLIER_FROM_SCRGB;
		}
		break;
	}
}
