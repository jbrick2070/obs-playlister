-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Phase 2 step 6 spike (PLAN section 4, R-BEH-5, R-DIR-9): drives a plain Media Source (ffmpeg_source) through the
-- cases the player has to tell apart, and logs, against the moment of each update, every media signal it raises and
-- every change of its state, time, duration and video size. Run it with
--   run-harness.ps1 -Script spike-child-signals.lua ...
-- with PLAYLISTER_SPIKE_JPG naming a valid JPEG. It checks nothing; TESTING.md records what it shows.
obs = obslua

local FILES = {}
for path in string.gmatch(os.getenv("PLAYLISTER_HARNESS_FILES") or "", "[^|]+") do
	table.insert(FILES, path)
end
local JPG = os.getenv("PLAYLISTER_SPIKE_JPG")
local TEMP = (os.getenv("TEMP") or "."):gsub("\\", "/")
local CORRUPT_MP4 = TEMP .. "/playlister-spike-corrupt.mp4"
local CORRUPT_JPG = TEMP .. "/playlister-spike-corrupt.jpg"
local MISSING = TEMP .. "/playlister-spike-missing.mp4"

local src, item = nil, nil
local t0 = 0
local last = ""
local cases = {}
local index, case_start = 0, 0

local function now_ms()
	return obs.os_gettime_ns() / 1000000
end

local function log(s)
	obs.script_log(obs.LOG_INFO, "HARNESS SPIKE " .. s)
end

local SIGNALS = { "media_play", "media_pause", "media_restart", "media_stopped", "media_next", "media_previous",
	"media_started", "media_ended", "update" }
local handlers = {}
for _, name in ipairs(SIGNALS) do
	handlers[name] = function(cd)
		log(string.format("  +%d ms signal %s", now_ms() - t0, name))
	end
end

local function observe()
	local s = string.format("state=%d time=%d duration=%d size=%dx%d", obs.obs_source_media_get_state(src),
		obs.obs_source_media_get_time(src), obs.obs_source_media_get_duration(src), obs.obs_source_get_width(src),
		obs.obs_source_get_height(src))
	if s ~= last then
		log(string.format("  +%d ms %s", now_ms() - t0, s))
		last = s
	end
end

local function write_garbage(path)
	local file = io.open(path, "wb")
	local bytes = {}
	for i = 1, 20000 do
		bytes[i] = string.char((i * 7919) % 256)
	end
	file:write(table.concat(bytes))
	file:close()
end

local function local_file(path, clear_on_end)
	return function(data)
		obs.obs_data_set_bool(data, "is_local_file", true)
		obs.obs_data_set_string(data, "local_file", path)
		obs.obs_data_set_bool(data, "clear_on_media_end", clear_on_end ~= false)
	end
end

local function url(address)
	return function(data)
		obs.obs_data_set_bool(data, "is_local_file", false)
		obs.obs_data_set_string(data, "input", address)
	end
end

local function case(name, duration_ms, fill, action)
	table.insert(cases, { name = name, duration = duration_ms, fill = fill, action = action })
end

local function start_case(c)
	log("case " .. c.name)
	t0 = now_ms()
	last = ""
	if c.fill then
		local data = obs.obs_data_create()
		c.fill(data)
		obs.obs_source_update(src, data)
		obs.obs_data_release(data)
		log(string.format("  +%d ms obs_source_update returned", now_ms() - t0))
	end
	if c.action then
		c.action()
	end
	observe()
end

case("valid mp4", 3000, local_file(FILES[3]))
case("missing file", 3000, local_file(MISSING))
case("corrupt mp4", 3000, local_file(CORRUPT_MP4))
case("valid jpg (clear_on_media_end on, the default)", 3000, local_file(JPG))
case("empty path after the jpg (does the size go to 0?)", 2000, local_file(""))
case("valid jpg (clear_on_media_end off)", 3000, local_file(JPG, false))
case("empty path after the jpg kept on screen", 2000, local_file(""))
case("valid jpg again (clear_on_media_end off)", 2000, local_file(JPG, false))
case("media stop after the kept jpg", 2000, nil, function() obs.obs_source_media_stop(src) end)
case("corrupt jpg", 3000, local_file(CORRUPT_JPG))
case("dead URL (nothing listens on port 9)", 16000, url("http://127.0.0.1:9/playlister-spike.mp4"))
case("valid mp4 after the URL", 3000, local_file(FILES[3]))

local function create_child()
	local data = obs.obs_data_create()
	src = obs.obs_source_create("ffmpeg_source", "playlister-spike-child", data, nil)
	obs.obs_data_release(data)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), src)
	obs.obs_source_release(scene_source)
	-- obslua connects handlers later, on its own thread; the first case waits 1.5 s
	for _, name in ipairs(SIGNALS) do
		obs.signal_handler_connect(obs.obs_source_get_signal_handler(src), name, handlers[name])
	end
end

local function tick()
	if index == 0 then
		if not src then
			if now_ms() - case_start >= 1000 then
				create_child()
				case_start = now_ms()
			end
			return
		end
		if now_ms() - case_start < 1500 then
			return
		end
		index = 1
		case_start = now_ms()
		start_case(cases[index])
		return
	end
	observe()
	if now_ms() - case_start < cases[index].duration then
		return
	end
	index = index + 1
	if index > #cases then
		obs.timer_remove(tick)
		for _, name in ipairs(SIGNALS) do
			obs.signal_handler_disconnect(obs.obs_source_get_signal_handler(src), name, handlers[name])
		end
		obs.obs_sceneitem_remove(item)
		obs.obs_source_release(src)
		os.remove(CORRUPT_MP4)
		os.remove(CORRUPT_JPG)
		log("finished")
		obs.script_log(obs.LOG_INFO, "HARNESS DONE")
		return
	end
	case_start = now_ms()
	start_case(cases[index])
end

function script_description()
	return "obs-playlister child signal spike"
end

function script_load(settings)
	write_garbage(CORRUPT_MP4)
	write_garbage(CORRUPT_JPG)
	case_start = now_ms()
	obs.timer_add(tick, 50)
end
