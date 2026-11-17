# Changelog

All notable changes to Playlister are listed here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
version numbers are the ones in `buildspec.json`.

## [Unreleased]

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

[Unreleased]: https://github.com/jbrick2070/obs-playlister/compare/0.5.2...HEAD
[0.5.2]: https://github.com/jbrick2070/obs-playlister/compare/0.5.1...0.5.2
[0.5.1]: https://github.com/jbrick2070/obs-playlister/compare/0.5.0...0.5.1
[0.5.0]: https://github.com/jbrick2070/obs-playlister/compare/0.4.0...0.5.0
[0.4.0]: https://github.com/jbrick2070/obs-playlister/compare/0.3.1...0.4.0
[0.3.1]: https://github.com/jbrick2070/obs-playlister/compare/0.3.0...0.3.1
[0.3.0]: https://github.com/jbrick2070/obs-playlister/releases/tag/0.3.0
