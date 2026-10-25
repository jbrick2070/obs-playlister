-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Runtime harness for OBS 33: drives OBS Playlister sources through the transport, Properties and empty-folder paths
-- and logs "HARNESS PASS|FAIL <check>" lines. Run it with run-harness.ps1, which passes three media files of
-- different durations in PLAYLISTER_HARNESS_FILES (separated by "|") and an empty folder in
-- PLAYLISTER_HARNESS_EMPTY_FOLDER; items are told apart by their durations. Each step waits for its condition (or
-- its timeout), checks the result of the previous action, then performs the next one.
obs = obslua

local FILES = {}
for path in string.gmatch(os.getenv("PLAYLISTER_HARNESS_FILES") or "", "[^|]+") do
	table.insert(FILES, path)
end
local EMPTY_FOLDER = os.getenv("PLAYLISTER_HARNESS_EMPTY_FOLDER")
local PLAYING, PAUSED, STOPPED, ENDED = 1, 4, 5, 6
local src, item = nil, nil
local D = {}
local mark = {}
local steps = {}
local index, step_start = 1, 0

local function log(s)
	obs.script_log(obs.LOG_INFO, "HARNESS " .. s)
end

local function check(name, ok, detail)
	log((ok and "PASS " or "FAIL ") .. name .. " (" .. (detail or "") .. ")")
end

local function now_ms()
	return obs.os_gettime_ns() / 1000000
end

local function st()
	return obs.obs_source_media_get_state(src)
end
local function dur()
	return obs.obs_source_media_get_duration(src)
end
local function tm()
	return obs.obs_source_media_get_time(src)
end
local function status()
	return string.format("state=%d time=%d duration=%d", st(), tm(), dur())
end
local function current_file_name()
	local settings = obs.obs_source_get_settings(src)
	local name = obs.obs_data_get_string(settings, "current_file_name")
	obs.obs_data_release(settings)
	return name
end

local function update_settings(fn)
	local data = obs.obs_data_create()
	fn(data)
	obs.obs_source_update(src, data)
	obs.obs_data_release(data)
end

-- sets the playlist to these rows, each { path, uuid }; the source follows a row by its uuid
local function set_playlist(data, rows)
	local playlist = obs.obs_data_array_create()
	for _, r in ipairs(rows) do
		local row = obs.obs_data_create()
		obs.obs_data_set_string(row, "value", r[1])
		obs.obs_data_set_string(row, "uuid", r[2])
		obs.obs_data_array_push_back(playlist, row)
		obs.obs_data_release(row)
	end
	obs.obs_data_set_array(data, "playlist", playlist)
	obs.obs_data_array_release(playlist)
end

-- creates the source from the settings fill() writes and shows it in the current scene
local function create_source(name, fill)
	local settings = obs.obs_data_create()
	fill(settings)
	src = obs.obs_source_create("obs_playlister_source", name, settings, nil)
	obs.obs_data_release(settings)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), src)
	obs.obs_source_release(scene_source)
end

local function remove_source()
	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(src)
	src, item = nil, nil
end

local function seek_near_end()
	obs.obs_source_media_set_time(src, dur() - 1500)
end

-- min_wait: always wait this long; ready: then wait until it returns true, at most timeout ms in total
local function step(name, min_wait, timeout, ready, run)
	table.insert(steps, { name = name, min_wait = min_wait, timeout = timeout, ready = ready, run = run })
end

step("create", 1000, 1000, nil, function()
	if #FILES ~= 3 then
		error("PLAYLISTER_HARNESS_FILES must name three media files of different durations")
	end
	if not EMPTY_FOLDER then
		error("PLAYLISTER_HARNESS_EMPTY_FOLDER must name an empty folder")
	end
	create_source("playlister-harness", function(settings)
		set_playlist(settings, { { FILES[1], "row-1" }, { FILES[2], "row-2" }, { FILES[3], "row-3" } })
		obs.obs_data_set_bool(settings, "loop", true)
	end)
	check("source created and shown", src ~= nil and item ~= nil)
end)

step("first item plays", 1000, 8000, function() return st() == PLAYING and dur() > 0 end, function()
	D[1] = dur()
	check("first item plays", st() == PLAYING, status())
	seek_near_end()
end)

step("auto-advance at end", 1000, 20000, function() return dur() > 0 and dur() ~= D[1] and st() == PLAYING end, function()
	D[2] = dur()
	check("the end of item 1 auto-advances to item 2", st() == PLAYING and D[2] ~= D[1] and tm() < 5000, status())
	obs.obs_source_media_next(src)
end)

step("next", 500, 8000, function() return dur() > 0 and dur() ~= D[2] and st() == PLAYING end, function()
	D[3] = dur()
	check("next goes to item 3", st() == PLAYING and D[3] ~= D[1] and D[3] ~= D[2], status())
	obs.obs_source_media_previous(src)
end)

step("previous", 500, 8000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("previous goes back to item 2", dur() == D[2] and st() == PLAYING, status())
	obs.obs_source_media_stop(src)
end)

step("stop", 4000, 4000, nil, function()
	check("stop stays stopped on the same item", st() == STOPPED and dur() == D[2], status())
	obs.obs_source_media_play_pause(src, false)
end)

step("play from stop", 500, 8000, function() return st() == PLAYING end, function()
	check("play after stop reloads item 2 and plays", st() == PLAYING and dur() == D[2] and tm() < 5000, status())
	obs.obs_source_media_play_pause(src, true)
end)

step("pause", 1000, 1000, nil, function()
	mark.paused_time = tm()
	check("pause pauses", st() == PAUSED, status())
end)

step("pause holds", 1500, 1500, nil, function()
	check("paused time does not move", tm() == mark.paused_time and st() == PAUSED, status())
	obs.obs_source_media_play_pause(src, false)
end)

step("unpause", 500, 5000, function() return tm() > mark.paused_time end, function()
	check("unpause plays on", st() == PLAYING and tm() > mark.paused_time, status())
	obs.obs_source_media_set_time(src, 30000)
end)

step("seek", 500, 5000, function() return tm() > 20000 end, function()
	check("seek moves item 2 to about 30 s", tm() > 20000 and dur() == D[2], status())
	mark.before_restart = tm()
	obs.obs_source_media_restart(src)
end)

step("restart", 1500, 1500, nil, function()
	check("restart replays item 2 from the start", st() == PLAYING and tm() < 10000 and dur() == D[2], status())
	update_settings(function(data) obs.obs_data_set_int(data, "speed_percent", 150) end)
end)

step("speed change", 2000, 2000, nil, function()
	check("speed change keeps playing item 2", st() == PLAYING and dur() == D[2], status())
	obs.obs_sceneitem_set_visible(item, false)
end)

step("hide", 2500, 2500, nil, function()
	check("hide stops without advancing (stop/restart)", st() == STOPPED and dur() == D[2], status())
	obs.obs_sceneitem_set_visible(item, true)
end)

step("show", 500, 8000, function() return st() == PLAYING end, function()
	check("show restarts item 2", st() == PLAYING and dur() == D[2] and tm() < 5000, status())
	update_settings(function(data)
		obs.obs_data_set_bool(data, "loop", false)
		obs.obs_data_set_int(data, "speed_percent", 100)
	end)
end)

step("loop off", 300, 300, nil, function()
	obs.obs_source_media_next(src)
end)

step("last item", 500, 8000, function() return dur() == D[3] and dur() > 0 end, function()
	check("next goes to item 3", dur() == D[3] and st() == PLAYING, status())
	seek_near_end()
end)

-- The child reports ENDED before this source's video_tick takes the end and returns to item 1, and Lua timers run
-- before the sources tick, so a restart sent on the first ENDED can still restart item 3. ENDED on two polls in a
-- row (100 ms apart) means video_tick has run since.
step("end of list", 1000, 20000, function()
	mark.ended_polls = st() == ENDED and (mark.ended_polls or 0) + 1 or 0
	return mark.ended_polls >= 2 or dur() ~= D[3]
end, function()
	check("with loop off the end of the last item ends the list", st() == ENDED and dur() == D[3], status())
	obs.obs_source_media_restart(src)
end)

-- settings reach a video source at its next tick (libobs deferred update), so each settings change gets its
-- own step before the action that depends on it
step("restart after end", 500, 8000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("restart after the end plays item 1", dur() == D[1] and st() == PLAYING, status())
	update_settings(function(data) obs.obs_data_set_bool(data, "loop", true) end)
end)

step("loop on", 300, 300, nil, function()
	obs.obs_source_media_previous(src)
end)

step("wrap backward", 500, 8000, function() return dur() == D[3] and dur() > 0 end, function()
	check("with loop on previous from item 1 wraps to item 3", dur() == D[3] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("wrap forward", 500, 8000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("with loop on next from item 3 wraps to item 1", dur() == D[1] and st() == PLAYING, status())
	update_settings(function(data) obs.obs_data_set_int(data, "visibility_behavior", 3) end)
end)

step("stop/play-next behavior on", 300, 300, nil, function()
	obs.obs_sceneitem_set_visible(item, false)
end)

step("hide plays next", 2500, 2500, nil, function()
	check("stop/play-next behavior: hide does not start the next item", st() ~= PLAYING, status())
	obs.obs_sceneitem_set_visible(item, true)
end)

step("show next", 500, 8000, function() return st() == PLAYING and dur() > 0 end, function()
	check("stop/play-next behavior: show plays item 2", st() == PLAYING and dur() == D[2], status())
	update_settings(function(data) obs.obs_data_set_int(data, "visibility_behavior", 0) end)
end)

step("stop/restart behavior on", 300, 300, nil, function()
	obs.obs_source_media_next(src)
	obs.obs_source_media_play_pause(src, true)
end)

step("next then pause", 500, 8000, function() return dur() == D[3] and dur() > 0 end, function()
	check("next then pause in a row: both happen, in order", dur() == D[3] and st() == PAUSED, status())
	-- Properties, its two buttons and save take the list lock (R-THR-1); a self-deadlock hangs OBS here. The buttons
	-- and save write into the live settings, so a placeholder written there first shows that each one ran (the
	-- throttled save from video_tick writes the same keys, but never inside this one call).
	local props = obs.obs_source_properties(src)
	local count = obs.obs_property_list_item_count(obs.obs_properties_get(props, "select_file"))
	check("properties list the three items after No File Selected", count == 4, "entries=" .. count)
	local settings = obs.obs_source_get_settings(src)
	obs.obs_data_set_string(settings, "current_file_name", "placeholder")
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "refresh_filename"), src)
	check("refresh filename names item 3", current_file_name() == "3: " .. FILES[3], current_file_name())
	obs.obs_data_set_string(settings, "select_file", "1")
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "play_selected"), src)
	obs.obs_properties_destroy(props)
	obs.obs_data_set_int(settings, "current_media_index", 99)
	obs.obs_source_save(src)
	local saved = obs.obs_data_get_int(settings, "current_media_index")
	obs.obs_data_release(settings)
	check("save writes the resume point, item 1", saved == 0, "current_media_index=" .. saved)
end)

step("play selected", 500, 8000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("play selected file plays item 1", dur() == D[1] and st() == PLAYING, status())
	check("current file names item 1", current_file_name() == "1: " .. FILES[1], current_file_name())
	remove_source()
end)

-- A second source whose saved current row is an empty folder, as after an OBS restart once that folder was emptied:
-- its first update moves on to the next playable row (R-UP-33, R-UP-61). Always play, because the restart that
-- stop/restart sends when the source is shown would load the next row anyway. run-harness.ps1 also fails the run if
-- the folder is ever loaded, which would mean it was taken for a file row.
step("source with an empty folder row", 1500, 1500, nil, function()
	create_source("playlister-harness-second", function(settings)
		set_playlist(settings, { { EMPTY_FOLDER, "empty" }, { FILES[1], "f1" }, { FILES[2], "f2" } })
		obs.obs_data_set_bool(settings, "loop", true)
		obs.obs_data_set_int(settings, "current_media_index", 0)
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with an empty folder row created and shown", src ~= nil and item ~= nil)
end)

step("empty current folder", 1000, 8000, function() return st() == PLAYING and dur() > 0 end, function()
	check("an empty current folder row moves on to the next row", st() == PLAYING and dur() == D[1], status())
	obs.obs_source_media_next(src)
end)

step("next after the empty folder", 500, 8000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("next goes on to the last row", dur() == D[2] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("wrap past the empty folder", 500, 8000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("with loop on next wraps past the empty folder row", dur() == D[1] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("last row again", 500, 8000, function() return dur() == D[2] and st() == PLAYING end, function()
	update_settings(function(data)
		obs.obs_data_set_bool(data, "loop", false)
		obs.obs_data_set_bool(data, "shuffle", true)
	end)
end)

step("shuffle on, loop off", 300, 300, nil, function()
	obs.obs_source_media_next(src)
end)

-- in list order the last row has no next with loop off, so only the shuffler can reach the other file
step("shuffle next from the last row", 500, 8000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("with shuffle on and loop off next from the last row plays the other file", dur() == D[1] and
		st() == PLAYING, status())
	-- one update turns shuffle off and removes the playing row, so the current row falls back to the empty folder
	-- (R-THR-4: the shuffler follows that list change while shuffle is off)
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", false)
		set_playlist(data, { { EMPTY_FOLDER, "empty" }, { FILES[2], "f2" } })
	end)
end)

step("playing row removed", 500, 8000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("removing the playing row falls back past the empty folder row", dur() == D[2] and st() == PLAYING,
		status())
	obs.obs_source_media_set_time(src, 30000)
end)

-- a row with an empty path is left out of the list, so the playing row's index must not count it (R-UP-77);
-- counted, the update points at the empty folder row, which moves on and restarts the file
step("seek before the blank row", 500, 5000, function() return tm() > 20000 end, function()
	mark.before_blank = tm()
	update_settings(function(data)
		set_playlist(data, { { "", "blank" }, { EMPTY_FOLDER, "empty" }, { FILES[2], "f2" } })
	end)
end)

step("blank row added", 1500, 1500, nil, function()
	check("a row with an empty path added above the playing row neither moves nor restarts it",
		dur() == D[2] and st() == PLAYING and tm() >= mark.before_blank, status() .. " before=" .. mark.before_blank)
	-- editing the playing row's path plays the new file at once (R-UP-32)
	update_settings(function(data)
		set_playlist(data, { { "", "blank" }, { EMPTY_FOLDER, "empty" }, { FILES[3], "f2" } })
	end)
end)

step("playing row edited", 500, 8000, function() return dur() == D[3] and st() == PLAYING end, function()
	check("editing the playing row's path plays the new file", dur() == D[3] and st() == PLAYING, status())
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", true)
		obs.obs_data_set_bool(data, "loop", true)
	end)
end)

step("shuffle and loop on", 300, 300, nil, function()
	mark.before_next = tm()
	obs.obs_source_media_next(src)
end)

-- a smoke test of the shuffler with one item; in list order, next with loop on replays it too
step("one file left", 500, 8000, function()
	return dur() == D[3] and st() == PLAYING and tm() < mark.before_next
end, function()
	check("with one file left next replays it (shuffle and loop on)", dur() == D[3] and st() == PLAYING and
		tm() < mark.before_next, status() .. " before=" .. mark.before_next)
	remove_source()
end)

step("removed", 1500, 1500, nil, function()
	log("DONE")
end)

local function tick()
	if index > #steps then
		return
	end
	local s = steps[index]
	local elapsed = now_ms() - step_start
	if elapsed < s.min_wait then
		return
	end
	if s.ready and elapsed < s.timeout then
		local ok, ready = pcall(s.ready)
		if ok and not ready then
			return
		end
	end
	log(string.format("step %s (after %d ms)", s.name, elapsed))
	local ok, err = pcall(s.run)
	if not ok then
		check("step " .. s.name .. " ran", false, tostring(err))
	end
	index = index + 1
	step_start = now_ms()
	if index > #steps then
		obs.timer_remove(tick)
	end
end

function script_description()
	return "obs-playlister runtime harness"
end

function script_load(settings)
	step_start = now_ms()
	obs.timer_add(tick, 100)
end
