# obs-playlister -- Manual test checklist

Automated tests run with:

```
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
```

## Runtime harness (automated, needs OBS)

`tests/obs/run-harness.ps1` runs `tests/obs/playlister-harness.lua` inside a portable OBS 33 and drives one
Playlister source through: play, auto-advance at the end of a file, next and previous, stop (no advance),
Play then Pause at once from stop (both happen: the item reloads and stays paused), Play again, pause, seek, restart, a speed change, hide and show (stop/restart and stop/play-next), end of
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
still being written and play it once admitted, and with loop off keep the ended last item through a folder change; a
newest-only source on the same folder then plays that copy, keeps playing it when a newer copy arrives and takes its
place in the list, and plays the newer one after its end (R-DIR-8); its migrated predecessor uses a junction named like a media file, which only the kind the
migration saves keeps a folder. A fourth source has a row saved as a folder that does not exist: it must stay a
folder and be passed over (the runner fails the run if a folder path is ever loaded as a file), and a row saved
without a uuid must be given one and play when chosen by it. Two more sources check R-BEH-5: a missing file and a
file that is no media are skipped for the file after them, and a list of only those stops instead of cycling (Play
tries them once more and stops again); a corrupt still fails by its 5 s startup window and the list stops; the runner
fails the run unless the corrupt file was loaded exactly 3 times and the corrupt still once. A last source holds a
valid still the runner makes (R-DIR-9) for 4 s: playing with the hold as its duration and on screen, a seek inside it
ignored, a pause that freezes the hold, a stop after which nothing moves on, Play that holds it again from the start,
and then the next row. After the newest-only source, a source with Lock list at start on the harness folder
shows the folder options as one labeled group in Properties (ranges, tooltips and the Browse dialog's still images
checked; N, the interval and the Apply button follow their options), keeps a new file waiting for 5 s while it
plays, and adds it, without a reload, once Apply waiting changes now is pressed (R-LIST-3, R-DIR-11, R-UP-10).
Along the way the status block (R-LIST-5) is read as Properties shows it: file rows with nothing to report, a stale
choice as a warning right after Play Selected, an empty folder row, a folder's file still being written, "1 of 5
files" in the newest mode, the locked list's waiting change and its absence once applied, the missing folder as an
error, failed rows as errors, the line saying why playback stopped, and no setting holding it. It needs a portable
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
| 2026-10-24 | 33.0.0 portable | Phase 2 step 6 (part 2) | 95 passed, 0 failed, no memory leaks, three runs in a row; the still held 4 s, paused and resumed, stayed stopped after a stop, and was held again after Play |
| 2026-10-25 | 33.0.0 portable | Phase 2 step 6 (part 3) | 101 passed, 0 failed, no memory leaks, three runs in a row; the evicted item played on, restarted from its copy with Current File naming its row alone, and the newer file followed its end. Mutation check: with the eviction treated as a deletion, playback stops when the newer file arrives and 3 checks fail |
| 2026-10-25 | 33.0.0 portable | Phase 2 step 7 (part 1) | 105 passed, 0 failed, no memory leaks, three runs in a row; the new file waited 5 s with the list locked and joined within one poll of the button. Mutation checks: with the button's request removed, the file is still waiting 15 s later (1 check fails); with the lock option ignored, the file joins at once (1 check fails) |
| 2026-10-25 | 33.0.0 portable | Phase 2 step 7 (part 2) | 116 passed, 0 failed, no memory leaks, three runs in a row, and once more after the stale-choice check also read the info type. Mutation checks: with the snapshot's change count left at 0, the waiting-change check fails; without the status update in Play Selected, the stale notice is missing from the open Properties and 2 checks fail. How the dialog draws the block is item 2.1 |
| 2026-10-25 | 33.0.0 portable | Phase 2 step 8 (part 2) | 117 passed, 0 failed, no memory leaks, four runs (the last after the QA fixes); Play then Pause at once from stop left item 2 paused at 120 ms. Mutation check: without the pending-load rule the pause is dropped and the item plays on to 2.4 s (1 check fails) |
| 2026-10-25 | 33.0.0 portable | 0.3.0 (version bump) | 117 passed, 0 failed, no memory leaks; the load line reads `plugin loaded successfully (version 0.3.0)` |
| 2026-10-25 | 33.0.0 portable | 0.3.1 (renamed Playlister) | 117 passed, 0 failed, no memory leaks; the load line reads `plugin loaded successfully (version 0.3.1)`; the sources are found by their unchanged id |
| 2026-10-28 | 33.0.0 portable | Phase 3a step 5 (part 1), ecc1439 | 117 passed, 0 failed, no memory leaks, twice. Every run on 2026-10-27/28 before this failed with media time stuck at 40 ms: Studio Mode was on in this portable OBS (it is shared with other projects' tests), so the harness sources sat in a preview scene that never played. With Studio Mode on the same build failed 11 checks; with it off, nothing failed. The runner now turns Studio Mode off for its run and restores user.ini |

## Phase 2 acceptance (automated, needs OBS)

`tests/obs/phase2-acceptance.lua` runs the PLAN step 8 items that can be staged on this machine against the real
render folder:

```powershell
.\tests\obs\run-harness.ps1 -ObsRoot C:\Users\jeffr\OBS-Studio-33-Portable -Files $files `
  -Script phase2-acceptance.lua -Otr C:\Users\jeffr\Documents\ComfyUI\output\otr\obs -TimeoutSeconds 600
```

The runner makes a temporary folder of hard links to every render (a link shares its file's dates, and removing it
leaves the file), lists their names in date order, prepares a folder of two links to move away and back and an empty
folder with a deny-read entry for the current user on that folder alone (removed afterwards), and checks whether this
machine reaches its own admin share. Everything the script writes into the link folder it removes again. The runner refuses a render folder holding a file named `zz-...` (the script's own names), and
the script refuses a link folder the runner did not make.

| # | Check | Expected |
|---|-------|----------|
| 2.2 | The render folder as a folder row, default options; then newest only; then newest 5 | first listing in seconds; the list in date order, oldest first, exactly as the folder's dates sort (Directory Sorter parity, R-DIR-12); "1: folder - N files"; the oldest plays first; newest only lists the newest render alone ("1 of N files"); newest 5 lists the five newest, oldest first |
| 2.3 | A new render copied in | joins within about 3 s, as the newest |
| 2.4 | The producer contract: written as `.mp4.part`, then renamed to `.mp4` | not listed under the partial name; joins once renamed |
| 2.5 | A file growing 1 MB every 100 ms for 8 s; a file held open for writing | neither is listed while written; the held file is counted ("waiting for 1 file to finish writing") and playback goes on; each joins within about 3 s once complete |
| 2.6 | A case-only rename; A -> B -> C with shuffle on and Lock list at start, then Apply waiting changes now; renaming and deleting the playing render | the renamed item keeps its place and shows its new name; the renames wait ("N changes waiting") and are applied at once as one renamed item while playback goes on; Windows refuses to rename or delete the playing file (the child holds it open; every run so far) and playback goes on |
| 2.7 | Sort by name, descending, mid-play | the list follows natural order, descending; playback keeps its place |
| 2.8 | 2,000 small text files at once, with the folder check off | a render copied in afterwards joins within 12 s (about 3 s by notification; after a run of overflows the folder is listed on the 10 s interval instead, and a dead watch would wait for the 60 s safety net); the text files are not listed. The burst can overflow the change buffer (the log says "more changes than the buffer holds" when it does); in these runs it did not, and the overflow path is covered by `test-folder-state` and by 2.11 |
| 2.9 | A watched folder moved away, then back | "the folder went away ...; showing the last list, from <time>" as an error, the list kept; back to "2 files" when it returns |
| 2.10 | A folder the user may not read, added as a row | still a folder row (by its attributes), "access denied (permissions or security software)" as an error; the log names Win32 error 5 (the runner fails the run without that line) |
| 2.11 | The media folder by a UNC path (`\\localhost\C$\...`) | listed, plays, and still listed 12 s later, past the folder check; the share's overflow right after the first listing is logged and listed again |
| 2.14 | A folder listed for the first time while a render is still being copied into it (the plugin's side of OBS starting during a copy) | the admitted renders play within about 3 s; the copy is counted as being written and not listed while it grows; it joins within about 3 s once complete |
| 2.19 | A corrupt still in a source shown in no scene (Pause when not visible), then shown; a valid file with Close file when inactive and Always play in a source shown in no scene, then shown | hidden for 10 s, the still is not failed, and once shown it fails within its 5 s window; the hidden file is not failed and plays once shown. (A paused source's window waits too, checked by `test-startup`; a pause is ignored for a child that has already failed, as for a stopped one) |

| Date | OBS | Build | Result |
|------|-----|-------|--------|
| 2026-10-25 | 33.0.0 portable | Phase 2 step 8 (part 1) | 29 passed, 0 failed, no memory leaks, in four runs (the last on the final code); the first listing of 177 renders took 91 to 699 ms; one overflow logged per run (the share's, right after its first listing); the renders unchanged afterwards (177 files, none written, one link each). The first runs, before the fixes, failed 2.10 (the denied folder became a file row) and 2.11 (the UNC row flapped with Win32 error 234) |
| 2026-10-25 | 33.0.0 portable | Phase 2 step 8 (part 2) | 38 passed, 0 failed, no memory leaks, three runs (the last after the QA fixes); the admitted renders played 0.2 s after a first listing during a copy, and the copy joined 2.6 s after it was complete; the hidden still failed 5.0 s after it was shown; the renders unchanged and nothing left behind |
| 2026-10-28 | 33.0.0 portable | Phase 3a step 5 (part 1), ecc1439 | 38 passed, 0 failed, no memory leaks; folder publishing moved to `providers/publication.c` in Phase 3a step 3, and every item passed unchanged (the render folder now holds 30 files; first listing 199 ms) |

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
`data\locale\en-US.ini`. From Phase 2 on these run on the current build (the waiver in PLAN section 7), so the
version expected in 1.1 and 1.2 is that build's (0.3.1 now).

| # | Check | Expected | Result |
|---|-------|----------|--------|
| 1.1 | Start OBS 33 and open the log | `[obs-playlister] plugin loaded successfully (version 0.2.0)` on the Phase 1 build, the current build's version on later ones; module list shows `obs-playlister.dll`; source list shows `'Playlister' (obs_playlister_source)` | 2026-10-24, OBS 33.0.0 portable (plugin in `plugins\obs-playlister\`): pass. Load line logged; the Plugin Manager's modules.json lists OBS Playlister 0.2.0 with source `obs_playlister_source`; clean exit, 0 leaks. 2026-10-24, OBS 33.0.0 system install (plugin in `C:\ProgramData\obs-studio\plugins\obs-playlister\`, Phase 1 build): pass. Load line logged next to `[media-playlist-source] plugin loaded successfully (version 0.1.3)`. |
| 1.2 | Tools > Plugin Manager | "Playlister" ("OBS Playlister" before 0.3.1), version 0.2.0 on the Phase 1 build and the current build's version on later ones, no Legacy chip; Media Playlist Source still shows its Legacy chip. Note which manifest fields are displayed (PLAN V7) and whether the Discover tab is still "Coming soon" (D-10) | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.3 | Windows security (R-PLAT-10) | `Get-AuthenticodeSignature` on the DLL reports NotSigned; Windows Security protection history and the Code Integrity log show nothing for `obs64.exe` or `obs-playlister.dll` | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.4 | Add a "Playlister" source; open Properties | Labels read "Loop Playlist", "Shuffle", "Use hardware decoding when available" (checked by default, D-11), never raw keys | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.5 | Add two video files; play | first file plays; Current File shows `1: <path>` after Refresh Filename | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.6 | Hardware decoding | Task Manager > GPU shows "Video Decode" activity while a file plays | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.7 | Next / Previous (media controls and hotkeys) | moves between the two files; hotkeys appear under the source as "Next" / "Previous" | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.8 | Loop on, let the last file end | wraps to the first file | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.9 | Loop off, let the last file end | stops; restart plays again per Restart behavior | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.10 | Shuffle on, Loop on, two files: both play once, then one starts the second pass; pick the other file in Select File, press Play Selected File and let it play to the end (R-UP-75) | no crash; a new shuffled pass starts | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.11 | Shuffle on, Loop on, three files: all three play once; while the fourth file plays (the first of the second pass), remove it from the list, then add it back (R-UP-73) | no crash at either edit; playback continues | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.12 | Speed 50% and 150% | playback speed changes (the video restarts, as the warning says) | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.13 | Each visibility behavior: hide and show the source | Stop/restart, Pause/unpause, Always play, Stop/play next behave as named | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.14 | Restart behavior: current file vs first file | Restart replays the current file, or starts at file 1 | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.15 | Add a folder row; play | its files play in order; Select File lists them as `N-M: <path>` | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.16 | Play Selected File with a chosen item | jumps to that item | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.17 | Move a listed file away, restart OBS | the missing-files dialog offers to find or remove it | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.17a | As 1.17, but before acting in the dialog add a row to the playlist and apply, then point the dialog at the moved file (R-UP-67, Phase 2 step 1.4 build) | the row is relocated; no crash | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.17b | Add a folder row and apply, close OBS, rename that folder, start OBS (R-UP-34, R-LIST-1; Phase 2 step 4 build) | the missing-files dialog does not list the folder; the folder row is passed over; renaming the folder back and pressing OK in Properties plays its files again | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.18 | Close OBS while item 2 plays, start OBS again | item 2 resumes | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.19 | Script `select_index` with `media_index` past the end | nothing happens (R-UP-5) | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.20 | Both plugins at once | a Media Playlist Source and a Playlister source work side by side | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 1.21 | Packaging (D-2, PLAN V9) | each zip extracts to the layout above; the Inno installer (unsigned, SmartScreen prompt expected) installs the same tree; the portable zip loads in a portable OBS 33 | 2026-10-24, Phase 1 build (CI run 40134077740): partial. Both zip layouts are as above, and the CI portable zip loads in a portable OBS 33.0.0. The Inno installer was not run (it needs an administrator); that part is pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |

## Phase 2 (version 0.3.0)

Setup as Phase 1, with the Phase 2 build. Items the runtime harness covers are not repeated here.

| # | Check | Expected | Result |
|---|-------|----------|--------|
| 2.1 | Open Properties of a source whose list has a file, a folder and a missing file; press Esc without changing anything; then choose an item, remove it from the folder, press Play Selected File | the Status block under the playlist shows one row per line (`1: file`, `2: folder - N files`, `3: file - could not be played` once it failed), in the error color when a row failed; Esc closes Properties without asking to save; after Play Selected File the block shows the "list changed" notice in the warning color, and Refresh Filename clears it | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.12 | Close OBS while a folder item plays; start it again | the same folder item resumes, not the folder's first item | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.13 | OBS hotkeys for Play/Pause, Restart, Stop, Next and Previous on a folder row; Next then Play/Pause at once | each acts as its button does; the load and the pause both happen; Stop never advances | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.15 | A watched folder on a removable drive: unplug it, plug it back | "the folder went away" with the last list kept; the row recovers when the drive is back | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.16 | A watched folder on another machine's share; stall the share (pull its network) during a scan, then remove the source | the row shows the error and keeps its list; removing the source returns at once (otherwise record a residual risk); on a share without change notifications the log says `rescan-only` and the 10 s check still finds a new render | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.17 | A OneDrive folder with cloud-only files as a folder row | "N cloud-only files not downloaded ..." as a warning; no download starts | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.18 | A still followed by a video in a folder row | no still frame lingers into the video | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.20 | The operator's OTR scene: replace the Directory Sorter script with a folder row on the render folder, default options | the same order as the script gave (date modified, ascending); a new render joins in about 3 s; the script is no longer needed | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.21 | A render held open for writing for a whole session (a stuck render process), then an OBS restart | the other renders play and the list resumes where it was; the held file joins once released | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |
| 2.22 | Start OBS while a 300 MB render is being copied into the watched folder (with Defender scanning it) | the admitted renders play within about 3 s; the new one joins when its copy and the scan finish (2.14 checks the plugin's side) | Pending: not run yet; the version was bumped to 0.3.0 without it (operator decision, 2026-10-25) |

## Phase 3a (playlists and URL rows, before 0.4.0)

Setup as Phase 2, with the Phase 3a build (commit f44900c or later). The Phase 2 runtime harness and acceptance
script pass on this build (2026-10-28; the stuck playback of the night before was Studio Mode left on, see the
harness table). Run the harness again after any change, and go on only when it passes. The parsers,
the fetching and the list provider are covered by `ctest` (`test-url`, `test-parse-m3u`, `test-parse-pls`,
`test-fetch`, `test-provider-list`); these items check them in OBS.

`tests/obs/phase3a-acceptance.lua` runs every item that needs no internet, no certificate and no eyes (all but 3.8,
3.16, 3.17 and the real feed of 3.9), in about 3 minutes:

```powershell
.\tests\obs\run-harness.ps1 -ObsRoot C:\Users\jeffr\OBS-Studio-33-Portable -Files $files `
  -Script phase3a-acceptance.lua -TimeoutSeconds 900
```

The runner copies `tests/fixtures/playlists` into a temporary server root with hard links to the three files in
`media/`, and serves it with `tests/manual/serve-fixtures.ps1` twice, on free localhost ports: one copy for lists, one
for media (`/media/1` to `/media/3`, as `video/mp4` from addresses without an extension, with byte ranges). Each
logs each request when it starts and each media body when it ends, with the user agent and the bytes read for the client, so 3.7 can see that the plugin read only the headers. The three files must be on the same NTFS volume as the temp folder (they are hard-linked), and the first must play for more than about 40 s. The
script writes its own lists into the root and edits them there; the runner stops both servers and removes the root.
For the manual items, start the server by hand with `powershell -NoProfile -File tests\manual\serve-fixtures.ps1`
(port 8765).

An error row reads "web address - empty; <the error>": the row is listed and holds nothing. The checks accept that.

| # | Check | Expected | Result |
|---|-------|----------|--------|
| 3.1 | A local .m3u of three files (relative names with either separator, one #EXTINF title) as a row | "1: playlist - 3 files; no dates; list order"; listed in list order and the first one plays; Select File shows the title | Passed (acceptance) |
| 3.2 | Edit that .m3u while it plays (add a line) | the new entry joins within the folder check interval; the playing item is not restarted | Passed (acceptance; joined after 6.7 s, playback went on) |
| 3.3 | Delete the .m3u, then put it back | the row keeps its list and shows "playlist file not found" with the last-list time; it recovers when the file is back | Passed (acceptance) |
| 3.4 | A local .pls with File2 before File1, and a VLC-style .m3u (file:/// lines) | entries in File order; the file: entries are listed as the files on disk and play | Passed (acceptance) |
| 3.5 | /fixtures/vlc.m3u from the server as a row | "1: playlist - 1 file; ...; 2 lines could not be used": a list on a server may not name files on this computer, so only its radio stream entry stays | Passed (acceptance) |
| 3.6 | /extensionless-m3u and /extensionless-pls as rows | "web address"; read as M3U and PLS by Content-Type | Passed (acceptance) |
| 3.7 | A direct MP4 URL without an extension, served with Content-Type video/mp4 | plays at once; the plugin makes one request and closes it after the headers, then the child fetches the media itself | Passed (acceptance; playing 3.5 s after the source was made, the plugin's request ended after 64-128 KB were read for it, of a 50 MB file) |
| 3.8 | A live HLS URL (.m3u8) and an internet radio stream URL | each plays as one entry; the HLS row is not fetched again on the interval | Pending (manual, needs the internet) |
| 3.9 | A podcast RSS feed | "web address - ...; a podcast feed: feeds come in a later version" | Passed (acceptance, a local feed); a public feed is pending |
| 3.10 | A refused port, a name that does not resolve, /status/429, /html; a served list that answers 429 after a good fetch | each row shows its error ("could not be reached", "server name could not be found", "HTTP 429", "a web page ...") and nothing plays from it; the 429 after a good fetch keeps the last list and playback goes on | Passed (acceptance) |
| 3.11 | /gzip-at-cap and /gzip-over-cap | at the cap the list loads (empty: the body is comments); over it, "the list is over 16 MB" | Passed (acceptance) |
| 3.12 | /redirect/5 and /redirect/6 | five redirects load the list; six show "too many redirects" | Passed (acceptance) |
| 3.13 | /hang as a row; then /hang again, removed after 5 s | "timed out" after about 30 s (the receive timeout: the server takes the request and never answers; the 60 s deadline is for a body that keeps trickling, covered by `test-fetch`); removing the source returns at once and OBS stays responsive | Passed (acceptance; timed out after 31-34 s, removal took 0 ms, next tick after 0.1 s) |
| 3.14 | "Check web lists every" changed to 30 s while a served list plays, then the list edited on disk | the interval change does not restart playback; the change appears within 30 s (plus the fetch) | Passed (acceptance; appeared after 25.6 s) |
| 3.15 | Lock list at start on, with a playlist row whose file is edited during playback | "1 change waiting" until Apply waiting changes now, which adds it without a restart | Passed (acceptance) |
| 3.16 | An HTTPS playlist on a real server, and one with an expired or self-signed certificate | the first loads; the second shows "secure connection failed" | Pending (manual) |
| 3.17 | The Browse dialog | a Playlists group lists .m3u, .m3u8 and .pls files | Pending (manual) |

| Date | OBS | Build | Result |
|------|-----|-------|--------|
| 2026-10-28 | 33.0.0 portable | Phase 3a step 5 (part 1), ecc1439 | 40 passed (41 checks after the QA pass; see the next row), 0 failed, no memory leaks, three runs in a row; the servers stopped and the root removed after each. Mutation checks: with a media response read as a list, the 3.7 address does not play and the plugin's request reads 16 MB (3 checks fail); with the last good list dropped on an error, 3.3 and the 429 after a good fetch lose the list and playback stops (4 checks fail) |
| 2026-10-28 | 33.0.0 portable | Phase 3a step 5 (part 1), ecc1439, after the QA fixes | 41 passed, 0 failed, no memory leaks, three runs in a row; the plugin's request to the media address ended after 64-128 KB were read for it, the player made 3 requests of its own; timed out after 32.7-34.5 s |

## Known residual risks

- Startup is judged by media time or audio only (R-BEH-5, step 6 spike). A live stream that delivers video but whose
  media time stays 0 and that has no audio would be skipped after its 15 s window; not observed, as no such stream
  was at hand.
- The plugin DLL and installer are unsigned (R-PLAT-10, D-10).
- Upstream defects R-UP-60 to R-UP-67, R-UP-69 to R-UP-72 and R-UP-76 to R-UP-78 were present in 0.2.0; 0.3.0
  fixes them (Phase 2 steps 1 to 6).
- The version was bumped to 0.3.0 (operator decision, 2026-10-25) before the manual checks above were run: Phase 1 items 1.2 to
  1.20 and the installer part of 1.21, and Phase 2 items 2.1, 2.12, 2.13, 2.15 to 2.18 and 2.20 to 2.22. Until they
  are, what only they cover is unverified, among it: Windows security and the Plugin Manager entry, the Properties
  dialog as drawn, hotkeys, hardware decoding, a real OBS restart, a render held open across one, removable
  drives, another machine's share and its stalls, OneDrive cloud-only files, still-to-video transitions, OBS started
  during a large copy, the installer, and the operator's OTR scene without the Directory Sorter script.
