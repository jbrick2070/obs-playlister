-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Runtime harness for OBS 33: drives OBS Playlister sources through the transport, Properties and empty-folder paths
-- and logs "HARNESS PASS|FAIL <check>" lines. Run it with run-harness.ps1, which passes three media files of
-- different durations in PLAYLISTER_HARNESS_FILES (separated by "|"), an empty folder in
-- PLAYLISTER_HARNESS_EMPTY_FOLDER, a folder of hard links to the three in PLAYLISTER_HARNESS_MEDIA_FOLDER and a path
-- that does not exist in PLAYLISTER_HARNESS_MISSING_FOLDER; items are told apart by their durations. Each step waits
-- for its condition (or its timeout), checks the result of the previous action, then performs the next one.
obs = obslua

local FILES = {}
for path in string.gmatch(os.getenv("PLAYLISTER_HARNESS_FILES") or "", "[^|]+") do
	table.insert(FILES, path)
end
local EMPTY_FOLDER = os.getenv("PLAYLISTER_HARNESS_EMPTY_FOLDER")
local MEDIA_FOLDER = os.getenv("PLAYLISTER_HARNESS_MEDIA_FOLDER") -- 1.mp4 to 3.mp4: the three files, in order
local MISSING_FOLDER = os.getenv("PLAYLISTER_HARNESS_MISSING_FOLDER") -- never created
local MEDIA_JUNCTION = os.getenv("PLAYLISTER_HARNESS_MEDIA_JUNCTION") -- MEDIA_FOLDER again, named "<...>.mp4"
-- entries that cannot play (R-BEH-5): one that does not exist, and one the script writes that is no media
local HARNESS_TEMP = (os.getenv("TEMP") or "."):gsub("\\", "/")
local MISSING_FILE = HARNESS_TEMP .. "/playlister-harness-missing.mp4"
local CORRUPT_FILE = HARNESS_TEMP .. "/playlister-harness-corrupt.mp4"
local CORRUPT_STILL = HARNESS_TEMP .. "/playlister-harness-corrupt.jpg"
local STILL = os.getenv("PLAYLISTER_HARNESS_STILL") -- a valid 320x180 jpg the runner makes
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

-- sets the playlist to these rows, each { path, uuid }; the source follows a row by its uuid; a row given no uuid is
-- saved without one, as by an older OBS
local function set_playlist(data, rows)
	local playlist = obs.obs_data_array_create()
	for _, r in ipairs(rows) do
		local row = obs.obs_data_create()
		obs.obs_data_set_string(row, "value", r[1])
		if r[2] then
			obs.obs_data_set_string(row, "uuid", r[2])
		end
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

-- counts the source's media_next signals while connected. obslua connects signal handlers later, on its own
-- thread, so a count is taken long after connecting, as the change across one call (signals come on this thread).
local media_next_count = 0
local media_next_connected = false
local function count_media_next(cd)
	media_next_count = media_next_count + 1
end

local function count_media_next_signals()
	obs.signal_handler_connect(obs.obs_source_get_signal_handler(src), "media_next", count_media_next)
	media_next_connected = true
end

local function remove_source()
	if media_next_connected then
		obs.signal_handler_disconnect(obs.obs_source_get_signal_handler(src), "media_next", count_media_next)
		media_next_connected = false
	end
	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(src)
	src, item = nil, nil
end

local function select_file_entries()
	local props = obs.obs_source_properties(src)
	local count = obs.obs_property_list_item_count(obs.obs_properties_get(props, "select_file"))
	obs.obs_properties_destroy(props)
	return count
end

-- the values Select File stores, in list order, joined by spaces (No File Selected's "0" first)
local function select_file_values()
	local props = obs.obs_source_properties(src)
	local list = obs.obs_properties_get(props, "select_file")
	local values = {}
	for i = 0, obs.obs_property_list_item_count(list) - 1 do
		table.insert(values, obs.obs_property_list_item_string(list, i))
	end
	obs.obs_properties_destroy(props)
	return table.concat(values, " ")
end

-- chooses this value in Select File and presses Play Selected File, as Properties does
local function play_selected(value)
	local settings = obs.obs_source_get_settings(src)
	obs.obs_data_set_string(settings, "select_file", value)
	obs.obs_data_release(settings)
	local props = obs.obs_source_properties(src)
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "play_selected"), src)
	obs.obs_properties_destroy(props)
end

-- whether a file exists (the handle opened to find out is closed again)
local function exists(path)
	local file = io.open(path, "rb")
	if file then
		file:close()
		return true
	end
	return false
end

-- copies a file byte for byte
local function copy_file(from, to)
	local input = io.open(from, "rb")
	local bytes = input:read("*a")
	input:close()
	local output = io.open(to, "wb")
	output:write(bytes)
	output:close()
end

-- the Current File text after Refresh Filename (it is written on updates and button presses only, upstream's note)
local function refreshed_file_name()
	local props = obs.obs_source_properties(src)
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "refresh_filename"), src)
	obs.obs_properties_destroy(props)
	return current_file_name()
end

-- a stale choice is reported in the Current File text instead of played (R-UP-20)
local function reports_stale_choice()
	return current_file_name():find("list changed", 1, true) ~= nil
end

-- presses a Properties button of the source, as Properties does
local function press(name)
	local props = obs.obs_source_properties(src)
	obs.obs_property_button_clicked(obs.obs_properties_get(props, name), src)
	obs.obs_properties_destroy(props)
end

-- R-LIST-3: the shared folder options as Properties shows them: one group holding every option and the Apply button,
-- each labeled from the locale file, the numbers with the ranges of PLAN section 3.4, the tooltips set; N shown only
-- in the newest N mode, the interval enabled only while the folder check is on, the Apply button only while the list
-- is locked, both as opened (the source's settings: full list, check on, list locked) and as the options change; and
-- the Browse dialog's still images (R-UP-10). Returns the first thing that does not hold, or nil.
local function folder_options_problem()
	local props = obs.obs_source_properties(src)
	local function visible(name)
		return obs.obs_property_visible(obs.obs_properties_get(props, name))
	end
	local function enabled(name)
		return obs.obs_property_enabled(obs.obs_properties_get(props, name))
	end
	-- as opened, before any change: obs_source_properties applied the source's settings
	local opened_problem = nil
	if visible("newest_count") or not enabled("folder_check_interval_s") or not enabled("apply_waiting_changes") then
		opened_problem = "as opened, N, the interval or the Apply button does not follow the source's settings"
	end
	local group = obs.obs_properties_get(props, "folder_options")
	local content = group and obs.obs_property_group_content(group)
	local problem = nil
	if not content then
		problem = "no folder_options group"
	elseif obs.obs_property_description(group) ~= "Folder and feed options" then
		problem = "group label " .. obs.obs_property_description(group)
	end
	local labels = {
		include_subfolders = "Include subfolders",
		extension_filter = "File types to play",
		sort_key = "Sort by",
		sort_descending = "Descending",
		container_mode = "Files to play",
		newest_count = "N (files)",
		still_duration_s = "Still image duration",
		folder_check_enabled = "Check folder every N seconds",
		folder_check_interval_s = "N (between checks)",
		lock_list_at_start = "Lock list at start",
		apply_waiting_changes = "Apply waiting changes now",
	}
	for name, label in pairs(labels) do
		local property = content and obs.obs_properties_get(content, name)
		if not problem and not property then
			problem = name .. " is not in the group"
		elseif not problem and obs.obs_property_description(property) ~= label then
			problem = name .. " is labeled " .. obs.obs_property_description(property)
		end
	end
	local ranges = {
		newest_count = { 1, 1000, "" },
		still_duration_s = { 1, 3600, " s" },
		folder_check_interval_s = { 2, 3600, " s" },
	}
	for name, range in pairs(ranges) do
		local property = obs.obs_properties_get(props, name)
		if not problem and property and (obs.obs_property_int_min(property) ~= range[1] or
			obs.obs_property_int_max(property) ~= range[2] or
			(obs.obs_property_int_suffix(property) or "") ~= range[3]) then
			problem = name .. " has the range " .. obs.obs_property_int_min(property) .. " to " ..
				obs.obs_property_int_max(property)
		end
	end
	for _, name in ipairs({ "extension_filter", "folder_check_enabled", "lock_list_at_start", "apply_waiting_changes" }) do
		local property = obs.obs_properties_get(props, name)
		if not problem and property and (obs.obs_property_long_description(property) or "") == "" then
			problem = name .. " has no tooltip"
		end
	end
	local filter = obs.obs_property_editable_list_filter(obs.obs_properties_get(props, "playlist")) or ""
	if not problem and (not filter:find("Still Images (*.jpg *.jpeg *.png *.webp *.bmp);;", 1, true) or
		not filter:find("*.flac *.jpg *.jpeg *.png *.webp *.bmp);;Video Files", 1, true)) then
		problem = "the Browse filter has no still images: " .. filter
	end
	problem = problem or opened_problem
	-- each option alone, so neither control can follow the other's option
	local function shown(mode, check_on, lock_on)
		local data = obs.obs_data_create()
		obs.obs_data_set_int(data, "container_mode", mode)
		obs.obs_data_set_bool(data, "folder_check_enabled", check_on)
		obs.obs_data_set_bool(data, "lock_list_at_start", lock_on)
		obs.obs_properties_apply_settings(props, data)
		obs.obs_data_release(data)
		return visible("newest_count"), enabled("folder_check_interval_s"), enabled("apply_waiting_changes")
	end
	if not problem then
		local newest_n_visible, interval_when_check_on, apply_when_lock_off = shown(2, true, false)
		local full_visible, interval_when_check_off, apply_when_lock_on = shown(0, false, true)
		if not newest_n_visible or full_visible then
			problem = "N is not shown in the newest N mode alone"
		elseif interval_when_check_off or not interval_when_check_on then
			problem = "the interval does not follow the folder check"
		elseif apply_when_lock_off or not apply_when_lock_on then
			problem = "the Apply button does not follow the list lock"
		end
	end
	obs.obs_properties_destroy(props)
	return problem
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
	if not MEDIA_FOLDER then
		error("PLAYLISTER_HARNESS_MEDIA_FOLDER must name the folder of the three files")
	end
	if not MISSING_FOLDER then
		error("PLAYLISTER_HARNESS_MISSING_FOLDER must name a folder that does not exist")
	end
	if not MEDIA_JUNCTION then
		error("PLAYLISTER_HARNESS_MEDIA_JUNCTION must name a junction to the media folder")
	end
	if not STILL then
		error("PLAYLISTER_HARNESS_STILL must name a valid jpg")
	end
	create_source("playlister-harness", function(settings)
		set_playlist(settings, { { FILES[1], "row-1" }, { FILES[2], "row-2" }, { FILES[3], "row-3" } })
		obs.obs_data_set_bool(settings, "loop", true)
	end)
	check("source created and shown", src ~= nil and item ~= nil)
	count_media_next_signals()
end)

step("first item plays", 1000, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	D[1] = dur()
	check("first item plays", st() == PLAYING, status())
	seek_near_end()
end)

step("auto-advance at end", 1000, 20000, function() return dur() > 0 and dur() ~= D[1] and st() == PLAYING end, function()
	D[2] = dur()
	check("the end of item 1 auto-advances to item 2", st() == PLAYING and D[2] ~= D[1] and tm() < 5000, status())
	obs.obs_source_media_next(src)
end)

step("next", 500, 15000, function() return dur() > 0 and dur() ~= D[2] and st() == PLAYING end, function()
	D[3] = dur()
	check("next goes to item 3", st() == PLAYING and D[3] ~= D[1] and D[3] ~= D[2], status())
	obs.obs_source_media_previous(src)
end)

step("previous", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("previous goes back to item 2", dur() == D[2] and st() == PLAYING, status())
	obs.obs_source_media_stop(src)
end)

step("stop", 4000, 4000, nil, function()
	check("stop stays stopped on the same item", st() == STOPPED and dur() == D[2], status())
	obs.obs_source_media_play_pause(src, false)
end)

step("play from stop", 500, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("play after stop reloads item 2 and plays", st() == PLAYING and dur() == D[2] and tm() < 5000, status())
	obs.obs_source_media_play_pause(src, true)
end)

-- the pause reaches the child a tick or two later, so the mark waits for the time to read the same twice in a row
step("pause", 1000, 5000, function()
	local now = tm()
	local settled = st() == PAUSED and mark.pause_poll == now
	mark.pause_poll = now
	return settled
end, function()
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

step("show", 500, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("show restarts item 2", st() == PLAYING and dur() == D[2] and tm() < 5000, status())
	update_settings(function(data)
		obs.obs_data_set_bool(data, "loop", false)
		obs.obs_data_set_int(data, "speed_percent", 100)
	end)
end)

step("loop off", 300, 300, nil, function()
	obs.obs_source_media_next(src)
end)

step("last item", 500, 15000, function() return dur() == D[3] and dur() > 0 end, function()
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
step("restart after end", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("restart after the end plays item 1", dur() == D[1] and st() == PLAYING, status())
	update_settings(function(data) obs.obs_data_set_bool(data, "loop", true) end)
end)

step("loop on", 300, 300, nil, function()
	obs.obs_source_media_previous(src)
end)

step("wrap backward", 500, 15000, function() return dur() == D[3] and dur() > 0 end, function()
	check("with loop on previous from item 1 wraps to item 3", dur() == D[3] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("wrap forward", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
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

step("show next", 500, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("stop/play-next behavior: show plays item 2", st() == PLAYING and dur() == D[2], status())
	update_settings(function(data) obs.obs_data_set_int(data, "visibility_behavior", 0) end)
end)

step("stop/restart behavior on", 300, 300, nil, function()
	obs.obs_source_media_next(src)
	obs.obs_source_media_play_pause(src, true)
end)

-- the pause waits for the new file to show progress (R-UP-79), so the step waits for it to land and settle
step("next then pause", 500, 15000, function()
	local now = tm()
	local settled = dur() == D[3] and st() == PAUSED and mark.next_pause_poll == now
	mark.next_pause_poll = now
	return settled
end, function()
	check("next then pause in a row: both happen, in order", dur() == D[3] and st() == PAUSED, status())
	mark.next_pause_time = tm()
	-- Properties, its two buttons and save take the list lock (R-THR-1); a self-deadlock hangs OBS here. The buttons
	-- and save write into the live settings, so a placeholder written there first shows that each one ran (the
	-- throttled save from video_tick writes the same keys, but never inside this one call).
	local props = obs.obs_source_properties(src)
	local count = obs.obs_property_list_item_count(obs.obs_properties_get(props, "select_file"))
	check("properties list the three items after No File Selected", count == 4, "entries=" .. count)
	-- R-UP-20: each item is stored by its identity, a file row as "<row uuid>:0:"
	check("select file stores the rows' identities", select_file_values() == "0 row-1:0: row-2:0: row-3:0:",
		select_file_values())
	local settings = obs.obs_source_get_settings(src)
	obs.obs_data_set_string(settings, "current_file_name", "placeholder")
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "refresh_filename"), src)
	check("refresh filename names item 3", current_file_name() == "3: " .. FILES[3], current_file_name())
	-- R-UP-65, R-UP-71: with nothing chosen yet (No File Selected, the default) the button does nothing; upstream
	-- split an empty value (a crash) and raised media_next anyway. The signal is raised inside the click.
	local before = media_next_count
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "play_selected"), src)
	check("play selected with nothing chosen raises no media_next", media_next_count == before,
		"media_next raised " .. (media_next_count - before) .. " times")
	obs.obs_properties_destroy(props)
	obs.obs_data_set_int(settings, "current_media_index", 99)
	obs.obs_source_save(src)
	local saved = obs.obs_data_get_int(settings, "current_media_index")
	obs.obs_data_release(settings)
	check("save writes the resume point, item 3", saved == 2, "current_media_index=" .. saved)
end)

step("nothing chosen", 1000, 1000, nil, function()
	check("play selected with nothing chosen leaves item 3 paused", dur() == D[3] and st() == PAUSED, status())
	-- R-UP-79: the pause sent right after the load holds; its time has not moved for a second
	check("a pause sent right after a load holds", tm() == mark.next_pause_time,
		status() .. " paused at " .. mark.next_pause_time)
	local props = obs.obs_source_properties(src)
	local settings = obs.obs_source_get_settings(src)
	-- an empty value, as a scene saved before anything was chosen can hold (R-UP-65's guard, not the default)
	obs.obs_data_set_string(settings, "select_file", "")
	local before = media_next_count
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "play_selected"), src)
	check("play selected with an empty choice raises no media_next", media_next_count == before,
		"media_next raised " .. (media_next_count - before) .. " times")
	obs.obs_data_release(settings)
	obs.obs_properties_destroy(props)
	-- R-UP-20: a position stored before Phase 2 (upstream's "1") is no identity, so it is reported, not guessed at
	before = media_next_count
	play_selected("1")
	check("play selected with a position from before Phase 2 is reported, not played", media_next_count == before and
		reports_stale_choice() and dur() == D[3], current_file_name() .. ", " .. status())
	before = media_next_count
	play_selected("row-1:0:")
	check("play selected with item 1 chosen raises media_next once", media_next_count == before + 1,
		"media_next raised " .. (media_next_count - before) .. " times")
end)

step("play selected", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
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

step("empty current folder", 1000, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("an empty current folder row moves on to the next row", st() == PLAYING and dur() == D[1], status())
	-- R-UP-66: an empty folder has nothing to select; upstream listed it as a file
	local entries = select_file_entries()
	check("select file lists the two files after No File Selected, not the empty folder", entries == 3,
		"entries=" .. entries)
	obs.obs_source_media_next(src)
end)

step("next after the empty folder", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("next goes on to the last row", dur() == D[2] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("wrap past the empty folder", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("with loop on next wraps past the empty folder row", dur() == D[1] and st() == PLAYING, status())
	obs.obs_source_media_next(src)
end)

step("last row again", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
	update_settings(function(data)
		obs.obs_data_set_bool(data, "loop", false)
		obs.obs_data_set_bool(data, "shuffle", true)
	end)
end)

step("shuffle on, loop off", 300, 300, nil, function()
	obs.obs_source_media_next(src)
end)

-- in list order the last row has no next with loop off, so only the shuffler can reach the other file
step("shuffle next from the last row", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("with shuffle on and loop off next from the last row plays the other file", dur() == D[1] and
		st() == PLAYING, status())
	-- one update turns shuffle off and removes the playing row, so the current row falls back to the empty folder
	-- (R-THR-4: the shuffler follows that list change while shuffle is off)
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", false)
		set_playlist(data, { { EMPTY_FOLDER, "empty" }, { FILES[2], "f2" } })
	end)
end)

step("playing row removed", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
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

step("playing row edited", 500, 15000, function() return dur() == D[3] and st() == PLAYING end, function()
	check("editing the playing row's path plays the new file", dur() == D[3] and st() == PLAYING, status())
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", true)
		obs.obs_data_set_bool(data, "loop", true)
	end)
end)

-- the file reports its duration before its time moves, so the mark waits until it has played a little
step("shuffle and loop on", 300, 5000, function() return tm() > 500 end, function()
	mark.before_next = tm()
	obs.obs_source_media_next(src)
end)

-- a smoke test of the shuffler with one item; in list order, next with loop on replays it too
step("one file left", 500, 15000, function()
	return dur() == D[3] and st() == PLAYING and tm() < mark.before_next
end, function()
	check("with one file left next replays it (shuffle and loop on)", dur() == D[3] and st() == PLAYING and
		tm() < mark.before_next, status() .. " before=" .. mark.before_next)
	-- the playing row's path becomes the empty folder, with a file after it: the child is cleared
	update_settings(function(data)
		set_playlist(data, { { "", "blank" }, { EMPTY_FOLDER, "empty" }, { EMPTY_FOLDER, "f2" }, { FILES[1], "f1" } })
	end)
end)

step("playing row emptied", 1500, 1500, nil, function()
	-- R-UP-78: nothing is loaded, but the child would still report the PLAYING of its last media
	check("a cleared source reports stopped while the list still has files", st() == STOPPED, status())
	obs.obs_source_media_next(src)
end)

step("next after a clear", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("next after a clear plays the file after the empty rows", dur() == D[1] and st() == PLAYING, status())
	remove_source()
end)

local function setting_string(key)
	local settings = obs.obs_source_get_settings(src)
	local value = obs.obs_data_get_string(settings, key)
	obs.obs_data_release(settings)
	return value
end

local function setting_int(key)
	local settings = obs.obs_source_get_settings(src)
	local value = obs.obs_data_get_int(settings, key)
	obs.obs_data_release(settings)
	return value
end

-- the uuid of playlist row i (counting from 0) in the source's settings
local function row_uuid(i)
	local settings = obs.obs_source_get_settings(src)
	local playlist = obs.obs_data_get_array(settings, "playlist")
	local row = obs.obs_data_array_item(playlist, i)
	local uuid = obs.obs_data_get_string(row, "uuid")
	obs.obs_data_release(row)
	obs.obs_data_array_release(playlist)
	obs.obs_data_release(settings)
	return uuid
end

-- "<kind> <path>" saved for a row in row_kinds (R-LIST-1), or "" when nothing is
local function row_kind(uuid)
	local settings = obs.obs_source_get_settings(src)
	local kinds = obs.obs_data_get_obj(settings, "row_kinds")
	local result = ""
	if kinds then
		local kind = obs.obs_data_get_obj(kinds, uuid)
		if kind then
			result = obs.obs_data_get_string(kind, "kind") .. " " .. obs.obs_data_get_string(kind, "path")
			obs.obs_data_release(kind)
		end
		obs.obs_data_release(kinds)
	end
	obs.obs_data_release(settings)
	return result
end

-- A third source with a folder row and settings saved before Phase 2 (schema 0): the migration turns the saved
-- file name into a key and the row index into the row's uuid (R-LIST-4), and playback resumes there (R-KEY-2). The
-- folder's name ends in ".mp4", so only the kind the migration saves keeps it a folder (R-LIST-1).
step("source with old settings", 1500, 1500, nil, function()
	create_source("playlister-harness-resume", function(settings)
		-- the row with an empty path is not counted: index 0 is the folder row (R-UP-77)
		set_playlist(settings, { { "", "blank" }, { MEDIA_JUNCTION, "media" } })
		-- name order, so the folder items are 1.mp4 to 3.mp4 (hard links share their files' dates)
		obs.obs_data_set_int(settings, "sort_key", 1)
		obs.obs_data_set_int(settings, "current_media_index", 0)
		obs.obs_data_set_string(settings, "current_folder_item_filename", "2.mp4")
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with a folder row and old settings created and shown", src ~= nil and item ~= nil)
	-- R-UP-80: OBS can save the scene collection before the source's first update; that save must leave the saved
	-- resume point and schema alone
	obs.obs_source_save(src)
end)

step("migrated resume", 1000, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("old settings resume on the saved folder item", dur() == D[2] and st() == PLAYING, status())
	check("the migration writes schema 1, the folded key and the row uuid", setting_int("schema_version") == 1 and
		setting_string("current_item_key") == "2.MP4" and setting_string("current_row_uuid") == "media",
		"schema " .. setting_int("schema_version") .. ", key " .. setting_string("current_item_key") .. ", row " ..
		setting_string("current_row_uuid"))
	check("the migration saves the kind of a folder named like a media file", row_kind("media") == "folder " ..
		MEDIA_JUNCTION, row_kind("media"))
	obs.obs_source_media_next(src)
end)

step("next in the folder", 500, 15000, function() return dur() == D[3] and st() == PLAYING end, function()
	obs.obs_source_save(src)
	check("save writes the playing folder item's key, occurrence and row", setting_string("current_item_key") ==
		"3.MP4" and setting_int("current_item_occurrence") == 1 and setting_string("current_row_uuid") == "media",
		"key " .. setting_string("current_item_key") .. ", row " .. setting_string("current_row_uuid"))
	-- a later update must not migrate again (it would turn the old file name back into the key)
	update_settings(function(data) obs.obs_data_set_bool(data, "loop", true) end)
end)

step("no second migration", 1000, 1000, nil, function()
	obs.obs_source_save(src)
	check("a later update leaves the resume key alone", setting_string("current_item_key") == "3.MP4" and
		dur() == D[3], "key " .. setting_string("current_item_key") .. ", " .. status())
	remove_source()
end)

-- as after a restart, with a row added above: the uuid finds the row although the saved index points elsewhere
step("restarted source", 1500, 1500, nil, function()
	create_source("playlister-harness-resume-again", function(settings)
		set_playlist(settings, { { FILES[1], "f1" }, { MEDIA_FOLDER, "media" } })
		-- name order, so the folder items are 1.mp4 to 3.mp4 (hard links share their files' dates)
		obs.obs_data_set_int(settings, "sort_key", 1)
		obs.obs_data_set_int(settings, "schema_version", 1)
		obs.obs_data_set_int(settings, "current_media_index", 0)
		obs.obs_data_set_string(settings, "current_row_uuid", "media")
		obs.obs_data_set_string(settings, "current_item_key", "3.MP4")
		obs.obs_data_set_int(settings, "current_item_occurrence", 1)
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("restarted source created and shown", src ~= nil and item ~= nil)
	count_media_next_signals()
end)

step("resume by uuid and key", 1000, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("the row uuid and item key resume the folder item, not the row at the saved index", dur() == D[3] and
		st() == PLAYING, status())
	-- R-LIST-1: a folder row's kind is saved with its path; a file row's is not
	check("row kinds keep the folder row with its path and not the file row",
		row_kind("media") == "folder " .. MEDIA_FOLDER and row_kind("f1") == "",
		"media: " .. row_kind("media") .. "; f1: " .. row_kind("f1"))
	-- R-UP-20: a folder item is stored by its row, occurrence and key
	check("select file stores the folder items' identities",
		select_file_values() == "0 f1:0: media:1:1.MP4 media:1:2.MP4 media:1:3.MP4", select_file_values())
	-- an occurrence that is not there is not swapped for another item of its key, and a folder row itself is no
	-- item; neither plays or moves to another item
	local before = media_next_count
	play_selected("media:2:2.MP4")
	check("play selected with an item that is not there is reported, not played", reports_stale_choice() and
		dur() == D[3] and media_next_count == before, current_file_name() .. ", " .. status() .. ", media_next " ..
		(media_next_count - before))
	play_selected("media:0:")
	check("play selected with a folder row's own identity is reported, not played", reports_stale_choice() and
		dur() == D[3] and media_next_count == before, current_file_name() .. ", " .. status() .. ", media_next " ..
		(media_next_count - before))
	check("a stale choice resets Select File to No File Selected", setting_string("select_file") == "0",
		setting_string("select_file"))
end)

step("stale choice", 1000, 1000, nil, function()
	check("a stale choice leaves the playing item alone", dur() == D[3] and st() == PLAYING, status())
	play_selected("media:1:2.MP4")
end)

step("folder item selected", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("play selected plays the folder item it names", dur() == D[2] and st() == PLAYING, status())
	check("current file names that folder item", current_file_name() == "2-2: " .. MEDIA_FOLDER .. "/2.mp4",
		current_file_name())
	-- the folder changes while an item plays (R-DIR-8): a file that is not playing is renamed so it sorts first
	mark.before_rename = tm()
	local ok, err = os.rename(MEDIA_FOLDER .. "/3.mp4", MEDIA_FOLDER .. "/0.mp4")
	check("a file in the watched folder renamed", ok == true, tostring(err))
end)

step("renamed while playing", 500, 15000, function()
	return select_file_values():find("media:1:0.MP4", 1, true) ~= nil
end, function()
	check("the rename reaches the list, in name order",
		select_file_values() == "0 f1:0: media:1:0.MP4 media:1:1.MP4 media:1:2.MP4", select_file_values())
	check("the playing item keeps playing through the change, not restarted", dur() == D[2] and st() == PLAYING and
		tm() > mark.before_rename, status() .. " before=" .. mark.before_rename)
	-- the Current File text is written on updates and button presses only (upstream's note), so Refresh first
	local props = obs.obs_source_properties(src)
	obs.obs_property_button_clicked(obs.obs_properties_get(props, "refresh_filename"), src)
	obs.obs_properties_destroy(props)
	check("current file names the playing item at its new position",
		current_file_name() == "2-3: " .. MEDIA_FOLDER .. "/2.mp4", current_file_name())
	os.rename(MEDIA_FOLDER .. "/0.mp4", MEDIA_FOLDER .. "/3.mp4")
	-- a new file joins once the write-complete gate admits it (R-DIR-4); a copy the harness owns, as the hard links
	-- are the user's own files
	copy_file(FILES[1], MEDIA_FOLDER .. "/9.mp4")
	mark.before_add = tm()
end)

step("file added while playing", 500, 15000, function()
	return select_file_values():find("media:1:9.MP4", 1, true) ~= nil
end, function()
	check("a file added to the watched folder joins the list",
		select_file_values() == "0 f1:0: media:1:1.MP4 media:1:2.MP4 media:1:3.MP4 media:1:9.MP4",
		select_file_values())
	check("the playing item keeps playing while the file joins", dur() == D[2] and st() == PLAYING and
		tm() > mark.before_add, status() .. " before=" .. mark.before_add)
	-- loop is on by default, as upstream; off, the list ends after the folder's last item
	update_settings(function(data) obs.obs_data_set_bool(data, "loop", false) end)
	play_selected("media:1:3.MP4")
end)

-- R-DIR-4: the file after the playing one is rewritten in place; while its new version is gated it keeps its place
-- but is not loaded, so Next passes it (and with loop off nothing follows)
step("third folder item", 500, 15000, function() return dur() == D[3] and st() == PLAYING end, function()
	local output = io.open(MEDIA_FOLDER .. "/9.mp4", "ab")
	output:write(string.rep("\0", 4096))
	output:close()
end)

step("rewrite pending", 1200, 1200, nil, function()
	obs.obs_source_media_next(src)
end)

step("next passes the rewrite", 800, 800, nil, function()
	check("next passes a file still being rewritten", dur() == D[3] and st() == PLAYING and
		refreshed_file_name() == "2-3: " .. MEDIA_FOLDER .. "/3.mp4", refreshed_file_name() .. ", " .. status())
end)

step("rewrite admitted", 3000, 3000, nil, function()
	obs.obs_source_media_next(src)
end)

step("next plays the rewritten file", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("once admitted the rewritten file plays", refreshed_file_name() == "2-4: " .. MEDIA_FOLDER .. "/9.mp4",
		refreshed_file_name())
	seek_near_end()
end)

-- the list ends on the folder's last item (loop off); the item stays loaded while the folder changes
step("list ended on a folder item", 500, 15000, function() return st() == ENDED end, function()
	copy_file(FILES[3], MEDIA_FOLDER .. "/8.mp4")
end)

step("file added after the end", 500, 15000, function()
	return select_file_values():find("media:1:8.MP4", 1, true) ~= nil
end, function()
	-- save writes the ended item's name; it must still point at a live item after the folder's new listing
	obs.obs_source_save(src)
	check("after the list ended, a folder change keeps the ended item and its state",
		current_file_name() == "2-5: " .. MEDIA_FOLDER .. "/9.mp4" and st() == ENDED,
		current_file_name() .. ", " .. status())
	remove_source()
end)

-- A fourth source whose first row was saved as a folder that is gone now (R-LIST-1, rule 1). It stays a folder with
-- nothing to play, so playback moves on to the next row and its path is never handed to the child as a file
-- (run-harness.ps1 fails the run if it is); without the saved kind it would be a file row. The second row was saved
-- without a uuid and is given one, so Select File can name it.
-- R-DIR-8: a newest-only row whose playing file is no longer the newest; the newest now is 9.mp4, the copy the
-- harness rewrote above (a copy of the first file). A newer copy arrives while 9.mp4 plays: 9.mp4 plays to its end
-- from the source's own copy of its entry, and then the newer file plays.
step("source with a newest-only folder", 1500, 1500, nil, function()
	-- 8.mp4, copied in after the rewrite, would be newer still; no source plays it
	os.remove(MEDIA_FOLDER .. "/8.mp4")
	create_source("playlister-harness-newest", function(settings)
		set_playlist(settings, { { MEDIA_FOLDER, "media" } })
		obs.obs_data_set_int(settings, "container_mode", 1)
		obs.obs_data_set_bool(settings, "loop", false)
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with a newest-only folder created and shown", src ~= nil and item ~= nil)
end)

step("newest plays", 500, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("newest only plays the newest file alone", dur() == D[1] and select_file_values() == "0 media:1:9.MP4",
		select_file_values() .. ", " .. status())
	copy_file(FILES[2], MEDIA_FOLDER .. "/7.mp4")
	mark.before_newer = tm()
end)

step("a newer file arrives", 500, 15000, function()
	return select_file_values() == "0 media:1:7.MP4"
end, function()
	check("the newer file replaces it in the list while it keeps playing", select_file_values() == "0 media:1:7.MP4" and
		dur() == D[1] and st() == PLAYING and tm() > mark.before_newer,
		select_file_values() .. ", " .. status() .. " before=" .. mark.before_newer)
	mark.before_restart = tm()
	obs.obs_source_media_restart(src)
end)

-- Restart (current file, the default) starts the evicted item again, not the newer one
step("evicted item restarted", 500, 15000, function()
	return st() == PLAYING and dur() == D[1] and tm() < mark.before_restart
end, function()
	check("restart starts the evicted item again", st() == PLAYING and dur() == D[1] and tm() < mark.before_restart,
		status() .. " before=" .. mark.before_restart)
	check("current file names the evicted item by its row alone", refreshed_file_name() == "1: " .. MEDIA_FOLDER ..
		"/9.mp4", refreshed_file_name())
	seek_near_end()
end)

step("evicted item ended", 500, 15000, function() return dur() == D[2] and st() == PLAYING end, function()
	check("after the evicted item's end, the newest file plays", refreshed_file_name() == "1-1: " .. MEDIA_FOLDER ..
		"/7.mp4", refreshed_file_name())
	remove_source()
end)

-- R-DIR-11: with "Lock list at start" on, a new file in the folder waits until "Apply waiting changes now" is pressed,
-- and the playing item plays on through it
step("source with a locked list", 1500, 1500, function() return true end, function()
	create_source("playlister-harness-locked", function(settings)
		set_playlist(settings, { { MEDIA_FOLDER, "media" } })
		-- name order, so 1.mp4, the first file (longer than this flow), plays throughout
		obs.obs_data_set_int(settings, "sort_key", 1)
		obs.obs_data_set_bool(settings, "lock_list_at_start", true)
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with a locked list created and shown", src ~= nil and item ~= nil)
end)

step("locked list plays", 500, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	local problem = folder_options_problem()
	check("properties show the folder options as one labeled group", problem == nil, problem)
	mark.locked_values = select_file_values()
	copy_file(FILES[3], MEDIA_FOLDER .. "/6.mp4")
end)

-- longer than the 500 ms debounce and the 2 s write-complete wait together
step("a new file waits", 5000, 5000, nil, function()
	check("with the list locked, the new file waits", select_file_values() == mark.locked_values and st() == PLAYING,
		select_file_values() .. ", " .. status())
	mark.before_apply = tm()
	mark.duration_before_apply = dur()
	press("apply_waiting_changes")
end)

local function lists_new_file()
	return select_file_values():find(" media:1:6.MP4", 1, true) ~= nil
end
step("waiting changes applied", 500, 10000, lists_new_file, function()
	check("apply waiting changes now adds the new file while the item plays on", lists_new_file() and
		st() == PLAYING and dur() == mark.duration_before_apply and tm() > mark.before_apply,
		select_file_values() .. ", " .. status() .. " before=" .. mark.before_apply)
	remove_source()
end)

-- the copies go once the child that may hold the last one open is released; tried until they are gone
local COPIES = { "6.mp4", "7.mp4", "8.mp4", "9.mp4" }
local function copies_left()
	for _, name in ipairs(COPIES) do
		if exists(MEDIA_FOLDER .. "/" .. name) then
			return true
		end
	end
	return false
end
step("copies removed", 0, 10000, function()
	for _, name in ipairs(COPIES) do
		os.remove(MEDIA_FOLDER .. "/" .. name)
	end
	return not copies_left()
end, function()
	check("the harness copies in the watched folder are removed", not copies_left())
end)

step("source with an absent folder row", 1500, 1500, nil, function()
	create_source("playlister-harness-absent", function(settings)
		set_playlist(settings, { { MISSING_FOLDER, "gone" }, { FILES[2] } })
		local kinds = obs.obs_data_create()
		local kind = obs.obs_data_create()
		obs.obs_data_set_string(kind, "kind", "folder")
		obs.obs_data_set_string(kind, "path", MISSING_FOLDER)
		obs.obs_data_set_obj(kinds, "gone", kind)
		obs.obs_data_set_obj(settings, "row_kinds", kinds)
		obs.obs_data_release(kind)
		obs.obs_data_release(kinds)
		obs.obs_data_set_int(settings, "schema_version", 1)
		obs.obs_data_set_string(settings, "current_row_uuid", "gone")
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with an absent folder row created and shown", src ~= nil and item ~= nil)
end)

step("absent folder passed over", 1000, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("a row saved as a folder that is gone moves on to the next row", dur() == D[2] and st() == PLAYING,
		status())
	check("the absent folder keeps its saved kind", row_kind("gone") == "folder " .. MISSING_FOLDER, row_kind("gone"))
	local uuid = row_uuid(1)
	check("a row saved without a uuid is given one", uuid ~= "" and uuid ~= "gone", uuid)
	check("select file lists the file row only, by its new uuid", select_file_values() == "0 " .. uuid .. ":0:",
		select_file_values())
	play_selected(uuid .. ":0:")
	check("play selected plays that row", not reports_stale_choice() and current_file_name() == "2: " .. FILES[2],
		current_file_name())
	remove_source()
end)

-- R-BEH-5: entries that cannot play are skipped: a missing file ends at once, and so does a file that is no media
step("source with failing rows", 1500, 1500, nil, function()
	create_source("playlister-harness-failing", function(settings)
		set_playlist(settings, { { MISSING_FILE, "m" }, { CORRUPT_FILE, "c" }, { FILES[1], "f1" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with failing rows created and shown", src ~= nil and item ~= nil)
end)

step("failing rows skipped", 500, 15000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("a missing file and a corrupt one are skipped for the file after them", dur() == D[1] and st() == PLAYING,
		status())
	remove_source()
end)

-- with nothing that can play, auto-advance stops instead of cycling (loop is on); Play tries once more
step("source with only failing rows", 1500, 1500, nil, function()
	create_source("playlister-harness-all-failing", function(settings)
		set_playlist(settings, { { MISSING_FILE, "m" }, { CORRUPT_FILE, "c" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with only failing rows created and shown", src ~= nil and item ~= nil)
end)

step("everything failed", 3000, 3000, nil, function()
	check("a list where nothing plays stops instead of cycling", st() == STOPPED, status())
	obs.obs_source_media_play_pause(src, false)
end)

step("play after everything failed", 3000, 3000, nil, function()
	check("play tries the failed entries once more and stops again", st() == STOPPED, status())
	remove_source()
end)

-- a still ends at once by nature, so a corrupt one fails by its startup window (5 s), not by its end; until then it
-- is not advanced past at tick rate (loop is on, and it is the only row)
step("source with a corrupt still", 1500, 1500, nil, function()
	create_source("playlister-harness-corrupt-still", function(settings)
		set_playlist(settings, { { CORRUPT_STILL, "s" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with a corrupt still created and shown", src ~= nil and item ~= nil)
end)

step("corrupt still failed", 7000, 7000, nil, function()
	check("a corrupt still fails by its startup window and the list stops", st() == STOPPED, status())
	remove_source()
end)

-- R-DIR-9: a still is held for the still duration (4 s here) with the source's own state, duration and time; a seek
-- inside it is ignored, a pause freezes the hold, and then playback moves on
step("source with a still", 1500, 1500, nil, function()
	create_source("playlister-harness-still", function(settings)
		set_playlist(settings, { { STILL, "still" }, { FILES[1], "f1" } })
		obs.obs_data_set_int(settings, "still_duration_s", 4)
		obs.obs_data_set_bool(settings, "loop", false)
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	check("source with a still created and shown", src ~= nil and item ~= nil)
end)

step("still held", 300, 10000, function() return st() == PLAYING and dur() == 4000 end, function()
	check("a still is held: playing, its duration the hold, on screen", st() == PLAYING and dur() == 4000 and
		obs.obs_source_get_width(src) == 320, status() .. " width=" .. obs.obs_source_get_width(src))
	obs.obs_source_media_set_time(src, 3500)
end)

step("seek in a still", 300, 300, nil, function()
	check("a seek inside a still is ignored", dur() == 4000 and tm() < 3000, status())
	obs.obs_source_media_play_pause(src, true)
end)

step("still paused", 1000, 1000, nil, function()
	check("a held still pauses", st() == PAUSED and dur() == 4000, status())
	mark.still_paused_at = tm()
end)

step("still stays paused", 1000, 1000, nil, function()
	check("a paused hold does not run", st() == PAUSED and tm() == mark.still_paused_at,
		status() .. " paused at " .. mark.still_paused_at)
	obs.obs_source_media_play_pause(src, false)
end)

step("still resumed", 500, 500, nil, function()
	check("the hold runs again after unpause", st() == PLAYING and tm() > mark.still_paused_at, status())
	obs.obs_source_media_stop(src)
end)

-- a stop ends the hold: nothing moves on by itself, however long; Play holds the still again from the start
step("still stopped", 4500, 4500, nil, function()
	check("a stopped still stays stopped: the hold ended, nothing moved on", st() == STOPPED, status())
	obs.obs_source_media_play_pause(src, false)
end)

step("still played again", 300, 10000, function() return st() == PLAYING and dur() == 4000 end, function()
	check("play after a stop holds the still again from the start", st() == PLAYING and dur() == 4000 and
		tm() < 2000, status())
end)

step("hold over", 500, 10000, function() return dur() == D[1] and st() == PLAYING end, function()
	check("after the hold, playback moves on to the next row", dur() == D[1] and st() == PLAYING, status())
	remove_source()
end)

step("removed", 1500, 1500, nil, function()
	os.remove(CORRUPT_FILE)
	os.remove(CORRUPT_STILL)
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
	for _, path in ipairs({ CORRUPT_FILE, CORRUPT_STILL }) do
		local corrupt = assert(io.open(path, "wb"))
		corrupt:write(string.rep("not media ", 2000))
		corrupt:close()
	end
	os.remove(MISSING_FILE)
	step_start = now_ms()
	obs.timer_add(tick, 100)
end
