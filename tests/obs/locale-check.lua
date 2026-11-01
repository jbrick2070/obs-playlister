-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Translations in OBS (R-I18N-1, R-I18N-5; TESTING.md, "Translations"): with OBS running in another language, a
-- Playlister's Properties shows that language's text, both a label the source sets (Play Selected File) and a message
-- built in the providers through pl_text (a playlist file that does not exist). Run it with
--   run-harness.ps1 -Script locale-check.lua -Language de-DE
-- The runner sets OBS's language for the run and names its language file in PLAYLISTER_LOCALE_FILE; this script reads
-- the expected texts from that file (UTF-8), as os.getenv on Windows would garble them. It logs
-- "HARNESS PASS|FAIL <check>" and "HARNESS DONE" when finished.

obs = obslua

local TEMP = (os.getenv("TEMP") or "."):gsub("\\", "/")
local MISSING_LIST = TEMP .. "/playlister-locale-missing.m3u"

-- key -> text of an OBS locale file
local function read_locale(path)
	local texts = {}
	local file = path and io.open(path, "rb")
	if not file then
		return texts
	end
	for line in file:lines() do
		local key, text = line:match('^([^#=%s][^=]-)%s*=%s*"(.*)"%s*\r?$')
		if key then
			texts[key] = text
		end
	end
	file:close()
	return texts
end

local EXPECTED = read_locale(os.getenv("PLAYLISTER_LOCALE_FILE"))
local EXPECT_PLAY_SELECTED = EXPECTED["PlaySelectedFile"] or ""
local EXPECT_KIND_PLAYLIST = EXPECTED["RowKind.Playlist"] or ""
local EXPECT_NOT_FOUND = EXPECTED["RowError.PlaylistNotFound"] or ""

local src = nil
local item = nil
local started = 0

local function log(s)
	obs.script_log(obs.LOG_INFO, "HARNESS " .. s)
end

local function check(name, ok, detail)
	log((ok and "PASS " or "FAIL ") .. name .. " (" .. (detail or "") .. ")")
end

local function now_ms()
	return obs.os_gettime_ns() / 1000000
end

local function create_source()
	local settings = obs.obs_data_create()
	local playlist = obs.obs_data_array_create()
	local row = obs.obs_data_create()
	obs.obs_data_set_string(row, "value", MISSING_LIST)
	obs.obs_data_set_string(row, "uuid", "row-1")
	obs.obs_data_array_push_back(playlist, row)
	obs.obs_data_release(row)
	obs.obs_data_set_array(settings, "playlist", playlist)
	obs.obs_data_array_release(playlist)
	src = obs.obs_source_create("obs_playlister_source", "playlister-locale", settings, nil)
	obs.obs_data_release(settings)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), src)
	obs.obs_source_release(scene_source)
end

local function finish()
	local props = obs.obs_source_properties(src)
	local button = obs.obs_properties_get(props, "play_selected")
	local label = button and obs.obs_property_description(button) or ""
	local block = obs.obs_properties_get(props, "row_status")
	local status = block and obs.obs_property_long_description(block) or ""
	local version_line = obs.obs_properties_get(props, "plugin_version")
	local version = version_line and obs.obs_property_description(version_line) or ""
	obs.obs_properties_destroy(props)

	check("the expected texts came from the language file", EXPECT_PLAY_SELECTED ~= "" and EXPECT_KIND_PLAYLIST ~= "" and
		EXPECT_NOT_FOUND ~= "", EXPECT_PLAY_SELECTED .. " | " .. EXPECT_KIND_PLAYLIST .. " | " .. EXPECT_NOT_FOUND)
	check("a label the source sets is in the language OBS runs in", label == EXPECT_PLAY_SELECTED, label)
	-- "1: kind", as the kind alone is often part of the not-found message
	check("the row kind in the status block is in that language",
		status:find("1: " .. EXPECT_KIND_PLAYLIST, 1, true) ~= nil, status)
	check("a message built in a provider (pl_text) is in that language",
		status:find(EXPECT_NOT_FOUND, 1, true) ~= nil, status)
	check("the version line names Playlister", version:match("^Playlister %d+%.%d+%.%d+") ~= nil, version)

	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(src)
	src, item = nil, nil
	log("DONE")
end

local function tick()
	if not src then
		create_source()
		started = now_ms()
		return
	end
	-- the list provider reports the missing file on its first read, at once; 3 s is ample
	if now_ms() - started >= 3000 then
		obs.timer_remove(tick)
		finish()
	end
end

function script_description()
	return "obs-playlister translations check"
end

function script_load(settings)
	os.remove(MISSING_LIST)
	obs.timer_add(tick, 100)
end
