-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Runtime harness for OBS 33: drives one OBS Playlister source through the transport paths and logs
-- "HARNESS PASS|FAIL <check>" lines. Run it with run-harness.ps1, which passes three media files of different
-- durations in PLAYLISTER_HARNESS_FILES (separated by "|"); items are told apart by their durations. Each step
-- waits for its condition (or its timeout), checks the result of the previous action, then performs the next one.
obs = obslua

local FILES = {}
for path in string.gmatch(os.getenv("PLAYLISTER_HARNESS_FILES") or "", "[^|]+") do
	table.insert(FILES, path)
end
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

local function update_settings(fn)
	local data = obs.obs_data_create()
	fn(data)
	obs.obs_source_update(src, data)
	obs.obs_data_release(data)
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
	local settings = obs.obs_data_create()
	local playlist = obs.obs_data_array_create()
	for i, path in ipairs(FILES) do
		local row = obs.obs_data_create()
		obs.obs_data_set_string(row, "value", path)
		obs.obs_data_set_string(row, "uuid", "harness-row-" .. i)
		obs.obs_data_array_push_back(playlist, row)
		obs.obs_data_release(row)
	end
	obs.obs_data_set_array(settings, "playlist", playlist)
	obs.obs_data_set_bool(settings, "loop", true)
	obs.obs_data_array_release(playlist)
	src = obs.obs_source_create("obs_playlister_source", "playlister-harness", settings, nil)
	obs.obs_data_release(settings)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), src)
	obs.obs_source_release(scene_source)
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

step("end of list", 1000, 20000, function() return st() == ENDED or dur() ~= D[3] end, function()
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
	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(src)
	src = nil
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
