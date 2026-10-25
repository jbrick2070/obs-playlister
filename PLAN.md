# obs-playlister -- Plan

Status: 2026-10-24. Approved by the operator (decisions: REQUIREMENTS section 14).
Phase 1 (0.2.0) is implemented; Phases 2-4 are not started.
Revision: four-round panel review folded in (kibitz-runs/2026-10-24-obs-playlister-plan/).
Companion to REQUIREMENTS.md; requirement IDs (R-...) and decisions (D-...)
refer to that file. "src N" means src/media-playlist-source.c line N at f8d90bd.

---

## 1. Toolchain findings on this machine (checked 2026-10-24)

| Component | Found | Needed | Action |
|-----------|-------|--------|--------|
| Visual Studio | Build Tools 2022, 17.14.37502 (no IDE), `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`. Components: VC.Tools.x86.x64, MSVC 14.44.35207 (`cl.exe` 14.44.35228), Windows11SDK.26100, VC.CMake.Project | A VS 2022 or newer C/C++ toolchain | None. The `Visual Studio 17 2022` generator works with Build Tools. OBS 33 itself is built with VS 2026 (release notes); see risk R1. |
| Windows SDK | 10.0.26100.0 only | OBS 33 CMake requires >= 10.0.20348; OBS 33 CI uses 26100 | Change `CMakePresets.json` from `x64,version=10.0.22621` to `x64,version=10.0.26100.0`; 22621 is not installed, so the current preset cannot configure. |
| CMake | 4.4.0, `C:\Program Files\CMake\bin` | 3.28..3.30 per `cmake_minimum_required` | None. |
| Ninja | not on PATH | not needed (VS generator) | None. |
| git | 2.56.0.windows.3; user Jeffrey A. Brick; `core.autocrlf=true` | any | None. Working tree is CRLF, index is LF (87 of 90 files show CRLF locally; that is autocrlf, not the repo). |
| gh | 2.89.0, logged in as `jbrick2070` (keyring, https) | authenticated | None. |
| OBS Studio | 32.2.2 installed at `C:\Program Files\obs-studio` (binary dated 2026-09-05). OBS 33.0.0 released 2026-10-23. | 33.x | D-1: upgrade before the Phase 1 smoke test. The build does not need OBS installed; the Legacy-flag check does. |
| Existing plugins | ProgramData (legacy `bin\64bit` layout): `media-playlist-source` 0.1.3, `StreamDeckPlugin`, `win-spout`. Program Files legacy dir: 33 DLLs (advanced-scene-switcher and others; these stop loading in OBS 34). | -- | Leave alone (R-PLAT-8). Good live examples of what "Legacy" looks like in the Plugin Manager. |
| PowerShell 7 (`pwsh`) | not installed | only `.github/scripts/*.ps1` need it, and they also refuse to run outside CI | None for local builds; we call `cmake` directly. |
| Inno Setup 6 (`iscc`) | not installed | `.exe` installer | D-2. `winget install JRSoftware.InnoSetup` if wanted. |
| clang-format | not on PATH | CI format check pins exactly 17.0.3 (`build-aux/.run-format.zsh` 36-54 rejects older and newer, brew `clang-format@17`) | Install exactly 17.0.3 if a Windows build of it can be had (`winget install LLVM.LLVM --version 17.0.3`); a current LLVM formats differently and turns CI red. If not, CI's format check is authoritative (R-STD-3). |
| gersemi | not installed (Python tool) | CI CMake format check | Do not install; keep CMake edits gersemi-clean by hand (2-space indent, 120 cols). |
| Python | 3.10 on PATH | not used by the plugin or its build | Nothing. Used only by the kibitz review tooling, outside the plugin. |
| Network | GitHub API reachable | dependency downloads at first configure | None. |
| Windows security | Defender real-time on; Controlled Folder Access off; Smart App Control off; user-mode WDAC off; no ASR rules visible without admin; unsigned `media-playlist-source.dll` loads in OBS today; no Defender/Code Integrity block events for OBS in 30 days; deny-write probe on an OTR mp4 OK | an unsigned plugin DLL must load; the plugin must read the OTR folder | None now (R-PLAT-10). Re-checked in the Phase 1 smoke test. |

## 2. OBS 33 facts that drive the build (verified against sources)

1. Plugin loading was overhauled in 33.0 (PR obsproject/obs-studio#13594). New Windows layout, from `frontend/plugin-manager/PluginModuleLoader_Windows.cpp` at tag 33.0.0:
   - System: `C:\ProgramData\obs-studio\plugins\<Plugin>\<Plugin>.dll`, data `...\<Plugin>\data`
   - User: `%OBS_PLUGINS_PATH%\<Plugin>\<Plugin>.dll`, data `...\<Plugin>\data` (env var only; there is no `%APPDATA%` plugin folder)
   - Portable: `<OBS root>\plugins\<Plugin>\<Plugin>.dll`, data `...\<Plugin>\data`
   - Legacy (still loaded, flagged Legacy): `...\plugins\<Plugin>\bin\64bit\<Plugin>.dll`; `C:\Program Files\obs-studio\obs-plugins\64bit\<Plugin>.dll` (removed in 34.0)
   - OBS tries the new locations first and skips a module it already loaded if it meets it again in a legacy location.
2. Plugin Manager metadata: libobs reads `<data>/manifest.json` if present (`obs_module_load_metadata`, libobs/obs-module.c 101-140). The Plugin Manager shows a version only from it; the display name comes from it, else from the exported `obs_module_name()`. `obs-module.h` 192-195 already declares `obs_module_name` and `obs_module_description` with `MODULE_EXPORT`, so defining them in `plugin-main.c` exports them.
3. Toolchain: OBS 33 CI uses `Visual Studio 18 2026` and SDK 10.0.26100.0 (`CMakePresets.json` at 33.0.0). Its CMake only hard-fails for SDK < 10.0.20348. C17/C++17. So VS 2022 + SDK 26100 should compile libobs 33; it is untested by upstream (risk R1).
4. Dependency manifest moved. obs-studio 33 has no root `buildspec.json`; versions live in `CMakePresets.json` under `vendor."obsproject.com/obs-studio".dependencies`:
   - prebuilt obs-deps `2026-10-23`, windows-x64 SHA-256 `7508972a867a0ef68c26cd8bdba61fd999edd0a92fa9eb9585774de9bf51bfbe`
   - qt6 `2026-10-23`, windows-x64 SHA-256 `78cd7a138464bb6edcdd3b85fcd63a5a7c69e369629963f3bc395bab74fb3f47`, debugSymbols windows-x64 `5c97809dcb663a51e5b5f47983359d03a213a8c711c4b158a1b62429b8dff6be`
   - The plugin template's own `buildspec.json` flow is unchanged (it downloads these itself), so we only update our values. The obs-studio 33.0.0 source zip hash is computed in Phase 1 step 4.
5. Configure options renamed: OBS 33 uses `ENABLE_FRONTEND` and `ENABLE_SCRIPTING` (root `CMakeLists.txt` 18-19, both default ON). The template passes `-DENABLE_UI:BOOL=OFF` and the misspelled `-DCMAKE_ENABLE_SCRIPTING=OFF` (`cmake/common/buildspec_common.cmake` 58, 70), so both are ignored and the full frontend would configure. The same lines build their arguments wrongly: two `-D` options in one string become one argument (the scripting flag ends up inside `CMAKE_SYSTEM_VERSION`), `-A` and its value are one argument, and `CMAKE_PREFIX_PATH` is wrapped in literal single quotes (R-UP-56). `obs-frontend-api` is still defined with the frontend off (`frontend/CMakeLists.txt` 3-7), so the template's build target keeps working.
6. The plugin template upstream (obsproject/obs-plugintemplate master, last commit 2025-12-09) is still on OBS 31.1.1, SDK 22621 and the `bin\64bit` install layout. There is nothing to cherry-pick for the 33 layout; we change `cmake/windows/helpers.cmake` and the packaging scripts ourselves. The template's `defaults.cmake` (commit 013ffca) sets the install prefix to `$ENV{ALLUSERSPROFILE}/obs-studio/plugins`; this repo's copy still uses `$ENV{APPDATA}` (R-UP-52) and is changed in Phase 1 step 4.
7. `ffmpeg_source` settings keys this plugin writes (`local_file`, `input`, `is_local_file`, `hw_decode`, `close_when_inactive`, `restart_on_activate`, `speed_percent`, `log_changes`) all still exist in `plugins/obs-ffmpeg/obs-ffmpeg-source.c` at 33.0.0. `clear_on_media_end` defaults to true (line 121). `media_stopped` still emits ENDED when the state was not STOPPED (lines 274-286), so the upstream `user_stopped` workaround (R-UP-41) stays through Phase 1 and is replaced by the player's end attribution in Phase 2 step 1.4. No failure signal exists; `media_started` fires before any decode, and nothing fires when the open fails (318-333, R-UP-69).
8. Release notes list a breaking change to `gs_image_file`; this plugin does not use it. The deprecated `monitoring_type` and hidden-source APIs are not used either.

## 3. Target architecture

Two layers, split so Phase 4 (VJ modes) touches only the second:

- Source-input layer: one provider per container row (R-LIST-1, R-THR-2). File rows are built synchronously and have no provider. Providers produce immutable snapshots of plain entries. All parsing and decision logic is OBS-free.
- Playback layer: the OBS source. Owns the child `ffmpeg_source` (only through `player.c` and its thread), the shuffler, loop, visibility and restart behavior, still-image hold, failure tracking, transport, hotkeys, properties.

### 3.1 Files

```
src/
  plugin-main.c                  module entry; exports obs_module_name/description (Phase 1)
  plugin-support.c.in / .h       template logging (unchanged)
  media-playlist-source.c / .h   Phase 1: identity changes only, no move (moved to obs/ in Phase 2 step 1.5)
  shuffler.c / .h                shuffle order unchanged throughout. Phase 1: crash fixes (R-UP-68,
                                 R-UP-73, R-UP-74, R-UP-75) and test-harness fixes (R-TST-4).
                                 Phase 2 step 4: find_media_index matches by row id, item key and
                                 occurrence
  playlist.h                     upstream entry struct; Phase 2 adds an owned pl_entry
Phase 2 adds:
  obs/
    playlister-source.c / .h     media-playlist-source.c moved; properties, settings, migration,
                                 transport, hotkeys, proc handler, snapshot apply, row status text
    player.c / .h                the player thread: the only code that touches the child source;
                                 command FIFO, load handoff, end attribution, startup window,
                                 still hold, failure tracking; the Phase 4 boundary
  core/                          libobs util only (bmem, dstr, darray, platform); no sources, graphics
                                 or module APIs; errors returned, never logged; headless tests
    entry.h / entry-list.c       struct pl_entry and lists
    sort.c, filter.c, modes.c    R-DIR-5/6/7/10
    keys.c                       R-KEY-1 occurrence numbering (folding happens in the Win32 provider)
    merge.c                      successor after delete, eviction, rename aliases (R-DIR-8)
    folder-state.c               write-complete gate (R-DIR-4) and watch modes (R-DIR-3);
                                 stat, probe, clock and events injected
    url.c, natural.c             URL resolution and percent decoding; natural compare
    command-queue.c              player FIFO: ordering, wake, shutdown (section 3.3), trace-tested
  providers/
    provider.h                   section 3.2
    provider-folder-win.c        ReadDirectoryChangesW worker (Win32)
Phase 3 adds:
  providers/fetch.h, fetch-winhttp.c   async WinHTTP (R-REM-5); a local-file backend for tests
                                       and for local playlist files
  providers/provider-list.c            one provider for local playlist files and every http(s)
                                       row: classification (R-LIST-1), refresh timer, last-good
                                       retention, dispatch to the parsers below
  core/parse-m3u.c, parse-pls.c, parse-feed.c, parse-http-index.c, date.c, xml-reader.h
  third_party/yxml/ + third_party/.clang-format (DisableFormat: true) (pending D-4)
tests/
  CMakeLists.txt, test-shuffler.c (Phase 1), test-<module>.c per core module, fixtures/,
  check-locale-keys.cmake (Phase 2),
  manual/serve-fixtures.ps1 (PowerShell HttpListener for TESTING.md; not run by ctest)
```

### 3.2 Provider interface (core/entry.h, providers/provider.h)

```c
enum pl_entry_kind { PL_ENTRY_MEDIA, PL_ENTRY_STILL, PL_ENTRY_STREAM };
enum pl_status_level { PL_STATUS_OK, PL_STATUS_WARNING, PL_STATUS_ERROR };

struct pl_entry {
	char *location;          /* UTF-8 absolute path or URL */
	char *title;             /* may be NULL */
	char *key;               /* R-KEY-1 base identity, never suffixed */
	uint32_t occurrence;     /* 1, 2, ... among identical keys, in source order */
	uint32_t source_ordinal; /* position as the container gave it; tie-breaker */
	char *previous_key;      /* set when renamed since the last publication (R-DIR-8) */
	enum pl_entry_kind kind;
	bool has_date;
	int64_t date_unix_ms;
	uint64_t size;           /* 0 when unknown */
};

struct pl_snapshot {
	uint64_t instance_id;      /* provider instance; registered by the source before create */
	uint64_t generation;       /* orders snapshots within one instance only */
	uint64_t content_revision; /* bumps only when entries changed; resets the full pass (R-BEH-5) */
	struct pl_entry *entries;  /* every filtered entry, sorted: membership (R-DIR-8) */
	size_t count;
	size_t *visible;           /* indexes into entries after the mode (newest only/N) */
	size_t visible_count;
	bool initial_scan_complete;
	char **pending_keys;       /* keys still in the write-complete gate (R-DIR-4, R-KEY-2) */
	size_t pending_count;
	enum pl_status_level level;
	char *status_message;      /* NULL when OK */
	int64_t last_ok_unix_ms;   /* 0 = never */
	bool stale;                /* last good list kept after a failure (R-BEH-3) */
};
void pl_snapshot_free(struct pl_snapshot *snapshot);

struct pl_container_options {
	bool include_subfolders;
	const char *extension_filter; /* R-DIR-5 format */
	enum pl_sort_key sort_key;    /* LIST, NAME, DATE */
	bool sort_descending;
	enum pl_mode mode;            /* FULL, NEWEST_ONLY, NEWEST_N */
	uint32_t newest_count;
	bool folder_check_enabled;    /* R-DIR-3, default true */
	uint32_t folder_check_interval_s; /* R-DIR-3, default 10 */
	uint32_t remote_refresh_s;    /* constant 300 until the Phase 3a setting exists */
	bool lock_list_at_start;      /* R-DIR-11 */
};

/* Called on the worker thread. Takes ownership of snapshot. Must not be called after
 * pl_provider_destroy returns. Must not create or destroy providers. */
typedef void (*pl_publish_fn)(void *owner, struct pl_snapshot *snapshot);

struct pl_provider;
/* Copies location, row_uuid, options and the filter string before returning. The caller has
 * already registered instance_id as the row's expected instance (R-THR-2). */
struct pl_provider *pl_provider_create(enum pl_row_kind kind, const char *location, const char *row_uuid,
				       uint64_t instance_id, const struct pl_container_options *options,
				       pl_publish_fn publish, void *owner);
void pl_provider_refresh_now(struct pl_provider *provider);
/* Filter, sort, direction, mode, N, folder check interval, refresh interval and lock-list-at-start:
 * applied to the running provider (view options re-applied to the cached raw list and republished;
 * intervals re-armed); no rescan, no re-fetch, the admitted-file cache is kept. */
void pl_provider_set_options(struct pl_provider *provider, const struct pl_container_options *options);
/* R-DIR-11: playback was triggered, or "Apply waiting changes now" was pressed; apply the kept
 * snapshot through the normal publish path. */
void pl_provider_release_held(struct pl_provider *provider);
/* Idempotent: signal, cancel blocking work, join, free unconsumed snapshots. */
void pl_provider_destroy(struct pl_provider *provider);
```

Only a change of a row's location or of "include subfolders" re-creates a provider. Replacement protocol (R-THR-3), only in `update` and `destroy`: under the list lock retire the old instance ids, detach the providers and register the replacement ids; release the lock; destroy and join the old providers; create the replacements. A location change also clears the row's entries and makes it awaiting (R-THR-4). `media_file_data` (playlist.h) gains an owned `struct pl_entry *entry` (bmem, released by `free_files` through `pl_entry_free`); item key, occurrence, kind, title, size and date are read from it, never duplicated; the upstream bmem fields stay as they are, and rows without an entry (upstream code paths, the shuffler tests) fall back to `filename`.

### 3.3 Threading and lifecycle (R-THR-1..5)

- List mutex: guards entries, current-item pointers and the shuffler everywhere, including `get_properties` and `save`. Never held while calling into libobs in a way that can call back (child updates, parent signals, `obs_source_save`) or while joining a thread.
- Player thread: one per source, created in `create`, the only code that changes the child. Producers (update, hotkeys, Properties buttons, `select_index`, `video_tick`, `publish`) append owned commands (`LOAD`, `PLAY_PAUSE`, `STOP`, `RESTART`, `CLEAR`, `SET_CHILD_SETTINGS`, each with copied strings) to the FIFO under the list lock and set the thread's wake event. The thread pops and executes until the FIFO is empty, then waits. Nothing is dropped by age. Shutdown: a `QUIT` command, then join. Ordering, wake and shutdown logic is `core/command-queue.c`, trace-tested (load then pause, load then settings change, quit while busy).
- Load handoff (on the player thread): blank the child's video (exact call settled by the Phase 2 step 6 spike), clear the audio queues and the event atomics, set the new load epoch and "auto-advance eligible", set "loading", write the full child settings kept in the player (`hw_decode`, `close_when_inactive`, `restart_on_activate`, `speed_percent`, `clear_on_media_end`) with the path, call `obs_source_update`, clear "loading"; if the path is the one already loaded, also `obs_source_media_restart` (upstream `should_restart`, src 218-243). End events raised while "loading" is set are discarded (they belong to the old media). Every executed index change sets the save-dirty flag. As built in step 1.2: libobs defers `obs_source_update` on a video source to that source's next tick (graphics thread) and then raises its `update` signal, so every child update waits for that signal (2 s cap, logged); a load is an unload (empty path, ends ignored until applied, since destroying media still opening reports ENDED) followed by the load, which also makes a same-path load restart the child without `obs_source_media_restart`.
- Our own stops: `STOP`, `CLEAR` and the hide-stop of a visibility behavior clear "auto-advance eligible" and any pending end before touching the child; `CLEAR` stops the child, never the parent (R-UP-72); `RESTART` is a new load (new epoch, new window). This replaces upstream's sticky `user_stopped` (R-UP-41, R-BEH-6).
- Child signal handlers (`media_started`, `media_ended`, the audio capture callback) take no lock and only write atomics: started-epoch, ended-epoch (unless "loading"), audio-seen-epoch.
- `video_tick` keeps its upstream audio work (fixed per R-UP-60) and, in this order each tick: (1) startup evidence for the current load (media time > 0, non-zero child size, audio-seen-epoch = current epoch) and the startup window, which advances only while playing and active, or Always play without close-when-inactive (R-BEH-5); (2) the still hold (R-DIR-9); (3) a pending end on an eligible load: decide the next entry under the list lock (bounded loop skipping empty and awaiting rows, R-KEY-2) and enqueue `LOAD`; (4) if "Lock list at start" is on and a playback trigger was recorded since the last tick (set by play, restart and activate; R-DIR-11), call `pl_provider_release_held` on each container row's provider (outside the list lock); the "Apply waiting changes now" button calls it directly; (5) the throttled save: if the save-dirty flag is set and a second has passed, `obs_source_save` with no plugin lock held. Parent signals (`obs_source_media_ended`) are emitted the same way, and `enum_active_sources` takes no lock.
- Snapshot apply runs inside `publish` on the provider's worker, under the list lock, in this order: (1) reject the snapshot unless its instance id is the row's registered one; (2) build the row's new `folder_items` from the snapshot's visible entries (owned `pl_entry` copies plus the upstream bmem fields) and its membership key set from all entries; (3) move the old `folder_items` into a local and install the new ones, `is_folder` set on every container row so `build_shuffled_files` flattens it; (4) `set_parents`; (5) for every entry with `previous_key`, re-key the matching old item in the local, and update the current-item and resume keys (renames held under R-DIR-11 are composed); (6) `shuffler_update_files(&files)`, which flattens the new items while matching against shuffler pointers that still point into the old items in the local (called on every list change, shuffle on or off, R-THR-4); (7) re-point `current_media` and `actual_media` into the new items, or, when the current item was evicted, to the source's owned playing copy (R-DIR-8); (8) free the local old items; (9) settle an awaiting row (R-KEY-2) and enqueue a `LOAD` if it auto-starts; release the lock; set save-dirty. With "Lock list at start" on, the provider keeps its newest snapshot instead of publishing, and publishes it when `pl_provider_release_held` is called.
- Provider stop is idempotent: set the stop event; the worker (sole owner of its directory and WinHTTP handles) wakes from `WaitForMultipleObjects`, `CancelIoEx`, `GetOverlappedResult(..., TRUE)`, closes the handle; async WinHTTP handles are closed by the worker and it waits for `HANDLE_CLOSING`; `CancelSynchronousIo` on the worker thread interrupts a scan or probe stuck on a stalled share; join; free. `destroy`, in order: destroy every provider; `QUIT` and join the player thread; disconnect the child's `media_started`/`media_ended` handlers and remove its audio capture callback (R-UP-70); release the child; free state.
- Paths are UTF-8 internally, canonicalized with `GetFullPathNameW` and given `\\?\` or `\\?\UNC\` only at Win32 calls (R-THR-5).

Status: each snapshot carries its row status; `playlister-source.c` writes the status block (`row_status`, R-LIST-5) when Properties opens and when Refresh is pressed, and logs every change (R-BEH-2). The player keeps its own failed-entry status (R-BEH-5).

### 3.4 Settings keys

| Key | Type | Default | Range | Since |
|-----|------|---------|-------|-------|
| `playlist` | array of {`value` string, `uuid` string} | empty | -- | upstream |
| `loop` | bool | true | -- | upstream |
| `shuffle` | bool | false | -- | upstream |
| `visibility_behavior` | int | 0 (stop/restart) | 0-3 | upstream |
| `restart_behavior` | int | 0 (current file) | 0-1 | upstream |
| `hw_decode` | bool | true (upstream: no default, so false) | -- | upstream; default on from Phase 1 (D-11) |
| `close_when_inactive` | bool | false | -- | upstream |
| `speed_percent` | int | 100 | 1-200 | upstream |
| `current_media_index` | int | 0 | -- | upstream (fallback only from Phase 2) |
| `current_folder_item_filename` | string | "" | -- | upstream; read by the 0 -> 1 migration, then no longer written |
| `current_file_name` | string (info text) | " " | -- | upstream |
| `select_file` | string | "0" (none) | `<row_uuid>:<occurrence>:<item_key>` from Phase 2 | upstream; default added (R-UP-65) |
| `schema_version` | int | 0 (registered as 0 so absence means 0) | 0-1 | Phase 2; set to 1 by the migration (R-LIST-4) |
| `current_row_uuid` | string | "" | -- | Phase 2 |
| `current_item_key` | string | "" | -- | Phase 2 |
| `current_item_occurrence` | int | 1 | >= 1 | Phase 2 |
| `row_kinds` | object: row uuid -> {"kind": "folder" (later "playlist", "url"), "path": string} | {} | -- | Phase 2 (R-LIST-1) |
| `row_status` | string (info text, transient) | "" | -- | Phase 2 (R-LIST-5) |
| `include_subfolders` | bool | false | -- | Phase 2 |
| `extension_filter` | string | `mp4 m4v mov mkv webm avi flv ts mpg mxf gif mp3 m4a mka aac ogg opus wav flac jpg jpeg png webp bmp` | R-DIR-5 format; text field | Phase 2 |
| `sort_key` | int | 2 (date; Directory Sorter parity, R-DIR-12) | 0 list, 1 name, 2 date | Phase 2 |
| `sort_descending` | bool | false | -- | Phase 2 |
| `container_mode` | int | 0 (full) | 0 full, 1 newest only, 2 newest N | Phase 2 |
| `newest_count` | int | 5 | 1-1000 | Phase 2 |
| `still_duration_s` | int | 10 | 1-3600 | Phase 2 |
| `folder_check_enabled` | bool | true | -- | Phase 2 (R-DIR-3; Directory Sorter "Check Interval") |
| `folder_check_interval_s` | int | 10 | 2-3600 | Phase 2 (R-DIR-3) |
| `lock_list_at_start` | bool | false | -- | Phase 2 (R-DIR-11, D-9) |
| `remote_refresh_s` | int | 300 | 30-86400 | Phase 3a (Phase 2 uses the constant) |

## 4. Phases

### Phase 0 -- this plan
Done when the operator approves REQUIREMENTS.md and PLAN.md and answers the decisions Phase 1 needs (D-1, D-2, D-3, D-5, D-6, D-8). D-7 and D-9 are needed before Phase 2, D-4 before Phase 3b.

### Phase 1 -- clean fork (identity, OBS 33, new layout)

Goal: obs-playlister builds against OBS 33.0.0, installs in the new layout, loads in OBS 33 without a Legacy flag, and everything in REQUIREMENTS section 3 still works. Version becomes 0.2.0. No file moves. No behavior changes beyond identity, metadata, layout, four shuffler crash fixes (R-UP-68, R-UP-73, R-UP-74, R-UP-75; R-UP-75 was found by the post-code QA) and hardware decoding on by default (D-11).

1. Repo hygiene first (R-STD-8). `.gitignore`: add `!REQUIREMENTS.md`, `!PLAN.md`, `!TESTING.md`, `!/tests`. (`!/third_party` comes with Phase 3b.)
2. Identity (R-ID-3, D-3). `buildspec.json`: `name` obs-playlister, `displayName` OBS Playlister, `version` 0.2.0 (read by the manifest and the load log line), `author`/`email`/`website` (D-3), new `uuids.windowsApp` GUID (new installer AppId). macOS `bundleId` left as is (bootstrap reads it; non-goal). `src/media-playlist-source.h:158`: source id `obs_playlister_source`. `src/media-playlist-source.c`: hotkey names `OBSPlaylister.*` (777-790) and the display-name locale key `OBSPlaylister` (449). `data/locale/en-US.ini`: rename that key, keep the rest. Settings keys are unchanged (per-source, no collision). `mps_defaults` registers `hw_decode` = true (D-11).
3. Module metadata (R-PLAT-9). `plugin-main.c`: define `obs_module_name()` ("OBS Playlister") and `obs_module_description()`. `cmake/common/bootstrap.cmake`: also parse `displayName` into `PLUGIN_DISPLAY_NAME` (bootstrap runs before `project()` and before the target exists, CMakeLists.txt 3-14, so it only parses). New `cmake/common/manifest.json.in`: static fields (`description`, `long_description`, `urls`, `has_banner: false`, `has_icon: false`, `os_arch`) in the template, `@PLUGIN_DISPLAY_NAME@`, `@CMAKE_PROJECT_NAME@`, `@PLUGIN_VERSION@` substituted. `set_target_properties_plugin` in `cmake/windows/helpers.cmake` (next to the `resource.rc.in` configure, lines 70-71) does `configure_file(... manifest.json @ONLY)` into the binary dir and registers it with the existing `target_add_resource()` (helpers.cmake 107-121), which installs it to `<name>/data/` and copies it to the rundir.
4. Build system for OBS 33 (R-PLAT-2/5/6/7, R-UP-56).
   - `buildspec.json` dependencies: obs-studio `33.0.0`, prebuilt and qt6 `2026-10-23` with the hashes in section 2.4; macOS hash entries removed together with the macOS CI job (D-5). Source-zip hash: `Invoke-WebRequest https://github.com/obsproject/obs-studio/archive/refs/tags/33.0.0.zip -OutFile <scratch>\33.0.0.zip`, `Get-FileHash -Algorithm SHA256`, repeat once and require the same hash, then delete the file.
   - `CMakePresets.json`: `"architecture": "x64,version=10.0.26100.0"`; generator stays `Visual Studio 17 2022`.
   - `cmake/common/buildspec_common.cmake`: build the nested configure arguments as CMake lists, one element per argument: `-A` and `${arch},version=...` as two elements; `-DCMAKE_SYSTEM_VERSION=...`, `-DENABLE_PLUGINS:BOOL=OFF`, `-DENABLE_FRONTEND:BOOL=OFF` and `-DENABLE_SCRIPTING:BOOL=OFF` as separate elements (replacing the ignored `ENABLE_UI` and the misspelled, swallowed `CMAKE_ENABLE_SCRIPTING`); `-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}` without the literal quotes, passed as one quoted CMake argument so its `;` survive.
   - `cmake/windows/helpers.cmake`: RUNTIME and LIBRARY destinations become `"${target}"` (dll and pdb next to `data/`). Rundir: dll and pdb to `rundir/<config>/${target}/`; both `target_install_resources` (lines 96-99) and `target_add_resource` (lines 115-116) copy to `rundir/<config>/${target}/data/` (today both land one level too high). No install or copy destination uses `bin/64bit`.
   - `cmake/windows/defaults.cmake` (this repo's still says `$ENV{APPDATA}`): default install prefix `$ENV{ALLUSERSPROFILE}/obs-studio/plugins`, as in template commit 013ffca, so an elevated `cmake --install` lands in the OBS 33 location; staging always passes `--prefix`.
   - `.github/scripts/Package-Windows.ps1`: delete `release-portable/<config>`, old `release-portable/<name>-*-windows-*.zip` files and the Inno `Package` staging dir before staging; create `release-portable/<config>/plugins/`; portable zip is `plugins/obs-playlister/{obs-playlister.dll, data/...}` (R-PLAT-5); drop the `obs-plugins/` and `data/obs-plugins/` copy.
   - `.github/scripts/Build-Windows.ps1`: delete `release/<config>` before `cmake --install` so no file from an older layout (or the upstream plugin name) is packaged; after the build, `Invoke-External ctest --test-dir build_x64 -C <config> --output-on-failure --no-tests=error` (R-TST-2).
   - `.github/workflows/build-project.yaml`: the "Upload Exe Artifacts" step runs only when `package` is true (no `.exe` is built otherwise).
   - `.github/workflows/build-project.yaml` and `push.yaml` (D-5): disable macOS and Ubuntu jobs and remove the `macos-universal`, `ubuntu-24.04-x86_64` and `sources` rows (the source tarball came from the Ubuntu job) and the `*.pkg`, `*.deb`, `*.ddeb`, `*.tar.xz` globs from the release job; Windows runner `windows-2022`, as in the template (the first push showed that the `windows-2025` label now runs the `windows-2025-vs2026` image, which has no VS 2022).
   - `PLUGIN-TEMPLATE.md`: record the deliberate divergences (install layout, manifest, OBS 33 configure flags and argument lists, default install prefix) so a future template sync does not revert them.
5. Test scaffold (R-TST-2/4, D-8). Root `CMakeLists.txt`: `option(OBS_PLAYLISTER_BUILD_TESTS ... ON)`, `enable_testing()`, `add_subdirectory(tests)`. `tests/test-shuffler.c`:
   ```c
   #undef NDEBUG
   #define TEST_SHUFFLER
   #include "shuffler.c"
   #include <stdlib.h>
   #include <windows.h>
   /* + the R-UP-73 and R-UP-75 regression tests, which reach shuffler internals through this include */
   int main(void)
   {
   	_set_error_mode(_OUT_TO_STDERR);
   	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
   	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   	return test_shuffler();
   }
   ```
   `tests/CMakeLists.txt`: the target links only `OBS::libobs` and has `src/` on its include path; it is registered with `add_test()`, and `set_tests_properties(test-shuffler PROPERTIES ENVIRONMENT_MODIFICATION ...)` prepends to `PATH` the directory of `OBS::libobs`'s imported location and the `bin` folder of every `CMAKE_PREFIX_PATH` entry that contains `share/obs-deps/VERSION`, computed at configure time. Nothing is copied and nothing comes from an installed OBS. shuffler.c edits in this step, all in R-TST-4's list and nothing else: the R-UP-68 guards on both operands (308 and 314), the R-UP-73 loop and saturating decrement, the R-UP-74 return, the R-UP-75 history bookkeeping in `shuffler_select_index` (175 and 181), `ArrayDeepCopy` (duplicate `id`, re-point each folder item's `parent_id`), the NULL check in `media_equal`, and the corrected expectation in `test_has_prev_next_empty`. A deliberately failing assert is tried once locally to prove the test can fail. If any test still fails after these, stop and decide with the operator whether it is a harness fault or a shuffler defect.
6. Copyright and docs (R-LIC-1/2). Add `Copyright (C) 2026 Jeffrey A. Brick` under Ian Rodriguez's line in every header touched; first header line becomes `obs-playlister (based on Media Playlist Source)`. `cmake/windows/resources/resource.rc.in:22`: `"(C) 2023 Ian Rodriguez, (C) ${CURRENT_YEAR} ${PLUGIN_AUTHOR}"`. LICENSE unchanged. README rewritten: what it is, attribution, the independent-reimplementation note (R-LIC-3), install layout, build steps, the unsigned-DLL note (R-PLAT-10), and the corrected `select_index` behavior (R-UP-5).
7. Build locally. First configure downloads obs-deps, Qt6 and the OBS 33 source into `.deps/` and builds libobs Debug and Release (expect 10-20 minutes once). Warnings are errors by default (R-UP-51):
   ```
   cmake --preset windows-x64
   cmake --build --preset windows-x64 --config RelWithDebInfo
   ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
   Remove-Item -Recurse -Force release/RelWithDebInfo -ErrorAction SilentlyContinue
   cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo
   ```
8. Install and verify in OBS 33 (after D-1): copy `release/RelWithDebInfo/obs-playlister` to `C:\ProgramData\obs-studio\plugins\` (elevated). Expect `[obs-playlister] plugin loaded successfully (version 0.2.0)` in the log; Plugin Manager shows "OBS Playlister 0.2.0" without a Legacy chip, while Media Playlist Source still shows one. Then the packaging paths: extract both zips and inspect their trees, run the Inno installer (D-2), and load the portable zip in a portable OBS.
9. `TESTING.md` v1: the Phase 1 smoke checklist with expected log lines (R-TST-6): load, no Legacy flag, version shown, Windows security (R-PLAT-10: `Get-AuthenticodeSignature` reports NotSigned as expected, the load line is present, Windows Security protection history and the Code Integrity log show no entry for `obs64.exe` or `obs-playlister.dll`), Properties labels show text not keys ("Loop Playlist", not `LoopPlaylist`), add 2 files, play, next/prev, loop, shuffle, the shuffle-and-loop crash paths (R-UP-73, R-UP-75), speed, each visibility behavior, restart behavior, hotkeys, missing-file dialog, OBS restart resumes the same item, `select_index` with an out-of-range index does nothing.
10. Post-code QA by a Claude Sonnet 5.5 agent on the Phase 1 diff (R-STD-9); fix confirmed findings and re-run `ctest`.
11. Commit (message cites R-IDs and the QA outcome), one push (R-STD-4), post-push check (R-STD-5).

Acceptance: build warning-free with warnings as errors; `ctest` passes (test-shuffler: 16 upstream tests plus the R-UP-73 and R-UP-75 regression tests and a randomized invariant test, each proven able to fail); every TESTING.md Phase 1 item passes on OBS 33.0.0; `C:\ProgramData\obs-studio\plugins\obs-playlister\` contains exactly `obs-playlister.dll`, `obs-playlister.pdb`, `data\manifest.json`, `data\locale\en-US.ini`; both zips and the installer produce that same tree; both plugins coexist; CI on the push is green for format and Windows.

Risks:
- R1: libobs 33 does not build with MSVC 14.44 (VS 2022). Mitigation: it is C17/C++17 and OBS 32 built with VS 2022; if it fails, temporarily remove `OUTPUT_QUIET` from the nested build in `buildspec_common.cmake` to see the errors, then stop and raise installing VS 2026 Build Tools (R-PLAT-6). Taking that path adds a `windows-x64-vs2026` configure preset with generator `Visual Studio 18 2026` and points the build preset at it; no silent workaround.
- R2: obs-studio 33's own dependency bootstrap may still try to download into its tree. Mitigation: we pin the identical obs-deps release and pass a clean `CMAKE_PREFIX_PATH`, so `_check_deps_version` finds a match; a second download costs time, not correctness.
- R3: GitHub source-archive hash drift. Mitigation: hash is computed from the file we actually fetch, twice; if a later configure fails the hash check, re-verify rather than loosen it.
- R4: the test's PATH does not cover a libobs runtime dependency. Mitigation: `dumpbin /dependents` on `obs.dll` names them (section 8); add the missing `.deps` folder to the same PATH list; still no copying and no change to the shuffler.

### Phase 2 -- one list, folders, still images, failure handling

Order matters: each step is releasable on its own.

1. Upstream defects and lock discipline, as five commits in this order, none changing features:
   1. Header split, a pure move: the `obs_source_info` definition (media-playlist-source.h 157-189), the static filter strings (86-89) and the static prototypes (91-155) go into the .c file; the header keeps the struct and enums. Check: it compiles and the load line appears.
   2. `player.c`: the player thread, command FIFO, load handoff, end attribution, same-path restart and "auto-advance eligible" (section 3.3, R-BEH-6). Every child call is routed through it, which takes `obs_source_update` and `obs_source_media_*` out from under the list lock; `obs_source_save` moves to the save-dirty path (R-UP-62). The child's handlers become lock-free. Check: full TESTING.md Phase 1. As built, this step also took R-UP-41 (`user_stopped` removed), R-UP-61 (empty folder rows skipped in a bounded loop), R-UP-63 (automatic advance ends the list exactly when next cannot move: folder items left, a later playable row, loop or the shuffler count), R-UP-64 (an empty folder row is never selected), R-UP-70 (destroy order) and R-UP-72 (CLEAR stops the child, parent signals outside the lock, enum without the lock), because the new threading depends on them or the QA found them in code this step changed; and the transport producers (play/pause, restart, stop) push under the list lock. Which end may advance is `core/load-tracker.c` (trace-tested): every load, stop or clear takes a request number in queue order; only the newest request's load is eligible, and a superseded load is skipped before it touches the child; a hide withdraws in the deactivate callback, because the stop it queues runs as a media action one tick later (OBS 33 queues `obs_source_media_*` too). Checked with `ctest` and the runtime harness (`tests/obs/`) in OBS 33.0.0.
   3. Only then add the lock to `get_properties`, `save` and the other unlocked readers (R-UP-44, R-THR-1). Check: `ctest` plus play, next, previous and Properties. As built: `update` builds the new list (folder listings included) before taking the lock, then swaps it in, moves the current-item pointers, updates the shuffler (on every list change, shuffle on or off, R-THR-4) and writes the current file name in one locked section, and frees the old list after unlock. The current row is matched against the built list (R-UP-77). An update that leaves the current folder row empty moves on under the same lock, or clears the child when nothing follows; for that, `play_media_at_index` no longer tests the stale `actual_media` (R-UP-33). Both mutexes are initialised before any callback that takes them is registered. The runtime harness gained Properties (the Select File list, Refresh Filename, Play Selected File) and save, which now take the lock, and a second source whose current row is an empty folder, which also covers shuffle around a list edit, a row with an empty path and an edit of the playing row.
   4. The remaining defects: R-UP-57 (dead locale macros, together with the locale-key test), R-UP-60 (audio), R-UP-65 to R-UP-67, R-UP-71 (the `media_next` signal Play Selected raises with nothing selected), R-UP-76 (shuffle `next` after a list edit), R-UP-78 (the state reported after a clear) and the R-UP-74 regression test. As built: the audio drain is `core/audio-queue.c` with a ctest; the locale-key ctest and its negative case run in CTest; `test-shuffler-release` builds the shuffler with `NDEBUG` for R-UP-74; Play Selected raises `media_next` only after it played something, with the calldata libobs gives its own media signals; the runtime harness checks Play Selected with nothing chosen, the Select File list with an empty folder, and the state after a clear. R-UP-67 has a manual TESTING.md item. Each gets its regression item. Check: full TESTING.md Phase 1.
   5. Move the file to `src/obs/playlister-source.c` and `src/obs/player.c`; CMakeLists `target_sources` and includes in the same commit. Check: it compiles and the load line appears. As built: `git mv` of both files and their headers; `src` became a private include directory of the plugin target, so every include keeps its path; the only code change is the source's own include line.
2. (Folded into step 1; the step numbers below are kept because other text cites them.)
3. Core modules with tests: `entry-list`, `natural`, `sort` (list order, natural name, date; R-DIR-10 ordering; `source_ordinal` ties), `filter` (R-DIR-5 format, whole extensions, always-skip list), `modes`, `keys`, `merge` (successor after delete, eviction vs deletion through membership, rename aliases including A -> B -> C composition), `folder-state` (write-complete: both admission paths, growing file, stable file, locked-but-stable file, vanished file, permanently locked file, batched samples, deadline queue; watch modes: notify, overflow, rescan-only, missing, recovery), `command-queue` (ordering, wake, quit while busy). As built, in three commits: (1) `entry`, `natural`, `filter`, `sort` (with a stable index merge sort the other modules share), `modes` and `keys`, each with its ctest; (2) `merge`; (3) `folder-state` (the write-complete gate and the watch-mode machine, driven by the provider with a monotonic and a wall clock). The modules are compiled into the plugin from the start, with its warnings as errors, though nothing calls them until step 4.
4. Keys, settings and the shuffler match. `media_file_data` gains the owned `pl_entry`. `find_media_index` (shuffler.c 296-316) and `find_folder_item_index` (src 320-332) compare parent row id, item key and occurrence when both items have an entry, else `filename` (so the upstream shuffler tests stay valid). Every list change calls `shuffler_update_files` (R-THR-4). The Select File dropdown stores identities (R-UP-20). Settings gain the Phase 2 keys in section 3.4 and the idempotent 0 -> 1 migration (R-LIST-4); resume uses `current_row_uuid`, `current_item_key` and `current_item_occurrence` with the awaiting rules (R-KEY-2); `row_kinds` is written when a folder is first detected; classification follows R-LIST-1 (playlist detection waits for Phase 3a). As built, in two commits: (1) items carry their entry and are found by key and occurrence (folder lookups, the shuffler, the source's current item), the Phase 2 settings and their defaults, the 0 -> 1 migration, resume and save by row uuid, key and occurrence; (2) Select File identities (`pl_selection_*` in `core/keys.c`; looked up and played under one hold of the list lock; a stale choice is reported, not played) and row kinds (`core/row-kind.c`: the URL rule and rules (1), (3) and (5); `row_kinds` rebuilt on every update and seeded by the migration); a row with a saved folder kind leaves the missing-files dialog (R-UP-34).
5. `provider-folder-win.c`: worker that alone opens and closes its directory handle (`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED`), overlapped `ReadDirectoryChangesW` (FILE_NAME, DIR_NAME, LAST_WRITE, SIZE; subtree per option) into a 64 KB DWORD-aligned buffer (the network limit), waits on {overlapped event, stop event, next candidate deadline}, 500 ms debounce, paired rename events, initial-scan probe of every file and an admitted-tuple cache that follows paired renames without a new wait (`previous_key`), Win32 paths canonicalized with `GetFullPathNameW` before the `\\?\` prefix (R-THR-5), the row root opened even when it is a reparse point and reparse points below it skipped (R-DIR-1), cloud placeholders skipped unopened (R-DIR-5), write-complete deadline queue with batched samples, `pending_keys` and `initial_scan_complete` in each snapshot, re-open of the handle on recovery, scans with `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)` so size and date come without opening files, key folding in UTF-16, watch-mode transitions and their log lines (R-DIR-3), the kept snapshot for "Lock list at start" (R-DIR-11), stop per section 3.3. UNC rows are classified here, never on the UI thread (R-LIST-1). To settle in this step: a file rewritten in place after admission (a render saved again under the same name) goes back through the gate (`core/folder-state.c` un-admits a changed tuple), but its entry keeps its key and place in the snapshot while the new version is pending, so the playing item and the shuffler history are not disturbed; the item is not loaded while it is pending, and that rule gets an R-DIR-4 note and a harness check. As built, in parts: (1) the provider interface (`providers/provider.h`, `provider.c`) and the folder worker (`provider-folder-win.c`) with a headless ctest on real temporary folders (`test-provider-folder`: listing and filters, a new file through the gate, a rename keeping its identity, a deletion, an in-place rewrite keeping its version, view and filter changes, lock list at start, subfolders with a hidden folder and a junction loop, a missing folder going and coming back); the gate keeps each file's last admitted version (`core/folder-state.c`), which settles the membership half of the in-place rewrite item. (2) The source integration: a registry of row providers kept across updates (R-THR-3), the snapshot apply on the worker under the list lock (renames, items, shuffler, the playing item by key; a removed playing item moves on, R-DIR-8), awaiting rows and the resume target with its 30 s wait (R-KEY-2), the lock-list triggers (R-DIR-11), and `update` no longer lists folders. (3) The in-place rewrite's other half (an item whose key is pending is not loaded: navigation, Play Selected and resume pass over or wait for it), UNC rows as folders without a disk question (R-LIST-1 rule 4, `pl_path_is_unc`) and their missing-files exclusion (R-UP-34). The owned playing copy for eviction comes with the newest modes in Properties (step 7).
6. Player features (`player.c`): first a spike that settles how to blank the child's video so its size reads 0 after a still, recorded in TESTING.md; then the startup window and progress-based success (R-BEH-5, fixes R-UP-69) with the active-only rule, audio evidence through the capture callback's epoch atomic, failure marks keyed by entry, size and date (file rows stat'ed at load), the full-pass breaker reset by `content_revision`; the still hold with the source's own state, duration and time, no seeking inside a still (R-DIR-9); the owned playing-entry copy and eviction (R-DIR-8). The real OBS 33 behavior for a missing file, a corrupt mp4, a corrupt jpg and a dead URL (which signals fire, in what order, and whether `media_started` is raised inside `obs_source_update`) is recorded in TESTING.md here. As built, in parts: (1) the spike (`tests/obs/spike-child-signals.lua`, findings in TESTING.md: the size is no evidence, so startup is judged by media time or audio and needs no blanking call), the startup window (`core/startup.c`), failure marks and the stop when nothing may play (R-BEH-5); (2) the still hold (R-DIR-9): `clear_on_media_end` written on every load, the hold started by the still's startup success, run by the source's own state, and reported as the source's state, duration and time; (3) the owned playing copy and eviction (R-DIR-8): an evicted playing item plays to its end from the copy, then its row goes on with its first visible item.
7. Properties: the shared "Folder and feed options" group (R-LIST-3, D-7) with the extension filter as a text field, the still duration in seconds, "Check folder every N seconds" (checkbox on, 10 s; R-DIR-3) "Lock list at start" with its tooltip and the "Apply waiting changes now" button (R-DIR-11, D-9); the status block (`row_status`, R-LIST-5) with the pending texts ("scanning", "waiting for N files to finish writing"); Refresh re-renders status (R-BEH-2). Every new label and message gets its en-US.ini key in the same commit, enforced by the locale-key ctest. The playlist Browse dialog gains a still-image filter group (R-UP-10); missing-files reporting skips folder and UNC rows (R-UP-34).
8. TESTING.md Phase 2:
   - OTR folder `C:\Users\jeffr\Documents\ComfyUI\output\otr\obs\` (173 mp4, 100-300 MB) in full, newest-only and newest-5 modes, with the initial scan finishing in seconds.
   - Directory Sorter parity (R-DIR-12): with default options the play order matches what the script produced (date modified, ascending), a new render appears within about 3 s, the 10 s folder check runs (log shows the rescans at debug level) and still finds a file when notifications are off (a share in `rescan-only`); with "Lock list at start" on, a new render waits ("1 change waiting") until the next Play/Restart/show or the "Apply waiting changes now" button, and no Stop is ever needed.
   - Write-complete: a slow copy of a 300 MB file does not play until done; a file held open for write does not join; OBS started while a 300 MB copy into the folder is in progress starts playing the admitted files within about 3 s and adds the copy when it finishes; one permanently write-locked file does not block the others or resume.
   - Changes while playing: delete the playing file; rename the playing file; case-only rename; rename A -> B -> C with shuffle on and "Lock list at start" on, then "Apply waiting changes now"; newest-only receiving a newer file while the old one plays; change a shared option mid-play (playback keeps its place, and a sort or mode change causes no rescan).
   - Watching: overflow by dropping 2,000 small files at once; a UNC path via a local share; delete the watched folder and restore it; unplug a removable drive; OBS restart while the watched folder is missing, then restore it (the row recovers as a folder); a row root that is a junction; a stalled share during a scan, then remove the source (teardown must not wait for a network timeout; if a stall cannot be staged here, record it as a residual risk).
   - Startup and resume: OBS restart resumes inside a watched folder; a brand-new source whose only row is a folder starts by itself once the scan lands.
   - Transport: load then immediate pause from a hotkey (both happen); Stop never advances; a one-item list loops by restarting the same file; Play Selected on the current item restarts it; Play Selected with "No File Selected" does nothing.
   - Failures and stills: a missing file and a corrupt mp4 in the list (each skipped within its startup window); a list where every entry is corrupt (auto-advance stops with an error); stills with the 10 s hold, a corrupt jpg, image-to-video transitions (no lingering frame); a corrupt file loaded right after a still is skipped; a hidden or paused source never times out an item; close-when-inactive with Always play does not time out while hidden.
   - File access (R-PLAT-10, R-DIR-5): a test folder with a deny-read ACL shows "access denied" with the Win32 code; a OneDrive folder with cloud-only files shows the placeholder warning and starts no download; a fresh 300 MB render is admitted once Defender's real-time scan releases it.
   - Upstream regressions: next onto an empty folder row, a single folder row with loop off plays every file, Play Selected with nothing chosen, `select_index` into an empty folder row, shuffle over mixed file and folder rows, audio present from the first second.
   - The recommended producer contract (temp name then rename).

Acceptance: `ctest` covers every Phase 2 core module, the locale keys and the shuffler; all TESTING.md Phase 1 and 2 items pass; the Python script is no longer needed for the OTR scene.

### Phase 3 -- remote sources, in three increments

3a. Playlists and URL rows. Classification gains playlist files and http(s) URLs (R-LIST-1); the Browse dialog gains a playlist-file group; the refresh-interval setting and property appear (section 3.4). `target_link_libraries(... PRIVATE winhttp)` in the commit that adds `fetch-winhttp.c`. `fetch.h` is two-phase: headers (status, normalized content type, content length, final URL) first, then cancel, bounded prefix, or full body. `fetch-winhttp.c`: async WinHTTP GET, callback flags ALL_COMPLETIONS, REDIRECT and HANDLES; 10 s resolve/connect/send and 30 s receive timeouts and a 60 s overall deadline; max 5 redirects; final URL from `WINHTTP_OPTION_URL`; 16 MB cap for list bodies only, counted after `WINHTTP_OPTION_DECOMPRESSION`; truncation judged from the request error and from `Content-Length` only without `Content-Encoding`; `User-Agent: obs-playlister/<version>`; the worker alone closes its handles and waits for `HANDLE_CLOSING`; one result code per fetch (R-REM-5). `provider-list.c`: classification steps (3) and (4) (reading at most 64 KB to sniff; a direct-media result closes the request without reading the body and is not a size error), one fetch in flight, coalesced refreshes, last-good retention and the stale flag, interval changes applied in place (R-REM-4). Rows that resolve to one direct entry or one HLS stream skip filter, sort, mode and refresh. `parse-m3u.c` (BOM, CRLF, `#EXTINF`, Windows-path vs RFC 3986 resolution, comments, HLS detection -> single stream entry) and `parse-pls.c`, with fixtures including malformed inputs. Tests use a fetch stub; `tests/manual/serve-fixtures.ps1` serves fixtures, redirects, a hang, an oversized body and gzip for TESTING.md.
3b. Feeds. `core/date.c` (RFC 822/2822 with named zones and numeric offsets, ISO 8601, invalid -> dateless), `core/xml-reader.h` over the D-4 parser in `third_party/` (+ `third_party/.clang-format` with `DisableFormat: true`, + `.gitignore` allowlist, R-STD-8), `parse-feed.c` (RSS 2.0 + Atom, enclosures, guid/id keys with occurrences, the R-REM-2 filter rule, mixed dated and dateless items) dispatched from `provider-list.c`.
3c. HTTP index. `parse-http-index.c` (href HTML-entity decoding, percent-decoded display names) with fixtures for Apache, nginx autoindex, IIS and Python `http.server`; dateless (R-DIR-10); dispatched from `provider-list.c`.
TESTING.md Phase 3: a local M3U, an M3U served by `serve-fixtures.ps1`, an extensionless M3U and PLS URL identified by Content-Type, a direct MP4 URL (must play without being downloaded first), a live stream URL, an HLS URL, a public podcast RSS, an nginx-style index, an unreachable URL, a hostname that does not resolve, a 429 response (last-good kept, error shown), a URL that returns HTML that is not an index, a gzip response at and over the size cap, a redirect chain, the 60 s overall deadline, removing the source while a fetch hangs in each of resolve, headers and body (must return at once, and no callback afterwards).

Acceptance per increment: its parsers and helpers are covered by `ctest`; its TESTING.md items pass; no new DLLs ship.

### Phase 4 -- VJ modes (boundary only)

Nothing is built. The only commitment is R-VJ-2: everything that touches the child source lives in `player.c` and its thread (Phase 2 steps 1.2 and 6), and providers never know how entries are shown.

## 5. Test strategy summary (R-TST)

- Unit (CTest, headless, libobs util only): every `core/` module, with fixtures as files under `tests/fixtures/`, including malformed inputs for each parser. Async behavior is tested through its pure parts: merge, keys, folder state machines and the command queue with injected events and clocks; fetch is a stub.
- Locale keys: `tests/check-locale-keys.cmake` via `cmake -P`, with a negative case.
- Shuffler: `test-shuffler` wraps `shuffler.c` and links libobs like every other test (D-8); it is shown to fail on a deliberately broken assert once.
- Integration (manual, TESTING.md): anything that needs libobs, the child source, real file system events or a network. Each item names its expected log line (R-TST-6).
- Run after every code change: `ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error`. CI runs the same on Windows.

## 6. Repo hygiene per change (R-STD)

- After coding, before commit: `ctest` green, then post-code QA by a Claude Sonnet 5.5 agent (R-STD-9) on the step's diff with the R-IDs it claims; confirmed findings fixed, `ctest` re-run, QA outcome noted in the commit message.
- Before commit: no BOM, no zero-byte file, banned word absent, clang-format 17.0.3 applied if installed, new top-level paths allowlisted in `.gitignore`.
- Push: exactly one `git push origin master`. On failure, hand over:
  ```
  cd "C:\Users\jeffr\Documents\obs-playlister"
  git status
  git push origin master
  ```
- After push: `git rev-parse HEAD` equals `git rev-parse origin/master`; BOM and zero-byte scans clean.

## 7. Order of work

Phase 1 is one commit series on `master` (personal fork, no PR flow). Phases 2 and 3 each start with a REQUIREMENTS.md check (R-STD-6) and end with a version bump (0.3.0 after Phase 2; 0.4.0, 0.5.0, 0.6.0 after 3a, 3b, 3c). Nothing in Phase 2 starts until the Phase 1 acceptance list is green on OBS 33.0.0. (Waived by the operator on 2026-10-24: Phase 2 started with only TESTING.md 1.1 and the packaging checks done; the remaining Phase 1 items run on Phase 2 builds.) Decisions are needed at the first phase that depends on them (REQUIREMENTS section 14).

## 8. Verify-at-build checklist

Each item names the phase, the check, and the expected result. Anything that cannot be staged on this machine is recorded in TESTING.md as a residual risk, not marked verified.

| # | Phase | Check | Expected |
|---|-------|-------|----------|
| V1 | 1 | libobs 33 and `obs-frontend-api` build with MSVC 14.44 + SDK 26100 (R1) | `.deps` holds `obs.dll` and `lib/cmake/libobs/`; otherwise stop at decision R1 |
| V2 | 1 | `.deps/obs-studio-33.0.0/build_x64/CMakeCache.txt` after the first configure | `ENABLE_PLUGINS:BOOL=OFF`, `ENABLE_FRONTEND:BOOL=OFF`, `ENABLE_SCRIPTING:BOOL=OFF`, `CMAKE_PREFIX_PATH` without quote characters, `CMAKE_SYSTEM_VERSION` without a `-D` inside |
| V3 | 1 | SHA-256 of `33.0.0.zip` from two downloads | identical; pinned in buildspec.json |
| V4 | 1 | `dumpbin /dependents` on `.deps/.../obs.dll`; run `ctest` from a shell with a minimal PATH | every dependency is in the test's PATH list; `test-shuffler` passes without an installed OBS on PATH |
| V5 | 1 | one temporary failing assert in `test-shuffler` | ctest reports Failed, non-zero exit, no dialog |
| V6 | 1 | `dumpbin /exports obs-playlister.dll` | `obs_module_name`, `obs_module_description` listed |
| V7 | 1 | Plugin Manager in OBS 33.0.0 | "OBS Playlister 0.2.0", no Legacy chip; Media Playlist Source shows one. Record which manifest fields are shown and the accepted `os_arch` value; if no version appears, check where OBS looked for `manifest.json` before changing anything else |
| V8 | 1 | Properties labels | text from en-US.ini, never raw keys |
| V9 | 1 | packaging paths | both zips extract to the R-PLAT-2/5 trees; the Inno installer installs the same tree; the portable zip loads in a portable OBS |
| V10 | 1 | R-PLAT-10 | NotSigned reported; load line present; no protection-history or Code Integrity entries |
| V11 | 1 | CI on the first push | `windows-2022` builds with VS 2022 and SDK 26100 (or a decision is raised; the first push failed on `windows-2025`, now a VS 2026 image, and in gersemi 0.21 on its warning about `.gersemirc` and on reformatting the `target_sources` block, whose `# cmake-format` fence gersemi 0.21 ignores); ctest step passes with `--no-tests=error`; Upload Exe gated on `package`; release graph has no macOS/Ubuntu rows |
| V12 | 1 | clang-format 17.0.3 availability on Windows | installed and used; otherwise a red format job is fixed by one formatting-only commit (R-STD-3) |
| V13 | 1 | shuffle order across two OBS restarts (R-UP-12) | recorded; if identical every start, raise seeding as a decision rather than change it silently |
| V14 | 2 | how to blank the child before a load (step 6 spike) | after the blank, the child's width reads 0 within one tick, so a previous still's size cannot count as success |
| V15 | 2 | OBS 33 failure signatures for a missing file, a corrupt mp4, a corrupt jpg, a dead URL | which of `media_started`/`media_ended` fire, in what order, and whether `media_started` is raised inside `obs_source_update`; the attribution rule (R-BEH-5) is checked against the recording |
| V16 | 2 | LOAD latency on the player thread and frame pacing (each child update now waits for the child's deferred update, normally one frame; a 2 s timeout is logged) | a local OTR mp4 loads without dropped frames in the Stats window; the graphics thread never waits on a load |
| V17 | 2 | signal disconnect and audio-callback removal at destroy | no callback runs after `destroy` returns (stress: remove the source during playback 50 times) |
| V18 | 2 | `CancelSynchronousIo` against a stalled SMB scan or probe | teardown returns without waiting for a network timeout, or the case is recorded as a residual risk |
| V19 | 2 | `ReadDirectoryChangesW` on a UNC share with the 64 KB buffer | notifications arrive, or the folder reports `rescan-only` with its log line |
| V20 | 2 | a folder item path longer than 260 characters | the child plays it, or the limit is documented in README and TESTING.md |
| V21 | 3a | WinHTTP async cancel and close | no callback after provider destroy; HANDLE_CLOSING always seen; the 60 s deadline and the decompressed size cap both enforced (`serve-fixtures.ps1`) |
