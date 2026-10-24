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

#include "media-playlist-source.h"

static const char *media_filter =
	" (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm *.mp3 *.m4a *.ogg *.aac *.wav *.opus *.flac);;";
static const char *video_filter = " (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.m4a *.mka *.aac *.ogg *.wav *.opus *.flac);;";

static void set_current_media_index(struct media_playlist_source *mps, size_t index);
static size_t get_total_file_count(struct media_playlist_source *mps);

static inline void reset_folder_item_index(struct media_playlist_source *mps);

static bool valid_extension(const char *ext);

static void clear_media_source(void *data);
static void update_media_source(void *data, bool forced);

static void select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index);

static void select_index_proc(void *data, calldata_t *cd);
static void play_folder_item_at_index(void *data, size_t index);
static void play_media_at_index(void *data, size_t index, bool play_last_folder_item, bool backward);

static size_t find_folder_item_index(struct darray *array, const char *filename);

static void set_media_state(void *data, enum obs_media_state state);
static enum obs_media_state mps_get_state(void *data);

static void mps_end_reached(void *data);

static void media_source_ended(void *data, calldata_t *cd);
void mps_audio_callback(void *data, obs_source_t *source, const struct audio_data *audio_data, bool muted);
static bool play_selected_clicked(obs_properties_t *props, obs_property_t *property, void *data);

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

static const char *mps_getname(void *unused);
static int64_t mps_get_duration(void *data);
static int64_t mps_get_time(void *data);
static void mps_set_time(void *data, int64_t ms);
static void mps_play_pause(void *data, bool pause);
static void mps_restart(void *data);
static void mps_stop(void *data);
static void mps_playlist_next(void *data);
static void mps_playlist_prev(void *data);
static void mps_activate(void *data);
static void mps_deactivate(void *data);
static void *mps_create(obs_data_t *settings, obs_source_t *source);
static void mps_destroy(void *data);
static void mps_video_render(void *data, gs_effect_t *effect);
static bool mps_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate);
static void mps_video_tick(void *data, float seconds);
static void mps_enum_sources(void *data, obs_source_enum_proc_t cb, void *param);
static uint32_t mps_width(void *data);
static uint32_t mps_height(void *data);
static void mps_defaults(obs_data_t *settings);
static void update_current_filename_property(struct media_playlist_source *mps, obs_property_t *p);
static obs_properties_t *mps_properties(void *data);
static void mps_update(void *data, obs_data_t *settings);
static void mps_save(void *data, obs_data_t *settings);
static void mps_load(void *data, obs_data_t *settings);
static void missing_file_callback(void *src, const char *new_path, void *data);
static obs_missing_files_t *mps_missingfiles(void *data);

static void set_parents(struct darray *array);
static void add_file(struct darray *array, const char *path, const char *id);
static void free_files(struct darray *array);

struct obs_source_info media_playlist_source_info = {
	.id = "obs_playlister_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = mps_getname,
	.create = mps_create,
	.destroy = mps_destroy,
	.update = mps_update,
	.save = mps_save,
	//.load = mps_load,
	.activate = mps_activate,
	.deactivate = mps_deactivate,
	.video_render = mps_video_render,
	.video_tick = mps_video_tick,
	//.audio_render = mps_audio_render,
	.enum_active_sources = mps_enum_sources,
	.get_width = mps_width,
	.get_height = mps_height,
	.get_defaults = mps_defaults,
	.get_properties = mps_properties,
	.missing_files = mps_missingfiles,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = mps_play_pause,
	.media_restart = mps_restart,
	.media_stop = mps_stop,
	.media_next = mps_playlist_next,
	.media_previous = mps_playlist_prev,
	.media_get_state = mps_get_state,
	.media_get_duration = mps_get_duration,
	.media_get_time = mps_get_time,
	.media_set_time = mps_set_time,
	//.video_get_color_space = mps_video_get_color_space,
};

#define S_PLAYLIST "playlist"
#define S_LOOP "loop"
#define S_SHUFFLE "shuffle"
#define S_VISIBILITY_BEHAVIOR "visibility_behavior"
#define S_RESTART_BEHAVIOR "restart_behavior"
#define S_CURRENT_FILE_NAME "current_file_name"
#define S_SELECT_FILE "select_file"

#define S_CURRENT_MEDIA_INDEX "current_media_index"
#define S_CURRENT_FOLDER_ITEM_FILENAME "current_folder_item_filename"
#define S_ID "uuid"
#define S_IS_URL "is_url"
#define S_SPEED "speed_percent"
#define S_REFRESH_FILENAME "refresh_filename"

/* Settings this source shares with the child media source, which player.c writes */
#define S_FFMPEG_HW_DECODE "hw_decode"
#define S_FFMPEG_CLOSE_WHEN_INACTIVE "close_when_inactive"

#define T_(text) obs_module_text(text)
#define T_PLAYLIST T_("Playlist")
#define T_LOOP T_("LoopPlaylist")
#define T_SHUFFLE T_("Shuffle")
#define T_VISIBILITY_BEHAVIOR T_("VisibilityBehavior")
#define T_VISIBILITY_BEHAVIOR_STOP_RESTART T_("VisibilityBehavior.StopRestart")
#define T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE T_("VisibilityBehavior.PauseUnpause")
#define T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY T_("VisibilityBehavior.AlwaysPlay")
#define T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT T_("VisibilityBehavior.StopPlayNext")
#define T_RESTART_BEHAVIOR T_("RestartBehavior")
#define T_RESTART_BEHAVIOR_CURRENT_FILE T_("RestartBehavior.CurrentFile")
#define T_RESTART_BEHAVIOR_FIRST_FILE T_("RestartBehavior.FirstFile")
#define T_CURRENT_FILE_NAME T_("CurrentFileName")
#define T_SELECT_FILE T_("SelectFile")
#define T_NO_FILE_SELECTED T_("NoFileSelected")
#define T_USE_HARDWARE_DECODING T_("UseHardwareDecoding")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE T_("CloseFileWhenInactive")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP T_("CloseFileWhenInactive.Tooltip")
#define T_SPEED T_("Speed")
#define T_SPEED_WARNING T_("SpeedWarning")
#define T_REFRESH_FILENAME T_("RefreshFilename")

#define T_PLAY_PAUSE T_("PlayPause")
#define T_RESTART T_("Restart")
#define T_STOP T_("Stop")
#define T_PLAYLIST_NEXT T_("Next")
#define T_PLAYLIST_PREV T_("Previous")

/* The resume point is saved from video_tick, never under the list lock (R-UP-43, R-UP-62) */
static inline void mark_save_dirty(struct media_playlist_source *mps)
{
	os_atomic_set_bool(&mps->save_dirty, true);
}

static inline void set_current_media_index(struct media_playlist_source *mps, size_t index)
{
	if (get_total_file_count(mps) > 0) {
		if (index >= mps->files.num) {
			index = 0;
		}
		mps->current_media_index = index;
		mps->current_media = &mps->files.array[index];
	} else {
		mps->current_media_index = 0;
		mps->current_media = NULL;
		mps->actual_media = NULL;
		mps->current_folder_item_index = 0;
		bfree(mps->current_media_filename);
		mps->current_media_filename = NULL;
	}
}

static size_t get_total_file_count(struct media_playlist_source *mps)
{
	size_t count = 0;
	struct media_file_data item;

	for (size_t i = 0; i < mps->files.num; i++) {
		item = mps->files.array[i];
		if (item.is_folder) {
			count += item.folder_items.num;
		} else {
			count += 1;
		}
	}

	return count;
}

/* Requires setting current media index first
 */
static inline void set_current_folder_item_index(struct media_playlist_source *mps, size_t index)
{
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
	if (mps->current_media) {
		if (!mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;
			mps->actual_media = mps->current_media;
			return;
		}

		if (!mps->current_media->folder_items.num) {
			/* R-UP-64: an empty folder row has no item to point at */
			mps->current_folder_item_index = 0;
			mps->actual_media = NULL;
			return;
		}
		if (index < mps->current_media->folder_items.num) {
			mps->current_folder_item_index = index;
		} else {
			mps->current_folder_item_index = index = 0;
		}
		mps->actual_media = &mps->current_media->folder_items.array[index];
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = NULL;
	}
}

static inline void reset_folder_item_index(struct media_playlist_source *mps)
{
	mps->current_folder_item_index = 0;
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
}

static bool valid_extension(const char *ext)
{
	struct dstr haystack = {0};
	struct dstr needle = {0};
	bool valid = false;

	if (!ext || !*ext)
		return false;

	dstr_copy(&haystack, media_filter);
	dstr_cat(&haystack, video_filter);
	dstr_cat(&haystack, audio_filter);

	dstr_cat_ch(&needle, '*');
	dstr_cat(&needle, ext);

	valid = dstr_find_i(&haystack, needle.array);

	dstr_free(&haystack);
	dstr_free(&needle);
	return valid;
}

static void update_current_filename_setting(struct media_playlist_source *mps, obs_data_t *data)
{
	struct dstr long_desc = {0};
	if (!mps || !data)
		return;
	if (!mps->actual_media) {
		obs_data_set_string(data, S_CURRENT_FILE_NAME, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_data_set_string(data, S_CURRENT_FILE_NAME, long_desc.array);
	dstr_free(&long_desc);
}

/* Empties path selected in media source,
 * for when there is no item in the list
 * (the player stops the child, not this source, R-UP-72)
 */
static void clear_media_source(void *data)
{
	struct media_playlist_source *mps = data;
	pl_player_clear(&mps->player);
	mps->state = OBS_MEDIA_STATE_STOPPED;
}

/* Checks if the media source has to be updated, because updating its
 * settings causes it to restart. Can also force update it.
 * Should first call set_current_media_index before calling this
 *
 * Forced updates:
 * * Using play_folder_item_at_index
 * * Using play_media_at_index
 * * during mps_update (files/folders can be changed)
 */
static void update_media_source(void *data, bool forced)
{
	struct media_playlist_source *mps = data;
	if (mps->current_media->is_folder) {
		assert(mps->current_folder_item_index < mps->current_media->folder_items.num);
		mps->actual_media = &mps->current_media->folder_items.array[mps->current_folder_item_index];
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = mps->current_media;
	}

	/* The player thread updates the child (R-THR-1). It writes the speed and the other child settings with the
	 * path, and unloads first, so an unchanged path restarts too (upstream's should_restart). */
	if (forced) {
		pl_player_load(&mps->player, mps->actual_media->path, mps->actual_media->is_url);
		mps->state = OBS_MEDIA_STATE_PLAYING;
	}
}

static void select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index)
{
	pthread_mutex_lock(&mps->mutex);
	if (media_index < mps->files.num && mps->files.array[media_index].is_folder &&
	    !mps->files.array[media_index].folder_items.num) {
		/* R-UP-64: nothing to play in an empty folder row; the current item keeps playing */
		obs_log(LOG_INFO, "select_index: row %zu is an empty folder; ignored", media_index + 1);
	} else if (media_index < mps->files.num) {
		set_current_media_index(mps, media_index);
		set_current_folder_item_index(mps, folder_item_index);
		if (mps->actual_media) {
			update_media_source(mps, true);
			if (mps->shuffle) {
				shuffler_select(&mps->shuffler, mps->actual_media);
			}
			mark_save_dirty(mps);
		}
	}
	pthread_mutex_unlock(&mps->mutex);
}

static void select_index_proc(void *data, calldata_t *cd)
{
	struct media_playlist_source *mps = data;
	long long media_index = 0;
	long long folder_item_index = 0;
	calldata_get_int(cd, "media_index", &media_index);
	calldata_get_int(cd, "folder_item_index", &folder_item_index);
	select_index_proc_(mps, media_index, folder_item_index);
}

static void play_folder_item_at_index(void *data, size_t index)
{
	struct media_playlist_source *mps = data;
	if (mps->current_media->is_folder && index < mps->current_media->folder_items.num) {
		mps->current_folder_item_index = index;
		mps->actual_media = &mps->current_media->folder_items.array[index];
		bfree(mps->current_media_filename);
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
		update_media_source(mps, true);
		mark_save_dirty(mps);
	}
}

/* R-UP-61: skips empty folder rows in the direction of travel, wrapping only with loop on. Upstream asked its own
 * source for next/previous instead; on OBS 33 that only queues a media action (obs-source.c), which moved on one
 * tick later, and with synchronous media calls it would have re-entered next/previous under the list lock.
 * Returns files.num when no row qualifies. */
static size_t skip_empty_folder_rows(struct media_playlist_source *mps, size_t index, bool backward)
{
	size_t num = mps->files.num;
	for (size_t tries = 0; index < num && tries < num; tries++) {
		struct media_file_data *row = &mps->files.array[index];
		if (!row->is_folder || row->folder_items.num)
			return index;
		if (backward)
			index = index > 0 ? index - 1 : (mps->loop ? num - 1 : num);
		else
			index = index + 1 < num ? index + 1 : (mps->loop ? 0 : num);
	}
	return num;
}

static void play_media_at_index(void *data, size_t index, bool play_last_folder_item, bool backward)
{
	struct media_playlist_source *mps = data;
	index = skip_empty_folder_rows(mps, index, backward);
	if (index >= mps->files.num) {
		/* Nothing playable in that direction. With nothing playable at all, clear the child (R-UP-33); otherwise
		 * leave the current item playing, as upstream did at the end of a list without loop. */
		if (!get_total_file_count(mps)) {
			set_current_media_index(mps, 0);
			clear_media_source(mps);
		}
		return;
	}
	set_current_media_index(mps, index);
	if (!mps->actual_media) {
		clear_media_source(mps);
		return;
	}

	if (mps->current_media->is_folder) {
		// when Previous Item is clicked to go back to a folder
		if (play_last_folder_item) {
			mps->current_folder_item_index = mps->current_media->folder_items.num - 1;
		} else {
			mps->current_folder_item_index = 0;
		}

		play_folder_item_at_index(mps, mps->current_folder_item_index);
		return;
	}
	update_media_source(mps, true);
	mark_save_dirty(mps);
}

static size_t find_folder_item_index(struct darray *array, const char *filename)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		if (strcmp(files.array[i].filename, filename) == 0) {
			return i;
		}
	}

	return DARRAY_INVALID;
}

/* ------------------------------------------------------------------------- */

static void set_media_state(void *data, enum obs_media_state state)
{
	struct media_playlist_source *mps = data;
	mps->state = state;
}

static enum obs_media_state mps_get_state(void *data)
{
	struct media_playlist_source *mps = data;
	enum obs_media_state media_state;
	if (get_total_file_count(mps) > 0) {
		media_state = obs_source_media_get_state(mps->current_media_source);
	} else {
		media_state = OBS_MEDIA_STATE_NONE;
	}
	UNUSED_PARAMETER(mps);
	return media_state;
}

/* Called from video_tick with no lock held; the parent's media_ended signal is emitted outside the lock */
static void mps_end_reached(void *data)
{
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->mutex);
	set_media_state(mps, OBS_MEDIA_STATE_ENDED);
	/* back to the first playable row, so Play or Restart starts there; its folder position starts at item 1 */
	size_t first = skip_empty_folder_rows(mps, 0, false);
	set_current_media_index(mps, first < mps->files.num ? first : 0);
	reset_folder_item_index(mps);
	pthread_mutex_unlock(&mps->mutex);
	obs_source_media_ended(mps->source);
	mark_save_dirty(mps);
}

/* The child's media_ended handler takes no lock and only records the end (R-THR-1); video_tick decides what
 * follows. Ends caused by our own stop, hide or reload never advance, because those requests withdraw the
 * load's eligibility first (R-BEH-6). That replaces upstream's sticky user_stopped flag (R-UP-41): in OBS 33 a
 * deactivate still reports ENDED (obs-ffmpeg-source.c media_stopped), and the flag could swallow a real end. */
static void media_source_ended(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct media_playlist_source *mps = data;
	pl_player_child_ended(&mps->player);
}

void mps_audio_callback(void *data, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->audio_mutex);
	size_t size = audio_data->frames * sizeof(float);
	for (size_t i = 0; i < mps->num_channels; i++) {
		deque_push_back(&mps->audio_data[i], audio_data->data[i], size);
	}
	deque_push_back(&mps->audio_frames, &audio_data->frames, sizeof(audio_data->frames));
	deque_push_back(&mps->audio_timestamps, &audio_data->timestamp, sizeof(audio_data->timestamp));
	pthread_mutex_unlock(&mps->audio_mutex);
}

static bool play_selected_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	signal_handler_t *sh = obs_source_get_signal_handler(mps->source);
	obs_data_t *settings = obs_source_get_settings(mps->source);
	size_t media_index = 0;
	size_t folder_item_index = 0;
	const char *index_str = obs_data_get_string(settings, S_SELECT_FILE);
	if (strcmp(index_str, "0") != 0) {
		char **indexes = strlist_split(index_str, '-', false);

		media_index = strtol(indexes[0], NULL, 10) - 1;
		if (indexes[1]) {
			folder_item_index = strtol(indexes[1], NULL, 10) - 1;
		}
		select_index_proc_(mps, media_index, folder_item_index);

		strlist_free(indexes);
	}
	obs_data_release(settings);
	/*update_current_filename_property(
		mps, obs_properties_get(props, S_CURRENT_FILE_NAME));*/
	update_current_filename_setting(mps, settings);
	//obs_source_update_properties(mps->source);

	signal_handler_signal(sh, "media_next", NULL);
	return true;
}

static bool refresh_filename_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	update_current_filename_setting(mps, settings);
	obs_source_update_properties(mps->source);
	obs_data_release(settings);
	return true;
}

/* ------------------------------------------------------------------------- */

static const char *mps_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OBSPlaylister");
}

static void free_files(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *file = &files.array[i];
		bfree(file->filename);
		bfree(file->path);
		bfree(file->id);
		if (file->is_folder) {
			for (size_t j = 0; j < file->folder_items.num; j++) {
				struct media_file_data *folder_item = &file->folder_items.array[j];
				bfree(folder_item->filename);
				bfree(folder_item->path);
				bfree(folder_item->id);
			}
		}
		da_free(file->folder_items);
	}

	da_free(files);
}

static int64_t mps_get_duration(void *data)
{
	struct media_playlist_source *mps = data;

	return obs_source_media_get_duration(mps->current_media_source);
}

static int64_t mps_get_time(void *data)
{
	struct media_playlist_source *mps = data;

	return obs_source_media_get_time(mps->current_media_source);
}

static void mps_set_time(void *data, int64_t ms)
{
	struct media_playlist_source *mps = data;

	pl_player_set_time(&mps->player, ms);
}

static bool state_stopped(enum obs_media_state state)
{
	return state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_NONE;
}

/* Stopped by our own state, set when a request is made, or by the child's, set when it ran: the child applies
 * queued actions a tick or more later, and reports PLAYING or PAUSED even for media it ignored */
static bool playback_stopped(struct media_playlist_source *mps)
{
	return state_stopped(mps->state) || state_stopped(obs_source_media_get_state(mps->current_media_source));
}

/* The current row is a file, or a folder whose current item exists */
static bool current_item_playable(struct media_playlist_source *mps)
{
	return mps->current_media && (!mps->current_media->is_folder ||
				      mps->current_folder_item_index < mps->current_media->folder_items.num);
}

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_play_pause(mps->source, mps->state == OBS_MEDIA_STATE_PLAYING);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_restart(mps->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_stop(mps->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_next(mps->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_previous(mps->source);
}

static void mps_play_pause(void *data, bool pause)
{
	struct media_playlist_source *mps = data;

	pthread_mutex_lock(&mps->mutex);
	/* Play while stopped, ended or empty is a new load of the current item, as OBS's own media controls do with
	 * restart: the child ignores an unpause once stopped, and a new load may auto-advance again (R-BEH-6). A pause
	 * then would only make the child report PAUSED for media that cannot resume, so it is ignored. */
	bool stopped = playback_stopped(mps);
	if (!pause) {
		if (!stopped) {
			pl_player_play_pause(&mps->player, false);
			set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
		} else if (current_item_playable(mps)) {
			update_media_source(mps, true);
		}
	} else if (!stopped) {
		pl_player_play_pause(&mps->player, true);
		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	}
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_restart(void *data)
{
	struct media_playlist_source *mps = data;

	pthread_mutex_lock(&mps->mutex);
	/* RESTART is a new load (R-BEH-6); after the list ended, the current item is the first one, as upstream did.
	 * The loads set the PLAYING state; nothing playable leaves it as it was. */
	if (mps->restart_behavior == RESTART_BEHAVIOR_FIRST_FILE) {
		play_media_at_index(mps, 0, false, false);
	} else if (mps->restart_behavior == RESTART_BEHAVIOR_CURRENT_FILE) {
		if (current_item_playable(mps))
			update_media_source(mps, true);
	}
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_stop(void *data)
{
	struct media_playlist_source *mps = data;

	/* the stop withdraws the load's eligibility before the child sees it, so the ENDED that may follow does not
	 * advance (R-BEH-6) */
	pthread_mutex_lock(&mps->mutex);
	pl_player_stop(&mps->player);
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
	pthread_mutex_unlock(&mps->mutex);
}

/* Moves to the next item and queues its load, with the list lock held. Returns false when there is none: the end
 * of the list without loop, only empty folder rows ahead, or the shuffle used up. That answer is also the
 * end-of-list test for automatic advance (R-UP-63), so a folder row's remaining items and the shuffler count. */
static bool playlist_next_locked(struct media_playlist_source *mps)
{
	if (!get_total_file_count(mps))
		return false;

	if (mps->shuffle) {
		if (!shuffler_has_next(&mps->shuffler))
			return false;
		mps->actual_media = shuffler_next(&mps->shuffler);
		bfree(mps->current_media_filename);
		if (mps->actual_media->parent_id) {
			mps->current_media = mps->actual_media->parent;
			mps->current_media_filename = bstrdup(mps->actual_media->filename);
			mps->current_folder_item_index = mps->actual_media->index;
		} else {
			mps->current_media = mps->actual_media;
			mps->current_media_filename = NULL;
			mps->current_folder_item_index = 0;
		}
		mps->current_media_index = mps->current_media->index;
		update_media_source(mps, true);
		mark_save_dirty(mps);
		return true;
	}

	if (mps->current_media->is_folder && mps->current_media->folder_items.num > 0 &&
	    mps->current_folder_item_index < mps->current_media->folder_items.num - 1) {
		++mps->current_folder_item_index;
		play_folder_item_at_index(mps, mps->current_folder_item_index);
		return true;
	}

	size_t next;
	if (mps->current_media_index < mps->files.num - 1) {
		next = mps->current_media_index + 1;
	} else if (mps->loop) {
		next = 0;
	} else {
		return false;
	}
	/* the index moves only to a playable row; with only empty folder rows ahead and loop off, the current item
	 * keeps playing, as at the end of the list */
	next = skip_empty_folder_rows(mps, next, false);
	if (next >= mps->files.num)
		return false;
	mps->current_media_index = next;
	play_media_at_index(mps, next, false, false);
	return true;
}

static void mps_playlist_next(void *data)
{
	struct media_playlist_source *mps = data;

	pthread_mutex_lock(&mps->mutex);
	playlist_next_locked(mps);
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_playlist_prev(void *data)
{
	struct media_playlist_source *mps = data;
	bool is_first_folder_item = false;

	pthread_mutex_lock(&mps->mutex);
	if (!get_total_file_count(mps))
		goto end;

	if (mps->shuffle) {
		if (shuffler_has_prev(&mps->shuffler)) {
			mps->actual_media = shuffler_prev(&mps->shuffler);
			bfree(mps->current_media_filename);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_media_filename = bstrdup(mps->actual_media->filename);
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
				mps->current_media_filename = NULL;
				mps->current_folder_item_index = 0;
			}
			mps->current_media_index = mps->current_media->index;
			update_media_source(mps, true);
			mark_save_dirty(mps);
		}
		goto end;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_folder_item_index > 0) {
			--mps->current_folder_item_index;
			play_folder_item_at_index(mps, mps->current_folder_item_index);
			goto end;
		} else {
			is_first_folder_item = true;
		}
	}

	if (!mps->current_media->is_folder || is_first_folder_item) {
		size_t prev;
		if (mps->current_media_index > 0) {
			prev = mps->current_media_index - 1;
		} else if (mps->loop) {
			prev = mps->files.num - 1;
		} else {
			goto end;
		}
		prev = skip_empty_folder_rows(mps, prev, true);
		if (prev >= mps->files.num)
			goto end;
		mps->current_media_index = prev;
		play_media_at_index(mps, prev, is_first_folder_item, true);
	}

end:
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_activate(void *data)
{
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->mutex);
	bool has_files = get_total_file_count(mps) > 0;
	pthread_mutex_unlock(&mps->mutex);
	if (!has_files)
		return;

	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		obs_source_media_restart(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		/* a plain unpause, as upstream: a source the user stopped stays stopped when it is shown again. Our own
		 * state counts too, because the hide's pause may still be on its way to the child. */
		pthread_mutex_lock(&mps->mutex);
		if (mps->state == OBS_MEDIA_STATE_PAUSED ||
		    obs_source_media_get_state(mps->current_media_source) == OBS_MEDIA_STATE_PAUSED) {
			pl_player_play_pause(&mps->player, false);
			set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
		}
		pthread_mutex_unlock(&mps->mutex);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		// we only play next when the source is deactivated so we don't do anything here
	}
}

static void mps_deactivate(void *data)
{
	struct media_playlist_source *mps = data;

	/* A hide-stop withdraws auto-advance now, in this tick and before video_tick looks for an end: the child's own
	 * deactivate stops its media and can report ENDED (obs-ffmpeg-source.c), while the stop below is a media action
	 * that runs at this source's next tick (R-BEH-6) */
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		pl_player_withdraw(&mps->player);
		obs_source_media_stop(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		obs_source_media_play_pause(mps->source, true);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		pl_player_withdraw(&mps->player);
		obs_source_media_stop(mps->source);
		obs_source_media_next(mps->source);
	}
}

static void mps_destroy(void *data)
{
	struct media_playlist_source *mps = data;

	/* R-UP-70: stop the player thread, disconnect the child's handler and audio callback (both wait for a
	 * callback in flight), and only then release the child */
	pl_player_shutdown(&mps->player);
	if (mps->current_media_source) {
		signal_handler_t *sh = obs_source_get_signal_handler(mps->current_media_source);
		signal_handler_disconnect(sh, "media_ended", media_source_ended, mps);
		obs_source_remove_audio_capture_callback(mps->current_media_source, mps_audio_callback, mps);
	}
	obs_source_release(mps->current_media_source);
	shuffler_destroy(&mps->shuffler);
	free_files(&mps->files.da);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		deque_free(&mps->audio_data[i]);
	}
	deque_free(&mps->audio_frames);
	deque_free(&mps->audio_timestamps);
	pthread_mutex_destroy(&mps->mutex);
	pthread_mutex_destroy(&mps->audio_mutex);
	bfree(mps->current_media_filename);
	bfree(mps);
}

static void *mps_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct media_playlist_source *mps = bzalloc(sizeof(*mps));

	mps->first_update = true;
	mps->source = source;

	shuffler_init(&mps->shuffler);

	/* Internal media source */
	obs_data_t *media_source_data = obs_data_create();
	obs_data_set_bool(media_source_data, "log_changes", false);
	mps->current_media_source =
		obs_source_create_private("ffmpeg_source", "current_media_source", media_source_data);
	obs_data_release(media_source_data);
	obs_source_add_active_child(mps->source, mps->current_media_source);
	obs_source_add_audio_capture_callback(mps->current_media_source, mps_audio_callback, mps);

	signal_handler_t *sh_media_source = obs_source_get_signal_handler(mps->current_media_source);
	signal_handler_connect(sh_media_source, "media_ended", media_source_ended, mps);

	mps->play_pause_hotkey = obs_hotkey_register_source(source, "OBSPlaylister.PlayPause",
							    obs_module_text("PlayPause"), play_pause_hotkey, mps);

	mps->restart_hotkey = obs_hotkey_register_source(source, "OBSPlaylister.Restart", obs_module_text("Restart"),
							 restart_hotkey, mps);

	mps->stop_hotkey =
		obs_hotkey_register_source(source, "OBSPlaylister.Stop", obs_module_text("Stop"), stop_hotkey, mps);

	mps->next_hotkey = obs_hotkey_register_source(source, "OBSPlaylister.PlaylistNext",
						      obs_module_text("PlaylistNext"), next_hotkey, mps);

	mps->prev_hotkey = obs_hotkey_register_source(source, "OBSPlaylister.PlaylistPrev",
						      obs_module_text("PlaylistPrev"), previous_hotkey, mps);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void select_index(int media_index, int folder_item_index)", select_index_proc, mps);

	pthread_mutex_init_value(&mps->mutex);
	if (pthread_mutex_init(&mps->mutex, NULL) != 0)
		goto error;

	pthread_mutex_init_value(&mps->audio_mutex);
	if (pthread_mutex_init(&mps->audio_mutex, NULL) != 0)
		goto error;

	/* before the first update, which queues the first load */
	if (!pl_player_start(&mps->player, mps->current_media_source))
		goto error;

	obs_source_update(source, NULL);

	return mps;

error:
	mps_destroy(mps);
	return NULL;
}

static void mps_video_render(void *data, gs_effect_t *effect)
{
	struct media_playlist_source *mps = data;
	if (mps->actual_media) {
		obs_source_video_render(mps->current_media_source);
	} else {
		obs_source_video_render(NULL);
	}

	UNUSED_PARAMETER(effect);
}

static bool mps_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate)
{
	struct media_playlist_source *mps = data;
	if (!mps->current_media_source)
		return false;

	struct obs_source_audio_mix child_audio;
	uint64_t source_ts;

	/*if (obs_source_audio_pending(mps->current_media_source))
		return false;*/

	source_ts = obs_source_get_audio_timestamp(mps->current_media_source);
	if (!source_ts)
		return false;

	obs_source_get_audio_mix(mps->current_media_source, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			memcpy(out, in, AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS * sizeof(float));
		}
	}

	*ts_out = source_ts;

	UNUSED_PARAMETER(sample_rate);
	return true;
}

static void mps_video_tick(void *data, float seconds)
{
	struct media_playlist_source *mps = data;
	//UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);

	const audio_t *a = obs_get_audio();
	const struct audio_output_info *aoi = audio_output_get_info(a);
	pthread_mutex_lock(&mps->audio_mutex);
	while (mps->audio_frames.size > 0) {
		struct obs_source_audio audio;
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		deque_pop_front(&mps->audio_frames, &audio.frames, sizeof(audio.frames));
		deque_pop_front(&mps->audio_timestamps, &audio.timestamp, sizeof(audio.timestamp));
		for (size_t i = 0; i < mps->num_channels; i++) {
			audio.data[i] = (uint8_t *)mps->audio_data[i].data + mps->audio_data[i].start_pos;
		}
		obs_source_output_audio(mps->source, &audio);
		for (size_t i = 0; i < mps->num_channels; i++) {
			deque_pop_front(&mps->audio_data[i], NULL, audio.frames * sizeof(float));
		}
	}
	mps->num_channels = audio_output_get_channels(a);
	pthread_mutex_unlock(&mps->audio_mutex);

	/* Automatic advance is decided here, from the end the child recorded, and executed by the player thread, so
	 * nothing recurses on the media thread and the graphics thread never waits on a load (R-THR-1). Next runs
	 * directly rather than as a queued media action, so a stop queued meanwhile still comes after it; the
	 * parent's next and ended signals are raised with no plugin lock held. */
	if (os_atomic_load_long(&mps->player.tracker.ended_epoch) != 0) {
		/* the end is taken and acted on under the list lock, so a selection made meanwhile either withdraws it
		 * first or comes after the advance; never both */
		pthread_mutex_lock(&mps->mutex);
		bool ended = pl_player_take_end(&mps->player);
		bool moved = ended && playlist_next_locked(mps);
		pthread_mutex_unlock(&mps->mutex);
		if (moved) {
			calldata_t cd = {0};
			calldata_set_ptr(&cd, "source", mps->source);
			signal_handler_signal(obs_source_get_signal_handler(mps->source), "media_next", &cd);
			calldata_free(&cd);
		} else if (ended) {
			mps_end_reached(mps);
		}
	}

	/* the resume point, at most once a second and with no plugin lock held (R-UP-43, R-UP-62) */
	if (os_atomic_load_bool(&mps->save_dirty)) {
		uint64_t now = os_gettime_ns();
		if (now - mps->last_save_ns >= 1000000000ULL) {
			os_atomic_set_bool(&mps->save_dirty, false);
			mps->last_save_ns = now;
			obs_source_save(mps->source);
		}
	}

	//if (mps->restart_on_activate && mps->use_cut) {
	//	mps->elapsed = 0.0f;
	//	mps->cur_item = mps->randomize ? random_file(mps) : 0;
	//	do_transition(mps, false);
	//	mps->restart_on_activate = false;
	//	mps->use_cut = false;
	//	mps->stop = false;
	//	return;
	//}

	//if (mps->pause_on_deactivate || mps->manual || mps->stop || mps->paused)
	//	return;

	///* ----------------------------------------------------- */
	///* do transition when slide time reached                 */
	//mps->elapsed += seconds;

	//if (mps->elapsed > mps->slide_time) {
	//	mps->elapsed -= mps->slide_time;

	//	if (!mps->loop && mps->cur_item == mps->files.num - 1) {
	//		if (mps->hide)
	//			do_transition(mps, true);
	//		else
	//			do_transition(mps, false);

	//		return;
	//	}

	//	if (mps->randomize) {
	//		size_t next = mps->cur_item;
	//		if (mps->files.num > 1) {
	//			while (next == mps->cur_item)
	//				next = random_file(mps);
	//		}
	//		mps->cur_item = next;

	//	} else if (++mps->cur_item >= mps->files.num) {
	//		mps->cur_item = 0;
	//	}

	//	if (mps->files.num)
	//		do_transition(mps, false);
	//}
}

static void mps_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct media_playlist_source *mps = data;

	/* no lock: the child pointer never changes after create, and libobs calls this with its own locks held
	 * (R-UP-72) */
	cb(mps->source, mps->current_media_source, param);
}

static uint32_t mps_width(void *data)
{
	struct media_playlist_source *mps = data;
	return obs_source_get_width(mps->current_media_source);
}

static uint32_t mps_height(void *data)
{
	struct media_playlist_source *mps = data;
	return obs_source_get_height(mps->current_media_source);
}

static void mps_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_int(settings, S_VISIBILITY_BEHAVIOR, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_data_set_default_int(settings, S_RESTART_BEHAVIOR, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_data_set_default_string(settings, S_CURRENT_FILE_NAME, " ");
	obs_data_set_default_int(settings, S_SPEED, 100);
	/* GPU decoding by default (operator decision D-11); upstream left it off */
	obs_data_set_default_bool(settings, S_FFMPEG_HW_DECODE, true);
}

static void add_media_to_selection(obs_property_t *list, struct media_file_data *data)
{
	struct dstr key = {0};
	struct dstr name = {0};

	if (data->parent) {
		dstr_catf(&key, "%zu-%zu", data->parent->index + 1, data->index + 1);
	} else if (data->folder_items.num) {
		for (size_t i = 0; i < data->folder_items.num; i++) {
			add_media_to_selection(list, &data->folder_items.array[i]);
		}
		return;
	} else {
		dstr_catf(&key, "%zu", data->index + 1);
	}
	dstr_copy_dstr(&name, &key);
	dstr_cat(&name, ": ");
	dstr_cat(&name, data->path);
	obs_property_list_add_string(list, name.array, key.array);
	dstr_free(&name);
	dstr_free(&key);
}

static void update_current_filename_property(struct media_playlist_source *mps, obs_property_t *p)
{
	struct dstr long_desc = {0};
	if (!mps || !p)
		return;
	if (!mps->actual_media) {
		obs_property_set_long_description(p, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_property_set_long_description(p, long_desc.array);
	dstr_free(&long_desc);
}

static obs_properties_t *mps_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	struct dstr filter = {0};
	struct dstr exts = {0};
	struct dstr path = {0};
	obs_property_t *p;

	obs_properties_add_bool(props, S_LOOP, T_LOOP);
	obs_properties_add_bool(props, S_SHUFFLE, T_SHUFFLE);

	// get last directory opened for editable list
	if (mps) {
		pthread_mutex_lock(&mps->mutex);
		if (mps->files.num) {
			struct media_file_data *last = da_end(mps->files);
			const char *slash;

			dstr_copy(&path, last->path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&mps->mutex);
	}

	p = obs_properties_add_list(props, S_VISIBILITY_BEHAVIOR, T_VISIBILITY_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_RESTART, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT, VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE, VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY, VISIBILITY_BEHAVIOR_ALWAYS_PLAY);

	p = obs_properties_add_list(props, S_RESTART_BEHAVIOR, T_RESTART_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_CURRENT_FILE, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_FIRST_FILE, RESTART_BEHAVIOR_FIRST_FILE);

	obs_properties_add_bool(props, S_FFMPEG_HW_DECODE, T_USE_HARDWARE_DECODING);

	p = obs_properties_add_bool(props, S_FFMPEG_CLOSE_WHEN_INACTIVE, T_FFMPEG_CLOSE_WHEN_INACTIVE);
	obs_property_set_long_description(p, T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	p = obs_properties_add_editable_list(props, S_PLAYLIST, T_PLAYLIST, OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					     filter.array, path.array);
	dstr_free(&path);
	dstr_free(&filter);
	dstr_free(&exts);

	p = obs_properties_add_text(props, S_CURRENT_FILE_NAME, T_CURRENT_FILE_NAME, OBS_TEXT_INFO);
	obs_property_set_long_description(p, "Due to OBS limitations, this will only update if any settings"
					     " are changed, the selected file is played, or the Properties "
					     "window is reopened. It will not update when the video ends.");
	obs_properties_add_button2(props, S_REFRESH_FILENAME, T_REFRESH_FILENAME, refresh_filename_clicked, mps);
	//update_current_filename_property(mps, p);

	p = obs_properties_add_list(props, S_SELECT_FILE, T_SELECT_FILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_NO_FILE_SELECTED, "0");
	if (mps) {
		for (size_t i = 0; i < mps->files.num; i++) {
			add_media_to_selection(p, &mps->files.array[i]);
		}
	}

	obs_properties_add_button2(props, "play_selected", "Play Selected File", play_selected_clicked, mps);

	p = obs_properties_add_int_slider(props, S_SPEED, T_SPEED, 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_text(props, "", T_SPEED_WARNING, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	obs_data_array_release(array);
	obs_data_release(settings);

	return props;
}

/* Sets the parent field of each media_file_data. This is necessary to be
 * called AFTER the darray is size is modified (because of array reallocation)
 */
static void set_parents(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;
	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *item = &files.array[i];
		for (size_t j = 0; j < item->folder_items.num; j++) {
			struct media_file_data *folder_item = &item->folder_items.array[j];
			folder_item->parent = item;
		}
	}
}

static void add_file(struct darray *array, const char *path, const char *id)
{
	DARRAY(struct media_file_data) new_files;
	new_files.da = *array;
	struct media_file_data *data = da_push_back_new(new_files);

	data->id = bstrdup(id);
	data->index = new_files.num - 1;
	data->path = bstrdup(path);
	data->is_url = strstr(path, "://") != NULL;
	da_init(data->folder_items);

	os_dir_t *dir = os_opendir(path);

	if (dir) {
		struct dstr dir_path = {0};
		struct os_dirent *ent;

		data->is_folder = true;

		while (true) {
			const char *ext;

			ent = os_readdir(dir);
			if (!ent)
				break;
			if (ent->directory)
				continue;

			ext = os_get_path_extension(ent->d_name);
			if (!valid_extension(ext))
				continue;

			struct media_file_data folder_item = {0};
			folder_item.filename = bstrdup(ent->d_name);
			folder_item.parent_id = data->id;
			folder_item.index = data->folder_items.num;
			dstr_copy(&dir_path, path);
			dstr_cat_ch(&dir_path, '/');
			dstr_cat(&dir_path, ent->d_name);
			folder_item.path = bstrdup(dir_path.array);

			da_push_back(data->folder_items, &folder_item);
		}

		dstr_free(&dir_path);
		os_closedir(dir);
	}

	*array = new_files.da;
}

static void mps_update(void *data, obs_data_t *settings)
{
	DARRAY(struct media_file_data) new_files;
	DARRAY(struct media_file_data) old_files;
	struct media_playlist_source *mps = data;
	obs_data_array_t *array;
	size_t count;
	bool shuffle = false;
	bool shuffle_changed = false;
	enum visibility_behavior visibility_behavior = mps->visibility_behavior;
	bool visibility_behavior_changed = false;
	bool item_edited = false;
	bool restart_on_activate = true;
	const char *old_media_path = NULL;
	long long new_speed;
	//const char *mode;

	/* ------------------------------------- */
	/* get settings data */

	da_init(new_files);

	mps->visibility_behavior = obs_data_get_int(settings, S_VISIBILITY_BEHAVIOR);
	if (mps->visibility_behavior != visibility_behavior) {
		visibility_behavior_changed = true;
	}
	mps->restart_behavior = obs_data_get_int(settings, S_RESTART_BEHAVIOR);
	shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	shuffle_changed = mps->shuffle != shuffle;
	mps->shuffle = shuffle;
	mps->loop = obs_data_get_bool(settings, S_LOOP);
	shuffler_set_loop(&mps->shuffler, mps->loop);
	new_speed = obs_data_get_int(settings, S_SPEED);
	mps->speed = new_speed;

	/* Internal media source settings */
	mps->use_hw_decoding = obs_data_get_bool(settings, S_FFMPEG_HW_DECODE);
	mps->close_when_inactive = obs_data_get_bool(settings, S_FFMPEG_CLOSE_WHEN_INACTIVE);
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_ALWAYS_PLAY ||
	    mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		restart_on_activate = false;
	}
	/* the player thread applies these to the child and writes them again with every load */
	struct pl_child_settings child_settings = {
		.hw_decode = mps->use_hw_decoding,
		.close_when_inactive = mps->close_when_inactive,
		.restart_on_activate = restart_on_activate,
		.speed_percent = mps->speed,
	};
	pl_player_set_child_settings(&mps->player, &child_settings);
	/* the child's state is only read here; our own state is set when requests are made */
	enum obs_media_state child_state = obs_source_media_get_state(mps->source);
	if (visibility_behavior_changed && !obs_source_active(mps->source) &&
	    (child_state == OBS_MEDIA_STATE_PLAYING || child_state == OBS_MEDIA_STATE_PAUSED)) {
		mps_deactivate(mps);
	}

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	if (!mps->first_update && mps->current_media) {
		old_media_path = mps->current_media->path;
	}
	if (mps->first_update) {
		mps->current_media_filename = bstrdup(obs_data_get_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME));
		mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	}

	bool found = false;
	pthread_mutex_lock(&mps->mutex);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		const char *id = obs_data_get_string(item, S_ID);

		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}

		if (!mps->first_update && mps->current_media && strcmp(id, mps->current_media->id) == 0) {
			// check for current_media->id only if media isn't changed, allowing scripts to set the index.
			mps->current_media_index = i;
			found = true;
			if (old_media_path)
				item_edited = strcmp(old_media_path, path) != 0;
		}
		add_file(&new_files.da, path, id);
		obs_data_release(item);
	}
	set_parents(&new_files.da);

	if (mps->shuffle) {
		if (shuffle_changed) {
			shuffler_reshuffle(&mps->shuffler);
		}
		shuffler_update_files(&mps->shuffler, &new_files.da);
	} else if (shuffle_changed) {
		bfree(mps->current_media_filename);
		if (mps->actual_media && mps->actual_media->parent_id)
			mps->current_media_filename = bstrdup(mps->actual_media->filename);
		else
			mps->current_media_filename = NULL;
	}
	old_files.da = mps->files.da;
	mps->files.da = new_files.da;
	pthread_mutex_unlock(&mps->mutex);

	free_files(&old_files.da);

	if (found || mps->first_update) {
		set_current_media_index(mps, mps->current_media_index);
	} else {
		set_current_media_index(mps, 0);
	}

	if (get_total_file_count(mps)) {
		if (item_edited) {
			mps->current_folder_item_index = 0;
		} else if (mps->current_media && mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;

			/* Find that file in the folder */
			if (mps->current_media_filename) {
				// some files may have been added/deleted so current file index is changed
				mps->current_folder_item_index = find_folder_item_index(
					&mps->current_media->folder_items.da, mps->current_media_filename);
				if (mps->current_folder_item_index == DARRAY_INVALID) {
					mps->current_folder_item_index = 0;
					found = false;
				}
			}

			if (mps->current_media->folder_items.num == 0) {
				mps_playlist_next(mps);
			} else {
				set_current_folder_item_index(mps, mps->current_folder_item_index);
				if (mps->shuffle)
					shuffler_select(&mps->shuffler, mps->actual_media);
			}
		} else {
			mps->actual_media = mps->current_media;
			if (mps->shuffle)
				shuffler_select(&mps->shuffler, mps->actual_media);
		}

		if (mps->first_update || !found || item_edited) {
			/* Clear if last file is a folder and is empty */
			if (mps->current_media->is_folder && mps->current_media->folder_items.num == 0) {
				clear_media_source(mps);
			} else {
				update_media_source(mps, true);
			}
		}
	} else if (!mps->first_update) {
		bfree(mps->current_media_filename);
		mps->current_media_filename = NULL;
		clear_media_source(mps);
	}
	mark_save_dirty(mps);

	/* So Current File Name is updated */
	update_current_filename_setting(mps, settings);
	/* ------------------------- */

	//if (mps->files.num) {
	//	do_transition(mps, false);

	//	if (mps->manual)
	//		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	//	else
	//		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);

	//	obs_source_media_started(mps->source);
	//}
	obs_data_array_release(array);
	mps->first_update = false;
}

static void mps_save(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX, mps->current_media_index);
	obs_data_set_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME, mps->current_media_filename);
	update_current_filename_setting(mps, settings);
}

static void mps_load(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	if (mps->files.num)
		obs_log(LOG_DEBUG, "%s", mps->files.array[mps->current_media_index].id);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct media_playlist_source *mps = src;
	const char *orig_path = data;

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *file = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);
}

static obs_missing_files_t *mps_missingfiles(void *data)
{
	struct media_playlist_source *mps = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (strcmp(path, "") != 0) {
			if (!os_file_exists(path) && strstr(path, "://") == NULL) {
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source, (void *)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}
