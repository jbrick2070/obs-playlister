-- obs-playlister
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Phase 2 acceptance (TESTING.md, "Phase 2", PLAN step 8) on the real render folder: run it with
--   run-harness.ps1 -Script phase2-acceptance.lua -Otr <render folder> -TimeoutSeconds 600
-- The runner makes a temporary folder of hard links to every file in the render folder (PLAYLISTER_HARNESS_OTR_FOLDER;
-- links share their file's dates, and removing a link leaves the file), lists their names in date order
-- (PLAYLISTER_HARNESS_OTR_ORDER, one per line), and prepares a folder to move away and back
-- (PLAYLISTER_HARNESS_MOVE_FOLDER), a folder the current user may not read (PLAYLISTER_HARNESS_DENIED_FOLDER) and,
-- when this machine reaches it, the media folder by a UNC path (PLAYLISTER_HARNESS_UNC_FOLDER). Every file this
-- script puts into the link folder it removes again. It logs "HARNESS PASS|FAIL <check>" lines like the runtime
-- harness, whose helpers it repeats.
obs = obslua

local FILES = {}
for path in string.gmatch(os.getenv("PLAYLISTER_HARNESS_FILES") or "", "[^|]+") do
	table.insert(FILES, path)
end
local OTR_FOLDER = os.getenv("PLAYLISTER_HARNESS_OTR_FOLDER")
local OTR_ORDER = os.getenv("PLAYLISTER_HARNESS_OTR_ORDER")
local MOVE_FOLDER = os.getenv("PLAYLISTER_HARNESS_MOVE_FOLDER") -- two links, moved away and back
local DENIED_FOLDER = os.getenv("PLAYLISTER_HARNESS_DENIED_FOLDER")
local UNC_FOLDER = os.getenv("PLAYLISTER_HARNESS_UNC_FOLDER") -- unset when the admin share cannot be reached
local PLAYING, STOPPED = 1, 5
local HARNESS_TEMP = (os.getenv("TEMP") or "."):gsub("\\", "/")
local CORRUPT_STILL = HARNESS_TEMP .. "/playlister-acceptance-corrupt.jpg" -- written and removed by this script
local src, item = nil, nil
local NAMES = {} -- the render folder's files, oldest first
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

-- the values Select File stores, in list order (No File Selected's "0" left out)
local function listed()
	local props = obs.obs_source_properties(src)
	local list = obs.obs_properties_get(props, "select_file")
	local values = {}
	for i = 1, obs.obs_property_list_item_count(list) - 1 do
		table.insert(values, obs.obs_property_list_item_string(list, i))
	end
	obs.obs_properties_destroy(props)
	return values
end

local function lists(value)
	for _, v in ipairs(listed()) do
		if v == value then
			return true
		end
	end
	return false
end

-- the status block (R-LIST-5) as Properties shows it when built, and its info type (0 normal, 1 warning, 2 error)
local function status_block()
	local props = obs.obs_source_properties(src)
	local block = obs.obs_properties_get(props, "row_status")
	local text = block and obs.obs_property_long_description(block) or ""
	local info_type = block and obs.obs_property_text_info_type(block) or -1
	obs.obs_properties_destroy(props)
	return text, info_type
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

local function copy_file(from, to)
	local input = io.open(from, "rb")
	local bytes = input:read("*a")
	input:close()
	local output = io.open(to, "wb")
	output:write(bytes)
	output:close()
end

local function exists(path)
	local file = io.open(path, "rb")
	if file then
		file:close()
		return true
	end
	return false
end

-- an item's identity in the render folder's row: names are ASCII, so the key is the name in upper case (R-KEY-1)
local function otr(name)
	return "otr:1:" .. string.upper(name)
end

local function in_otr(name)
	return OTR_FOLDER .. "/" .. name
end

-- natural order of names, as the plugin sorts them (R-DIR-6, core/natural.c): digit runs by value, the rest by bytes
-- with ASCII letters folded to lower case (so "_" sorts before letters)
local function natural_less(a, b)
	a, b = string.lower(a), string.lower(b)
	local i, j = 1, 1
	while i <= #a and j <= #b do
		local da, db = a:match("^%d+", i), b:match("^%d+", j)
		if da and db then
			local ta, tb = da:gsub("^0+", ""), db:gsub("^0+", "")
			if #ta ~= #tb then
				return #ta < #tb
			end
			if ta ~= tb then
				return ta < tb
			end
			i, j = i + #da, j + #db
		else
			local ca, cb = a:sub(i, i), b:sub(j, j)
			if ca ~= cb then
				return ca < cb
			end
			i, j = i + 1, j + 1
		end
	end
	return #a - i < #b - j
end

local function expected_values(names)
	local values = {}
	for _, name in ipairs(names) do
		table.insert(values, otr(name))
	end
	return table.concat(values, " ")
end

local function step(name, min_wait, timeout, ready, run)
	table.insert(steps, { name = name, min_wait = min_wait, timeout = timeout, ready = ready, run = run })
end

-- 2.2: the render folder in full, newest-only and newest-5 modes; the first listing lands in seconds, in date order
-- (Directory Sorter parity, R-DIR-12)
step("render folder source", 1000, 1000, nil, function()
	for line in io.lines(OTR_ORDER) do
		if line ~= "" then
			table.insert(NAMES, line)
		end
	end
	create_source("acceptance-render-folder", function(settings)
		set_playlist(settings, { { OTR_FOLDER, "otr" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	mark.created = now_ms()
	check("render folder source created with " .. #NAMES .. " files to expect", src ~= nil and #NAMES > 5)
end)

step("render folder listed", 0, 60000, function() return #listed() == #NAMES end, function()
	local elapsed = now_ms() - mark.created
	check("2.2 the first listing of the render folder lands in seconds", #listed() == #NAMES and elapsed < 10000,
		string.format("%d files in %d ms", #listed(), elapsed))
	check("2.2 with default options the list is in date order, oldest first (Directory Sorter parity)",
		table.concat(listed(), " ") == expected_values(NAMES), "first " .. tostring(listed()[1]))
	local text, info_type = status_block()
	check("2.2 the status block counts the files", text == "1: folder - " .. #NAMES .. " files" and info_type == 0,
		text)
end)

step("oldest plays", 0, 15000, function() return st() == PLAYING and dur() > 0 end, function()
	check("2.2 the oldest render plays first", current_file() == "1-1: " .. in_otr(NAMES[1]), current_file())
	update_settings(function(data) obs.obs_data_set_int(data, "container_mode", 1) end)
	mark.mode_changed = now_ms()
end)

step("newest only", 0, 5000, function() return #listed() == 1 end, function()
	check("2.2 newest only lists the newest render alone", table.concat(listed(), " ") == otr(NAMES[#NAMES]),
		table.concat(listed(), " ") .. string.format(" after %d ms", now_ms() - mark.mode_changed))
	check("2.2 the status block shows one of all", (status_block()) == "1: folder - 1 of " .. #NAMES .. " files",
		(status_block()))
	update_settings(function(data)
		obs.obs_data_set_int(data, "container_mode", 2)
		obs.obs_data_set_int(data, "newest_count", 5)
	end)
end)

step("newest five", 0, 5000, function() return #listed() == 5 end, function()
	local newest = {}
	for i = #NAMES - 4, #NAMES do
		table.insert(newest, NAMES[i])
	end
	check("2.2 newest 5 lists the five newest renders, oldest first", table.concat(listed(), " ") ==
		expected_values(newest), table.concat(listed(), " "))
	update_settings(function(data) obs.obs_data_set_int(data, "container_mode", 0) end)
end)

-- 2.3: a new render joins within about 3 s (the 500 ms debounce and the 2 s write-complete wait), at the end
step("full list again", 0, 5000, function() return #listed() == #NAMES end, function()
	copy_file(FILES[2], in_otr("zz-acceptance-render.mp4"))
	mark.copied = now_ms()
end)

step("new render joins", 0, 15000, function() return lists(otr("zz-acceptance-render.mp4")) end, function()
	local elapsed = now_ms() - mark.copied
	local values = listed()
	check("2.3 a new render joins the list within about 3 s, as the newest", values[#values] ==
		otr("zz-acceptance-render.mp4") and elapsed < 4000, string.format("%d ms", elapsed))
	-- the producer contract: written under a skipped name, then renamed (PLAN step 8)
	copy_file(FILES[3], in_otr("zz-acceptance-contract.mp4.part"))
end)

step("partial name skipped", 5000, 5000, nil, function()
	check("2.4 a render written as .part is not listed", not lists(otr("zz-acceptance-contract.mp4.part")) and
		#listed() == #NAMES + 1, #listed() .. " listed")
	os.rename(in_otr("zz-acceptance-contract.mp4.part"), in_otr("zz-acceptance-contract.mp4"))
	mark.renamed = now_ms()
end)

step("renamed render joins", 0, 15000, function() return lists(otr("zz-acceptance-contract.mp4")) end, function()
	check("2.4 renamed to its final name, it joins", lists(otr("zz-acceptance-contract.mp4")),
		string.format("%d ms", now_ms() - mark.renamed))
	mark.slow = io.open(in_otr("zz-acceptance-slow.mp4"), "wb")
	mark.slow_written, mark.slow_seen, mark.slow_started = 0, false, now_ms()
end)

-- 2.5: a slow copy (1 MB every 100 ms for 8 s) is not listed until it is complete, then joins within about 3 s
step("slow copy", 0, 20000, function()
	if now_ms() - mark.slow_started < 8000 then
		mark.slow:write(string.rep("\0", 1024 * 1024))
		mark.slow:flush()
		mark.slow_written = mark.slow_written + 1
		mark.slow_seen = mark.slow_seen or lists(otr("zz-acceptance-slow.mp4"))
		return false
	end
	return true
end, function()
	mark.slow:close()
	mark.slow_closed = now_ms()
	check("2.5 a file being copied slowly is not listed while it grows", not mark.slow_seen,
		mark.slow_written .. " MB written")
end)

step("slow copy joins", 0, 15000, function() return lists(otr("zz-acceptance-slow.mp4")) end, function()
	check("2.5 once the slow copy is complete it joins within about 3 s", lists(otr("zz-acceptance-slow.mp4")) and
		now_ms() - mark.slow_closed < 4000, string.format("%d ms", now_ms() - mark.slow_closed))
	mark.held = io.open(in_otr("zz-acceptance-held.mp4"), "wb")
	mark.held:write(string.rep("\0", 1024 * 1024))
	mark.held:flush()
	mark.before_held = { duration = dur(), time = tm() }
end)

-- 2.5: a file held open for writing does not join, and does not stop playback or the other files
step("held open", 6000, 6000, nil, function()
	local text = status_block()
	check("2.5 a file held open for writing does not join, and is counted as being written",
		not lists(otr("zz-acceptance-held.mp4")) and text:find("waiting for 1 file to finish writing", 1, true) ~= nil,
		text)
	check("2.5 playback goes on while a file is held open", st() == PLAYING and dur() == mark.before_held.duration and
		tm() > mark.before_held.time, status())
	mark.held:close()
	mark.held_closed = now_ms()
end)

step("held file joins", 0, 15000, function() return lists(otr("zz-acceptance-held.mp4")) end, function()
	check("2.5 once closed, the held file joins", lists(otr("zz-acceptance-held.mp4")),
		string.format("%d ms", now_ms() - mark.held_closed))
	-- 2.6: a case-only rename keeps the item where it is
	mark.case_before = table.concat(listed(), " ")
	os.rename(in_otr(NAMES[5]), in_otr(string.upper(NAMES[5])))
end)

step("case-only rename", 3000, 3000, nil, function()
	local props = obs.obs_source_properties(src)
	local list = obs.obs_properties_get(props, "select_file")
	local label = obs.obs_property_list_item_name(list, 5)
	obs.obs_properties_destroy(props)
	check("2.6 a case-only rename keeps the item's place and shows its new name", table.concat(listed(), " ") ==
		mark.case_before and label:find(string.upper(NAMES[5]), 1, true) ~= nil, label)
	os.rename(in_otr(string.upper(NAMES[5])), in_otr(NAMES[5]))
	-- 2.6: A -> B -> C with shuffle on and the list locked, then Apply waiting changes now
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", true)
		obs.obs_data_set_bool(data, "lock_list_at_start", true)
	end)
end)

step("rename chain while locked", 3000, 3000, nil, function()
	mark.chain_before = table.concat(listed(), " ")
	mark.before_chain = { duration = dur(), time = tm() }
	os.rename(in_otr(NAMES[10]), in_otr("zz-chain-b.mp4"))
end)

step("second rename", 1500, 1500, nil, function()
	os.rename(in_otr("zz-chain-b.mp4"), in_otr("zz-chain-c.mp4"))
end)

step("renames waiting", 3000, 3000, nil, function()
	local text = status_block()
	check("2.6 with the list locked, the renames wait", table.concat(listed(), " ") == mark.chain_before and
		text:find("changes waiting", 1, true) ~= nil, text)
	press("apply_waiting_changes")
	mark.applied = now_ms()
end)

step("renames applied", 0, 10000, function() return lists(otr("zz-chain-c.mp4")) end, function()
	local elapsed = now_ms() - mark.applied
	check("2.6 applied at once, A -> B -> C is one item renamed, and playback goes on", elapsed < 1500 and
		lists(otr("zz-chain-c.mp4")) and
		not lists(otr(NAMES[10])) and not lists(otr("zz-chain-b.mp4")) and #listed() == #NAMES + 4 and
		st() == PLAYING and dur() == mark.before_chain.duration and tm() > mark.before_chain.time,
		string.format("%d ms, ", elapsed) .. status())
	update_settings(function(data)
		obs.obs_data_set_bool(data, "shuffle", false)
		obs.obs_data_set_bool(data, "lock_list_at_start", false)
	end)
end)

-- 2.7: a sort change mid-play reorders the list without disturbing playback
step("sort change", 1500, 1500, nil, function()
	os.rename(in_otr("zz-chain-c.mp4"), in_otr(NAMES[10]))
	mark.before_sort = { duration = dur(), time = tm() }
	update_settings(function(data)
		obs.obs_data_set_int(data, "sort_key", 1)
		obs.obs_data_set_bool(data, "sort_descending", true)
	end)
end)

step("sorted by name", 3000, 3000, nil, function()
	local names = {}
	for _, name in ipairs(NAMES) do
		table.insert(names, name)
	end
	for _, name in ipairs({ "zz-acceptance-render.mp4", "zz-acceptance-contract.mp4", "zz-acceptance-slow.mp4",
		"zz-acceptance-held.mp4" }) do
		table.insert(names, name)
	end
	table.sort(names, function(a, b) return natural_less(b, a) end)
	check("2.7 sorted by name, descending (natural order)", table.concat(listed(), " ") == expected_values(names),
		tostring(listed()[1]))
	check("2.7 a sort change keeps playback where it was", st() == PLAYING and dur() == mark.before_sort.duration and
		tm() > mark.before_sort.time, status())
	update_settings(function(data)
		obs.obs_data_set_int(data, "sort_key", 2)
		obs.obs_data_set_bool(data, "sort_descending", false)
		obs.obs_data_set_bool(data, "folder_check_enabled", false)
	end)
	-- 2.8: 2,000 small files at once, a burst that can overflow the change buffer (the log says when it does); with
	-- the folder check off, only notifications bring the render copied in afterwards
	for i = 1, 2000 do
		local file = io.open(in_otr(string.format("zz-overflow-%04d.txt", i)), "wb")
		file:write("x")
		file:close()
	end
	copy_file(FILES[1], in_otr("zz-after-overflow.mp4"))
	mark.after_overflow = now_ms()
end)

step("after overflow", 0, 20000, function() return lists(otr("zz-after-overflow.mp4")) end, function()
	local elapsed = now_ms() - mark.after_overflow
	-- notifications bring it within about 3 s; after a run of overflows the folder is rescanned on the interval (10 s)
	-- instead, and a dead watch would wait for the 60 s safety net
	check("2.8 after 2,000 files at once, a new render joins within 12 s with the folder check off (text files unlisted)",
		lists(otr("zz-after-overflow.mp4")) and #listed() == #NAMES + 5 and elapsed < 12000,
		string.format("%d listed, %d ms", #listed(), elapsed))
	update_settings(function(data) obs.obs_data_set_bool(data, "folder_check_enabled", true) end)
	for i = 1, 2000 do
		os.remove(in_otr(string.format("zz-overflow-%04d.txt", i)))
	end
	-- 2.6: the playing file cannot be renamed or deleted while the child holds it open; whatever Windows does,
	-- playback is sane
	mark.before_delete = { duration = dur(), time = tm() }
	local playing = current_file():match("^%d+%-%d+: (.*)$")
	mark.playing_link = playing and playing:sub(1, #OTR_FOLDER + 1) == OTR_FOLDER .. "/"
	mark.rename_result = mark.playing_link and select(2, os.rename(playing, in_otr("zz-playing-renamed.mp4"))) or "-"
	if mark.playing_link and exists(in_otr("zz-playing-renamed.mp4")) then
		os.rename(in_otr("zz-playing-renamed.mp4"), playing)
		mark.rename_result = "renamed"
	end
	mark.delete_result = mark.playing_link and select(2, os.remove(playing)) or "the playing item is no link here"
	mark.deleted = mark.playing_link and not exists(playing)
end)

step("delete the playing file", 3000, 3000, nil, function()
	check("2.6 renaming and deleting the playing render: " .. (mark.deleted and "deleted, playback moved on" or
		"Windows refused, playback goes on"), mark.playing_link and st() == PLAYING and (mark.deleted or (dur() ==
		mark.before_delete.duration and tm() > mark.before_delete.time)), "rename: " .. tostring(mark.rename_result) ..
		"; delete: " .. tostring(mark.delete_result) .. ", " .. status())
	remove_source()
	for _, name in ipairs({ "zz-acceptance-render.mp4", "zz-acceptance-contract.mp4", "zz-acceptance-slow.mp4",
		"zz-acceptance-held.mp4", "zz-after-overflow.mp4" }) do
		os.remove(in_otr(name))
	end
end)

-- 2.9: a watched folder that goes away keeps its last list, says so with its time, and recovers when it is back
step("folder to move", 1500, 1500, nil, function()
	create_source("acceptance-moving-folder", function(settings)
		set_playlist(settings, { { MOVE_FOLDER, "moving" }, { FILES[1], "file" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
end)

step("folder listed", 0, 15000, function() return (status_block()):find("1: folder - 2 files", 1, true) ~= nil end,
	function()
		-- the file row plays, so nothing in the folder is open
		local settings = obs.obs_source_get_settings(src)
		obs.obs_data_set_string(settings, "select_file", (listed())[3])
		obs.obs_data_release(settings)
		press("play_selected")
	end)

step("file row plays", 1500, 1500, nil, function()
	mark.moved = os.rename(MOVE_FOLDER, MOVE_FOLDER .. "-away")
	check("2.9 the watched folder moved away", mark.moved == true, tostring(mark.moved))
end)

step("folder gone", 0, 20000, function() return (status_block()):find("went away", 1, true) ~= nil end, function()
	local text, info_type = status_block()
	check("2.9 a folder that went away shows the error and the time of the last list it keeps",
		text:find("1: folder - 2 files; the folder went away", 1, true) ~= nil and
		text:find("showing the last list, from 20", 1, true) ~= nil and info_type == 2, text)
	os.rename(MOVE_FOLDER .. "-away", MOVE_FOLDER)
end)

step("folder back", 0, 30000, function() return (status_block()):find("1: folder - 2 files\n", 1, true) == 1 end,
	function()
		local text, info_type = status_block()
		check("2.9 the folder recovers when it is back", text == "1: folder - 2 files\n2: file" and info_type == 0, text)
		remove_source()
	end)

-- 2.10: a folder the user may not read shows access denied (R-PLAT-10): its attributes still make it a folder row
-- (R-LIST-1 rule 5), whose provider is refused
step("denied folder", 1500, 1500, nil, function()
	create_source("acceptance-denied-folder", function(settings)
		set_playlist(settings, { { DENIED_FOLDER, "denied" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
end)

step("access denied", 0, 15000, function() return (status_block()):find("access denied", 1, true) ~= nil end,
	function()
		local text, info_type = status_block()
		check("2.10 a folder the user may not read shows access denied as an error",
			text == "1: folder - access denied (permissions or security software)" and info_type == 2, text)
		remove_source()
	end)

-- 2.11: a folder by a UNC path (the admin share of this machine)
step("unc folder", 1500, 1500, nil, function()
	if not UNC_FOLDER then
		log("SKIP 2.11 the admin share \\\\localhost\\C$ cannot be reached here; a UNC path stays a manual check")
		return
	end
	create_source("acceptance-unc-folder", function(settings)
		set_playlist(settings, { { UNC_FOLDER, "unc" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
end)

step("unc listed", 0, 30000, function() return not UNC_FOLDER or (st() == PLAYING and dur() > 0) end, function()
	if not UNC_FOLDER then
		return
	end
	local text = status_block()
	check("2.11 a folder by a UNC path is listed and plays", text == "1: folder - 3 files" and st() == PLAYING, text ..
		", " .. status())
end)

-- past the 10 s folder check, where a lost watch showed before
step("unc stays listed", 12000, 12000, nil, function()
	if not UNC_FOLDER then
		return
	end
	local text, info_type = status_block()
	check("2.11 the UNC folder stays listed past the folder check", text == "1: folder - 3 files" and info_type == 0,
		text)
	remove_source()
end)

-- 2.14: a folder listed for the first time while a render is still being copied into it (as when OBS starts during
-- a copy): the admitted renders play at once, the copy is counted as being written and joins when it is complete
local function in_move(name)
	return MOVE_FOLDER .. "/" .. name
end

step("copy under way", 1500, 1500, nil, function()
	mark.copying = io.open(in_move("c.mp4"), "wb")
	mark.copying:write(string.rep("\0", 1024 * 1024))
	mark.copying:flush()
	create_source("acceptance-during-copy", function(settings)
		set_playlist(settings, { { MOVE_FOLDER, "copying" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
	end)
	mark.copy_source_created = now_ms()
	mark.copy_played_ms, mark.copy_listed, mark.copy_counted = nil, false, false
end)

step("first listing during a copy", 0, 20000, function()
	if now_ms() - mark.copy_source_created < 6000 then
		mark.copying:write(string.rep("\0", 1024 * 1024))
		mark.copying:flush()
		if not mark.copy_played_ms and st() == PLAYING and dur() > 0 then
			mark.copy_played_ms = now_ms() - mark.copy_source_created
		end
		mark.copy_listed = mark.copy_listed or lists("copying:1:C.MP4")
		mark.copy_counted = mark.copy_counted or
			(status_block()):find("waiting for 1 file to finish writing", 1, true) ~= nil
		return false
	end
	return true
end, function()
	mark.copying:close()
	mark.copy_closed = now_ms()
	check("2.14 listed for the first time during a copy, the admitted renders play within about 3 s",
		mark.copy_played_ms ~= nil and mark.copy_played_ms < 3500, tostring(mark.copy_played_ms) .. " ms")
	check("2.14 the copy is counted as being written and not listed while it grows", mark.copy_counted and
		not mark.copy_listed, "counted " .. tostring(mark.copy_counted) .. ", listed " .. tostring(mark.copy_listed))
end)

step("copy joins", 0, 15000, function() return lists("copying:1:C.MP4") end, function()
	check("2.14 once complete, the copy joins within about 3 s", lists("copying:1:C.MP4") and
		now_ms() - mark.copy_closed < 4000, string.format("%d ms", now_ms() - mark.copy_closed))
	remove_source()
end)

-- removed once nothing holds it open; tried until it is gone
step("copy removed", 0, 10000, function()
	os.remove(in_move("c.mp4"))
	return not exists(in_move("c.mp4"))
end, function()
	check("the copy written into the moving folder is removed", not exists(in_move("c.mp4")))
end)

-- 2.19: an item's startup is not judged while its source is hidden (R-BEH-5): a corrupt still in a source that is in
-- no scene being shown never fails, and fails within its 5 s window once shown; and with Close file when inactive and
-- Always play, a hidden source's valid file is not failed either. (A paused source's window waits too; test-startup
-- covers it. A corrupt file's child has failed at once, so a pause for it is rightly ignored as for a stopped one.)
local function current_scene_add(source)
	local scene_source = obs.obs_frontend_get_current_scene()
	local added = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), source)
	obs.obs_source_release(scene_source)
	return added
end

local function create_offscreen(name, fill)
	local settings = obs.obs_data_create()
	fill(settings)
	src = obs.obs_source_create("obs_playlister_source", name, settings, nil)
	obs.obs_data_release(settings)
	mark.offscreen = obs.obs_scene_create_private(name .. "-offscreen")
	obs.obs_scene_add(mark.offscreen, src)
end

local function remove_offscreen()
	if mark.shown_item then
		obs.obs_sceneitem_remove(mark.shown_item)
		mark.shown_item = nil
	end
	obs.obs_scene_release(mark.offscreen)
	mark.offscreen = nil
	obs.obs_source_release(src)
	src = nil
end

local function failed_shown()
	local text, info_type = status_block()
	return text:find("1: file - could not be played", 1, true) ~= nil and info_type == 2
end

step("hidden corrupt still", 1500, 1500, nil, function()
	local file = io.open(CORRUPT_STILL, "wb")
	file:write(string.rep("not media ", 2000))
	file:close()
	create_offscreen("acceptance-hidden", function(settings)
		set_playlist(settings, { { CORRUPT_STILL, "still" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 1)
	end)
end)

-- twice the startup window
step("hidden for 10 s", 10000, 10000, nil, function()
	-- loaded while hidden (its startup began), yet not judged
	local loaded = current_file() == "1: " .. CORRUPT_STILL and st() ~= 0
	check("2.19 a hidden source loads its item but does not judge its startup", loaded and
		obs.obs_source_active(src) == false and not failed_shown() and st() ~= STOPPED,
		current_file() .. ", " .. (status_block()) .. ", " .. status())
	mark.shown_item = current_scene_add(src)
	mark.shown_at = now_ms()
end)

step("shown, then judged", 0, 15000, failed_shown, function()
	check("2.19 shown, the corrupt still fails within its startup window", failed_shown() and
		now_ms() - mark.shown_at < 8000, string.format("%d ms", now_ms() - mark.shown_at))
	remove_offscreen()
	create_offscreen("acceptance-closed-when-hidden", function(settings)
		set_playlist(settings, { { FILES[1], "file" } })
		obs.obs_data_set_int(settings, "visibility_behavior", 2)
		obs.obs_data_set_bool(settings, "close_when_inactive", true)
	end)
end)

step("closed while hidden for 10 s", 10000, 10000, nil, function()
	check("2.19 with Close file when inactive and Always play, a hidden source's file is not failed",
		not failed_shown(), (status_block()) .. ", " .. status())
	mark.shown_item = current_scene_add(src)
end)

step("shown, it plays", 0, 15000, function() return st() == PLAYING and dur() > 0 and tm() > 1000 end, function()
	check("2.19 shown, the same file plays", st() == PLAYING and not failed_shown() and
		(status_block()) == "1: file", (status_block()) .. ", " .. status())
	remove_offscreen()
end)

step("corrupt still removed", 0, 10000, function()
	os.remove(CORRUPT_STILL)
	return not exists(CORRUPT_STILL)
end, function()
	check("the corrupt still this script wrote is removed", not exists(CORRUPT_STILL))
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
	return "obs-playlister Phase 2 acceptance"
end

function script_load(settings)
	if not (OTR_FOLDER and OTR_ORDER and MOVE_FOLDER and DENIED_FOLDER) or #FILES ~= 3 then
		error("run with run-harness.ps1 -Script phase2-acceptance.lua -Otr <render folder>")
	end
	-- it renames and removes files there: only ever the runner's folder of links, never a render folder itself
	if not OTR_FOLDER:find("/playlister-harness-otr-", 1, true) then
		error("PLAYLISTER_HARNESS_OTR_FOLDER is not a folder of links made by run-harness.ps1: " .. OTR_FOLDER)
	end
	step_start = now_ms()
	obs.timer_add(tick, 100)
end
