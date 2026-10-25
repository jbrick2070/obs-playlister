# OBS Playlister

A playlist source for OBS Studio 33 and later. It plays a list of local files,
folders and URLs through OBS's own Media Source, and editing the list does not
restart the clip that is playing.

OBS Playlister is a fork of [Media Playlist Source](https://github.com/CodeYan01/media-playlist-source)
by Ian Rodriguez, and it installs next to it without either one replacing the
other. The plan is for it to also replace the Directory Sorter script (watched
folders that keep themselves up to date) and to add remote playlists; see
[PLAN.md](PLAN.md) and [REQUIREMENTS.md](REQUIREMENTS.md).

## Status

Version 0.3.0 (Phase 2): folder rows follow their folders as files are added,
renamed or deleted, and a file still being written joins once it is complete;
folders can be sorted by date or name and shown in full, newest only or newest
N; "Lock list at start" holds changes until the next Play (or "Apply waiting
changes now"); Properties shows a
status line per row; entries that cannot play are skipped; still images are
held for a set time. The manual checks in OBS (TESTING.md) are still pending.
Version 0.2.0 (Phase 1) was the original plugin renamed and built for OBS 33.
Remote playlists come in Phase 3.

## Features (from Media Playlist Source)

- Edit the playlist without restarting the video, even if files are reordered.
- Edit any setting without restarting the video.
- Remembers the current file, so it plays again when OBS restarts.
- Play a chosen file or folder item from the list.
- Shuffle (based on VLC 4's implementation): adding or removing items does not
  reshuffle, Previous still works after you pick a file, and it reshuffles at
  the end of a pass without losing history.
- Shows the current file in the Properties window.
- Restart behavior: replay the current file or start from the first.
- Uses hardware (GPU) decoding when available, on by default.

## Install

Requires OBS Studio 33.0.0 or later on Windows x64. Older OBS versions do not
load this layout.

- **Zip:** unzip so that you get
  `C:\ProgramData\obs-studio\plugins\obs-playlister\obs-playlister.dll` and
  `C:\ProgramData\obs-studio\plugins\obs-playlister\data\`. Copying into
  ProgramData may need administrator rights.
- **Portable OBS:** unzip the portable zip into the OBS folder, giving
  `<OBS folder>\plugins\obs-playlister\`.
- **Installer:** run the `.exe`. It is not code-signed, so Windows SmartScreen
  shows "Windows protected your PC / Unknown publisher"; choose
  **More info, then Run anyway**. The zip installs without that prompt.

The DLL is unsigned. It loads with Windows' default settings, but it would be
refused if Smart App Control or a Windows Defender Application Control policy
for user-mode code were turned on.

In OBS, add a source of type **OBS Playlister**.

## Limitations

- The Properties window shows the current file, but it cannot update by itself
  when a video ends, because that can crash OBS while the window is open.
  Reopen the window or press Refresh Filename.
- No audio track or subtitle selection.
- Folder contents refresh when the settings are saved again (automatic
  watching arrives in Phase 2).

## For developers

The source id is `obs_playlister_source`. To see the setting keys, use
[obs_data_get_json](https://docs.obsproject.com/reference-settings#c.obs_data_get_json)
on the source's settings or look at the scene collection JSON.

To select a file from a script or another plugin:

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
struct calldata cd = {0};
calldata_set_int(&cd, "media_index", 3);       // 4th row of the playlist
calldata_set_int(&cd, "folder_item_index", 3); // 4th item of that row if it is a folder
proc_handler_call(ph, "select_index", &cd);
calldata_free(&cd);
```

- `media_index`: 0-based row in the playlist. If it is past the end of the
  list, the call does nothing (the original README said it falls back to 0; the
  code never did).
- `folder_item_index`: 0-based item in the folder at `media_index`. If it is
  past the end of the folder, item 0 is played. It is ignored for rows that are
  not folders.

## Building

Windows 11 x64 with Visual Studio 2022 (or newer) C++ build tools, Windows SDK
10.0.26100 and CMake 3.28 or newer. From the repository root:

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo
```

The first configure downloads the OBS 33.0.0 source and the matching obs-deps
into `.deps/` and builds libobs once. Manual OBS checks are listed in
[TESTING.md](TESTING.md).

## License and credits

GPL-2.0-or-later (see [LICENSE](LICENSE)).

- Media Playlist Source: Copyright (C) 2023 Ian Rodriguez. This project keeps
  his notices; his original project is at
  https://github.com/CodeYan01/media-playlist-source.
- The shuffler is based on VLC's playlist randomizer.
- obs-playlister changes: Copyright (C) 2026 Jeffrey A. Brick.
- The folder-watching feature planned for Phase 2 is written from scratch. It
  does not use code from the Directory Sorter for OBS script, which has no
  license.
