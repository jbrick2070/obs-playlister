-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Phase 3a acceptance (TESTING.md, "Phase 3a"): playlist files and web addresses in OBS. Run it with
--   run-harness.ps1 -Script phase3a-acceptance.lua -TimeoutSeconds 900
-- The runner makes a server root (PLAYLISTER_HARNESS_SERVER_ROOT): a copy of tests/fixtures/playlists and hard links
-- to the three files in media/clip-1.mp4 to clip-3.mp4. It serves that root with tests/manual/serve-fixtures.ps1
-- twice: lists on PLAYLISTER_HARNESS_LIST_SERVER and media (/media/1 to /media/3) on
-- PLAYLISTER_HARNESS_MEDIA_SERVER, each logging its requests (PLAYLISTER_HARNESS_LIST_LOG, _MEDIA_LOG). This script
-- writes its own lists into the root and edits them there; the runner removes the root. Items 3.8, 3.9 with a real
-- feed, 3.16 and 3.17 need the internet, a certificate or eyes, and stay manual. It logs "HARNESS PASS|FAIL <check>"
-- lines like the runtime harness, whose helpers it repeats.
obs = obslua

local ROOT = os.getenv("PLAYLISTER_HARNESS_SERVER_ROOT")
local LIST = os.getenv("PLAYLISTER_HARNESS_LIST_SERVER")
local MEDIA = os.getenv("PLAYLISTER_HARNESS_MEDIA_SERVER")
local MEDIA_LOG = os.getenv("PLAYLISTER_HARNESS_MEDIA_LOG")
local PLAYING = 1
local src, item = nil, nil
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
local function tm()
	return obs.obs_source_media_get_time(src)
end
local function status()
	return string.format("state=%d time=%d duration=%d", st(), tm(), obs.obs_source_media_get_duration(src))
end

local function update_settings(fn)
	local data = obs.obs_data_create()
	fn(data)
	obs.obs_source_update(src, data)
	obs.obs_data_release(data)
end

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

local function create_source(name, rows, fill)
	local settings = obs.obs_data_create()
	set_playlist(settings, rows)
	obs.obs_data_set_int(settings, "visibility_behavior", 2)
	if fill then
		fill(settings)
	end
	src = obs.obs_source_create("obs_playlister_source", name, settings, nil)
	obs.obs_data_release(settings)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), src)
	obs.obs_source_release(scene_source)
	mark.created = now_ms()
end

local function remove_source()
	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(src)
	src, item = nil, nil
end

-- the names Select File shows, in list order (No File Selected left out)
local function listed_names()
	local props = obs.obs_source_properties(src)
	local list = obs.obs_properties_get(props, "select_file")
	local names = {}
	for i = 1, obs.obs_property_list_item_count(list) - 1 do
		table.insert(names, obs.obs_property_list_item_name(list, i))
	end
	obs.obs_properties_destroy(props)
	return names
end

-- the status block (R-LIST-5) as Properties shows it, and its info type (0 normal, 1 warning, 2 error)
local function status_block()
	local props = obs.obs_source_properties(src)
	local block = obs.obs_properties_get(props, "row_status")
	local text = block and obs.obs_property_long_description(block) or ""
	local info_type = block and obs.obs_property_text_info_type(block) or -1
	obs.obs_properties_destroy(props)
	return text, info_type
end

-- line n of the status block, "" when there is none
local function row_line(n)
	local i = 0
	for line in ((status_block()) .. "\n"):gmatch("([^\n]*)\n") do
		i = i + 1
		if i == n then
			return line
		end
	end
	return ""
end

local function has(text, part)
	return text:find(part, 1, true) ~= nil
end

local function press(name)
	local props = obs.obs_source_properties(src)
	obs.obs_property_button_clicked(obs.obs_properties_get(props, name), src)
	obs.obs_properties_destroy(props)
end

-- the Current File text after Refresh Filename
local function current_file()
	press("refresh_filename")
	local settings = obs.obs_source_get_settings(src)
	local name = obs.obs_data_get_string(settings, "current_file_name")
	obs.obs_data_release(settings)
	return name
end

local function write_file(path, text)
	local file = io.open(path, "wb")
	file:write(text)
	file:close()
end

local function append_file(path, text)
	local file = io.open(path, "ab")
	file:write(text)
	file:close()
end

local function read_file(path)
	local file = io.open(path, "rb")
	if not file then
		return ""
	end
	local text = file:read("*a")
	file:close()
	return text
end

-- the names in order, each found once, after the one before it
local function in_order(names, parts)
	local at = 0
	for _, part in ipairs(parts) do
		local found = nil
		for i = at + 1, #names do
			if has(names[i], part) then
				found = i
				break
			end
		end
		if not found then
			return false
		end
		at = found
	end
	return true
end

-- a file: URL for a path in the root, with the characters a URL must not hold encoded
local function file_url(path)
	return "file:///" .. path:gsub("[^%w/:%-%._~]", function(c)
		return string.format("%%%02X", string.byte(c))
	end)
end

local function playing_on()
	return st() == PLAYING and tm() > 1000
end

local function step(name, min_wait, timeout, ready, run)
	table.insert(steps, { name = name, min_wait = min_wait, timeout = timeout, ready = ready, run = run })
end

-- 3.1: a local .m3u with relative names (either separator) and one #EXTINF title, as a row
step("local playlist", 1000, 1000, nil, function()
	write_file(ROOT .. "/local.m3u",
		"#EXTM3U\r\n#EXTINF:-1,First Title\r\nmedia/clip-1.mp4\r\nmedia\\clip-2.mp4\r\nmedia/clip-3.mp4\r\n")
	create_source("acceptance-local-m3u", { { ROOT .. "/local.m3u", "local" } })
end)

step("local playlist listed", 0, 15000, function() return has(row_line(1), "3 files") and playing_on() end, function()
	check("3.1 a local .m3u is a playlist row of 3 files, in list order by date sort",
		row_line(1) == "1: playlist - 3 files; no dates; list order", row_line(1))
	local names = listed_names()
	check("3.1 Select File lists the entries in list order", #names == 3 and
		in_order(names, { "clip-1.mp4", "clip-2.mp4", "clip-3.mp4" }), table.concat(names, " | "))
	check("3.1 Select File shows the #EXTINF title", has(names[1] or "", "(First Title)"), names[1] or "")
	check("3.1 the first entry plays", playing_on() and has(current_file(), "clip-1.mp4"),
		current_file() .. ", " .. status())
end)

-- 3.2: a line added while it plays joins on the folder check (10 s), and the playing item goes on
step("local playlist edited", 2000, 2000, nil, function()
	mark.time = tm()
	mark.edited = now_ms()
	append_file(ROOT .. "/local.m3u", "media/clip-1.mp4\r\n")
end)

step("edit joined", 0, 20000, function() return has(row_line(1), "4 files") end, function()
	local elapsed = now_ms() - mark.edited
	check("3.2 a line added to the .m3u joins within the folder check", has(row_line(1), "4 files") and
		elapsed < 15000, string.format("%s after %d ms", row_line(1), elapsed))
	check("3.2 the playing item is not restarted", playing_on() and has(current_file(), "clip-1.mp4") and
		tm() > mark.time + elapsed / 2, string.format("%s; time %d before the edit", status(), mark.time))
end)

-- 3.3: the list kept while its file is gone, back when it returns
step("local playlist deleted", 500, 500, nil, function()
	mark.text = read_file(ROOT .. "/local.m3u")
	os.remove(ROOT .. "/local.m3u")
end)

step("file missing", 0, 20000, function() return has(row_line(1), "playlist file not found") end, function()
	local line = row_line(1)
	check("3.3 a deleted .m3u keeps its list and says it is not found, with the last-list time", has(line, "4 files") and
		has(line, "playlist file not found") and line:find("showing the last list, from %d+%-%d+%-%d+ %d+:%d+:%d+") ~= nil,
		line)
	check("3.3 playback goes on", playing_on(), status())
	write_file(ROOT .. "/local.m3u", mark.text)
end)

step("file back", 0, 20000, function() return not has(row_line(1), "not found") end, function()
	check("3.3 the row recovers when the file is back",
		row_line(1) == "1: playlist - 4 files; no dates; list order", row_line(1))
	remove_source()
end)

-- 3.4: a .pls in File order, not line order, and a VLC-saved .m3u of file: URLs
step("pls and file URLs", 1000, 1000, nil, function()
	write_file(ROOT .. "/order.pls", "[playlist]\r\nNumberOfEntries=2\r\nFile2=media/clip-2.mp4\r\nTitle2=Second\r\n" ..
		"File1=media/clip-1.mp4\r\nTitle1=First\r\nVersion=2\r\n")
	write_file(ROOT .. "/vlc-local.m3u", "#EXTM3U\n#EXTINF:80,Clip Three\n" .. file_url(ROOT .. "/media/clip-3.mp4") ..
		"\n" .. file_url(ROOT .. "/media/clip-1.mp4") .. "\n")
	create_source("acceptance-pls-and-file-urls", { { ROOT .. "/order.pls", "pls" }, { ROOT .. "/vlc-local.m3u", "vlc" } })
end)

step("pls and file URLs listed", 0, 15000, function()
	return has(row_line(1), "2 files") and has(row_line(2), "2 files") and playing_on()
end, function()
	local names = listed_names()
	check("3.4 the .pls lists its entries in File order", #names == 4 and
		in_order(names, { "clip-1.mp4 (First)", "clip-2.mp4 (Second)" }), table.concat(names, " | "))
	check("3.4 the file: URLs are listed as the files on disk", #names == 4 and has(names[3], "clip-3.mp4") and
		not has(names[3], "file:") and not has(names[4], "file:") and not has(names[3], "%"),
		table.concat(names, " | "))
	obs.obs_source_media_next(src)
	obs.obs_source_media_next(src)
end)

step("file URL entry plays", 0, 15000, function() return playing_on() and has(current_file(), "clip-3.mp4") end,
	function()
		check("3.4 an entry from a file: URL plays", playing_on() and has(current_file(), "clip-3.mp4"),
			current_file() .. ", " .. status())
		remove_source()
	end)

-- 3.5, 3.6, 3.9 (a local feed), 3.10, 3.11, 3.12: one row each, read when all have an answer
local WEB_ROWS = {
	{ LIST .. "/fixtures/vlc.m3u", "vlc" },
	{ LIST .. "/extensionless-m3u", "m3u" },
	{ LIST .. "/extensionless-pls", "pls" },
	{ LIST .. "/fixtures/feed.rss", "feed" },
	{ "http://127.0.0.1:1/list", "refused" },
	{ "http://playlister-acceptance.invalid/list", "unresolved" },
	{ LIST .. "/status/429", "429" },
	{ LIST .. "/html", "html" },
	{ LIST .. "/gzip-at-cap", "at-cap" },
	{ LIST .. "/gzip-over-cap", "over-cap" },
	{ LIST .. "/redirect/5", "five" },
	{ LIST .. "/redirect/6", "six" },
}

step("web rows", 1000, 1000, nil, function()
	write_file(ROOT .. "/feed.rss", '<?xml version="1.0"?>\n<rss version="2.0"><channel><title>Acceptance</title>' ..
		'<item><title>One</title><enclosure url="' .. MEDIA .. '/media/1" type="video/mp4" length="1"/></item>' ..
		'<item><title>Two</title><enclosure url="' .. MEDIA .. '/media/2" type="video/mp4" length="1"/></item>' ..
		"</channel></rss>\n")
	create_source("acceptance-web-rows", WEB_ROWS)
end)

step("web rows answered", 0, 60000, function()
	for n = 1, #WEB_ROWS do
		local line = row_line(n)
		if not has(line, " - ") or has(line, "scanning") then
			return false
		end
	end
	return true
end, function()
	local function row(n, name, ok)
		check(name, ok, row_line(n))
	end
	row(1, "3.5 a list on a server keeps only its network entry, not files on this computer",
		has(row_line(1), "1: playlist - 1 file") and has(row_line(1), "2 lines could not be used"))
	-- relative.m3u: 5 entries resolve to addresses on the server; 2 local paths are unusable and 2 nested lists skipped
	row(2, "3.6 an extensionless address served as audio/x-mpegurl is read as M3U",
		has(row_line(2), "2: web address - 5 files") and has(row_line(2), "2 lines could not be used") and
		has(row_line(2), "2 playlists inside the list skipped"))
	row(3, "3.6 an extensionless address served as audio/x-scpls is read as PLS",
		has(row_line(3), "3: web address - 3 files"))
	row(4, "3.9 a feed says feeds come in a later version",
		has(row_line(4), "4: web address - ") and has(row_line(4), "a podcast feed: feeds come in a later version"))
	row(5, "3.10 a refused connection", has(row_line(5), "the server could not be reached"))
	row(6, "3.10 a name that does not resolve", has(row_line(6), "the server name could not be found"))
	row(7, "3.10 HTTP 429", has(row_line(7), "the server answered HTTP 429"))
	row(8, "3.10 a web page is not read as a list", has(row_line(8), "8: web address - ") and
		has(row_line(8), "a web page"))
	row(9, "3.11 a gzip list at the cap loads", has(row_line(9), "9: web address - empty") and
		not has(row_line(9), "16 MB"))
	row(10, "3.11 a gzip list over the cap", has(row_line(10), "the list is over 16 MB"))
	row(11, "3.12 five redirects load the list", has(row_line(11), "11: web address - 1 file"))
	row(12, "3.12 a sixth redirect", has(row_line(12), "too many redirects"))
	local _, info_type = status_block()
	check("3.10 the failing rows make the status block an error", info_type == 2, tostring(info_type))
	-- nothing plays from a row with no list, so the playing item, if any, is from a row that has one
	local playing = current_file()
	check("3.10 nothing plays from a failing row", not has(playing, "127.0.0.1:1") and
		not has(playing, ".invalid") and not has(playing, "/status/") and not has(playing, "/html") and
		not has(playing, "/redirect/6") and not has(playing, "gzip-over-cap"), playing)
	remove_source()
end)

-- 3.7: a media address without an extension plays at once; the plugin reads its headers only
step("direct media", 1000, 1000, nil, function()
	mark.media_log = #read_file(MEDIA_LOG)
	create_source("acceptance-direct-media", { { MEDIA .. "/media/2", "direct" } })
end)

step("direct media plays", 0, 20000, playing_on, function()
	check("3.7 a direct media address plays", playing_on(), string.format("%s after %d ms", status(),
		now_ms() - mark.created))
	check("3.7 its row is a web address", has(row_line(1), "1: web address"), row_line(1))
end)

-- the plugin's own request has been logged by now: the server logs a stream when its client goes
step("direct media requests", 3000, 3000, nil, function()
	-- every request is logged when it starts; a media body again when it ends, with the bytes read for it
	local ours, ours_ended, ours_bytes, others = 0, 0, 0, 0
	for line in read_file(MEDIA_LOG):sub(mark.media_log + 1):gmatch("[^\n]+") do
		local path, agent, bytes, outcome = line:match("^[^\t]*\t([^\t]*)\t([^\t]*)\t(%d+)\t([^\t]*)")
		if path == "/media/2" then
			local plugin = agent:find("^obs%-playlister") ~= nil
			if outcome == "started" then
				if plugin then
					ours = ours + 1
				else
					others = others + 1
				end
			elseif plugin then
				ours_ended = ours_ended + 1
				ours_bytes = ours_bytes + tonumber(bytes)
			end
		end
	end
	-- the file is tens of MB; a read of the headers leaves the server one or two 64 KB buffers ahead
	check("3.7 the plugin made one request and closed it after the headers", ours == 1 and ours_ended == 1 and
		ours_bytes < 1048576, string.format("%d requests, %d ended, %d bytes read for it", ours, ours_ended,
		ours_bytes))
	check("3.7 the player fetched the media itself", others >= 1, string.format("%d requests from the player", others))
	remove_source()
end)

-- 3.14 and 3.10 (a 429 after a good fetch): a served list of media addresses, its interval changed while it plays,
-- then edited, then failing
step("served list", 1000, 1000, nil, function()
	write_file(ROOT .. "/served.m3u", "#EXTM3U\n" .. MEDIA .. "/media/1\n" .. MEDIA .. "/media/3\n")
	create_source("acceptance-served-list", { { LIST .. "/switchable/served.m3u", "served" } })
end)

step("served list plays", 0, 20000, function() return has(row_line(1), "2 files") and playing_on() end, function()
	check("3.14 a served list of media addresses plays", has(row_line(1), "1: playlist - 2 files") and playing_on(),
		row_line(1) .. ", " .. status())
	mark.time = tm()
	mark.changed = now_ms()
	update_settings(function(data)
		obs.obs_data_set_int(data, "remote_refresh_s", 30)
	end)
end)

step("interval changed", 3000, 3000, nil, function()
	check("3.14 changing the interval does not restart playback", playing_on() and
		tm() > mark.time + (now_ms() - mark.changed) / 2, string.format("%s; time %d before", status(), mark.time))
	append_file(ROOT .. "/served.m3u", MEDIA .. "/media/2\n")
	mark.edited = now_ms()
end)

step("served edit joined", 0, 45000, function() return has(row_line(1), "3 files") end, function()
	local elapsed = now_ms() - mark.edited
	check("3.14 an edit to a served list appears within the 30 s interval", has(row_line(1), "3 files") and
		elapsed <= 35000, string.format("%s after %d ms", row_line(1), elapsed))
	check("3.14 playback goes on", playing_on(), status())
	write_file(ROOT .. "/served.m3u.status", "429")
	mark.failing = now_ms()
end)

step("served list failing", 0, 45000, function() return has(row_line(1), "HTTP 429") end, function()
	local line = row_line(1)
	check("3.10 a 429 after a good fetch keeps the last list", has(line, "3 files") and has(line, "HTTP 429") and
		has(line, "showing the last list"), line)
	check("3.10 playback goes on through the 429", playing_on(), status())
	os.remove(ROOT .. "/served.m3u.status")
	remove_source()
end)

-- 3.15: Lock list at start holds a playlist file's change until Apply waiting changes now
step("locked playlist", 1000, 1000, nil, function()
	write_file(ROOT .. "/locked.m3u", "media/clip-1.mp4\nmedia/clip-2.mp4\n")
	create_source("acceptance-locked-playlist", { { ROOT .. "/locked.m3u", "locked" } }, function(settings)
		obs.obs_data_set_bool(settings, "lock_list_at_start", true)
	end)
end)

step("locked playlist plays", 0, 15000, function() return has(row_line(1), "2 files") and playing_on() end,
	function()
		append_file(ROOT .. "/locked.m3u", "media/clip-3.mp4\n")
	end)

step("change waiting", 0, 20000, function() return has(row_line(1), "1 change waiting") end, function()
	local line = row_line(1)
	check("3.15 an edit to a locked playlist waits", has(line, "2 files") and has(line, "1 change waiting"), line)
	press("apply_waiting_changes")
end)

step("change applied", 0, 10000, function() return has(row_line(1), "3 files") end, function()
	local line = row_line(1)
	check("3.15 Apply waiting changes now adds it without a restart", has(line, "3 files") and
		not has(line, "waiting") and playing_on(), line .. ", " .. status())
	remove_source()
end)

-- 3.13, last: the list server answers one request at a time and holds /hang for two minutes
step("hang", 1000, 1000, nil, function()
	create_source("acceptance-hang", { { LIST .. "/hang", "hang" } })
end)

-- a server that takes the request and never answers meets the 30 s receive timeout before the 60 s deadline, which is
-- for a body that keeps trickling in (R-REM-5)
step("timed out", 0, 90000, function() return has(row_line(1), "timed out") end, function()
	local elapsed = now_ms() - mark.created
	check("3.13 a server that never answers times out after about 30 s", has(row_line(1), "timed out") and
		elapsed >= 28000 and elapsed <= 45000, string.format("%s after %d ms", row_line(1), elapsed))
	remove_source()
	create_source("acceptance-hang-removed", { { LIST .. "/hang", "hang" } })
end)

step("removed while hanging", 5000, 5000, nil, function()
	local before = now_ms()
	remove_source()
	mark.removed = now_ms()
	check("3.13 removing a source during a hanging fetch returns at once", mark.removed - before < 1000,
		string.format("%d ms", mark.removed - before))
end)

step("still responsive", 0, 0, nil, function()
	check("3.13 OBS stays responsive after the removal", now_ms() - mark.removed < 1000,
		string.format("next tick after %d ms", now_ms() - mark.removed))
end)

step("done", 1500, 1500, nil, function()
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
	return "obs-playlister Phase 3a acceptance"
end

function script_load(settings)
	if not (ROOT and LIST and MEDIA and MEDIA_LOG) then
		error("run with run-harness.ps1 -Script phase3a-acceptance.lua")
	end
	-- it writes and removes files there: only ever the runner's server root, never the repo's fixtures
	if not ROOT:find("/playlister-harness-3a-", 1, true) then
		error("PLAYLISTER_HARNESS_SERVER_ROOT is not a folder made by run-harness.ps1: " .. ROOT)
	end
	step_start = now_ms()
	obs.timer_add(tick, 100)
end
