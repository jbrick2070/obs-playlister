/*
obs-playlister (based on Media Playlist Source)
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com
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
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/deque.h>
#include <plugin-support.h>
#include "playlist.h"
#include "player.h"
#include "shuffler.h"
#include "core/audio-queue.h"
#include "core/startup.h"
#include "providers/provider.h"

/* A folder row's provider (PLAN section 3.2). It stays across updates while the row keeps its uuid and path; the
 * registry changes only under the list lock in update, and providers are created and destroyed outside it
 * (R-THR-3). */
struct row_provider {
	char *row_uuid;
	char *location;
	bool include_subfolders;
	uint64_t instance_id;         /* the only instance whose snapshots the row accepts (R-THR-2) */
	struct pl_provider *provider; /* NULL until created, or when it could not be */
};

/* R-KEY-2: where playback resumes once a folder row that was not listed yet settles */
struct resume_target {
	bool pending;
	bool from_start; /* the source's first update: a row that settles empty or missing is passed over */
	char *row_uuid;
	char *item_key; /* NULL: the row's first item */
	uint32_t item_occurrence;
	uint64_t wait_until_ns; /* the item is still being written: waited for until then (0: not waiting) */
	bool row_unavailable;   /* from the start, the row settled empty or its folder is missing */
};

/* clang-format off */

enum visibility_behavior {
	VISIBILITY_BEHAVIOR_STOP_RESTART,
	VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE,
	VISIBILITY_BEHAVIOR_ALWAYS_PLAY,
	VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT,
};

enum restart_behavior {
	RESTART_BEHAVIOR_CURRENT_FILE,
	RESTART_BEHAVIOR_FIRST_FILE,
};

/* clang-format on */

struct media_playlist_source {
	obs_source_t *source;
	obs_source_t *current_media_source;

	struct pl_player player;
	volatile bool save_dirty; /* set after an index change; video_tick saves at most once a second */
	uint64_t last_save_ns;    /* video_tick only */

	struct shuffler shuffler;
	bool shuffle;
	bool loop;
	bool use_hw_decoding;
	bool close_when_inactive;
	pthread_mutex_t mutex;
	DARRAY(struct media_file_data) files;
	struct media_file_data *current_media; // only for file/folder in the list
	struct media_file_data *actual_media;  // for both files and folder items
	size_t current_media_index;
	/* the playing folder item's identity within its row (R-KEY-1), to find it again after a rescan; NULL for a file
	 * row */
	char *current_item_key;
	uint32_t current_item_occurrence;
	// to know if current_folder_item_index will be used, check if current file is a folder
	size_t current_folder_item_index;
	long long speed;
	bool first_update;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;

	enum obs_media_state state;
	enum visibility_behavior visibility_behavior;
	enum restart_behavior restart_behavior;

	struct pl_audio_queue audio;  /* the child's audio, from its capture callback to video_tick (R-UP-60) */
	DARRAY(char *) missing_paths; /* owned copies given to missing-file callbacks, freed in destroy (R-UP-67) */

	/* folder rows (PLAN section 3.3), under the list lock */
	DARRAY(struct row_provider) providers;
	uint64_t last_instance_id;
	struct pl_container_options container_options; /* extension_filter points at container_filter */
	char *container_filter;
	struct resume_target resume;
	volatile bool release_held; /* a playback trigger: video_tick has the providers publish kept lists (R-DIR-11) */

	/* R-BEH-5 */
	struct pl_startup startup; /* the startup of the load that applied last; video_tick only */
	DARRAY(char *) failed;     /* entries that failed to play (failure_key), under the list lock */
	bool all_failed;           /* nothing left that may play: auto-advance stopped until something changes */

	/* R-DIR-9: the still being held, set by video_tick and read by the media callbacks */
	volatile bool hold_active;
	volatile long hold_elapsed_ms;
	volatile long hold_duration_ms;
	volatile long hold_epoch; /* the held load */
	int64_t hold_last_ms;     /* video_tick only */
	long still_duration_ms;   /* the still duration option */
};
