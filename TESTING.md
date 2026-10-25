# obs-playlister -- Manual test checklist

Automated tests run with:

```
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
```

## Runtime harness (automated, needs OBS)

`tests/obs/run-harness.ps1` runs `tests/obs/playlister-harness.lua` inside a portable OBS 33 and drives one
OBS Playlister source through: play, auto-advance at the end of a file, next and previous, stop (no advance),
play after stop, pause, seek, restart, a speed change, hide and show (stop/restart and stop/play-next), end of
the list with loop off, wrap-around with loop on, next then pause in a row, Properties (the Select File list,
Refresh Filename, the identities Select File stores, Play Selected File with nothing chosen, with a position
saved before Phase 2 (reported, not played) and with item 1 chosen, counting `media_next` signals)
and save, and removal; then a second source whose saved current row is an empty folder: it must move on to the
next row, the Select File list must leave the empty folder out, next and wrap must pass over the empty row, next
with shuffle on and loop off must leave the last row, removing the playing row while turning shuffle off must fall
back past the empty row, a row with an empty path added above the playing row must neither move nor restart it,
editing the playing row's path must play the new file, and when the playing row becomes an empty folder the
source must report stopped and next must still play the file after it. The runner fails the run if the empty
folder is ever loaded as a file. A third source has a folder row (the runner makes a temporary folder of hard links
to the three files): settings saved before Phase 2 must migrate and resume on the saved file, save must write the
new resume keys, and a restarted source must resume by row uuid and key, save the folder row's kind, list the
folder items by identity, report a choice whose occurrence is gone (or a folder row's own identity) and play a
folder item it names, then keep playing it while a file in its folder is renamed (the list follows in name
order) and while a new file is copied in (it joins once admitted), pass over a harness copy rewritten in place while it is
still being written and play it once admitted, and with loop off keep the ended last item through a folder change; its migrated predecessor uses a junction named like a media file, which only the kind the
migration saves keeps a folder. A fourth source has a row saved as a folder that does not exist: it must stay a
folder and be passed over (the runner fails the run if a folder path is ever loaded as a file), and a row saved
without a uuid must be given one and play when chosen by it. Two more sources check R-BEH-5: a missing file and a
file that is no media are skipped for the file after them, and a list of only those stops instead of cycling (Play
tries them once more and stops again); a corrupt still fails by its 5 s startup window and the list stops; the runner
fails the run unless the corrupt file was loaded exactly 3 times and the corrupt still once. It needs a portable
OBS 33 (with `portable_mode.txt`) and three local media files of different durations, and exits with 0 only
when every check passed.

```
.\tests\obs\run-harness.ps1 -ObsRoot <portable OBS 33 folder> -Files <a.mp4>, <b.mp4>, <c.mp4>
```

| Date | OBS | Build | Result |
|------|-----|-------|--------|
| 2026-10-24 | 33.0.0 portable | Phase 2 step 1.2 | 23 passed, 0 failed, no memory leaks |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 1.3 | 37 passed, 0 failed, no memory leaks. Mutation checks: with upstream's `actual_media` test put back into `play_media_at_index`, 5 checks fail; with upstream's settings-array index in `update` (R-UP-77), 3 fail |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 1.4 | 44 passed, 0 failed, no memory leaks. Mutation checks: with Play Selected raising `media_next` every time (R-UP-71), 1 check fails; with upstream's folder test in the Select File list (R-UP-66) and upstream's `get_state` (R-UP-78), 2 fail. Upstream's Play Selected with nothing chosen (R-UP-65) crashes, so it was not run |
| 2026-10-24 | 33.0.0 portable, beside a running OBS 33 | Phase 2 step 4 (part 1) | 52 passed, 0 failed, no memory leaks, in three runs in a row. Mutation check: without the R-UP-80 save guard the resume chain fails (5 checks). With another OBS running the portable copy's video thread sometimes stalls for seconds ("the media source has not applied an update for 2 s"); load waits allow 15 s. After the second QA pass (pause held across newer commands, progress as a third distinct time): 52 passed, 0 failed, no memory leaks, three runs in a row, no OBS running beside it, no startup-window warning |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 4 (part 2) | 69 passed, 0 failed, no memory leaks, three runs in a row; the runner's temporary folders and junction removed and the three media files unchanged. Mutation check: without the migration saving folder kinds, 5 checks fail and the runner reports the junction loaded as a file (314 loads, as loop is on and the unopenable row is the only one: the full-pass stop of R-BEH-5 is Phase 2 step 6) |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 5 (part 2) | 76 passed, 0 failed, no memory leaks, three runs in a row; folder rows now come from the folder provider asynchronously, and every earlier folder check passed unchanged |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 5 (part 3) | 79 passed, 0 failed, no memory leaks, three runs in a row. Mutation check: with the pending check disabled, Next loads the file still being rewritten (1 check fails) |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 6 (part 1) | 84 passed, 0 failed, no memory leaks, three runs in a row (before the QA fixes; see the next row); the corrupt file loaded 3 times. Mutation check: with the failure marks ignored, the list of failing entries cycles (31 loads of the corrupt file) and 2 checks plus the runner's load guard fail |
| 2026-10-24 | 33.0.0 portable | Phase 2 step 6 (part 1), after QA | 86 passed, 0 failed, no memory leaks, three runs in a row; the corrupt file loaded exactly 3 times and the corrupt still once. Mutation check: without a still's end waiting for its startup, the corrupt still loads 50 times and the run fails |

## OBS 33 child behavior (Phase 2 step 6 spike)

`tests/obs/spike-child-signals.lua` (run with `run-harness.ps1 -Script spike-child-signals.lua`, with
`PLAYLISTER_SPIKE_JPG` naming a valid JPEG) drives a plain Media Source and logs every media signal and every change
of state, time, duration and size against the moment of each update. Recorded 2026-10-24 in portable OBS 33.0.0:

| Case | What OBS 33 does |
| --- | --- |
| Any load | `obs_source_update` returns at once; on the next tick the update is applied, `media_started` fires (for every case below, a missing file included) and then the `update` signal |
| Valid mp4 | media time past 0 about 30 ms after the update, video size about 100 ms after |
| Missing file | `media_ended` about 20 ms after the update; state ENDED, time 0, size 0 |
| Corrupt mp4 | `media_ended` about 3 ms after the update; time 0 |
| Valid jpg, `clear_on_media_end` on (the default) | `media_ended` about 3 ms after the update; time 40 afterwards; the size never shows (the frame is cleared at the end) |
| Valid jpg, `clear_on_media_end` off | ends the same way; time 40, and the size stays (320x180) |
| Empty path after a kept jpg | the size stays: an unload does not blank a latched still |
| `obs_source_media_stop` after a kept jpg | `media_stopped`, state STOPPED, size 0 |
| Corrupt jpg | `media_ended` about 2 ms after the update; time 0 |
| Dead URL (`http://127.0.0.1:9/...`) | reports PLAYING with time 0 for 5.5 s, then `media_ended`; OBS starts it again by itself about 10 s later (another `media_started`) |
| Valid mp4 after the URL | for about 60 ms the size reads the earlier jpg's (320x180), then the mp4's |

So `media_started` proves nothing (R-BEH-5), the media time separates every case (a valid jpg reads 40, a corrupt one
0), and the video size is no evidence: it can come back from an earlier still. The player judges startup by the time
(or audio) only, so it needs no blanking call; `obs_source_media_stop` would blank, but as a queued media action it
could reach the next load.

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
| 1.1 | Start OBS 33 and open the log | `[obs-playlister] plugin loaded successfully (version 0.2.0)`; module list shows `obs-playlister.dll`; source list shows `'OBS Playlister' (obs_playlister_source)` | 2026-10-24, OBS 33.0.0 portable (plugin in `plugins\obs-playlister\`): pass. Load line logged; the Plugin Manager's modules.json lists OBS Playlister 0.2.0 with source `obs_playlister_source`; clean exit, 0 leaks. 2026-10-24, OBS 33.0.0 system install (plugin in `C:\ProgramData\obs-studio\plugins\obs-playlister\`, Phase 1 build): pass. Load line logged next to `[media-playlist-source] plugin loaded successfully (version 0.1.3)`. |
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
| 1.17a | As 1.17, but before acting in the dialog add a row to the playlist and apply, then point the dialog at the moved file (R-UP-67, Phase 2 step 1.4 build) | the row is relocated; no crash | |
| 1.17b | Add a folder row and apply, close OBS, rename that folder, start OBS (R-UP-34, R-LIST-1; Phase 2 step 4 build) | the missing-files dialog does not list the folder; the folder row is passed over; renaming the folder back and pressing OK in Properties plays its files again | |
| 1.18 | Close OBS while item 2 plays, start OBS again | item 2 resumes | |
| 1.19 | Script `select_index` with `media_index` past the end | nothing happens (R-UP-5) | |
| 1.20 | Both plugins at once | a Media Playlist Source and an OBS Playlister source work side by side | |
| 1.21 | Packaging (D-2, PLAN V9) | each zip extracts to the layout above; the Inno installer (unsigned, SmartScreen prompt expected) installs the same tree; the portable zip loads in a portable OBS 33 | |

## Known residual risks

- Startup is judged by media time or audio only (R-BEH-5, step 6 spike). A live stream that delivers video but whose
  media time stays 0 and that has no audio would be skipped after its 15 s window; not observed, as no such stream
  was at hand.
- The plugin DLL and installer are unsigned (R-PLAT-10, D-10).
- Upstream defects R-UP-60 to R-UP-67, R-UP-69 to R-UP-72 and R-UP-76 to R-UP-78 are still
  present in 0.2.0 and are fixed in Phase 2 step 1 (for example selecting an
  empty folder row crashes OBS, R-UP-64). Until then, avoid empty folder rows,
  and with shuffle on, do not edit the playing row's path in the same Apply as
  other list changes (R-UP-76).
