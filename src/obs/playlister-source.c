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

#include "playlister-source.h"

#include "core/filter.h"
#include "core/keys.h"
#include "core/merge.h"
#include "core/modes.h"
#include "core/row-kind.h"
#include "core/sort.h"
#include "providers/fold-key-win.h"

/* the audio queue's planes go straight into struct obs_source_audio */
_Static_assert(PL_AUDIO_MAX_PLANES == MAX_AV_PLANES, "the audio queue must have as many planes as libobs");

static const char *media_filter =
	" (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm *.mp3 *.m4a *.ogg *.aac *.wav *.opus *.flac);;";
static const char *video_filter = " (*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.m4a *.mka *.aac *.ogg *.wav *.opus *.flac);;";

static void set_current_media_index(struct media_playlist_source *mps, size_t index);
static size_t get_total_file_count(struct media_playlist_source *mps);

static inline void reset_folder_item_index(struct media_playlist_source *mps);

static void clear_media_source(void *data);
static void update_media_source(void *data, bool forced);
static void cancel_resume(struct media_playlist_source *mps);

static bool select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index);

static void select_index_proc(void *data, calldata_t *cd);
static void play_folder_item_at_index(void *data, size_t index);
static void play_media_at_index(void *data, size_t index, bool play_last_folder_item, bool backward);

static size_t find_folder_item_index(struct darray *array, const char *key, uint32_t occurrence);

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
static void missing_file_callback(void *src, const char *new_path, void *data);
static obs_missing_files_t *mps_missingfiles(void *data);

static void set_parents(struct darray *array);
static void add_file(struct darray *array, const char *path, const char *id, bool folder);
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
#define S_CURRENT_FOLDER_ITEM_FILENAME "current_folder_item_filename" /* read by the 0 -> 1 migration only */
/* Phase 2 settings (PLAN section 3.4) */
#define S_SCHEMA_VERSION "schema_version"
#define S_CURRENT_ROW_UUID "current_row_uuid"
#define S_CURRENT_ITEM_KEY "current_item_key"
#define S_CURRENT_ITEM_OCCURRENCE "current_item_occurrence"
#define S_INCLUDE_SUBFOLDERS "include_subfolders"
#define S_EXTENSION_FILTER "extension_filter"
#define S_SORT_KEY "sort_key"
#define S_SORT_DESCENDING "sort_descending"
#define S_CONTAINER_MODE "container_mode"
#define S_NEWEST_COUNT "newest_count"
#define S_STILL_DURATION "still_duration_s"
#define S_FOLDER_CHECK_ENABLED "folder_check_enabled"
#define S_FOLDER_CHECK_INTERVAL "folder_check_interval_s"
#define S_LOCK_LIST_AT_START "lock_list_at_start"
#define S_ROW_KINDS "row_kinds" /* row uuid -> {"kind": "folder", "path": ...} (R-LIST-1) */
#define ROW_KIND_FOLDER "folder"
#define SCHEMA_VERSION 1
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
#define T_SELECT_FILE_STALE T_("SelectFileStale")
#define T_USE_HARDWARE_DECODING T_("UseHardwareDecoding")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE T_("CloseFileWhenInactive")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP T_("CloseFileWhenInactive.Tooltip")
#define T_SPEED T_("Speed")
#define T_SPEED_WARNING T_("SpeedWarning")
#define T_REFRESH_FILENAME T_("RefreshFilename")

#define T_PLAY_PAUSE T_("PlayPause")
#define T_RESTART T_("Restart")
#define T_STOP T_("Stop")

/* The resume point is saved from video_tick, never under the list lock (R-UP-43, R-UP-62) */
static inline void mark_save_dirty(struct media_playlist_source *mps)
{
	os_atomic_set_bool(&mps->save_dirty, true);
}

/* Keeps the playing folder item's identity (R-KEY-1) to find it again after a rescan; a file row or no item keeps
 * none. The caller holds the list lock. */
static void remember_item(struct media_playlist_source *mps, const struct media_file_data *item)
{
	bfree(mps->current_item_key);
	mps->current_item_key = NULL;
	mps->current_item_occurrence = 1;
	if (item && item->parent_id) {
		mps->current_item_key = bstrdup(item->entry ? item->entry->key : item->filename);
		mps->current_item_occurrence = item->entry ? item->entry->occurrence : 1;
	}
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
		remember_item(mps, NULL);
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
	remember_item(mps, NULL);
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
		remember_item(mps, mps->actual_media);
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = NULL;
	}
}

static inline void reset_folder_item_index(struct media_playlist_source *mps)
{
	mps->current_folder_item_index = 0;
	remember_item(mps, NULL);
}

/* The caller holds the list lock */
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
	/* nothing is playing now; get_state and video_render go by this (R-UP-78). Callers hold the list lock. */
	mps->actual_media = NULL;
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
		/* any load decides what plays, so a wait for a folder row to settle is over (R-KEY-2) */
		cancel_resume(mps);
	}
}

/* Returns whether a selection was played. The caller holds the list lock. */
static bool select_index_locked(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index)
{
	bool played = false;
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
			played = true;
		}
	}
	return played;
}

/* Returns whether a selection was played */
static bool select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index)
{
	pthread_mutex_lock(&mps->mutex);
	bool played = select_index_locked(mps, media_index, folder_item_index);
	pthread_mutex_unlock(&mps->mutex);
	return played;
}

/* R-UP-20: the row and item a Select File identity names, exactly: an item whose occurrence is gone is not replaced
 * by another of its key, since the choice was that item. The caller holds the list lock. */
static bool find_selection(struct media_playlist_source *mps, const char *text, size_t *media_index,
			   size_t *folder_item_index)
{
	struct pl_selection selection;
	if (!pl_selection_parse(&selection, text))
		return false;

	bool found = false;
	for (size_t i = 0; i < mps->files.num && !found; i++) {
		struct media_file_data *row = &mps->files.array[i];
		if (strcmp(row->id, selection.row_uuid) != 0)
			continue;
		if (selection.occurrence == 0) {
			found = !row->is_folder;
			*media_index = i;
			*folder_item_index = 0;
			break;
		}
		for (size_t j = 0; row->is_folder && j < row->folder_items.num; j++) {
			const struct pl_entry *entry = row->folder_items.array[j].entry;
			if (entry && entry->occurrence == selection.occurrence &&
			    strcmp(entry->key, selection.item_key) == 0) {
				*media_index = i;
				*folder_item_index = j;
				found = true;
				break;
			}
		}
		break;
	}
	pl_selection_free(&selection);
	return found;
}

/* Tells listeners that the source moved to another item, with the calldata libobs gives its own media signals.
 * Raised with no plugin lock held. */
static void signal_media_next(struct media_playlist_source *mps)
{
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "source", mps->source);
	signal_handler_signal(obs_source_get_signal_handler(mps->source), "media_next", &cd);
	calldata_free(&cd);
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
		remember_item(mps, mps->actual_media);
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
	/* index is a playable row, so the loads below set actual_media. Upstream tested actual_media here, before
	 * anything set it: a stale or cleared pointer then decided between loading and clearing. */
	set_current_media_index(mps, index);

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
	remember_item(mps, mps->actual_media);
	mark_save_dirty(mps);
}

/* By key and occurrence (R-KEY-1); an item without an entry is matched by its file name, as upstream did */
static size_t find_folder_item_index(struct darray *array, const char *key, uint32_t occurrence)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	size_t same_key = DARRAY_INVALID;
	for (size_t i = 0; i < files.num; i++) {
		const struct media_file_data *item = &files.array[i];
		if (!item->entry) {
			if (strcmp(item->filename, key) == 0)
				return i;
			continue;
		}
		if (strcmp(item->entry->key, key) != 0)
			continue;
		if (item->entry->occurrence == occurrence)
			return i;
		if (same_key == DARRAY_INVALID)
			same_key = i;
	}

	/* the saved occurrence is gone but the key is there: that item (R-KEY-2) */
	return same_key;
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
	pthread_mutex_lock(&mps->mutex);
	bool has_files = get_total_file_count(mps) > 0;
	/* R-UP-78: after a clear nothing is loaded (no playing item), but the child keeps the state of the media it
	 * had, PLAYING included: its stop acts only on media it still has */
	bool loaded = mps->actual_media != NULL;
	pthread_mutex_unlock(&mps->mutex);
	if (!has_files)
		return OBS_MEDIA_STATE_NONE;
	if (!loaded)
		return OBS_MEDIA_STATE_STOPPED;
	return obs_source_media_get_state(mps->current_media_source);
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
	pl_audio_queue_push(&mps->audio, audio_data->data, audio_data->frames, audio_data->timestamp);
}

static bool play_selected_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	bool played = false;
	/* "0" is No File Selected, also the default, and an empty value is no choice either (R-UP-65) */
	const char *choice = obs_data_get_string(settings, S_SELECT_FILE);
	bool chosen = *choice && strcmp(choice, "0") != 0;
	pthread_mutex_lock(&mps->mutex);
	size_t media_index = 0;
	size_t folder_item_index = 0;
	/* R-UP-20: the identity is looked up and played under one hold of the lock, so the list cannot change between */
	bool stale = chosen && !find_selection(mps, choice, &media_index, &folder_item_index);
	if (chosen && !stale)
		played = select_index_locked(mps, media_index, folder_item_index);
	if (stale) {
		/* the item is gone or moved since Properties listed it (or the value predates identities): no guess. Shown
		 * in the Current File text until the row status block (R-LIST-5) takes it. */
		obs_log(LOG_INFO, "play selected: the chosen item is no longer in the list; refresh and choose again");
		obs_data_set_string(settings, S_CURRENT_FILE_NAME, T_SELECT_FILE_STALE);
		/* the dropdown has no entry for that value; it shows No File Selected until another item is chosen */
		obs_data_set_string(settings, S_SELECT_FILE, "0");
	} else {
		update_current_filename_setting(mps, settings);
	}
	pthread_mutex_unlock(&mps->mutex);
	/* released after its last use (R-UP-71) */
	obs_data_release(settings);
	//obs_source_update_properties(mps->source);

	/* R-UP-71: only a selection that was played is a move to another item */
	if (played)
		signal_media_next(mps);
	return true;
}

static bool refresh_filename_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	pthread_mutex_lock(&mps->mutex);
	update_current_filename_setting(mps, settings);
	pthread_mutex_unlock(&mps->mutex);
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

static void free_entry(struct pl_entry *entry)
{
	if (entry) {
		pl_entry_free(entry);
		bfree(entry);
	}
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
		free_entry(file->entry);
		for (size_t k = 0; k < file->pending_count; k++)
			bfree(file->pending_keys[k]);
		bfree(file->pending_keys);
		if (file->is_folder) {
			for (size_t j = 0; j < file->folder_items.num; j++) {
				struct media_file_data *folder_item = &file->folder_items.array[j];
				bfree(folder_item->filename);
				bfree(folder_item->path);
				bfree(folder_item->id);
				free_entry(folder_item->entry);
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
			/* Play from stopped or ended is a playback trigger: kept folder lists are published (R-DIR-11) */
			os_atomic_set_bool(&mps->release_held, true);
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

	/* a playback trigger: kept folder lists are published (R-DIR-11); video_tick asks the providers */
	os_atomic_set_bool(&mps->release_held, true);
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
	/* a user stop: a folder row that settles later does not start by itself (R-KEY-2) */
	cancel_resume(mps);
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
		remember_item(mps, mps->actual_media);
		if (mps->actual_media->parent_id) {
			mps->current_media = mps->actual_media->parent;
			mps->current_folder_item_index = mps->actual_media->index;
		} else {
			mps->current_media = mps->actual_media;
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
			remember_item(mps, mps->actual_media);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
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
		/* showing it is still a playback trigger for kept folder lists (R-DIR-11) */
		os_atomic_set_bool(&mps->release_held, true);
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

/* ------------------------------------------------------------------------- */
/* Folder rows and their providers (PLAN section 3.3). The callers hold the list lock unless a function says
 * otherwise. */

#define RESUME_WAIT_NS (30ULL * 1000000000ULL) /* R-KEY-2: how long a saved item still being written is waited for */

struct provider_start {
	char *row_uuid;
	char *location;
	uint64_t instance_id;
};

static void cancel_resume(struct media_playlist_source *mps)
{
	bfree(mps->resume.row_uuid);
	bfree(mps->resume.item_key);
	memset(&mps->resume, 0, sizeof(mps->resume));
}

static void start_resume(struct media_playlist_source *mps, const char *row_uuid, const char *item_key,
			 uint32_t item_occurrence, bool from_start)
{
	cancel_resume(mps);
	mps->resume.pending = true;
	mps->resume.from_start = from_start;
	mps->resume.row_uuid = bstrdup(row_uuid);
	mps->resume.item_key = item_key ? bstrdup(item_key) : NULL;
	mps->resume.item_occurrence = item_occurrence;
}

/* The folder row with this uuid, or NULL */
static struct media_file_data *folder_row(struct media_playlist_source *mps, const char *row_uuid, size_t *index)
{
	for (size_t i = 0; i < mps->files.num; i++) {
		struct media_file_data *row = &mps->files.array[i];
		if (row->is_folder && strcmp(row->id, row_uuid) == 0) {
			if (index)
				*index = i;
			return row;
		}
	}
	return NULL;
}

static bool key_pending(const struct media_file_data *row, const char *key)
{
	for (size_t i = 0; i < row->pending_count; i++) {
		if (strcmp(row->pending_keys[i], key) == 0)
			return true;
	}
	return false;
}

/* The item with exactly this key and occurrence, or DARRAY_INVALID: the playing item is that file or none */
static size_t exact_item_index(const struct media_file_data *row, const char *key, uint32_t occurrence)
{
	for (size_t i = 0; i < row->folder_items.num; i++) {
		const struct pl_entry *entry = row->folder_items.array[i].entry;
		if (entry && entry->occurrence == occurrence && strcmp(entry->key, key) == 0)
			return i;
	}
	return DARRAY_INVALID;
}

static void free_items(struct darray *array)
{
	DARRAY(struct media_file_data) items;
	items.da = *array;
	for (size_t i = 0; i < items.num; i++) {
		bfree(items.array[i].filename);
		bfree(items.array[i].path);
		bfree(items.array[i].id);
		free_entry(items.array[i].entry);
	}
	da_free(items);
	*array = items.da;
}

/* The row's items become the snapshot's visible entries, in display order (R-DIR-6, R-DIR-7), and its pending keys
 * the snapshot's */
static void install_items(struct media_file_data *row, const struct pl_snapshot *snapshot)
{
	DARRAY(struct media_file_data) items;
	da_init(items);
	da_reserve(items, snapshot->visible_count);
	for (size_t v = 0; v < snapshot->visible_count; v++) {
		const struct pl_entry *entry = &snapshot->entries[snapshot->visible[v]];
		struct media_file_data item = {0};
		item.path = bstrdup(entry->location);
		const char *name = entry->location;
		for (const char *c = entry->location; *c; c++) {
			if (*c == '/' || *c == '\\')
				name = c + 1;
		}
		item.filename = bstrdup(name);
		item.parent_id = row->id;
		item.index = v;
		item.entry = bzalloc(sizeof(*item.entry));
		pl_entry_copy(item.entry, entry);
		/* a rename is carried once, when the snapshot is applied */
		bfree(item.entry->previous_key);
		item.entry->previous_key = NULL;
		da_push_back(items, &item);
	}
	row->folder_items.da = items.da;

	for (size_t i = 0; i < row->pending_count; i++)
		bfree(row->pending_keys[i]);
	bfree(row->pending_keys);
	row->pending_keys = bmalloc((snapshot->pending_count ? snapshot->pending_count : 1) * sizeof(char *));
	for (size_t i = 0; i < snapshot->pending_count; i++)
		row->pending_keys[i] = bstrdup(snapshot->pending_keys[i]);
	row->pending_count = snapshot->pending_count;
}

/* R-DIR-8: before the shuffler matches old items with new ones, the old items take the keys they were renamed to,
 * all renames of the snapshot at once (a swap swaps); one a rename overwrote matches nothing. The remembered and
 * resume identities follow too. */
static void follow_renames(struct media_playlist_source *mps, struct media_file_data *row, struct darray *old_array,
			   const struct pl_snapshot *snapshot)
{
	DARRAY(struct media_file_data) old_items;
	old_items.da = *old_array;
	/* a shallow view: the rekey replaces (and frees) key strings, which go back into the items, an overwritten
	 * one as "" so it matches nothing */
	struct pl_entry *view = bmalloc((old_items.num ? old_items.num : 1) * sizeof(*view));
	for (size_t i = 0; i < old_items.num; i++)
		view[i] = *old_items.array[i].entry;
	pl_merge_rekey(view, old_items.num, snapshot->entries, snapshot->count);
	for (size_t i = 0; i < old_items.num; i++)
		old_items.array[i].entry->key = view[i].key ? view[i].key : bstrdup("");
	bfree(view);

	if (mps->current_media == row && mps->current_item_key) {
		struct pl_item_id id = {mps->current_item_key, mps->current_item_occurrence};
		if (pl_merge_follow_rename(&id, snapshot->entries, snapshot->count)) {
			char *key = bstrdup(id.key);
			bfree(mps->current_item_key);
			mps->current_item_key = key;
			mps->current_item_occurrence = id.occurrence;
		}
	}
	if (mps->resume.pending && mps->resume.item_key && strcmp(mps->resume.row_uuid, row->id) == 0) {
		struct pl_item_id id = {mps->resume.item_key, mps->resume.item_occurrence};
		if (pl_merge_follow_rename(&id, snapshot->entries, snapshot->count)) {
			char *key = bstrdup(id.key);
			bfree(mps->resume.item_key);
			mps->resume.item_key = key;
			mps->resume.item_occurrence = id.occurrence;
		}
	}
}

/* R-DIR-8: the playing item left its row (deleted, moved away or filtered out). Playback moves to the first item
 * that followed it in the previous order and is still listed (with shuffle on, the shuffler's next), else on past
 * the row as at its end; with nothing to go on to, the child is cleared. */
static void move_past_removed(struct media_playlist_source *mps, struct media_file_data *row, size_t row_index,
			      const struct darray *old_array, size_t position)
{
	DARRAY(struct media_file_data) old_items;
	old_items.da = *old_array;
	if (!mps->shuffle) {
		for (size_t j = position + 1; j < old_items.num; j++) {
			const struct pl_entry *entry = old_items.array[j].entry;
			size_t successor = exact_item_index(row, entry->key, entry->occurrence);
			if (successor != DARRAY_INVALID) {
				obs_log(LOG_INFO, "row %zu: the playing item is gone; playing the one that followed it",
					row_index + 1);
				play_folder_item_at_index(mps, successor);
				return;
			}
		}
		/* as at the row's end */
		mps->current_folder_item_index = row->folder_items.num ? row->folder_items.num - 1 : 0;
	}
	obs_log(LOG_INFO, "row %zu: the playing item is gone; going on past it", row_index + 1);
	if (!playlist_next_locked(mps))
		clear_media_source(mps);
}

/* Plays the resume target in its settled row: the saved item, or with it still being written a wait of up to 30 s,
 * or else the row's first item, each fallback logged (R-KEY-2) */
static void resume_in_row(struct media_playlist_source *mps, struct media_file_data *row, size_t row_index)
{
	size_t found = mps->resume.item_key ? find_folder_item_index(&row->folder_items.da, mps->resume.item_key,
								     mps->resume.item_occurrence)
					    : DARRAY_INVALID;
	if (found == DARRAY_INVALID && mps->resume.item_key && key_pending(row, mps->resume.item_key)) {
		if (!mps->resume.wait_until_ns) {
			mps->resume.wait_until_ns = os_gettime_ns() + RESUME_WAIT_NS;
			obs_log(LOG_INFO, "row %zu: the saved item is still being written; waiting up to 30 s for it",
				row_index + 1);
		}
		return;
	}
	if (found == DARRAY_INVALID) {
		if (mps->resume.item_key)
			obs_log(LOG_INFO, "row %zu: the saved item is not there; starting at the row's first item",
				row_index + 1);
		found = 0;
	}
	set_current_media_index(mps, row_index);
	set_current_folder_item_index(mps, found);
	if (mps->shuffle)
		shuffler_select(&mps->shuffler, mps->actual_media);
	/* the load ends the resume */
	update_media_source(mps, true);
	mark_save_dirty(mps);
}

/* From the start, the saved row has nothing to play (it settled empty, or its folder is missing): the next row with
 * something to play starts, as for an empty folder row at startup (R-UP-33); with none yet, the first row that
 * settles with items does (R-KEY-2) */
static void resume_after_row(struct media_playlist_source *mps, size_t row_index)
{
	size_t num = mps->files.num;
	for (size_t step = 1; step <= num; step++) {
		size_t index = (row_index + step) % num;
		const struct media_file_data *candidate = &mps->files.array[index];
		if (!candidate->is_folder || candidate->folder_items.num) {
			obs_log(LOG_INFO, "row %zu has nothing to play; starting at row %zu", row_index + 1, index + 1);
			play_media_at_index(mps, index, false, false);
			return;
		}
	}
}

/* The resume target's row has nothing to play: it settled empty, its folder is missing, or it cannot be watched.
 * From the start it is passed over (R-UP-33); after an edit the child is cleared, as upstream (R-UP-78). */
static void resume_row_has_nothing(struct media_playlist_source *mps, size_t target_index)
{
	if (!mps->resume.from_start) {
		cancel_resume(mps);
		clear_media_source(mps);
		return;
	}
	mps->resume.row_unavailable = true;
	resume_after_row(mps, target_index);
}

/* After a snapshot of row (at row_index) was applied: settle the resume target if it waits on this row, or on a
 * row that had nothing to play */
static void settle_resume(struct media_playlist_source *mps, struct media_file_data *row, size_t row_index,
			  const struct pl_snapshot *snapshot)
{
	size_t target_index = 0;
	struct media_file_data *target = folder_row(mps, mps->resume.row_uuid, &target_index);
	if (!target) {
		cancel_resume(mps);
		return;
	}
	if (target == row) {
		bool settled = snapshot->initial_scan_complete;
		bool missing = !settled && snapshot->level == PL_STATUS_ERROR;
		if (settled && row->folder_items.num) {
			resume_in_row(mps, row, row_index);
			return;
		}
		if (!settled && !missing)
			return;
		resume_row_has_nothing(mps, target_index);
		return;
	}
	if (mps->resume.row_unavailable)
		resume_after_row(mps, target_index);
}

/* The publish callback (PLAN section 3.3), on the provider's worker: under the list lock the snapshot replaces the
 * row's items, unless it comes from an instance the row no longer expects (R-THR-2). The playing item keeps
 * playing, found by key and occurrence (R-DIR-8), and the shuffler follows (R-THR-4). */
static void apply_snapshot(void *owner, struct pl_snapshot *snapshot)
{
	struct media_playlist_source *mps = owner;
	pthread_mutex_lock(&mps->mutex);
	const struct row_provider *registration = NULL;
	for (size_t i = 0; i < mps->providers.num && !registration; i++) {
		if (mps->providers.array[i].instance_id == snapshot->instance_id)
			registration = &mps->providers.array[i];
	}
	size_t row_index = 0;
	struct media_file_data *row = registration ? folder_row(mps, registration->row_uuid, &row_index) : NULL;
	if (!row) {
		pthread_mutex_unlock(&mps->mutex);
		pl_snapshot_free(snapshot);
		return;
	}

	bool current_row = mps->current_media == row;
	/* the item loaded last, whatever the state (after the list ended it stays on the last item); it is followed by
	 * its own identity, which the renames below update */
	struct media_file_data *loaded = mps->actual_media && mps->actual_media->parent == row ? mps->actual_media
											       : NULL;
	size_t loaded_position = loaded ? loaded->index : 0;

	DARRAY(struct media_file_data) old_items;
	old_items.da = row->folder_items.da;
	follow_renames(mps, row, &old_items.da, snapshot);
	install_items(row, snapshot);
	/* a listing settles the row (R-KEY-2), and so does a missing folder: it shows its error and is passed over
	 * until the folder comes back, rather than being waited for */
	if (snapshot->initial_scan_complete || snapshot->level == PL_STATUS_ERROR)
		row->awaiting = false;
	set_parents(&mps->files.da);
	shuffler_update_files(&mps->shuffler, &mps->files.da);

	if (loaded) {
		size_t found = loaded->entry ? exact_item_index(row, loaded->entry->key, loaded->entry->occurrence)
					     : DARRAY_INVALID;
		if (found != DARRAY_INVALID) {
			mps->actual_media = &row->folder_items.array[found];
			if (current_row)
				mps->current_folder_item_index = found;
		} else {
			mps->actual_media = NULL;
			if (current_row && mps->state == OBS_MEDIA_STATE_PLAYING) {
				move_past_removed(mps, row, row_index, &old_items.da, loaded_position);
			} else if (current_row) {
				/* stopped, paused or ended: nothing starts by itself; Play goes on from the item that
				 * followed it, if that is still listed */
				mps->current_folder_item_index = 0;
				for (size_t j = loaded_position + 1; j < old_items.num; j++) {
					const struct pl_entry *entry = old_items.array[j].entry;
					size_t successor = exact_item_index(row, entry->key, entry->occurrence);
					if (successor != DARRAY_INVALID) {
						mps->current_folder_item_index = successor;
						break;
					}
				}
				remember_item(mps, mps->current_folder_item_index < row->folder_items.num
							   ? &row->folder_items.array[mps->current_folder_item_index]
							   : NULL);
			}
			mark_save_dirty(mps);
		}
	} else if (current_row && mps->current_item_key) {
		/* nothing of this row loaded: its position follows the remembered item */
		size_t found = find_folder_item_index(&row->folder_items.da, mps->current_item_key,
						      mps->current_item_occurrence);
		mps->current_folder_item_index = found != DARRAY_INVALID ? found : 0;
	}
	free_items(&old_items.da);

	if (mps->resume.pending)
		settle_resume(mps, row, row_index, snapshot);
	/* the first items of a list that had none: the first row with something becomes the current one, without a
	 * load (with no current row, next and previous would have nothing to go from) */
	if (!mps->current_media && get_total_file_count(mps)) {
		size_t first = skip_empty_folder_rows(mps, 0, false);
		set_current_media_index(mps, first < mps->files.num ? first : 0);
		reset_folder_item_index(mps);
	}
	pthread_mutex_unlock(&mps->mutex);
	pl_snapshot_free(snapshot);
}

/* R-THR-3, under the list lock: the new list's folder rows keep their providers, and their items, while their uuid
 * and path are unchanged; a new folder row, or one whose path changed, gets a new instance id registered here, for
 * the caller to create once the lock is released; the providers of rows that are gone come back to be destroyed. A
 * change of include-subfolders re-creates every provider, and the rows keep their items until the new instances
 * publish (R-THR-4). */
static void reconcile_providers(struct media_playlist_source *mps, struct darray *new_rows_array,
				bool include_subfolders, struct darray *starts_out, struct darray *retired_out)
{
	DARRAY(struct media_file_data) new_rows;
	DARRAY(struct row_provider) kept;
	DARRAY(struct provider_start) starts;
	DARRAY(struct pl_provider *) retired;
	new_rows.da = *new_rows_array;
	da_init(kept);
	da_init(starts);
	da_init(retired);

	for (size_t i = 0; i < new_rows.num; i++) {
		struct media_file_data *row = &new_rows.array[i];
		if (!row->is_folder)
			continue;
		struct row_provider *same = NULL;
		for (size_t j = 0; j < mps->providers.num && !same; j++) {
			struct row_provider *candidate = &mps->providers.array[j];
			if (candidate->row_uuid && strcmp(candidate->row_uuid, row->id) == 0 &&
			    strcmp(candidate->location, row->path) == 0)
				same = candidate;
		}
		struct row_provider entry;
		if (same) {
			entry = *same;
			memset(same, 0, sizeof(*same));
			/* the items move from the old row; pointers into them stay valid */
			struct media_file_data *old_row = folder_row(mps, row->id, NULL);
			if (old_row) {
				row->folder_items.da = old_row->folder_items.da;
				da_init(old_row->folder_items);
				row->awaiting = old_row->awaiting;
				row->pending_keys = old_row->pending_keys;
				row->pending_count = old_row->pending_count;
				old_row->pending_keys = NULL;
				old_row->pending_count = 0;
			}
			if (entry.include_subfolders != include_subfolders) {
				if (entry.provider)
					da_push_back(retired, &entry.provider);
				entry.provider = NULL;
				entry.include_subfolders = include_subfolders;
				entry.instance_id = ++mps->last_instance_id;
			}
		} else {
			entry.row_uuid = bstrdup(row->id);
			entry.location = bstrdup(row->path);
			entry.include_subfolders = include_subfolders;
			entry.instance_id = ++mps->last_instance_id;
			entry.provider = NULL;
		}
		if (!entry.provider) {
			struct provider_start start = {bstrdup(entry.row_uuid), bstrdup(entry.location),
						       entry.instance_id};
			da_push_back(starts, &start);
		}
		da_push_back(kept, &entry);
	}
	/* rows that are gone, or whose path changed */
	for (size_t j = 0; j < mps->providers.num; j++) {
		struct row_provider *gone = &mps->providers.array[j];
		if (gone->provider)
			da_push_back(retired, &gone->provider);
		bfree(gone->row_uuid);
		bfree(gone->location);
	}
	da_free(mps->providers);
	mps->providers.da = kept.da;
	*starts_out = starts.da;
	*retired_out = retired.da;
}

/* With no lock held: the retired providers are destroyed (joined), then the new ones created, each stored in its
 * registration if that still expects it (R-THR-3) */
static void start_and_stop_providers(struct media_playlist_source *mps, struct darray *starts_array,
				     struct darray *retired_array, const struct pl_container_options *options)
{
	DARRAY(struct provider_start) starts;
	DARRAY(struct pl_provider *) retired;
	starts.da = *starts_array;
	retired.da = *retired_array;
	for (size_t i = 0; i < retired.num; i++)
		pl_provider_destroy(retired.array[i]);
	da_free(retired);

	for (size_t i = 0; i < starts.num; i++) {
		struct provider_start *start = &starts.array[i];
		struct pl_provider *provider = pl_provider_create(PL_ROW_FOLDER, start->location, start->row_uuid,
								  start->instance_id, options, apply_snapshot, mps);
		if (!provider)
			obs_log(LOG_ERROR, "the folder %s cannot be watched; its row stays empty", start->location);
		pthread_mutex_lock(&mps->mutex);
		size_t row_index = 0;
		struct media_file_data *row = provider ? NULL : folder_row(mps, start->row_uuid, &row_index);
		if (row) {
			/* never listed: not waited for, but passed over like a missing folder */
			row->awaiting = false;
			if (mps->resume.pending && strcmp(mps->resume.row_uuid, row->id) == 0)
				resume_row_has_nothing(mps, row_index);
		}
		bool stored = false;
		for (size_t j = 0; j < mps->providers.num && provider; j++) {
			if (mps->providers.array[j].instance_id == start->instance_id &&
			    !mps->providers.array[j].provider) {
				mps->providers.array[j].provider = provider;
				stored = true;
			}
		}
		pthread_mutex_unlock(&mps->mutex);
		if (provider && !stored)
			pl_provider_destroy(provider);
		bfree(start->row_uuid);
		bfree(start->location);
	}
	da_free(starts);
	*starts_array = starts.da;
	*retired_array = retired.da;
}

/* The shared container options (R-LIST-3, PLAN section 3.4), each clamped to its range; extension_filter points
 * into settings */
static void read_container_options(obs_data_t *settings, struct pl_container_options *options)
{
	memset(options, 0, sizeof(*options));
	options->include_subfolders = obs_data_get_bool(settings, S_INCLUDE_SUBFOLDERS);
	options->extension_filter = obs_data_get_string(settings, S_EXTENSION_FILTER);
	long long sort = obs_data_get_int(settings, S_SORT_KEY);
	options->sort_key = sort == PL_SORT_LIST || sort == PL_SORT_NAME ? (enum pl_sort_key)sort : PL_SORT_DATE;
	options->sort_descending = obs_data_get_bool(settings, S_SORT_DESCENDING);
	long long mode = obs_data_get_int(settings, S_CONTAINER_MODE);
	options->mode = mode == PL_MODE_NEWEST_ONLY || mode == PL_MODE_NEWEST_N ? (enum pl_mode)mode : PL_MODE_FULL;
	long long newest = obs_data_get_int(settings, S_NEWEST_COUNT);
	options->newest_count = newest < 1 ? 1 : newest > 1000 ? 1000 : (uint32_t)newest;
	options->folder_check_enabled = obs_data_get_bool(settings, S_FOLDER_CHECK_ENABLED);
	long long interval = obs_data_get_int(settings, S_FOLDER_CHECK_INTERVAL);
	options->folder_check_interval_s = interval < 2 ? 2 : interval > 3600 ? 3600 : (uint32_t)interval;
	options->lock_list_at_start = obs_data_get_bool(settings, S_LOCK_LIST_AT_START);
}

/* Whether the running providers need these options (include-subfolders re-creates them instead) */
static bool container_options_changed(const struct media_playlist_source *mps,
				      const struct pl_container_options *options)
{
	const struct pl_container_options *kept = &mps->container_options;
	return !mps->container_filter || strcmp(mps->container_filter, options->extension_filter) != 0 ||
	       kept->sort_key != options->sort_key || kept->sort_descending != options->sort_descending ||
	       kept->mode != options->mode || kept->newest_count != options->newest_count ||
	       kept->folder_check_enabled != options->folder_check_enabled ||
	       kept->folder_check_interval_s != options->folder_check_interval_s ||
	       kept->lock_list_at_start != options->lock_list_at_start;
}

static void keep_container_options(struct media_playlist_source *mps, const struct pl_container_options *options)
{
	bfree(mps->container_filter);
	mps->container_filter = bstrdup(options->extension_filter);
	mps->container_options = *options;
	mps->container_options.extension_filter = mps->container_filter;
}

static void mps_destroy(void *data)
{
	struct media_playlist_source *mps = data;

	/* R-THR-3: every provider first, so none publishes into a source being torn down; no lock is held, as a
	 * worker inside its publish waits for the list lock */
	for (size_t i = 0; i < mps->providers.num; i++) {
		pl_provider_destroy(mps->providers.array[i].provider);
		bfree(mps->providers.array[i].row_uuid);
		bfree(mps->providers.array[i].location);
	}
	da_free(mps->providers);
	cancel_resume(mps);
	bfree(mps->container_filter);

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
	pl_audio_queue_free(&mps->audio);
	for (size_t i = 0; i < mps->missing_paths.num; i++)
		bfree(mps->missing_paths.array[i]);
	da_free(mps->missing_paths);
	pthread_mutex_destroy(&mps->mutex);
	bfree(mps->current_item_key);
	bfree(mps);
}

static void *mps_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct media_playlist_source *mps = bzalloc(sizeof(*mps));

	mps->first_update = true;
	mps->source = source;

	shuffler_init(&mps->shuffler);

	/* before any callback that takes them is registered. The list lock holds a valid value before its init, because
	 * the error path destroys it; the audio queue frees nothing it did not set up. */
	pthread_mutex_init_value(&mps->mutex);
	if (pthread_mutex_init(&mps->mutex, NULL) != 0)
		goto error;
	/* R-UP-60: the channel count is known from the start, so no chunk is queued with zero planes. OBS changes it
	 * only on a restart, and creates sources only once its audio output exists. */
	if (!obs_get_audio()) {
		obs_log(LOG_ERROR, "OBS has no audio output; the source cannot be created");
		goto error;
	}
	if (!pl_audio_queue_init(&mps->audio, audio_output_get_channels(obs_get_audio())))
		goto error;

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
	/* only the check is locked; rendering the child must not hold the list lock */
	pthread_mutex_lock(&mps->mutex);
	bool has_media = mps->actual_media != NULL;
	pthread_mutex_unlock(&mps->mutex);
	if (has_media) {
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

	/* R-UP-60: every field is set, the planes past the channel count are NULL, and each chunk comes out of the
	 * queue whole even where it wraps around the end of a ring buffer */
	const struct audio_output_info *aoi = audio_output_get_info(obs_get_audio());
	if (aoi) {
		struct obs_source_audio audio = {0};
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		while (pl_audio_queue_pop(&mps->audio, audio.data, &audio.frames, &audio.timestamp))
			obs_source_output_audio(mps->source, &audio);
	}

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
			signal_media_next(mps);
		} else if (ended) {
			mps_end_reached(mps);
		}
	}

	/* R-KEY-2: a saved item still being written is waited for 30 s at most, then the row starts at its first item */
	if (mps->resume.wait_until_ns && os_gettime_ns() >= mps->resume.wait_until_ns) {
		pthread_mutex_lock(&mps->mutex);
		size_t row_index = 0;
		struct media_file_data *row = mps->resume.pending && mps->resume.wait_until_ns &&
							      os_gettime_ns() >= mps->resume.wait_until_ns
						      ? folder_row(mps, mps->resume.row_uuid, &row_index)
						      : NULL;
		if (row) {
			obs_log(LOG_INFO,
				"row %zu: the saved item did not finish writing within 30 s; starting at the row's first item",
				row_index + 1);
			bfree(mps->resume.item_key);
			mps->resume.item_key = NULL;
			mps->resume.wait_until_ns = 0;
			if (row->folder_items.num)
				resume_in_row(mps, row, row_index);
		}
		pthread_mutex_unlock(&mps->mutex);
	}

	/* R-DIR-11: after a playback trigger, every provider publishes the list it kept; release only sets a request,
	 * and providers change only in update and destroy, neither of which runs during a tick */
	if (os_atomic_load_bool(&mps->release_held)) {
		os_atomic_set_bool(&mps->release_held, false);
		pthread_mutex_lock(&mps->mutex);
		for (size_t i = 0; i < mps->providers.num; i++) {
			if (mps->providers.array[i].provider)
				pl_provider_release_held(mps->providers.array[i].provider);
		}
		pthread_mutex_unlock(&mps->mutex);
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
	/* No File Selected (R-UP-65); upstream left it empty */
	obs_data_set_default_string(settings, S_SELECT_FILE, "0");
	obs_data_set_default_int(settings, S_SPEED, 100);
	/* GPU decoding by default (operator decision D-11); upstream left it off */
	obs_data_set_default_bool(settings, S_FFMPEG_HW_DECODE, true);

	/* Phase 2 (PLAN section 3.4). A missing schema_version means 0, so upstream and Phase 1 settings migrate
	 * (R-LIST-4). The container options take effect with the folder provider (step 5) and appear in Properties
	 * in step 7. */
	obs_data_set_default_int(settings, S_SCHEMA_VERSION, 0);
	obs_data_set_default_string(settings, S_CURRENT_ROW_UUID, "");
	obs_data_set_default_string(settings, S_CURRENT_ITEM_KEY, "");
	obs_data_set_default_int(settings, S_CURRENT_ITEM_OCCURRENCE, 1);
	obs_data_set_default_bool(settings, S_INCLUDE_SUBFOLDERS, false);
	obs_data_set_default_string(settings, S_EXTENSION_FILTER, PL_FILTER_DEFAULT);
	obs_data_set_default_int(settings, S_SORT_KEY, PL_SORT_DATE);
	obs_data_set_default_bool(settings, S_SORT_DESCENDING, false);
	obs_data_set_default_int(settings, S_CONTAINER_MODE, PL_MODE_FULL);
	obs_data_set_default_int(settings, S_NEWEST_COUNT, 5);
	obs_data_set_default_int(settings, S_STILL_DURATION, 10);
	obs_data_set_default_bool(settings, S_FOLDER_CHECK_ENABLED, true);
	obs_data_set_default_int(settings, S_FOLDER_CHECK_INTERVAL, 10);
	obs_data_set_default_bool(settings, S_LOCK_LIST_AT_START, false);
}

/* Each item is shown by its position, "N: location" or "N-M: location", and stored by its identity (R-UP-20) */
static void add_media_to_selection(obs_property_t *list, struct media_file_data *data)
{
	struct dstr name = {0};
	char *identity = NULL;

	if (data->parent) {
		if (!data->entry)
			return;
		dstr_printf(&name, "%zu-%zu", data->parent->index + 1, data->index + 1);
		identity = pl_selection_format(data->parent->id, data->entry->occurrence, data->entry->key);
	} else if (data->is_folder) {
		/* R-UP-66: a folder lists its items, so an empty one lists nothing; upstream listed it as a file */
		for (size_t i = 0; i < data->folder_items.num; i++) {
			add_media_to_selection(list, &data->folder_items.array[i]);
		}
		return;
	} else {
		dstr_printf(&name, "%zu", data->index + 1);
		identity = pl_selection_format(data->id, 0, NULL);
	}
	dstr_cat(&name, ": ");
	dstr_cat(&name, data->path);
	obs_property_list_add_string(list, name.array, identity);
	dstr_free(&name);
	bfree(identity);
}

/* The caller holds the list lock (no caller today; upstream's calls are commented out) */
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
		pthread_mutex_lock(&mps->mutex);
		for (size_t i = 0; i < mps->files.num; i++) {
			add_media_to_selection(p, &mps->files.array[i]);
		}
		pthread_mutex_unlock(&mps->mutex);
	}

	obs_properties_add_button2(props, "play_selected", "Play Selected File", play_selected_clicked, mps);

	p = obs_properties_add_int_slider(props, S_SPEED, T_SPEED, 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_text(props, "", T_SPEED_WARNING, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	return props;
}

/* Sets the parent field of each media_file_data. This is necessary to be
 * called AFTER the darray is size is modified (because of array reallocation)
 * (and after a row's items moved to the row that replaced it, whose id string is its own)
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
			folder_item->parent_id = item->id;
		}
	}
}

/* libobs can only tell a directory by opening it, as upstream did */
static bool path_is_directory(const char *path, void *context)
{
	UNUSED_PARAMETER(context);
	os_dir_t *dir = os_opendir(path);
	if (dir)
		os_closedir(dir);
	return dir != NULL;
}

/* A folder kind saved for this row with an unchanged path (R-LIST-1, rule 1) */
static bool saved_folder_kind(obs_data_t *row_kinds, const char *id, const char *path)
{
	if (!row_kinds || !*id)
		return false;
	obs_data_t *saved = obs_data_get_obj(row_kinds, id);
	bool folder = saved && strcmp(obs_data_get_string(saved, "kind"), ROW_KIND_FOLDER) == 0 &&
		      strcmp(obs_data_get_string(saved, "path"), path) == 0;
	obs_data_release(saved);
	return folder;
}

static void save_folder_kind(obs_data_t *row_kinds, const char *id, const char *path)
{
	obs_data_t *kind = obs_data_create();
	obs_data_set_string(kind, "kind", ROW_KIND_FOLDER);
	obs_data_set_string(kind, "path", path);
	obs_data_set_obj(row_kinds, id, kind);
	obs_data_release(kind);
}

/* A folder row starts with no items: its provider lists the folder on its own worker (PLAN section 3.3), and until
 * then the row is awaiting (R-KEY-2). Upstream listed the folder here, on the graphics thread, on every update. */
static void add_file(struct darray *array, const char *path, const char *id, bool folder)
{
	DARRAY(struct media_file_data) new_files;
	new_files.da = *array;
	struct media_file_data *data = da_push_back_new(new_files);

	data->id = bstrdup(id);
	data->index = new_files.num - 1;
	data->path = bstrdup(path);
	data->is_url = strstr(path, "://") != NULL;
	data->is_folder = folder;
	data->awaiting = folder;
	da_init(data->folder_items);

	*array = new_files.da;
}

/* Every row gets a uuid of its own: one saved without it (by an older OBS or a script), or with the uuid of a row
 * above it (copied settings), is given one here, written into the row, so resume, Select File, row_kinds and the
 * folder providers can name it (R-KEY-2, R-UP-20, R-LIST-1, R-THR-2). Upstream matched every such row to the first
 * of them. */
static void assign_missing_row_uuids(obs_data_t *settings)
{
	obs_data_array_t *playlist = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(playlist);
	DARRAY(char *) seen;
	da_init(seen);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *row = obs_data_array_item(playlist, i);
		const char *id = obs_data_get_string(row, S_ID);
		bool repeated = false;
		for (size_t j = 0; j < seen.num && *id && !repeated; j++)
			repeated = strcmp(seen.array[j], id) == 0;
		if (!*id || repeated) {
			char *uuid = os_generate_uuid();
			obs_log(LOG_INFO, "playlist row %zu had %s; it now has %s", i + 1,
				repeated ? "the uuid of a row above it" : "no uuid", uuid);
			obs_data_set_string(row, S_ID, uuid);
			bfree(uuid);
			id = obs_data_get_string(row, S_ID);
		}
		char *copy = bstrdup(id);
		da_push_back(seen, &copy);
		obs_data_release(row);
	}
	for (size_t j = 0; j < seen.num; j++)
		bfree(seen.array[j]);
	da_free(seen);
	obs_data_array_release(playlist);
}

/* R-LIST-4: upstream and Phase 1 settings (schema 0) become schema 1 in the settings object itself, once, when they
 * carry a playlist. current_folder_item_filename becomes the folded current_item_key (a top-level folder item has
 * no subfolder part), and current_row_uuid is the row at current_media_index, counted as the source counts rows:
 * rows with an empty path are left out. Every row that opens as a folder now has its kind saved (R-LIST-1), so a
 * folder whose name ends in a media extension stays a folder as it was upstream. Idempotent: schema 1 is left
 * alone. */
static void migrate_settings(obs_data_t *settings)
{
	if (obs_data_get_int(settings, S_SCHEMA_VERSION) != 0 || !obs_data_has_user_value(settings, S_PLAYLIST))
		return;

	char *key = pl_fold_key(obs_data_get_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME));
	obs_data_set_string(settings, S_CURRENT_ITEM_KEY, key);
	bfree(key);
	obs_data_set_int(settings, S_CURRENT_ITEM_OCCURRENCE, 1);

	long long index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	obs_data_array_t *playlist = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(playlist);
	long long position = 0;
	obs_data_t *row_kinds = obs_data_create();
	for (size_t i = 0; i < count; i++) {
		obs_data_t *row = obs_data_array_item(playlist, i);
		const char *path = obs_data_get_string(row, "value");
		const char *id = obs_data_get_string(row, S_ID);
		if (path && *path) {
			if (position++ == index)
				obs_data_set_string(settings, S_CURRENT_ROW_UUID, id);
			if (*id && !strstr(path, "://") && path_is_directory(path, NULL))
				save_folder_kind(row_kinds, id, path);
		}
		obs_data_release(row);
	}
	obs_data_set_obj(settings, S_ROW_KINDS, row_kinds);
	obs_data_release(row_kinds);
	obs_data_array_release(playlist);
	obs_data_set_int(settings, S_SCHEMA_VERSION, SCHEMA_VERSION);
	obs_log(LOG_INFO, "settings migrated to schema %d", SCHEMA_VERSION);
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
	bool loop = false;
	enum visibility_behavior visibility_behavior = mps->visibility_behavior;
	bool visibility_behavior_changed = false;
	bool item_edited = false;
	bool resumed_row = false; /* first update: the saved row was found, or there is no uuid and the index stands */
	bool restart_on_activate = true;
	long long new_speed;
	//const char *mode;

	/* ------------------------------------- */
	/* get settings data */

	/* before the migration, which names the current row by its uuid */
	assign_missing_row_uuids(settings);
	migrate_settings(settings);
	da_init(new_files);

	mps->visibility_behavior = obs_data_get_int(settings, S_VISIBILITY_BEHAVIOR);
	if (mps->visibility_behavior != visibility_behavior) {
		visibility_behavior_changed = true;
	}
	mps->restart_behavior = obs_data_get_int(settings, S_RESTART_BEHAVIOR);
	/* shuffle and loop are list state and are applied under the list lock below */
	shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	loop = obs_data_get_bool(settings, S_LOOP);
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
	/* the child's state is only read here; our own state is set when requests are made. This calls our own
	 * get_state, which takes the list lock, so it must stay before the lock below. */
	enum obs_media_state child_state = obs_source_media_get_state(mps->source);
	if (visibility_behavior_changed && !obs_source_active(mps->source) &&
	    (child_state == OBS_MEDIA_STATE_PLAYING || child_state == OBS_MEDIA_STATE_PAUSED)) {
		mps_deactivate(mps);
	}

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	/* The new list, folder listings included, is built before the lock, so nothing waits on disk I/O through it.
	 * Each row's kind is detected (R-LIST-1), and the folder kinds are saved again for the rows that have them, so
	 * row_kinds keeps no row that is gone or whose path changed. */
	obs_data_t *row_kinds = obs_data_get_obj(settings, S_ROW_KINDS);
	obs_data_t *new_row_kinds = obs_data_create();
	struct pl_filter filter;
	pl_filter_parse(&filter, obs_data_get_string(settings, S_EXTENSION_FILTER));
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		const char *id = obs_data_get_string(item, S_ID);

		if (path && *path) {
			bool folder = pl_classify_row(path, saved_folder_kind(row_kinds, id, path), &filter,
						      path_is_directory, NULL) == PL_ROW_FOLDER;
			add_file(&new_files.da, path, id, folder);
			if (folder && *id)
				save_folder_kind(new_row_kinds, id, path);
		}
		obs_data_release(item);
	}
	pl_filter_free(&filter);
	obs_data_set_obj(settings, S_ROW_KINDS, new_row_kinds);
	obs_data_release(new_row_kinds);
	obs_data_release(row_kinds);
	set_parents(&new_files.da);
	struct pl_container_options container_options;
	read_container_options(settings, &container_options);

	/* R-UP-44, R-THR-1: the list, the current-item pointers and the shuffler change together under the list lock,
	 * and the old list is freed only after every pointer has moved to the new one */
	pthread_mutex_lock(&mps->mutex);
	shuffle_changed = mps->shuffle != shuffle;
	mps->shuffle = shuffle;
	mps->loop = loop;
	shuffler_set_loop(&mps->shuffler, loop);
	if (mps->first_update) {
		/* R-KEY-2: the row by its uuid (current_media_index only when the uuid is gone), and in a folder row the
		 * item by key and occurrence */
		const char *row_uuid = obs_data_get_string(settings, S_CURRENT_ROW_UUID);
		mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
		/* a row saved without a uuid (an old row the migration could not name) is found by its index alone */
		resumed_row = !*row_uuid;
		for (size_t i = 0; *row_uuid && i < new_files.num; i++) {
			if (strcmp(new_files.array[i].id, row_uuid) == 0) {
				mps->current_media_index = i;
				resumed_row = true;
				break;
			}
		}
		const char *item_key = obs_data_get_string(settings, S_CURRENT_ITEM_KEY);
		long long occurrence = obs_data_get_int(settings, S_CURRENT_ITEM_OCCURRENCE);
		bfree(mps->current_item_key);
		mps->current_item_key = *item_key ? bstrdup(item_key) : NULL;
		mps->current_item_occurrence = occurrence >= 1 && occurrence <= UINT32_MAX ? (uint32_t)occurrence : 1;
	}

	bool found = false;
	if (!mps->first_update && mps->current_media) {
		// check for current_media->id only if media isn't changed, allowing scripts to set the index.
		/* The index is the row's position in the new list, which leaves out rows with an empty path (R-UP-77) */
		for (size_t i = 0; i < new_files.num; i++) {
			if (strcmp(new_files.array[i].id, mps->current_media->id) == 0) {
				mps->current_media_index = i;
				found = true;
				item_edited = strcmp(mps->current_media->path, new_files.array[i].path) != 0;
			}
		}
	}

	/* Folder rows keep their providers and items while their uuid and path stay (R-THR-3); the options reach the
	 * running providers once the lock is released */
	DARRAY(struct provider_start) starts;
	DARRAY(struct pl_provider *) retired;
	DARRAY(struct pl_provider *) option_targets;
	da_init(option_targets);
	reconcile_providers(mps, &new_files.da, container_options.include_subfolders, &starts.da, &retired.da);
	set_parents(&new_files.da);
	if (container_options_changed(mps, &container_options)) {
		keep_container_options(mps, &container_options);
		for (size_t i = 0; i < mps->providers.num; i++) {
			if (mps->providers.array[i].provider)
				da_push_back(option_targets, &mps->providers.array[i].provider);
		}
	}

	/* R-KEY-2: playback waits for a folder row its provider has not listed yet; what plays keeps playing. A wait
	 * already on keeps its target while that row is still in the list. */
	bool defer = false;
	if (mps->resume.pending) {
		struct media_file_data *resume_row = NULL;
		for (size_t i = 0; i < new_files.num && !resume_row; i++) {
			if (new_files.array[i].is_folder && strcmp(new_files.array[i].id, mps->resume.row_uuid) == 0)
				resume_row = &new_files.array[i];
		}
		if (resume_row)
			defer = true;
		else
			cancel_resume(mps);
	}
	if (!defer) {
		size_t target_index = found || mps->first_update ? mps->current_media_index : 0;
		if (target_index >= new_files.num)
			target_index = 0;
		struct media_file_data *target = new_files.num ? &new_files.array[target_index] : NULL;
		if (target && target->is_folder && target->awaiting) {
			/* the saved or playing item's key, unless the row is not the one it was kept for */
			bool same_row = mps->first_update ? resumed_row : found && !item_edited;
			start_resume(mps, target->id, same_row ? mps->current_item_key : NULL,
				     mps->current_item_occurrence, mps->first_update);
			defer = true;
		}
	}

	/* the item key belongs to the row it was kept for: on another row (the playing row was deleted, or the saved row
	 * is gone) a file of the same name is a different item, so that row starts at its first item */
	if (!found && !resumed_row)
		remember_item(mps, NULL);

	/* R-THR-4: the shuffler follows every list change, shuffle on or off, while the old items are still alive */
	if (mps->shuffle && shuffle_changed)
		shuffler_reshuffle(&mps->shuffler);
	shuffler_update_files(&mps->shuffler, &new_files.da);
	/* only while the playing row is still there: a deleted row's item must not be remembered for another row */
	if (!mps->shuffle && shuffle_changed && found)
		remember_item(mps, mps->actual_media);
	old_files.da = mps->files.da;
	mps->files.da = new_files.da;
	/* points into the old list until a branch below sets it again; none may leave it dangling */
	mps->actual_media = NULL;

	if (found || mps->first_update) {
		set_current_media_index(mps, mps->current_media_index);
	} else {
		set_current_media_index(mps, 0);
	}

	bool handled = false;
	if (defer) {
		/* nothing loads here; the snapshot that settles the row does. After an edit the playing row is gone or
		 * moved, so the child is cleared now rather than left playing an item the list no longer has. A target
		 * with nothing to play from the start lets any row with something to play start now. */
		size_t resume_index = 0;
		if (!mps->resume.from_start)
			clear_media_source(mps);
		else if (mps->resume.row_unavailable && folder_row(mps, mps->resume.row_uuid, &resume_index))
			resume_after_row(mps, resume_index);
	} else if (get_total_file_count(mps)) {
		if (item_edited) {
			mps->current_folder_item_index = 0;
		} else if (mps->current_media && mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;

			/* Find that file in the folder */
			if (mps->current_item_key) {
				// some files may have been added/deleted so current file index is changed
				mps->current_folder_item_index =
					find_folder_item_index(&mps->current_media->folder_items.da,
							       mps->current_item_key, mps->current_item_occurrence);
				if (mps->current_folder_item_index == DARRAY_INVALID) {
					mps->current_folder_item_index = 0;
					found = false;
				}
			}

			if (mps->current_media->folder_items.num == 0) {
				/* the current folder row is empty now: move on, or clear when nothing follows */
				if (!playlist_next_locked(mps))
					clear_media_source(mps);
				handled = true;
			} else {
				set_current_folder_item_index(mps, mps->current_folder_item_index);
				/* the item found may be another occurrence of the key, when the saved one is gone */
				remember_item(mps, mps->actual_media);
				if (mps->shuffle)
					shuffler_select(&mps->shuffler, mps->actual_media);
			}
		} else {
			mps->actual_media = mps->current_media;
			if (mps->shuffle)
				shuffler_select(&mps->shuffler, mps->actual_media);
		}

		if (!handled && (mps->first_update || !found || item_edited)) {
			/* Clear if last file is a folder and is empty */
			if (mps->current_media->is_folder && mps->current_media->folder_items.num == 0) {
				clear_media_source(mps);
			} else {
				update_media_source(mps, true);
				remember_item(mps, mps->actual_media);
			}
		}
	} else if (!mps->first_update) {
		remember_item(mps, NULL);
		clear_media_source(mps);
	}
	mark_save_dirty(mps);

	/* So Current File Name is updated */
	update_current_filename_setting(mps, settings);
	mps->first_update = false;
	pthread_mutex_unlock(&mps->mutex);

	free_files(&old_files.da);
	/* R-THR-3: providers are destroyed and created with no lock held; the options reach the running ones */
	start_and_stop_providers(mps, &starts.da, &retired.da, &container_options);
	for (size_t i = 0; i < option_targets.num; i++)
		pl_provider_set_options(option_targets.array[i], &container_options);
	da_free(option_targets);
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
}

static void mps_save(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->mutex);
	/* R-UP-80: until the first update has applied the saved resume point, the settings still hold it, and the
	 * source's own state is empty; a scene-collection save in that window must not overwrite it (nor the schema,
	 * which would skip the migration) */
	if (mps->first_update) {
		pthread_mutex_unlock(&mps->mutex);
		return;
	}
	/* the resume point (R-KEY-2); current_folder_item_filename is no longer written (R-LIST-4). While playback waits
	 * for a folder row to settle, the point it waits for is the one saved. */
	obs_data_set_int(settings, S_SCHEMA_VERSION, SCHEMA_VERSION);
	size_t resume_index = 0;
	if (mps->resume.pending && folder_row(mps, mps->resume.row_uuid, &resume_index)) {
		obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX, resume_index);
		obs_data_set_string(settings, S_CURRENT_ROW_UUID, mps->resume.row_uuid);
		obs_data_set_string(settings, S_CURRENT_ITEM_KEY, mps->resume.item_key ? mps->resume.item_key : "");
		obs_data_set_int(settings, S_CURRENT_ITEM_OCCURRENCE, mps->resume.item_occurrence);
	} else {
		obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX, mps->current_media_index);
		obs_data_set_string(settings, S_CURRENT_ROW_UUID, mps->current_media ? mps->current_media->id : "");
		obs_data_set_string(settings, S_CURRENT_ITEM_KEY, mps->current_item_key ? mps->current_item_key : "");
		obs_data_set_int(settings, S_CURRENT_ITEM_OCCURRENCE, mps->current_item_occurrence);
	}
	update_current_filename_setting(mps, settings);
	pthread_mutex_unlock(&mps->mutex);
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
	obs_data_t *row_kinds = obs_data_get_obj(settings, S_ROW_KINDS);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		/* R-UP-34: file rows only; the relocation dialog takes files, and an absent folder row waits for its
		 * folder (R-LIST-1) */
		if (strcmp(path, "") != 0 && !saved_folder_kind(row_kinds, obs_data_get_string(item, S_ID), path)) {
			if (!os_file_exists(path) && strstr(path, "://") == NULL) {
				/* R-UP-67: the callback can run long after this, when the settings no longer hold path, so
				 * it gets a copy that lives as long as the source; a path reported before reuses its copy */
				char *owned_path = NULL;
				pthread_mutex_lock(&mps->mutex);
				for (size_t j = 0; j < mps->missing_paths.num && !owned_path; j++) {
					if (strcmp(mps->missing_paths.array[j], path) == 0)
						owned_path = mps->missing_paths.array[j];
				}
				if (!owned_path) {
					owned_path = bstrdup(path);
					da_push_back(mps->missing_paths, &owned_path);
				}
				pthread_mutex_unlock(&mps->mutex);
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source, owned_path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_release(row_kinds);
	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}
