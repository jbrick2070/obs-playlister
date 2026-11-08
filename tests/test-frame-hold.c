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

/* The frame hold (R-BEH-8): the replaced item's last picture is drawn until a newer load has applied and the child has
 * a picture again, or until the cap; and the held frame is drawn into another color space as libobs draws an async
 * frame */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include "test-common.h"

#include "core/frame-hold.h"

/* A cut between two files: the frame is captured before the unload, with load 4 applied; load 5 applies a few frames
 * later and the child shows nothing until its first frame */
static void test_cut(void)
{
	struct pl_frame_hold hold = {0};
	assert(!pl_frame_hold_tend(&hold, 4, true, 1000));

	pl_frame_hold_requested(&hold, true, 4, 1000);
	/* the unload leaves the old picture in the child: still the replaced item's, so the held frame is drawn */
	assert(pl_frame_hold_tend(&hold, 4, true, 1017));
	/* load 5 applied: the child shows nothing until its first frame (the blank this hold covers) */
	assert(pl_frame_hold_tend(&hold, 5, false, 1033));
	assert(pl_frame_hold_tend(&hold, 5, false, 1117));
	/* the first frame: the child is drawn from now on, and the hold stays over */
	assert(!pl_frame_hold_tend(&hold, 5, true, 1133));
	assert(!pl_frame_hold_tend(&hold, 5, false, 1150));
	assert(!pl_frame_hold_tend(&hold, 6, false, 1167));
}

/* A load that brings no picture (a file that fails to open, a slow URL) is not waited for past the cap */
static void test_cap(void)
{
	struct pl_frame_hold hold = {0};
	pl_frame_hold_requested(&hold, true, 2, 5000);
	assert(pl_frame_hold_tend(&hold, 3, false, 5000 + PL_FRAME_HOLD_CAP_MS - 1));
	assert(!pl_frame_hold_tend(&hold, 3, false, 5000 + PL_FRAME_HOLD_CAP_MS));
	/* a picture that comes later is drawn as the child's */
	assert(!pl_frame_hold_tend(&hold, 3, true, 5000 + PL_FRAME_HOLD_CAP_MS + 500));

	/* a clock that steps back counts as no time passed */
	pl_frame_hold_requested(&hold, true, 3, 9000);
	assert(pl_frame_hold_tend(&hold, 4, false, 8000));
}

/* Next pressed again before the first load showed a picture: the held frame stays, keeps its start (the cap bounds
 * the whole freeze) and waits for the newer load */
static void test_second_load_while_holding(void)
{
	struct pl_frame_hold hold = {0};
	pl_frame_hold_requested(&hold, true, 4, 1000);
	assert(pl_frame_hold_tend(&hold, 5, false, 1050));
	/* the second request finds no picture to capture */
	pl_frame_hold_requested(&hold, false, 5, 1060);
	/* load 5's first frame arriving now belongs to the item being replaced again: not drawn */
	assert(pl_frame_hold_tend(&hold, 5, true, 1080));
	/* load 6 applied and shows its first frame */
	assert(pl_frame_hold_tend(&hold, 6, false, 1100));
	assert(!pl_frame_hold_tend(&hold, 6, true, 1150));

	/* the cap still counts from the capture */
	pl_frame_hold_requested(&hold, true, 6, 2000);
	pl_frame_hold_requested(&hold, false, 7, 2900);
	assert(pl_frame_hold_tend(&hold, 7, false, 2000 + PL_FRAME_HOLD_CAP_MS - 1));
	assert(!pl_frame_hold_tend(&hold, 8, false, 2000 + PL_FRAME_HOLD_CAP_MS));
}

/* No picture and no hold (a load after a stop, or a hidden source): nothing is held */
static void test_nothing_to_hold(void)
{
	struct pl_frame_hold hold = {0};
	pl_frame_hold_requested(&hold, false, 3, 1000);
	assert(!pl_frame_hold_tend(&hold, 3, false, 1010));
	assert(!pl_frame_hold_tend(&hold, 4, false, 1020));
}

/* A recapture while holding (the newer item had a picture after all) holds that picture from now */
static void test_recapture(void)
{
	struct pl_frame_hold hold = {0};
	pl_frame_hold_requested(&hold, true, 4, 1000);
	pl_frame_hold_requested(&hold, true, 5, 1900);
	assert(pl_frame_hold_tend(&hold, 5, true, 1900 + PL_FRAME_HOLD_CAP_MS - 1));
	assert(!pl_frame_hold_tend(&hold, 6, true, 1900 + PL_FRAME_HOLD_CAP_MS - 1));
}

/* Stop, clear, blank: let go at once, whatever the child shows */
static void test_release(void)
{
	struct pl_frame_hold hold = {0};
	pl_frame_hold_requested(&hold, true, 4, 1000);
	pl_frame_hold_release(&hold);
	assert(!pl_frame_hold_tend(&hold, 4, false, 1010));
	assert(!pl_frame_hold_tend(&hold, 4, true, 1020));
}

struct capture_stub {
	bool works; /* what the capture returns */
	int calls;
};

static bool stub_capture(void *param)
{
	struct capture_stub *stub = param;
	stub->calls++;
	return stub->works;
}

static bool render(struct pl_frame_hold *hold, struct capture_stub *stub, bool release, bool has_media, bool request,
		   bool picture, long applied, int64_t now_ms)
{
	struct pl_frame_hold_input input = {release, has_media, request, picture, applied, now_ms};
	return pl_frame_hold_render(hold, &input, stub_capture, stub);
}

/* video_render's order: release, then capture, then tend */
static void test_render_order(void)
{
	struct capture_stub stub = {true, 0};
	struct pl_frame_hold hold = {0};

	/* a capture request with a picture holds at once */
	assert(render(&hold, &stub, false, true, true, true, 4, 1000));
	assert(stub.calls == 1);

	/* a stop and then a replacing load before the next render (Stop, Play, Next quickly): the release comes first,
	 * so the new capture holds */
	assert(render(&hold, &stub, true, true, true, true, 5, 1100));
	assert(stub.calls == 2);
	assert(render(&hold, &stub, false, true, false, false, 6, 1120));

	/* a release alone lets go, even of a frame that would still be held */
	assert(!render(&hold, &stub, true, true, false, false, 6, 1140));

	/* no item: let go, and nothing is captured */
	assert(render(&hold, &stub, false, true, true, true, 6, 2000));
	assert(!render(&hold, &stub, false, false, true, true, 6, 2010));
	assert(stub.calls == 3);

	/* no picture, or a capture that fails: no hold */
	assert(!render(&hold, &stub, false, true, true, false, 6, 3000));
	assert(stub.calls == 3);
	stub.works = false;
	assert(!render(&hold, &stub, false, true, true, true, 6, 3010));
	assert(stub.calls == 4);
}

/* A stop while the source was hidden left the release flag; nothing was rendered meanwhile. Shown again, it is
 * taken in the first render, together with the capture request of a load made meanwhile, which still holds; and a
 * stale flag taken on its own does not stop a later hold. */
static void test_stale_release_after_hidden_stop(void)
{
	struct capture_stub stub = {true, 0};
	struct pl_frame_hold hold = {0};
	assert(render(&hold, &stub, false, true, true, true, 4, 1000));
	/* hidden: no renders past the cap; the stop's flag waits */
	assert(render(&hold, &stub, true, true, true, true, 6, 9000));
	assert(render(&hold, &stub, false, true, false, false, 7, 9020));
	assert(!render(&hold, &stub, false, true, false, true, 7, 9040));

	/* the stale flag on its own, then a later replacing load */
	assert(!render(&hold, &stub, true, true, false, true, 7, 10000));
	assert(render(&hold, &stub, false, true, true, true, 7, 10500));
	assert(render(&hold, &stub, false, true, false, false, 8, 10520));
}

static void check_draw(enum gs_color_space captured, enum gs_color_space current, bool linear_srgb, const char *tech,
		       enum pl_hold_multiplier multiplier, bool expected_linear)
{
	struct pl_hold_draw draw;
	pl_frame_hold_draw_choice(captured, current, linear_srgb, &draw);
	assert(strcmp(draw.tech, tech) == 0);
	assert(draw.multiplier == multiplier);
	assert(draw.linear_srgb == expected_linear);
}

/* As libobs draws an async frame (obs-source.c, obs_source_render_async_video) */
static void test_draw_choice(void)
{
	/* the usual case: drawn in the space it was captured in, a plain copy that keeps the caller's sRGB handling */
	check_draw(GS_CS_SRGB, GS_CS_SRGB, false, "Draw", PL_HOLD_MULTIPLIER_NONE, false);
	check_draw(GS_CS_SRGB, GS_CS_SRGB, true, "Draw", PL_HOLD_MULTIPLIER_NONE, true);
	check_draw(GS_CS_709_EXTENDED, GS_CS_709_EXTENDED, false, "Draw", PL_HOLD_MULTIPLIER_NONE, false);
	check_draw(GS_CS_709_SCRGB, GS_CS_709_SCRGB, false, "Draw", PL_HOLD_MULTIPLIER_NONE, false);
	/* SDR into HDR targets: decoded to linear, scaled to the SDR white level for scRGB */
	check_draw(GS_CS_SRGB, GS_CS_709_EXTENDED, false, "Draw", PL_HOLD_MULTIPLIER_NONE, true);
	check_draw(GS_CS_SRGB, GS_CS_709_SCRGB, false, "DrawMultiply", PL_HOLD_MULTIPLIER_TO_SCRGB, true);
	check_draw(GS_CS_SRGB_16F, GS_CS_709_SCRGB, false, "DrawMultiply", PL_HOLD_MULTIPLIER_TO_SCRGB, false);
	/* HDR into SDR targets: tone mapped and encoded to sRGB */
	check_draw(GS_CS_709_EXTENDED, GS_CS_SRGB, false, "DrawTonemap", PL_HOLD_MULTIPLIER_NONE, true);
	check_draw(GS_CS_709_SCRGB, GS_CS_SRGB, false, "DrawMultiplyTonemap", PL_HOLD_MULTIPLIER_FROM_SCRGB, true);
	/* between the HDR spaces */
	check_draw(GS_CS_709_EXTENDED, GS_CS_709_SCRGB, false, "DrawMultiply", PL_HOLD_MULTIPLIER_TO_SCRGB, false);
	check_draw(GS_CS_709_SCRGB, GS_CS_709_EXTENDED, false, "DrawMultiply", PL_HOLD_MULTIPLIER_FROM_SCRGB, false);
}

int main(void)
{
	pl_test_no_crash_dialogs();

	long allocations = bnum_allocs();
	test_cut();
	test_cap();
	test_second_load_while_holding();
	test_nothing_to_hold();
	test_recapture();
	test_release();
	test_render_order();
	test_stale_release_after_hidden_stop();
	test_draw_choice();
	assert(bnum_allocs() == allocations);
	printf("frame hold tests: passed\n");
	return 0;
}
