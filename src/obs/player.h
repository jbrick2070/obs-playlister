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

#include <obs-module.h>
#include "core/command-queue.h"
#include "core/load-tracker.h"

/* The player thread is the only code that changes the child media source (R-THR-1, PLAN section 3.3).
 * Producers call the request functions, normally under the list lock; the thread executes the requests in
 * order, and the load tracker decides which end of the child may auto-advance (R-BEH-6). */
struct pl_player {
	obs_source_t *child;
	struct pl_command_queue queue;
	pthread_t thread;
	bool thread_started;
	os_event_t *child_updated;   /* signaled by the child's "update" signal */
	volatile bool shutting_down; /* stops waiting for child updates */
	struct pl_load_tracker tracker;
	volatile long applied_epoch; /* the load whose child update has applied: its startup is judged (R-BEH-5) */
	volatile long audio_epoch;   /* the applied load when the child last delivered audio */

	/* player thread only */
	struct pl_child_settings settings;
	bool starting;      /* the loaded media has not shown progress yet (R-UP-79) */
	bool starting_url;  /* that media is a URL: the longer startup window */
	uint64_t loaded_ns; /* when it was loaded */
	bool pause_held;    /* a pause waits for that media to start */
};

bool pl_player_start(struct pl_player *player, obs_source_t *child);
/* Queues QUIT behind any pending request, joins the thread and frees what is left */
void pl_player_shutdown(struct pl_player *player);

/* Loads path into the child; loading the path the child already has restarts it, so this is also restart */
void pl_player_load(struct pl_player *player, const char *path, bool is_url, bool still);
void pl_player_play_pause(struct pl_player *player, bool pause);
void pl_player_stop(struct pl_player *player);
/* Empties the child's path and stops the child (never the playlist source itself) */
void pl_player_clear(struct pl_player *player);
void pl_player_set_time(struct pl_player *player, int64_t ms);
void pl_player_set_child_settings(struct pl_player *player, const struct pl_child_settings *settings);
/* Withdraws auto-advance at once and supersedes queued loads, without a command of its own: for a hide whose stop
 * runs later as a queued media action */
void pl_player_withdraw(struct pl_player *player);

/* The child's media_ended handler: takes no lock, only records which load the end belongs to */
void pl_player_child_ended(struct pl_player *player);
/* For video_tick: consumes a pending end; true once when it belongs to the load that may auto-advance */
bool pl_player_take_end(struct pl_player *player);

/* The child's audio capture callback: audio arrived (R-BEH-5 evidence for the applied load) */
void pl_player_child_audio(struct pl_player *player);

/* For video_tick, without consuming anything: whether the child ended this load, and whether it may still
 * auto-advance (a stop, clear, hide or newer load withdraws it, and then its startup is not judged) */
bool pl_player_load_ended(struct pl_player *player, long epoch);
bool pl_player_load_eligible(struct pl_player *player, long epoch);

/* Logs "<what> <path>", a URL with scheme, host and path only (it can carry credentials) */
void pl_player_log_location(int level, const char *what, const char *path);
