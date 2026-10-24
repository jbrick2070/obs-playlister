# obs-playlister -- Manual test checklist

Automated tests run with:

```
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
```

## Runtime harness (automated, needs OBS)

`tests/obs/run-harness.ps1` runs `tests/obs/playlister-harness.lua` inside a portable OBS 33 and drives one
OBS Playlister source through: play, auto-advance at the end of a file, next and previous, stop (no advance),
play after stop, pause, seek, restart, a speed change, hide and show (stop/restart and stop/play-next), end of
the list with loop off, wrap-around with loop on, next then pause in a row, and removal. It needs a portable
OBS 33 (with `portable_mode.txt`) and three local media files of different durations, and exits with 0 only
when every check passed.

```
.\tests\obs\run-harness.ps1 -ObsRoot <portable OBS 33 folder> -Files <a.mp4>, <b.mp4>, <c.mp4>
```

| Date | OBS | Build | Result |
|------|-----|-------|--------|
| 2026-10-24 | 33.0.0 portable | Phase 2 step 1.2 | 23 passed, 0 failed, no memory leaks |

## Manual checks

This file lists what needs OBS itself. Each item names the expected
`[obs-playlister]` log line where there is one (REQUIREMENTS R-TST-6). OBS logs
are in `%APPDATA%\obs-studio\logs\`. Record the date, OBS version and result
next to each item.

## Phase 1 (version 0.2.0)

Setup: OBS Studio 33.0.0 installed (D-1). Plugin copied to
`C:\ProgramData\obs-studio\plugins\obs-playlister\` from
`release/RelWithDebInfo/obs-playlister`, so the folder holds exactly
`obs-playlister.dll`, `obs-playlister.pdb`, `data\manifest.json` and
`data\locale\en-US.ini`.

| # | Check | Expected | Result |
|---|-------|----------|--------|
| 1.1 | Start OBS 33 and open the log | `[obs-playlister] plugin loaded successfully (version 0.2.0)`; module list shows `obs-playlister.dll`; source list shows `'OBS Playlister' (obs_playlister_source)` | 2026-10-24, OBS 33.0.0 portable (plugin in `plugins\obs-playlister\`): pass. Load line logged; the Plugin Manager's modules.json lists OBS Playlister 0.2.0 with source `obs_playlister_source`; clean exit, 0 leaks. Not yet run on a system install. |
| 1.2 | Tools > Plugin Manager | "OBS Playlister", version 0.2.0, no Legacy chip; Media Playlist Source still shows its Legacy chip. Note which manifest fields are displayed (PLAN V7) and whether the Discover tab is still "Coming soon" (D-10) | |
| 1.3 | Windows security (R-PLAT-10) | `Get-AuthenticodeSignature` on the DLL reports NotSigned; Windows Security protection history and the Code Integrity log show nothing for `obs64.exe` or `obs-playlister.dll` | |
| 1.4 | Add an "OBS Playlister" source; open Properties | Labels read "Loop Playlist", "Shuffle", "Use hardware decoding when available" (checked by default, D-11), never raw keys | |
| 1.5 | Add two video files; play | first file plays; Current File shows `1: <path>` after Refresh Filename | |
| 1.6 | Hardware decoding | Task Manager > GPU shows "Video Decode" activity while a file plays | |
| 1.7 | Next / Previous (media controls and hotkeys) | moves between the two files; hotkeys appear under the source as "Next" / "Previous" | |
| 1.8 | Loop on, let the last file end | wraps to the first file | |
| 1.9 | Loop off, let the last file end | stops; restart plays again per Restart behavior | |
| 1.10 | Shuffle on, Loop on, two files: both play once, then one starts the second pass; pick the other file in Select File, press Play Selected File and let it play to the end (R-UP-75) | no crash; a new shuffled pass starts | |
| 1.11 | Shuffle on, Loop on, three files: all three play once; while the fourth file plays (the first of the second pass), remove it from the list, then add it back (R-UP-73) | no crash at either edit; playback continues | |
| 1.12 | Speed 50% and 150% | playback speed changes (the video restarts, as the warning says) | |
| 1.13 | Each visibility behavior: hide and show the source | Stop/restart, Pause/unpause, Always play, Stop/play next behave as named | |
| 1.14 | Restart behavior: current file vs first file | Restart replays the current file, or starts at file 1 | |
| 1.15 | Add a folder row; play | its files play in order; Select File lists them as `N-M: <path>` | |
| 1.16 | Play Selected File with a chosen item | jumps to that item | |
| 1.17 | Move a listed file away, restart OBS | the missing-files dialog offers to find or remove it | |
| 1.18 | Close OBS while item 2 plays, start OBS again | item 2 resumes | |
| 1.19 | Script `select_index` with `media_index` past the end | nothing happens (R-UP-5) | |
| 1.20 | Both plugins at once | a Media Playlist Source and an OBS Playlister source work side by side | |
| 1.21 | Packaging (D-2, PLAN V9) | each zip extracts to the layout above; the Inno installer (unsigned, SmartScreen prompt expected) installs the same tree; the portable zip loads in a portable OBS 33 | |

## Known residual risks

- The plugin DLL and installer are unsigned (R-PLAT-10, D-10).
- Upstream defects R-UP-60 to R-UP-67, R-UP-69 to R-UP-72 and R-UP-76 are still
  present in 0.2.0 and are fixed in Phase 2 step 1 (for example an empty folder
  row can deadlock next/previous, R-UP-61). Until then, avoid empty folder rows,
  and with shuffle on, do not edit the playing row's path in the same Apply as
  other list changes (R-UP-76).
