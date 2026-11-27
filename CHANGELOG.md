# Changelog

All notable changes to Playlister are listed here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
version numbers are the ones in `buildspec.json`.

## [Unreleased]

### Changed

- Arch Linux: `packaging/arch/PKGBUILD` builds release 1.5.0, now that it is published (it built 0.5.2).

### Fixed

- A web list whose server sends it as `text/html` (the default of a PHP script, as many IPTV `get.php` addresses
  are) kept playing only until its first refresh, which then refused it as "a web page instead of the list"; a
  list of 64 KB or more was refused at once. The start of such a body now decides on every fetch, as on the first:
  a playlist is read, a real web page is still refused, and only its first 64 KB are read.
- Windows: a watched folder that was renamed or deleted to the Recycle Bin while a new folder took its name kept
  its watch on the old folder, so new files in the new one showed only at the folder check, until OBS restarted.
  The next listing now notices the new folder and watches it.

## [1.5.0] - 2026-11-20

The suite-wide step to 1.5: every product of the Fadester suite now carries version 1.5, so that one round of hand
tests on Windows and Mac covers them together. The number is the suite's. What changed in Playlister since 0.5.2 is
below: the fixes from the code review of 2026-11-20, the changes from the review of the Properties window of the
same day, Italian, and a Mac installer that ends on the first three steps. None of it has been tested by hand in
OBS yet (TESTING.md: "Second pass", "Third pass" and "macOS").

### Added

- Italian (`it-IT`), the thirteenth language. Like the others it is an AI draft that no native speaker has checked,
  and no second model has reviewed it. In the eleven languages of 0.5.1 the texts added or changed since 0.5.2,
  about half of each file, have had no second review either (the texts up to 0.5.2 had one); the head of each
  language file says which.
- What plays now and what comes next can be read without opening Properties. Playlister registers the
  obs-websocket vendor `obs-playlister` with the request `get_now_next` and the event `now_next_changed`, for a
  controller such as a Stream Deck key; the same is the proc `get_now_next` and the signal `playlister_now_next`
  on the source. They carry the names of the playing and the next item, whether the next one is decided (with
  Shuffle on it is chosen only when it starts), the state, the worst row status and the item counts; never a
  path. README, "For controllers and scripts", has the fields. Fadester for OBS 1.5 shows the names on its Playlister keys.
- Properties: a **Next** line under **Now playing**.
- Status says "Waiting: plays when its scene is in Program." for a source that has items and is not on air, as a
  new source in a Studio Mode Preview scene is.
- A hotkey pressed while the source is not shown leaves a line in the OBS log.
- Status totals the rows: with more than one row, a line after them reads "37 items to play", the items the list
  plays as it stands (a folder that plays 5 of 40 files counts 5; an item that could not be played is not
  counted). It is left out while a row is still being read, cannot be read or plays the last list it could read,
  when it would not be the whole truth.
- A folder or playlist row names the files it could not play, the first three of them: "2 files could not be played
  (clip-07.mp4, intro.mov)". Before, it only counted them.
- **Apply waiting changes** answers in Status: "The waiting changes are being applied. Press Refresh to see the
  list.", or "No changes were waiting. Press Refresh to check again." Before, a press showed nothing.
- The user name and password of a web address show in no label of the Play item list, status line, log line or
  name sent to controllers, and no longer in **Now playing** and **Next**, which showed the address as it was
  typed and saved that text with the scene. An address inside a playlist entry's title is treated the same. The
  **Playlist** list itself still shows each address as it was typed, and the scene collection file keeps it, as
  that is what Playlister plays and finds the item by. Write a "/", "?" or "#" in a user name or password as %2F, %3F
  or %23, as in any web address. Otherwise the address means something else: it ends at that character, so it
  may go to another server than was meant, and that part of the user name or password may show in those labels.
- An item given as `file:///a.mp4` reads "a.mp4 (file:///)" in the Play item list.
- macOS: the installer ends on a page of its own, "Playlister is installed", with the first three steps: if OBS is
  open, quit it and open it again; in Sources, press + and choose Playlister; press + next to Playlist and choose
  your files or a folder (it plays when its scene is in Program). The page stands in place of macOS's standard
  success text and comes in the thirteen languages of the plugin; the twelve besides English are AI drafts that no
  native speaker has checked. The package was checked with `productbuild` and `pkgutil` on a Mac; reading the page
  in the installer is TESTING.md, M.17.

### Changed

- The Properties window opens on what most people need, in this order: that changes go live at once, the
  Playlist, its Status with Refresh right under it, Loop playlist, Shuffle, Now playing, Next, Play item and
  Play now. Everything else is behind **Show advanced options**. No setting changed its name or its meaning, so
  saved scenes behave as before.
- Status follows the Playlist as it is edited: it no longer says the list is empty after the first file was
  added. A row added a moment ago reads "new; press Refresh for its status".
- Words. "Visibility behavior" is **When not in Program**, with "Stop; start again when it returns", "Stop; play
  the next item when it returns", "Pause; continue when it returns" and "Keep playing". "A restart plays" is
  **Restart goes to**. "Current File" is **Now playing**; "Select File" and "Play Selected File" are **Play item**
  and **Play now**. "Hold changes until the next Play" is **Freeze the list during playback**, and its button,
  **Apply waiting changes**, is there only while the list is frozen. "Files to play" is **Play**; "File types to
  play" is **File types**. One thing that plays is an "item" everywhere; a playlist or web address row counts
  items, a folder files. The kind "URL" reads "web address". Labels are in sentence case.
- **Order** is one list ("Name, A to Z", "Name, Z to A", "Oldest first", "Newest first", "As listed", "As listed, reversed") in
  place of "Sort by" and "Descending". The default stays Oldest first, and `sort_key` and `sort_descending`
  stay the saved settings, so scenes and scripts are untouched.
- An empty **File types** field means all the usual video, audio and image types. It used to make folders play
  nothing, with no way back short of resetting every setting.
- The Play item list shows the name first and the folder after it ("2-14: clip-14.mp4 (D:/shows/loops)"), and a
  playlist entry's own title before its file name. A web address with no path shows its host ("example.com"), not
  a piece of its "https://".
- The tooltip of **Freeze the list during playback** is one sentence: "New, changed and removed files wait until
  the next Play or Restart, or until you press Apply waiting changes." What happens when the source returns to
  Program is in the README.
- A file or web address row that failed names its file, the likely cause and what happened: "could not be played
  (missing, moved, or not a format OBS can play); skipped". A missing folder and a list that cannot be read say
  when Playlister tries again: "checking again every 10 s". A folder with nothing to play says why: no files
  yet, files only in subfolders with Include subfolders off, or no files of the types to play. A row that is not
  read after 5 seconds says the drive is slow to answer.
- A playlist file no longer says "no dates; list order" with the default options. The note stays, reworded,
  where it matters: when only the newest entries play.
- The warning under Speed is about Speed only ("Changing Speed restarts the item that is playing."); that every
  change goes live is said at the top of the window.
- Web lists: both the Windows and the Mac and Linux fetchers now refuse a redirect from https to http and say so,
  and send a user name and password in the address to that server, and after a redirect only to the same scheme,
  host and port. Before, Windows dropped the user name and password, and Mac and Linux followed https to http.
  Entries such a list names by a relative address get the user name and password too while the list itself
  came from the server they were given for, and not after a redirect to another.
- Web lists behind a proxy: Windows now also uses the proxy the environment names (`http_proxy`, `https_proxy`,
  `all_proxy`, with `no_proxy`), as Mac and Linux did and do; with none named it uses the system's proxy settings,
  as before. OBS's media source, which plays the items, reads `http_proxy` and `no_proxy`.
- A web list that is not UTF-8 is read in the character set its server names. Windows-1252 is used when the server
  names none, names one this computer cannot convert from, or sends text that is not valid in the one it names.
- README: Install comes right after the Quick start with one download per system first; where Playlister comes
  from and the feature list are at the end; the table of four players is a link to START-HERE.md.
- macOS: the installer is titled "Playlister". It showed the project name, "obs-playlister".
- CI: a release tag whose version is not the one in `buildspec.json` fails the release; the GitHub actions are
  pinned to commits.
- The five strings that no code used (`NetworkCaching`, `AudioTrack`, `SubtitleEnabled`, `SubtitleTrack`,
  `VoskFilter`) are gone from every language file.

### Fixed

- A folder or playlist that is being read from a slow or stalled network share
  holds OBS up less. Removing its row, or removing the source, no longer waits
  for the reader at all: the wait moved off OBS's video thread to a background
  thread, so removing a folder row during a show does not freeze the picture
  for it. Quitting OBS still waits for a call that is already in flight on
  the share, but only for that one call: the folder scanner leaves its
  per-file checks as soon as it is asked to stop (it used to finish them,
  about 100 ms per remaining file on Windows), and every row's reader is asked
  to stop before any is waited for. On macOS and Linux a call on a share that
  has hung can still not be interrupted.
- A row whose path was just added or changed is no longer looked at on OBS's
  video thread to see whether it is a folder. A path on a drive that is
  spinning up or on a stalled share could hold the picture for that look. A
  background thread looks instead; the row reads "reading" for that moment.
  A path that was not a folder when it was looked at (a drive that was not
  connected, a share that was offline) is looked at again, in the
  background, at every later change of the source's settings, as before: it
  becomes a folder row when its drive is back.
- Mac and Linux: quitting, or removing a web address row, no longer waits for
  a name lookup that is not answered (a dead DNS server), with libcurl 7.87
  or later. With an older libcurl it still waits until the system's resolver
  gives up.
- Linux: a folder tree with more subfolders than the system lets one user
  watch no longer keeps every watch it made before it gave up, which left
  none for other rows and programs. A tree more than 100 levels deep is
  listed on the folder check instead of watched.
- A hotkey pressed in the moment a source is being removed can no longer
  reach the source after it was freed.
- A web playlist whose entries are very long addresses with thousands of
  `/` and `../` parts no longer keeps a background thread busy for minutes.
- A playlist is limited to 100,000 entries, and so is a folder. A list of
  millions of lines, from a web address or a local file, could ask for
  gigabytes of memory and bring OBS down. The row's status says how many
  entries were left out.
- A paused item that is deleted, or leaves its folder or playlist, is no longer
  resumed with sound and no picture, and the item after it is no longer
  skipped: Play starts the item that followed it.
- Deleting or editing the current row while the source is stopped, paused or at
  the end of its list no longer starts playback. The position moves to the
  row now in its place (after a deletion, the row that followed), and Play
  or Restart starts it. A source that was paused only because it is hidden
  plays from there when it is shown. Adding files to a source that had nothing
  to play still starts it.
- Shuffle: a manual Next from a hotkey or a remote control on Windows no longer
  draws the same "random" order in every OBS session, and lists longer than
  32,767 items are drawn from evenly. Playlister has its own random generator
  now, seeded once when the plugin loads.
- Shuffle: a change to a long list (a file added to a watched folder of
  thousands) no longer holds up OBS's video thread while the played items are
  matched with the new list.
- Windows: files and folders whose names start with a dot are left out of
  folder rows, as on macOS and Linux. That includes the `._name` side files a
  Mac writes next to each file on a USB drive or a network share, which used
  to show up as items that could not be played.
- A file being recorded or copied into a watched folder no longer makes
  Playlister list the whole folder twice a second. It waits for half a second
  of quiet, and at most 5 seconds.
- Lock list at start: holding back a change in a list of tens of thousands of
  entries no longer costs about a second on every update.

### Changed (tests)

- The Linux harness (`tests/obs/run-harness.sh`) fails when OBS has to be
  ended on exit, when OBS reports a memory leak, and when a check the script
  declares did not run. The script counts its own checks, so no number is
  kept by hand.

## [0.5.2] - 2026-11-17

### Added

- macOS: Playlister builds for macOS 13.0 or later as one universal binary
  (Apple Silicon and Intel). Releases gain
  `obs-playlister-<version>-macos-universal.pkg` (installs into
  `~/Library/Application Support/obs-studio/plugins/`) and a `.tar.xz` with
  the same `obs-playlister.plugin`. Both are unsigned and not notarized; README
  says how to open them. The tests run on a macOS runner on every push. On
  2026-11-17 the build was installed in OBS 33.0.1 on an Apple Silicon Mac
  (M6, macOS 27.3) and checked: it loads, its vendor requests answer, a
  Playlister source is created, and OBS quits with 0 memory leaks. The
  hands-on checks (TESTING.md, M.1 to M.16) are still to come.
- macOS folder rows: changes are seen through FSEvents, renames keep the
  playing item (also a file renamed twice in a row), Finder-hidden files are
  left out, iCloud Drive files that are not downloaded are listed but never
  opened, and a folder on a network volume (SMB, NFS, AFP, WebDAV) is checked
  on the folder check interval. Keys follow how macOS compares names, so a
  rename that only changes case keeps the item.
- macOS: "access denied" for a folder now says in the log where to allow OBS
  (System Settings > Privacy & Security > Files and Folders).

### Changed

- REQUIREMENTS: decision D-5 (no macOS) is revised; macOS is section 2b.

## [0.5.1] - 2026-11-15

### Added

- README: linked the suite-wide `START-HERE.md` setup and compatibility guide
  first at the top of `README.md` (`P1-5`).
- README: added the "Which video player when" comparison table across VJster,
  Visual Sidechain Tape Deck, and Playlister, showing speeds in both `%` and
  `x` forms (`P1-6`).
- Playlister speaks eleven languages besides English: Simplified Chinese,
  Spanish, Brazilian Portuguese, Russian, Japanese, German, French, Korean,
  Traditional Chinese, Turkish and Dutch. It follows the language OBS runs in.
- A source with an empty list says what to do: press + next to Playlist.
- A file or URL row that could not be played says why: it did not start, or it
  took too long to start.
- Properties says that Status, Current File and Select File do not update by
  themselves, where to set the hotkeys, and that hotkeys work only while the
  source is shown.
- "A restart plays" has a tooltip: it also decides what plays each time the
  source is shown again under "Stop when not visible, restart when visible".
- README: a Quick start, and sections on the status lines, the options and the
  hotkeys. This changelog.

### Changed

- No more black frames between clips. On Next, and when one clip ends and the
  next starts, the last frame of the old clip stays on screen until the new
  clip shows its first frame (at most 1 second).
- Clearer labels in Properties. Your settings are kept.
  - "Refresh Filename" is now "Refresh".
  - "Restart behavior" is now "A restart plays".
  - "Folder and feed options" is now "Folder and playlist options".
  - "Newest N files" is now "The newest files", with "How many newest files".
  - "Check folder every N seconds" is now "Check folders again on a timer",
    with "Time between folder checks".
  - "Check web lists every" is now "Time between web list checks".
  - "Lock list at start" is now "Hold changes until the next Play".
- The warning under Speed says that changes take effect at once, also while
  you are live, and which changes start the playing clip again.
- When nothing in the list can be played, the message says where Play is: in
  the OBS media controls.

### Fixed

- Shuffle with Loop off: after a finished pass, Play or Restart played row 1
  and then stopped after that one clip. Now it starts a new shuffled pass.
- Shuffle: after Previous and a change to the list, the clip that was playing
  could be picked again at once, or the wrong clip could come next.
- "Stop when not visible, play next when visible" with nothing left to play:
  the last frame stayed frozen on screen when the source was shown again.
- A playlist file on this computer that was read while a program rewrote it
  could replace the list with its first lines. A changed playlist file is now
  read only once it has stopped changing.
- A source whose player failed to start left its hotkeys behind, which could
  crash OBS when they were pressed.
- Large folders and playlists (tens of thousands of entries) no longer hold up
  hotkeys and the Properties window each time their list is applied.
- `buildspec.json`: macOS `bundleId` changed from upstream's
  `com.codeyan.media-playlist-source` to `com.jbrick2070.obs-playlister`.
- After the end of the list, a folder change or Stop no longer makes Play
  replay the last item instead of the first.
- A file renamed twice in quick succession keeps its place in the list instead
  of being treated as deleted.
- A web address whose query ends in an image extension is no longer taken for
  a still image.
- The OBS log no longer shows user names, passwords, queries or fragments of
  playlist web addresses.
- Linux: playlist files resolve relative names that contain a colon, and a
  watched folder recovers after the system drops change events.

## [0.5.0] - 2026-10-31

### Added

- Linux: Playlister builds on Ubuntu 24.04 and Arch Linux and runs inside OBS
  on Ubuntu 24.04 (OBS 32.2 or later). Every automated test passes on both,
  including watched folders, playlist files and web lists. Inside OBS, the 124
  runtime checks that pass on Windows also pass on Ubuntu 24.04 with OBS 32.2
  (run on a server with software rendering). The dialogs have not been looked
  at on Linux yet; reports from Linux users are very welcome (open an issue).
- Properties ends with the version you are running.

### Fixed

- A Playlister in a scene that is not showing (another scene, the Studio Mode
  preview) keeps a sensible size instead of shrinking to a dot. This fix was
  made for 0.4.1, which was never released on its own.

## [0.4.0] - 2026-10-28

### Added

- A row can be a playlist file (.m3u, .m3u8, .pls) or a web address. A
  playlist file is read again when it changes. A web address is a list on a
  server, checked again every few minutes (5 minutes by default), or a video or
  stream address that plays directly.
- A list that fails to load keeps its last good copy and shows why in the
  row's status line.
- The Browse dialog has a Playlists group.

Podcast feeds and web pages of links come in later versions. The checks that
need no internet run unattended in OBS; live streams, a public feed, HTTPS and
the Browse dialog are still to be checked by hand (TESTING.md).

## [0.3.1] - 2026-10-25

### Changed

- The plugin is called Playlister, as the OBS Project does not allow "OBS" in
  plugin names. Its files, source ID and this repository keep the identifier
  `obs-playlister`, so existing scenes, settings and hotkeys carry over.

## [0.3.0] - 2026-10-25

### Added

- Folder rows follow their folders as files are added, renamed or deleted, and
  a file still being written joins once it is complete.
- Folders can be sorted by date or name and shown in full, newest only or
  newest N.
- "Lock list at start" holds changes until the next Play (or "Apply waiting
  changes now").
- Properties shows a status line per row.
- Entries that cannot play are skipped.
- Still images are held for a set time.

The manual checks in OBS (TESTING.md) are still pending.

## 0.2.0 - 2026-10-24

### Changed

- The original Media Playlist Source, renamed and built for OBS 33.

[Unreleased]: https://github.com/jbrick2070/obs-playlister/compare/1.5.0...HEAD
[1.5.0]: https://github.com/jbrick2070/obs-playlister/compare/0.5.2...1.5.0
[0.5.2]: https://github.com/jbrick2070/obs-playlister/compare/0.5.1...0.5.2
[0.5.1]: https://github.com/jbrick2070/obs-playlister/compare/0.5.0...0.5.1
[0.5.0]: https://github.com/jbrick2070/obs-playlister/compare/0.4.0...0.5.0
[0.4.0]: https://github.com/jbrick2070/obs-playlister/compare/0.3.1...0.4.0
[0.3.1]: https://github.com/jbrick2070/obs-playlister/compare/0.3.0...0.3.1
[0.3.0]: https://github.com/jbrick2070/obs-playlister/releases/tag/0.3.0
