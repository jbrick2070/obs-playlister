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
#include "shuffler.h"

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

	struct shuffler shuffler;
	bool shuffle;
	bool loop;
	bool paused;
	bool user_stopped;
	bool use_hw_decoding;
	bool close_when_inactive;
	pthread_mutex_t mutex;
	DARRAY(struct media_file_data) files;
	struct media_file_data *current_media; // only for file/folder in the list
	struct media_file_data *actual_media;  // for both files and folder items
	size_t current_media_index;
	char *current_media_filename; // only used with folder_items
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

	struct deque audio_data[MAX_AUDIO_CHANNELS];
	struct deque audio_frames;
	struct deque audio_timestamps;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
};
