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

#include "player.h"

#include <plugin-support.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "core/url.h"

/* Media Source (ffmpeg_source) settings */
#define S_FFMPEG_LOCAL_FILE "local_file"
#define S_FFMPEG_INPUT "input"
#define S_FFMPEG_IS_LOCAL_FILE "is_local_file"
#define S_FFMPEG_HW_DECODE "hw_decode"
#define S_FFMPEG_CLOSE_WHEN_INACTIVE "close_when_inactive"
#define S_FFMPEG_RESTART_ON_ACTIVATE "restart_on_activate"
#define S_FFMPEG_SPEED "speed_percent"
#define S_FFMPEG_CLEAR_ON_MEDIA_END "clear_on_media_end"

static void write_child_settings(obs_data_t *data, const struct pl_child_settings *settings)
{
	obs_data_set_bool(data, S_FFMPEG_HW_DECODE, settings->hw_decode);
	obs_data_set_bool(data, S_FFMPEG_CLOSE_WHEN_INACTIVE, settings->close_when_inactive);
	obs_data_set_bool(data, S_FFMPEG_RESTART_ON_ACTIVATE, settings->restart_on_activate);
	obs_data_set_int(data, S_FFMPEG_SPEED, settings->speed_percent);
}

static bool child_settings_equal(const struct pl_child_settings *a, const struct pl_child_settings *b)
{
	return a->hw_decode == b->hw_decode && a->close_when_inactive == b->close_when_inactive &&
	       a->restart_on_activate == b->restart_on_activate && a->speed_percent == b->speed_percent;
}

/* URLs can carry credentials or signed query strings, so the log keeps scheme, host and path only */
void pl_player_log_location(int level, const char *what, const char *path)
{
	char *shown = pl_url_redacted(path);
	obs_log(level, "%s %s", what, shown);
	bfree(shown);
}

static void child_updated(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct pl_player *player = data;
	os_event_signal(player->child_updated);
}

static void child_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct pl_player *player = data;
	os_event_signal(player->child_stopped);
}

/* Waits until the child signals that it applied what was asked (what names it for the log), with no time limit.
 * Returns true when the child signaled; false when the player is shutting down, without waiting further (also when
 * the shutdown began before the call). */
static bool wait_for_child(struct pl_player *player, os_event_t *event, const char *what)
{
	bool logged = false;
	while (!os_atomic_load_bool(&player->shutting_down)) {
		if (os_event_timedwait(event, 2000) == 0)
			return true;
		if (!logged) {
			obs_log(LOG_WARNING, "the media source has not applied %s for 2 s; still waiting", what);
			logged = true;
		}
	}
	return false;
}

/* libobs applies obs_source_update to a video source at that source's next tick on the graphics thread
 * (obs-source.c, deferred update), and queues media calls for that tick too. Waiting for the child's "update"
 * signal keeps the commands in order: an unload is applied before the load after it, and a pause or seek after a
 * load acts on the new media. The wait has no time limit, because giving up early would let the old media's end
 * count against the next load; only a shutdown ends it. */
static void update_child(struct pl_player *player, obs_data_t *data)
{
	os_event_reset(player->child_updated);
	obs_source_update(player->child, data);
	wait_for_child(player, player->child_updated, "an update");
}

/* Updates the child while its ends are ignored. Destroying media that is still opening makes its decoder thread
 * fail and report ENDED (media.c, mp_media_thread_start); that end belongs to the media being torn down, and
 * counting it advanced again at once, a runaway chain of loads. The destroy joins that thread, so its end
 * arrives before the update completes. */
static void update_child_ignoring_ends(struct pl_player *player, obs_data_t *data)
{
	pl_load_tracker_set_ignore_ends(&player->tracker, true);
	update_child(player, data);
	pl_load_tracker_set_ignore_ends(&player->tracker, false);
}

static void unload_child(struct pl_player *player)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, S_FFMPEG_IS_LOCAL_FILE, true);
	obs_data_set_string(data, S_FFMPEG_INPUT, "");
	obs_data_set_string(data, S_FFMPEG_LOCAL_FILE, "");
	update_child_ignoring_ends(player, data);
	obs_data_release(data);
}

#define CAPTURE_WAIT_MS 100 /* R-BEH-8: about six frames at 60 fps */

/* R-BEH-8: before a load that replaces the item on screen, video_render keeps the child's picture, so the source can
 * draw it until the new item's first frame. The source is not always drawn (a hidden source, a stalled renderer), so
 * the wait is short and the load goes ahead without a hold when nothing answered. A shutdown ends the wait. */
static void capture_frame(struct pl_player *player)
{
	/* reset before the check: shutdown sets the flag and then signals, so neither can be missed */
	os_event_reset(player->capture_answered);
	if (os_atomic_load_bool(&player->shutting_down))
		return;
	os_atomic_set_bool(&player->capture_requested, true);
	os_event_timedwait(player->capture_answered, CAPTURE_WAIT_MS);
	/* withdrawn when unanswered; a capture already under way still ends before the load applies, as the load is
	 * applied at a tick on the graphics thread, where video_render runs too */
	os_atomic_set_bool(&player->capture_requested, false);
}

/* R-BEH-8: takes the child's picture off the screen at once. The child keeps its last frame at its end, and its own
 * stop clears the picture only while it still has its media (obs-ffmpeg-source.c, ffmpeg_source_stop), which an
 * unload, close-when-inactive or a URL's end takes away. */
static void blank_child(struct pl_player *player)
{
	os_atomic_set_bool(&player->frame_release, true);
	obs_source_output_video(player->child, NULL);
}

/* libobs applies a media stop at the child's next tick and then raises its "media_stopped" signal (obs-source.c,
 * process_media_actions). The child's own stop clears the picture only while it still has media; a URL's end or
 * close-when-inactive can destroy the media first, even after a stop was queued. Once the stop has applied nothing
 * runs, so the picture is taken away then, whatever the child did. */
static void stop_child(struct pl_player *player)
{
	os_atomic_set_bool(&player->frame_release, true);
	os_event_reset(player->child_stopped);
	obs_source_media_stop(player->child);
	if (wait_for_child(player, player->child_stopped, "a stop"))
		blank_child(player);
}

static void execute_load(struct pl_player *player, const struct pl_command *command)
{
	/* a newer load, stop or clear request decides what plays; opening this file first would only churn */
	if (pl_load_tracker_superseded(&player->tracker, command->request))
		return;

	if (command->hold_frame) {
		capture_frame(player);
		/* a newer request came during the wait: it would replace this load before it showed anything */
		if (pl_load_tracker_superseded(&player->tracker, command->request))
			return;
	}

	/* Unloading first also makes every load a path change, so the child restarts even when the path is the one
	 * it already had (upstream's should_restart, R-BEH-6) */
	unload_child(player);

	/* From here an end counts, including one raised while the child opens the new media: that is the new media
	 * failing to open, and advancing past it keeps upstream's skip of unopenable files */
	long epoch = pl_load_tracker_begin_load(&player->tracker, command->request);

	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, S_FFMPEG_IS_LOCAL_FILE, !command->is_url);
	obs_data_set_string(data, command->is_url ? S_FFMPEG_INPUT : S_FFMPEG_LOCAL_FILE, command->path);
	write_child_settings(data, &player->settings);
	/* On every load (upstream never wrote it, R-UP-21): the child keeps its last frame at its end. A still stays on
	 * screen while the source holds it (R-DIR-9); a media file's last frame stays only while the next item loads
	 * (R-BEH-8), and an end that nothing follows takes it off the screen (pl_player_blank) */
	obs_data_set_bool(data, S_FFMPEG_CLEAR_ON_MEDIA_END, false);
	update_child(player, data);
	obs_data_release(data);
	/* the new media's evidence counts from here: the old media was destroyed by the unload (R-BEH-5) */
	os_atomic_set_long(&player->applied_epoch, epoch);
	player->starting = true;
	player->starting_url = command->is_url;
	player->loaded_ns = os_gettime_ns();
	pl_player_log_location(LOG_INFO, "playing", command->path);
}

#define STARTUP_WINDOW_LOCAL_MS 5000 /* R-BEH-5 */
#define STARTUP_WINDOW_URL_MS 15000

/* R-UP-79: OBS's media playback clears its pause flag once its thread has opened the file (mp_media_reset,
 * shared/media-playback/media.c), so a pause that reaches the child before then is lost while the child reports
 * PAUSED. A pause is therefore held until the loaded media shows progress, within the startup window. Returns true
 * once that is settled: the media progressed, is not running, or the window ran out. Returns false when a newer
 * command is waiting, so commands never queue up behind the wait, or when the player shuts down. */
static bool wait_until_started(struct pl_player *player)
{
	if (!player->starting)
		return true;
	uint64_t window_ms = player->starting_url ? STARTUP_WINDOW_URL_MS : STARTUP_WINDOW_LOCAL_MS;
	uint64_t limit_ns = player->loaded_ns + window_ms * 1000000ULL;
	/* the window ran out while nothing waited on it: the media has had its time */
	if (os_gettime_ns() >= limit_ns) {
		player->starting = false;
		return true;
	}
	int64_t last_time = 0;
	int distinct_times = 0;
	while (!os_atomic_load_bool(&player->shutting_down) && !pl_command_queue_has_pending(&player->queue)) {
		enum obs_media_state state = obs_source_media_get_state(player->child);
		bool running = state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_OPENING ||
			       state == OBS_MEDIA_STATE_BUFFERING;
		if (!running) {
			player->starting = false;
			return true;
		}
		/* Progress is a third distinct time past 0. Before the reset clears the pause flag it decodes one frame of
		 * each stream, and each frame sets the time once (decode.c, mp_decode_next), so the reset shows at most
		 * two; only playback shows a third. The time is read only while the media runs, as the child frees its
		 * media when it stops. */
		int64_t media_time = obs_source_media_get_time(player->child);
		if (media_time > last_time) {
			last_time = media_time;
			if (++distinct_times == 3) {
				player->starting = false;
				return true;
			}
		}
		if (os_gettime_ns() >= limit_ns) {
			obs_log(LOG_WARNING,
				"the media showed no progress within its startup window; pausing it anyway");
			player->starting = false;
			return true;
		}
		os_sleep_ms(10);
	}
	return false;
}

/* Sends a held pause once the media has started. A newer command ends the wait with the pause still held: that
 * command runs, and the wait resumes after it unless it was a load, stop, clear or play, which drop the pause */
static void send_held_pause(struct pl_player *player)
{
	if (!player->pause_held || !wait_until_started(player))
		return;
	player->pause_held = false;
	obs_source_media_play_pause(player->child, true);
}

static void execute(struct pl_player *player, const struct pl_command *command)
{
	switch (command->type) {
	case PL_COMMAND_LOAD:
		player->pause_held = false;
		execute_load(player, command);
		os_atomic_inc_long(&player->loads_handled);
		break;
	case PL_COMMAND_PLAY_PAUSE:
		player->pause_held = command->pause;
		if (!command->pause)
			obs_source_media_play_pause(player->child, false);
		break;
	case PL_COMMAND_STOP:
		/* again here: a load executed after the request was made may have made itself eligible */
		pl_load_tracker_withdraw(&player->tracker);
		player->starting = false;
		player->pause_held = false;
		stop_child(player);
		break;
	case PL_COMMAND_CLEAR:
		pl_load_tracker_withdraw(&player->tracker);
		player->starting = false;
		player->pause_held = false;
		unload_child(player);
		/* the unload leaves the last frame in the child, and a stop without media does not clear it; waiting for
		 * this stop also keeps its media_stopped from ending a later stop's wait early */
		stop_child(player);
		break;
	case PL_COMMAND_BLANK:
		blank_child(player);
		break;
	case PL_COMMAND_SET_TIME:
		obs_source_media_set_time(player->child, command->time_ms);
		break;
	case PL_COMMAND_SET_CHILD_SETTINGS: {
		/* Every settings change on the source sends these, but the child reopens a URL on any update and a file on
		 * a speed or decoder change, so unchanged values are not sent on */
		if (child_settings_equal(&player->settings, &command->child))
			break;
		player->settings = command->child;
		obs_data_t *data = obs_data_create();
		write_child_settings(data, &player->settings);
		update_child_ignoring_ends(player, data);
		obs_data_release(data);
		break;
	}
	case PL_COMMAND_QUIT:
		break;
	}
	send_held_pause(player);
}

static void *player_thread(void *data)
{
	struct pl_player *player = data;
	struct pl_command command;
	os_set_thread_name("obs-playlister: player");

	for (;;) {
		while (pl_command_queue_pop(&player->queue, &command)) {
			bool quit = command.type == PL_COMMAND_QUIT;
			if (!quit)
				execute(player, &command);
			pl_command_free(&command);
			if (quit)
				return NULL;
		}
		pl_command_queue_wait(&player->queue, 1000);
	}
}

bool pl_player_start(struct pl_player *player, obs_source_t *child)
{
	player->child = child;
	player->settings.hw_decode = true;
	player->settings.restart_on_activate = true;
	player->settings.speed_percent = 100;
	if (!pl_command_queue_init(&player->queue))
		return false;
	if (os_event_init(&player->child_updated, OS_EVENT_TYPE_AUTO) != 0) {
		pl_command_queue_free(&player->queue);
		return false;
	}
	if (os_event_init(&player->capture_answered, OS_EVENT_TYPE_AUTO) != 0) {
		os_event_destroy(player->child_updated);
		pl_command_queue_free(&player->queue);
		return false;
	}
	if (os_event_init(&player->child_stopped, OS_EVENT_TYPE_AUTO) != 0) {
		os_event_destroy(player->capture_answered);
		os_event_destroy(player->child_updated);
		pl_command_queue_free(&player->queue);
		return false;
	}
	signal_handler_t *sh = obs_source_get_signal_handler(child);
	signal_handler_connect(sh, "update", child_updated, player);
	signal_handler_connect(sh, "media_stopped", child_stopped, player);
	if (pthread_create(&player->thread, NULL, player_thread, player) != 0) {
		signal_handler_disconnect(sh, "media_stopped", child_stopped, player);
		signal_handler_disconnect(sh, "update", child_updated, player);
		os_event_destroy(player->child_stopped);
		os_event_destroy(player->capture_answered);
		os_event_destroy(player->child_updated);
		pl_command_queue_free(&player->queue);
		return false;
	}
	player->thread_started = true;
	return true;
}

void pl_player_shutdown(struct pl_player *player)
{
	if (!player->thread_started)
		return;
	/* the graphics thread may be the one destroying the source, so a pending wait must not hold up the join */
	os_atomic_set_bool(&player->shutting_down, true);
	os_event_signal(player->child_updated);
	os_event_signal(player->capture_answered);
	os_event_signal(player->child_stopped);
	struct pl_command command = {.type = PL_COMMAND_QUIT};
	pl_command_queue_push(&player->queue, &command);
	pthread_join(player->thread, NULL);
	player->thread_started = false;
	signal_handler_t *sh = obs_source_get_signal_handler(player->child);
	signal_handler_disconnect(sh, "media_stopped", child_stopped, player);
	signal_handler_disconnect(sh, "update", child_updated, player);
	os_event_destroy(player->child_stopped);
	os_event_destroy(player->capture_answered);
	os_event_destroy(player->child_updated);
	pl_command_queue_free(&player->queue);
}

/* Load, stop and clear: withdraw before and after numbering the request, so a load the player is starting at
 * that moment cannot stay eligible (see pl_load_tracker_begin_load) */
static void push_request(struct pl_player *player, struct pl_command *command)
{
	pl_load_tracker_withdraw(&player->tracker);
	pl_command_queue_push_request(&player->queue, command, &player->tracker.request_sequence);
	pl_load_tracker_withdraw(&player->tracker);
}

void pl_player_load(struct pl_player *player, const char *path, bool is_url, bool still, bool hold_frame)
{
	struct pl_command command = {.type = PL_COMMAND_LOAD,
				     .path = bstrdup(path),
				     .is_url = is_url,
				     .still = still,
				     .hold_frame = hold_frame};
	/* counted before the thread can handle it, so the handled count never runs ahead */
	os_atomic_inc_long(&player->loads_requested);
	push_request(player, &command);
}

bool pl_player_load_pending(struct pl_player *player)
{
	return os_atomic_load_long(&player->loads_requested) != os_atomic_load_long(&player->loads_handled);
}

void pl_player_play_pause(struct pl_player *player, bool pause)
{
	struct pl_command command = {.type = PL_COMMAND_PLAY_PAUSE, .pause = pause};
	pl_command_queue_push(&player->queue, &command);
}

void pl_player_stop(struct pl_player *player)
{
	struct pl_command command = {.type = PL_COMMAND_STOP};
	push_request(player, &command);
}

void pl_player_clear(struct pl_player *player)
{
	struct pl_command command = {.type = PL_COMMAND_CLEAR};
	push_request(player, &command);
}

void pl_player_blank(struct pl_player *player)
{
	struct pl_command command = {.type = PL_COMMAND_BLANK};
	pl_command_queue_push(&player->queue, &command);
}

void pl_player_set_time(struct pl_player *player, int64_t ms)
{
	struct pl_command command = {.type = PL_COMMAND_SET_TIME, .time_ms = ms};
	pl_command_queue_push(&player->queue, &command);
}

void pl_player_set_child_settings(struct pl_player *player, const struct pl_child_settings *settings)
{
	struct pl_command command = {.type = PL_COMMAND_SET_CHILD_SETTINGS, .child = *settings};
	pl_command_queue_push(&player->queue, &command);
}

void pl_player_withdraw(struct pl_player *player)
{
	pl_load_tracker_supersede(&player->tracker);
}

void pl_player_child_ended(struct pl_player *player)
{
	pl_load_tracker_child_ended(&player->tracker);
}

bool pl_player_take_end(struct pl_player *player)
{
	return pl_load_tracker_take_end(&player->tracker);
}

void pl_player_child_audio(struct pl_player *player)
{
	os_atomic_set_long(&player->audio_epoch, os_atomic_load_long(&player->applied_epoch));
}

bool pl_player_load_ended(struct pl_player *player, long epoch)
{
	return epoch && os_atomic_load_long(&player->tracker.ended_epoch) == epoch;
}

bool pl_player_load_eligible(struct pl_player *player, long epoch)
{
	return epoch && os_atomic_load_long(&player->tracker.eligible_epoch) == epoch;
}

bool pl_player_take_capture_request(struct pl_player *player)
{
	return os_atomic_exchange_bool(&player->capture_requested, false);
}

void pl_player_capture_answered(struct pl_player *player)
{
	os_event_signal(player->capture_answered);
}

bool pl_player_take_frame_release(struct pl_player *player)
{
	return os_atomic_exchange_bool(&player->frame_release, false);
}
