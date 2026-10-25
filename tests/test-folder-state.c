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

/* The folder provider's state machines against a fake folder. Write-complete gate (R-DIR-4): both admission paths,
 * a growing file, a stable file, a locked-but-stable file, a vanished file, a permanently locked file that holds up
 * nothing, a denied file (R-PLAT-10), batched samples, the deadline queue, sample times, the two clocks, no second
 * probe of an admitted file, renames. Watch modes (R-DIR-3): notify, overflow, rescan-only, missing, denied and
 * recovery, with the interval on and off. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/folder-state.h"

/* ---- a fake folder ---- */

#define MAX_FILES 8
#define W PL_GATE_WINDOW_MS

struct fake_file {
	const char *name; /* key and location */
	uint64_t size;
	int64_t write_ms;
	bool exists;
	enum pl_probe_result probe; /* what a probe of an existing file returns */
	bool gone_at_probe;         /* deleted between the sample and the probe */
};

struct fake_folder {
	struct fake_file files[MAX_FILES];
	size_t count;
	int64_t sample_delay_ms; /* a slow share: each sample finishes this much after the tick's clock */
	int probes;
	int samples;
};

static struct fake_file *lookup(struct fake_folder *folder, const char *location)
{
	for (size_t i = 0; i < folder->count; i++) {
		if (strcmp(folder->files[i].name, location) == 0)
			return &folder->files[i];
	}
	return NULL;
}

static enum pl_probe_result fake_probe(void *context, const char *location)
{
	struct fake_folder *folder = context;
	folder->probes++;
	struct fake_file *file = lookup(folder, location);
	if (!file || !file->exists || file->gone_at_probe)
		return PL_PROBE_GONE;
	return file->probe;
}

/* sampled times run from the tick's clock, which the test passes through this global */
static int64_t tick_clock;

static bool fake_sample(void *context, const char *location, uint64_t *size, int64_t *write_ms, int64_t *sampled_ms)
{
	struct fake_folder *folder = context;
	folder->samples++;
	struct fake_file *file = lookup(folder, location);
	*sampled_ms = tick_clock + folder->sample_delay_ms;
	if (!file || !file->exists)
		return false;
	*size = file->size;
	*write_ms = file->write_ms;
	return true;
}

static void add_file(struct fake_folder *folder, const char *name, uint64_t size, int64_t write_ms)
{
	struct fake_file file = {name, size, write_ms, true, PL_PROBE_OK, false};
	folder->files[folder->count++] = file;
}

/* Lists the existing files (and those listed with exists still set) into the gate, as a directory scan would; the
 * monotonic and wall clocks are equal here unless a test says otherwise */
static void scan(struct pl_gate *gate, struct fake_folder *folder, bool initial, int64_t now)
{
	struct pl_gate_listing listing[MAX_FILES];
	size_t count = 0;
	for (size_t i = 0; i < folder->count; i++) {
		if (!folder->files[i].exists)
			continue;
		struct pl_gate_listing entry = {folder->files[i].name, folder->files[i].name, folder->files[i].size,
						folder->files[i].write_ms};
		listing[count++] = entry;
	}
	pl_gate_listing(gate, listing, count, initial, now, now, fake_probe, folder);
}

static size_t tick(struct pl_gate *gate, struct fake_folder *folder, int64_t now)
{
	tick_clock = now;
	return pl_gate_tick(gate, now, now, fake_sample, fake_probe, folder);
}

static const struct pl_gate_file *find(const struct pl_gate *gate, const char *key)
{
	for (size_t i = 0; i < gate->files.num; i++) {
		if (strcmp(gate->files.array[i].key, key) == 0)
			return &gate->files.array[i];
	}
	return NULL;
}

static bool is_admitted(const struct pl_gate *gate, const char *key)
{
	const struct pl_gate_file *file = find(gate, key);
	return file && file->admitted;
}

/* the gate keeps its files sorted by key */
static void assert_sorted(const struct pl_gate *gate)
{
	for (size_t i = 1; i < gate->files.num; i++)
		assert(strcmp(gate->files.array[i - 1].key, gate->files.array[i].key) < 0);
}

/* ---- gate ---- */

static void test_initial_scan(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	add_file(&folder, "old.mp4", 500, now - 60000); /* admitted at once */
	add_file(&folder, "young.mp4", 500, now - 500); /* written half a second ago: candidate, not probed */
	add_file(&folder, "writing.mp4", 900, now - 60000);
	lookup(&folder, "writing.mp4")->probe = PL_PROBE_BUSY; /* old, but held open */
	add_file(&folder, "vanishing.mp4", 10, now - 60000);
	lookup(&folder, "vanishing.mp4")->gone_at_probe = true; /* listed, then deleted before the probe */
	scan(&gate, &folder, true, now);

	assert(is_admitted(&gate, "old.mp4"));
	assert(find(&gate, "young.mp4") && !is_admitted(&gate, "young.mp4"));
	assert(find(&gate, "writing.mp4") && !is_admitted(&gate, "writing.mp4"));
	assert(!find(&gate, "vanishing.mp4"));
	lookup(&folder, "vanishing.mp4")->exists = false;
	assert(folder.probes == 3);
	assert(pl_gate_pending_count(&gate) == 2 && pl_gate_admitted_count(&gate) == 1);
	assert(pl_gate_next_deadline(&gate) == now + W);
	assert_sorted(&gate);

	/* one window later the young file is unchanged and old enough; the held one still is held */
	assert(tick(&gate, &folder, now + W) == 1);
	assert(is_admitted(&gate, "young.mp4") && !is_admitted(&gate, "writing.mp4"));

	/* the writer closes it: admitted at the next deadline */
	lookup(&folder, "writing.mp4")->probe = PL_PROBE_OK;
	assert(pl_gate_next_deadline(&gate) == now + 2 * W);
	assert(tick(&gate, &folder, now + 2 * W) == 1);
	assert(is_admitted(&gate, "writing.mp4"));
	assert(pl_gate_next_deadline(&gate) == INT64_MAX);

	/* a rescan probes nothing already admitted */
	int probes = folder.probes;
	scan(&gate, &folder, false, now + 10000);
	assert(folder.probes == probes && pl_gate_pending_count(&gate) == 0);
	pl_gate_free(&gate);
}

/* the same key under a new path (a case-only change of the folder name) keeps its admission and takes the path */
static void test_location_update(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	add_file(&folder, "C:/a/k.mp4", 10, now - 60000);
	struct pl_gate_listing first = {"K.MP4", "C:/a/k.mp4", 10, now - 60000};
	pl_gate_listing(&gate, &first, 1, true, now, now, fake_probe, &folder);
	assert(is_admitted(&gate, "K.MP4"));
	struct pl_gate_listing second = {"K.MP4", "C:/A/k.mp4", 10, now - 60000};
	pl_gate_listing(&gate, &second, 1, false, now + 1000, now + 1000, fake_probe, &folder);
	assert(is_admitted(&gate, "K.MP4") && strcmp(find(&gate, "K.MP4")->location, "C:/A/k.mp4") == 0);
	pl_gate_free(&gate);
}

static void test_growing_file(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	int64_t now = 100000;
	scan(&gate, &folder, true, now);

	/* new files in later listings always take the candidate path */
	add_file(&folder, "render.mp4", 1000, now);
	scan(&gate, &folder, false, now);
	assert(!is_admitted(&gate, "render.mp4"));

	for (int step = 1; step <= 3; step++) {
		now += W;
		struct fake_file *render = lookup(&folder, "render.mp4");
		render->size += 1000;
		render->write_ms = now - 100;
		assert(tick(&gate, &folder, now) == 0);
	}
	/* it stops growing: admitted once its size held for the window and its last write is the window old */
	now += W;
	assert(tick(&gate, &folder, now) == 1);
	assert(is_admitted(&gate, "render.mp4"));

	const struct pl_gate_file *render = pl_gate_find(&gate, "render.mp4");
	assert(render && render->has_version && render->version_size == 4000 &&
	       render->version_write_ms == now - W - 100);
	int64_t admitted_write = render->version_write_ms;

	/* an admitted file that changes becomes a candidate again, and keeps the version that was admitted */
	lookup(&folder, "render.mp4")->size = 5000;
	lookup(&folder, "render.mp4")->write_ms = now;
	scan(&gate, &folder, false, now);
	assert(!is_admitted(&gate, "render.mp4"));
	render = pl_gate_find(&gate, "render.mp4");
	assert(render->has_version && render->version_size == 4000 && render->version_write_ms == admitted_write);
	assert(render->size == 5000);

	/* the new version replaces it once admitted, one window after the listing saw it */
	now += W;
	assert(tick(&gate, &folder, now) == 1);
	render = pl_gate_find(&gate, "render.mp4");
	assert(render->admitted && render->version_size == 5000 && render->version_write_ms == now - W);

	/* a new file has no version until it is admitted */
	add_file(&folder, "next.mp4", 10, now);
	scan(&gate, &folder, false, now);
	assert(!pl_gate_find(&gate, "next.mp4")->has_version);
	assert(!pl_gate_find(&gate, "missing.mp4"));
	pl_gate_free(&gate);
}

static void test_vanished_locked_and_denied(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	scan(&gate, &folder, true, now);
	add_file(&folder, "locked.mp4", 100, now - 100);
	lookup(&folder, "locked.mp4")->probe = PL_PROBE_BUSY; /* held open forever */
	add_file(&folder, "gone.mp4", 100, now - 100);
	add_file(&folder, "fine.mp4", 100, now - 100);
	add_file(&folder, "late.mp4", 100, now - 100);
	add_file(&folder, "denied.mp4", 100, now - 100);
	lookup(&folder, "denied.mp4")->probe = PL_PROBE_DENIED;
	scan(&gate, &folder, false, now);
	assert(pl_gate_pending_count(&gate) == 5);

	lookup(&folder, "gone.mp4")->exists = false;       /* deleted before its second sample */
	lookup(&folder, "late.mp4")->gone_at_probe = true; /* deleted between its sample and its probe */
	folder.samples = 0;
	assert(tick(&gate, &folder, now + W) == 1);
	/* one batched pass sampled all five due candidates */
	assert(folder.samples == 5);
	assert(!find(&gate, "gone.mp4") && !find(&gate, "late.mp4"));
	assert(is_admitted(&gate, "fine.mp4"));
	assert(!is_admitted(&gate, "locked.mp4") && !find(&gate, "locked.mp4")->denied);
	/* access denied is kept apart from "still being written" (R-PLAT-10) */
	assert(find(&gate, "denied.mp4")->denied && pl_gate_denied_count(&gate) == 1);

	/* both keep being retried and never block the others; the denied one joins once access is granted */
	for (int i = 2; i <= 5; i++)
		assert(tick(&gate, &folder, now + i * W) == 0);
	lookup(&folder, "denied.mp4")->probe = PL_PROBE_OK;
	assert(tick(&gate, &folder, now + 6 * W) == 1);
	assert(is_admitted(&gate, "denied.mp4") && pl_gate_denied_count(&gate) == 0);
	assert(pl_gate_pending_count(&gate) == 1);

	/* a file a listing no longer shows leaves the gate */
	lookup(&folder, "locked.mp4")->exists = false;
	lookup(&folder, "late.mp4")->exists = false;
	scan(&gate, &folder, false, now + 20000);
	assert(!find(&gate, "locked.mp4") && gate.files.num == 2);
	assert_sorted(&gate);
	pl_gate_free(&gate);
}

static void test_deadline_queue_and_clocks(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	int64_t now = 100000;
	scan(&gate, &folder, true, now);
	add_file(&folder, "first.mp4", 10, now);
	scan(&gate, &folder, false, now);
	add_file(&folder, "second.mp4", 10, now + 1500);
	scan(&gate, &folder, false, now + 1500);

	/* the worker wakes at the earliest deadline, and a tick then samples only what is due */
	assert(pl_gate_next_deadline(&gate) == now + W);
	folder.samples = 0;
	assert(tick(&gate, &folder, now + W) == 1);
	assert(folder.samples == 1);
	assert(is_admitted(&gate, "first.mp4") && !is_admitted(&gate, "second.mp4"));
	assert(pl_gate_next_deadline(&gate) == now + 1500 + W);

	/* a wall-clock jump moves no window: they run on the monotonic clock */
	add_file(&folder, "third.mp4", 10, now);
	scan(&gate, &folder, false, now + 3600);
	tick_clock = now + 3700;
	pl_gate_tick(&gate, now + 3700, now + 3700 + 10 * 60000, fake_sample, fake_probe, &folder);
	assert(!is_admitted(&gate, "third.mp4"));
	pl_gate_free(&gate);
}

/* a slow share: a sample finishes after the tick's clock, and windows run from when samples were taken */
static void test_sample_times(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	scan(&gate, &folder, true, now);
	add_file(&folder, "slow.mp4", 10, now - 10000);
	scan(&gate, &folder, false, now);

	/* it grew; the sample that saw it finished 1.5 s late, so the window starts then */
	lookup(&folder, "slow.mp4")->size = 20;
	folder.sample_delay_ms = 1500;
	assert(tick(&gate, &folder, now + W) == 0);
	assert(find(&gate, "slow.mp4")->sampled_ms == now + W + 1500);
	assert(pl_gate_next_deadline(&gate) == now + 2 * W + 1500);

	folder.sample_delay_ms = 0;
	assert(tick(&gate, &folder, now + 2 * W) == 0); /* not due yet */
	assert(tick(&gate, &folder, now + 2 * W + 1500) == 1);
	pl_gate_free(&gate);
}

static void test_future_write_times(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	scan(&gate, &folder, true, now);
	/* a share whose clock runs an hour ahead, and one a second and a half ahead */
	add_file(&folder, "far.mp4", 10, now + 3600000);
	add_file(&folder, "near.mp4", 10, now + 1500);
	scan(&gate, &folder, false, now);
	/* the far one cannot be trusted and counts from when it was seen; the near one is in the past by now and
	 * must reach the window's age like any other */
	assert(tick(&gate, &folder, now + W) == 1);
	assert(is_admitted(&gate, "far.mp4") && !is_admitted(&gate, "near.mp4"));
	assert(pl_gate_next_deadline(&gate) == now + 1500 + W);
	assert(tick(&gate, &folder, now + 1500 + W) == 1);
	assert(is_admitted(&gate, "near.mp4"));
	pl_gate_free(&gate);
}

static void test_rename(void)
{
	struct fake_folder folder = {0};
	struct pl_gate gate;
	pl_gate_init(&gate);
	const int64_t now = 100000;
	add_file(&folder, "b.mp4", 10, now - 60000);
	add_file(&folder, "d.mp4", 20, now - 60000);
	scan(&gate, &folder, true, now);

	/* a paired rename with the same size and time keeps the admission: no new wait */
	assert(pl_gate_rename(&gate, "b.mp4", "f.mp4", "f.mp4", 10, now - 60000, now + 100));
	assert(is_admitted(&gate, "f.mp4") && !find(&gate, "b.mp4"));
	assert_sorted(&gate);
	/* renamed over d.mp4, which sorts before it: the replaced entry is gone */
	assert(pl_gate_rename(&gate, "f.mp4", "d.mp4", "d.mp4", 10, now - 60000, now + 200));
	assert(gate.files.num == 1 && is_admitted(&gate, "d.mp4") && find(&gate, "d.mp4")->size == 10);
	/* renamed and changed: a candidate again */
	assert(pl_gate_rename(&gate, "d.mp4", "a.mp4", "a.mp4", 99, now + 300, now + 300));
	assert(!is_admitted(&gate, "a.mp4") && find(&gate, "a.mp4")->sampled_ms == now + 300);
	/* a candidate renamed unchanged keeps its timing */
	assert(pl_gate_rename(&gate, "a.mp4", "c.mp4", "c.mp4", 99, now + 300, now + 900));
	assert(find(&gate, "c.mp4")->sampled_ms == now + 300 && find(&gate, "c.mp4")->deadline_ms == now + 300 + W);
	/* an unknown old name is not a rename the gate can carry */
	assert(!pl_gate_rename(&gate, "nobody.mp4", "e.mp4", "e.mp4", 1, now, now));
	pl_gate_free(&gate);
}

/* ---- watch modes ---- */

static void expect_change(struct pl_watch_actions actions, enum pl_watch_mode mode, const char *reason)
{
	assert(actions.mode_changed && actions.mode == mode);
	assert(actions.reason && strcmp(actions.reason, reason) == 0);
}

static void test_watch_notify_and_overflow(void)
{
	struct pl_watch watch;
	int64_t now = 0;
	pl_watch_init(&watch, true, 10, now);
	assert(watch.mode == PL_WATCH_STARTING && watch.next_ms == now);

	/* starting: notifications and overflows mean nothing yet */
	assert(!pl_watch_handle(&watch, PL_WATCH_CHANGED, now).rescan);
	assert(!pl_watch_handle(&watch, PL_WATCH_OVERFLOW, now).rearm);

	struct pl_watch_actions actions = pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	assert(actions.open && !actions.rescan);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARMED, now);
	expect_change(actions, PL_WATCH_NOTIFY, "notifications armed");
	assert(actions.rescan && watch.next_ms == now + 10000);

	/* the folder check runs on the interval alongside notifications */
	now = watch.next_ms;
	actions = pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	assert(actions.rescan && !actions.rearm && !actions.open && !actions.mode_changed);
	assert(watch.next_ms == now + 10000);
	actions = pl_watch_handle(&watch, PL_WATCH_CHANGED, now + 1);
	assert(actions.rescan && !actions.mode_changed);

	/* overflow: arm again and list everything; arming works, so nothing else changes */
	actions = pl_watch_handle(&watch, PL_WATCH_OVERFLOW, now + 2);
	assert(actions.rescan && actions.rearm && !actions.mode_changed);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARMED, now + 3);
	assert(!actions.rescan && !actions.mode_changed && actions.mode == PL_WATCH_NOTIFY);

	/* unchanged options change nothing; with the check interval off notify still rescans every 60 s */
	int64_t next = watch.next_ms;
	pl_watch_set_interval(&watch, true, 10, now + 4);
	assert(watch.next_ms == next);
	pl_watch_set_interval(&watch, false, 10, now + 4);
	assert(watch.next_ms == now + 4 + 60000);
}

static void test_watch_rescan_only(void)
{
	struct pl_watch watch;
	int64_t now = 0;
	pl_watch_init(&watch, true, 5, now);
	pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	struct pl_watch_actions actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARM_FAILED, now);
	expect_change(actions, PL_WATCH_RESCAN_ONLY, "notifications cannot be armed; rescanning instead");
	assert(actions.rescan && watch.next_ms == now + 5000);
	assert(!pl_watch_handle(&watch, PL_WATCH_CHANGED, now).rescan);
	assert(!pl_watch_handle(&watch, PL_WATCH_OVERFLOW, now).rearm);

	/* each scan tries to arm again */
	now = watch.next_ms;
	actions = pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	assert(actions.rescan && actions.rearm);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARM_FAILED, now);
	assert(!actions.rescan && !actions.mode_changed);

	now = watch.next_ms;
	pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARMED, now);
	expect_change(actions, PL_WATCH_NOTIFY, "notifications armed");
	assert(!actions.rescan);

	/* re-arming after an overflow fails: rescan-only; then a rescan finds the share gone: missing */
	pl_watch_handle(&watch, PL_WATCH_OVERFLOW, now + 1);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARM_FAILED, now + 1);
	assert(actions.mode_changed && actions.mode == PL_WATCH_RESCAN_ONLY && !actions.rescan);
	actions = pl_watch_handle(&watch, PL_WATCH_LOST, now + 2);
	expect_change(actions, PL_WATCH_MISSING, "the folder went away");
	assert(actions.close);

	/* with the check interval off, missing (like rescan-only) retries every 10 s */
	pl_watch_set_interval(&watch, false, 5, now + 3);
	assert(watch.next_ms == now + 3 + 10000);
	actions = pl_watch_handle(&watch, PL_WATCH_TIMER, watch.next_ms);
	assert(actions.open && watch.next_ms == now + 3 + 20000);
}

static void test_watch_missing_denied_and_recovery(void)
{
	struct pl_watch watch;
	int64_t now = 0;
	pl_watch_init(&watch, true, 10, now);
	pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	struct pl_watch_actions actions = pl_watch_handle(&watch, PL_WATCH_OPEN_FAILED, now);
	expect_change(actions, PL_WATCH_MISSING, "the folder cannot be opened");
	assert(actions.close && strcmp(pl_watch_mode_name(watch.mode), "missing") == 0);
	assert(!pl_watch_handle(&watch, PL_WATCH_CHANGED, now).rescan);
	assert(!pl_watch_handle(&watch, PL_WATCH_LOST, now).mode_changed);

	/* still missing at the next try: no new log line */
	now = watch.next_ms;
	actions = pl_watch_handle(&watch, PL_WATCH_TIMER, now);
	assert(actions.open && !actions.rescan && !actions.rearm);
	assert(!pl_watch_handle(&watch, PL_WATCH_OPEN_FAILED, now).mode_changed);

	/* the folder is back: it is opened afresh, armed and listed */
	now = watch.next_ms;
	assert(pl_watch_handle(&watch, PL_WATCH_TIMER, now).open);
	actions = pl_watch_handle(&watch, PL_WATCH_OPENED_ARMED, now);
	expect_change(actions, PL_WATCH_NOTIFY, "notifications armed");
	assert(actions.rescan);

	/* access denied is its own reason (R-PLAT-10) */
	actions = pl_watch_handle(&watch, PL_WATCH_DENIED, now + 1);
	expect_change(actions, PL_WATCH_MISSING, "access denied (permissions or security software)");
	assert(actions.close && watch.next_ms == now + 1 + 10000);

	/* an interval of 0 is read as the 2 s minimum, so the timer never spins */
	struct pl_watch fast;
	pl_watch_init(&fast, true, 0, 0);
	assert(fast.check_interval_s == PL_WATCH_INTERVAL_MIN_S);
	pl_watch_handle(&fast, PL_WATCH_TIMER, 0);
	pl_watch_handle(&fast, PL_WATCH_OPENED_ARMED, 0);
	assert(fast.next_ms == 2000);
	pl_watch_set_interval(&fast, true, 100000, 0);
	assert(fast.check_interval_s == PL_WATCH_INTERVAL_MAX_S);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_initial_scan();
	test_location_update();
	test_growing_file();
	test_vanished_locked_and_denied();
	test_deadline_queue_and_clocks();
	test_sample_times();
	test_future_write_times();
	test_rename();
	test_watch_notify_and_overflow();
	test_watch_rescan_only();
	test_watch_missing_denied_and_recovery();
	assert(bnum_allocs() == allocations);
	printf("folder state tests: passed\n");
	return 0;
}
