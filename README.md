# Playlister

> **Setting up the Fadester Suite?** Start with **[START-HERE.md](https://github.com/jbrick2070/streamdeck-fadester/blob/main/docs/START-HERE.md)** for what to install for your controller, which video player to pick, first-run OBS settings, and the compatibility matrix.

A playlist source for OBS Studio 33 and later. It plays video, audio and image
files, folders, playlist files and web addresses through OBS's own Media Source,
one after another, and editing the list does not restart the clip that is
playing.

![Playlister in OBS Studio 33: the Program scene "Transport" plays a Playlister source ("Transport Queue" in the Audio Mixer) while Studio Mode previews another scene. A frame of a real OBS window capture.](docs/images/playlister-hero.png)

## Quick start

1. Install Playlister (see [Install](#install), just below) and start OBS again.
2. In OBS, under **Sources**, press **+**, choose **Playlister** and press
   **OK**.
3. In the window that opens, press **+** at the right of **Playlist**. Choose
   **Add Files** (video, audio, images or a playlist file), **Add Directory**
   (a folder; new files in it are added by themselves) or **Add Path/URL** (a
   web address). Press **OK**.

It plays, one item after another, as soon as its scene is in Program. In
Studio Mode a new source sits in the Preview scene and waits there: **Status**
says "Waiting: plays when its scene is in Program." until you fade or cut that
scene to Program.

No setting has to be changed: the playlist loops, folders are watched, and
Playlister finds out by itself whether a row is a file, a folder, a playlist
file or a web address.

## Install

Needs OBS Studio 33.0.0 or later on Windows x64 or on macOS 13.0 or later (Apple Silicon and Intel), and OBS
Studio 32.2 or later on Linux x86_64 (the OBS Project's Ubuntu PPA has 32.2, Arch has 33). After installing, start
OBS again and follow the [Quick start](#quick-start).

- **Windows:** download and run the installer (`.exe`). It is not code-signed, so Windows shows "Windows
  protected your PC": press **More info**, then **Run anyway**.
- **macOS:** download the `.pkg`. It is not signed by Apple, so macOS refuses it the first time: right-click (or
  Control-click) the `.pkg`, choose **Open**, then **Open** again.
- **Ubuntu 24.04:** install the `.deb` from the release page.

### Other ways to install

#### Windows

- **Zip:** unzip so that you get
  `C:\ProgramData\obs-studio\plugins\obs-playlister\obs-playlister.dll` and
  `C:\ProgramData\obs-studio\plugins\obs-playlister\data\`. Copying into
  ProgramData may need administrator rights. The zip installs without the
  "Windows protected your PC" prompt.
- **Portable OBS:** unzip the portable zip into the OBS folder, giving
  `<OBS folder>\plugins\obs-playlister\`.

The DLL is unsigned. It loads with Windows' default settings, but it would be
refused if Smart App Control or a Windows Defender Application Control policy
for user-mode code were turned on.

#### macOS

- **Installer:** run `obs-playlister-<version>-macos-universal.pkg`. It installs
  `obs-playlister.plugin` into `~/Library/Application Support/obs-studio/plugins/`.
- **By hand:** unpack `obs-playlister-<version>-macos-universal.tar.xz` and move
  `obs-playlister.plugin` into `~/Library/Application Support/obs-studio/plugins/`.
- One build runs on Apple Silicon and Intel Macs.

> **Mac users, please read.** The macOS build is new: it is built and its tests pass automatically on every
> change. On 2026-11-17 it was installed in OBS 33.0.1 on an Apple Silicon Mac (M6, macOS 27.3) and checked: it
> loads, a Playlister source is created, and OBS quits with 0 memory leaks. The full
> hands-on test is still to come, so please report what you find. The `.pkg` is not
> signed or notarized by Apple, so macOS will refuse to open it the first time. To allow it, right-click (or
> Control-click) the `.pkg` and choose **Open**, then **Open** again; or try to open it once, then go to
> **System Settings → Privacy & Security** and click **Open Anyway**.

On macOS, a watched folder on a network volume (SMB, NFS, AFP, WebDAV) is checked on the folder check interval, as
macOS does not report changes made there by other machines. If a folder row says "access denied" for a folder in
Desktop, Documents, Downloads, an external drive or a network volume, allow OBS in **System Settings → Privacy &
Security → Files and Folders** (or **Full Disk Access**) and start OBS again.

#### Linux

- **Arch Linux:** build the package from
  [packaging/arch/PKGBUILD](packaging/arch/PKGBUILD) against Arch's own
  `obs-studio` package:

  ```
  git clone --branch 0.5.2 https://github.com/jbrick2070/obs-playlister.git
  cd obs-playlister/packaging/arch
  makepkg -si
  ```

  It installs `/usr/lib/obs-plugins/obs-playlister.so` and
  `/usr/share/obs/obs-plugins/obs-playlister/`. Without makepkg, the same build
  is `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF`, then `cmake --build build` and
  `sudo cmake --install build` (needs `cmake`, `ninja`, `simde` and `curl`).
- **Ubuntu 24.04:** install the `.deb` from the release page (built against
  the OBS Project's PPA, `ppa:obsproject/obs-studio`).
- The Flatpak OBS is not supported.

On Linux, a watched folder on a network drive (NFS, SMB, CIFS) or a FUSE file
system is checked on the folder check interval, as Linux does not report
changes made there by other machines.

## Status lines

Under the playlist, **Status** has one line for each row of the list: its
number, its kind and how it is doing. For example:

```
1: folder - 12 files
2: file
3: file clip-07.mp4 - could not be played (missing, moved, or not a format OBS can play); skipped
4: web address - reading
```

- The kinds are **file**, **folder**, **playlist** (an .m3u, .m3u8 or .pls
  list) and **web address**.
- A folder says `reading` until it is first read (and, after 5 seconds, that
  the drive is slow to answer), then how many files it plays: `12 files`, or
  `5 of 40 files` when only the newest play. A playlist or web address counts
  `items`. A row with nothing to play says why: `no files yet; new files are
  added as they arrive`, `no files here yet; files in subfolders need Include
  subfolders`, or `no files of the types to play (see File types)`.
- It can add: `waiting for 2 files to finish writing`, `3 changes waiting`
  (see Freeze the list during playback below), `1 file could not be played
  (clip-07.mp4)` with the names of the first three such files, and an error
  with what happens next, such as `folder not found, or not a folder; checking
  again every 10 s`. When the list cannot be read now,
  Playlister keeps playing the last good one and says
  `using the list read at <date and time>`.
- A file or web address that failed is named, with the likely cause:
  `could not be played (missing, moved, or not a format OBS can play)` when it
  ended before anything played, or `took too long to start (slow drive or
  address)` when nothing played within 5 seconds (15 seconds for an address or
  a file on a network share). Playlister skips it and plays the next item.
  Nothing is drawn over the picture.
- With more than one row, a line after the rows says how many items the list
  plays as it stands: `37 items to play`. A folder that plays `5 of 40 files`
  counts 5, and an item that could not be played is not counted. The line is
  left out while a row is still being read, cannot be read, or plays the last
  list it could read.
- When nothing in the list can play, a last line says so and playback stops.
  Fix the rows marked above it, or select the source and press Play in the
  source toolbar under the preview to try again.
- `Waiting: plays when its scene is in Program.` means the list is fine and
  the source is simply not on air.
- With no rows, Status tells you to press **+** at the right of Playlist.
- Status is shown as a warning or an error when one of its lines needs
  attention.

Status follows the list as you edit it; a row added a moment ago says `new;
press Refresh for its status`. What plays is not pushed into the open window:
press **Refresh**, right under Status, to update Status, Now playing, Next and
the Play item list.

The OBS log shows each change as it happens: **Help > Log Files > View Current
Log**, lines starting with `[obs-playlister]`.

## Options

Changes in the Properties window go live at once; there is no Cancel that
protects the output. Changing Speed or hardware decoding restarts the item
that is playing. Removing or changing the row that plays starts another item.

The window opens on what most people need:

- **Playlist**: the rows. Press **+** to add files, a folder or a web address.
- **Status** and **Refresh**: see [Status lines](#status-lines).
- **Loop playlist** (on): after the last item, start again at the first.
- **Shuffle** (off): play in a random order. Adding or removing items does not
  shuffle again.
- **Now playing** and **Next**: what plays, and what follows it. With Shuffle
  on, the next item is chosen only when it starts, so Next says so.
- **Play item** and **Play now**: jump to any item of the list.

Tick **Show advanced options** for the rest:

- **When not in Program**: what the source does while its scene is not on air
  or the source is hidden in it. By default it stops and starts again when it
  returns. The other choices: play the next item when it returns, pause and
  continue, or keep playing.
- **Restart goes to**: the start of the current item (default) or the first
  item in the list. With the default above this also decides what plays each
  time the source returns to Program.
- **Use hardware decoding when available** (on): decode on the graphics card.
- **Close the file when not in Program** (off): so the file can be replaced
  meanwhile.
- **Folders and playlist files**, shared by every folder, playlist and web
  address row of the source:
  - **Include subfolders** (off).
  - **File types**: file extensions, separated by spaces. Folders play only
    these. An empty field means all the usual video, audio and image types.
  - **Order**: Name (A to Z or Z to A), Oldest first (default), Newest first,
    or As listed. Oldest first is by the date a file was last changed, as the
    Directory Sorter script played a folder; a playlist file has no dates and
    plays in its own order.
  - **Play**: All files, Only the newest file, or Only the newest files, with
    **How many** (5).
  - **Still image duration** (10 s): how long a still image stays on screen.
  - **Check folders again on a timer** (on) and **Time between folder checks**
    (10 s): new files also arrive where the system does not report changes,
    such as some network shares. Playlist files on this computer are read
    again on the same timer when they change.
  - **Time between web list checks** (300 s): how often a list on the web is
    downloaded again.
  - **Freeze the list during playback** (off): when on, new, changed and
    removed files wait until the next Play or Restart, or until you press
    **Apply waiting changes**. They also join when the source returns to
    Program, if it is set to start again or to play the next item. Status
    counts them (`3 changes waiting`); after a press of the button it says
    that they are being applied, or that none were waiting at that moment,
    and **Refresh** shows the list as it is then.
- **Speed** (100% / 1x, range 1% to 200% / 0.01x to 2x): playback speed.
  Changing Speed restarts the item that is playing.

Web lists: a list address may carry a user name and password
(`https://user:password@host/list.m3u`). They are sent to that server, and
after a redirect only if it stays with the same scheme, host and port; never
to another host, nor from http to https. A redirect from https to http is
refused. A list that is not UTF-8 is read in the character set its server
names; it is read as Windows-1252 when the server names none, or names one
this computer cannot convert from, or the text is not valid in the one it
names.

Behind a proxy: Playlister uses the proxy the environment names (`http_proxy`
for http addresses, `https_proxy` for https, `all_proxy` for either that has
none of its own, and `no_proxy` for hosts to reach directly), for each address
of a redirect chain by itself. With none named, Windows uses the system's
proxy settings (but reaches a host named in `no_proxy` directly) and macOS and
Linux use none. On Windows `no_proxy` takes host names and addresses (IPv6 bare or in
brackets), with or without a port, and `*` for every host (a port is compared with the list address's own, not with one a redirect
leads to); address ranges such as `10.0.0.0/8` are not supported there. The items of a list are
played by OBS's own media source (FFmpeg), which looks at `http_proxy` and
`no_proxy` only, so set those for the items to play as well.

## Hotkeys

Hotkeys: OBS **Settings > Hotkeys**, under this source's name. They work only
while the source is shown.

Each Playlister source has Play/Pause, Restart, Stop, Next and Previous.
"Shown" means visible anywhere in OBS: in Program, in the Studio Mode Preview,
in a projector, or in the source's own Properties window, as with OBS's own
Media Source. A press while the source is not shown does nothing and leaves a
line in the OBS log. The buttons of the source toolbar and a controller work
on a hidden source too.

## Languages

Playlister follows the language OBS runs in (Settings > General > Language;
OBS needs a restart to change it). Besides English it speaks Simplified
Chinese, Spanish, Brazilian Portuguese, Russian, Japanese, German, French,
Italian, Korean, Traditional Chinese, Turkish and Dutch. Where OBS itself already translates
a string, Playlister starts from OBS's own translation. The rest was drafted
by an AI model (Claude), and the texts up to 0.5.2 were reviewed by a second one
(Gemini), whose corrections were taken; the Italian file and the texts changed
since have had no second review. No native speaker has checked any of it yet. If something reads wrong in your language, please open an
issue or a pull request: the files are in `data/locale/`, one per language,
and `en-US.ini` is the English source.

## Limitations

- The Properties window shows what plays now and next, but it cannot update
  by itself when an item ends, because that can crash OBS while the window is
  open. Press Refresh or reopen the window. A controller can show it live (see
  For controllers and scripts).
- With Shuffle on, the next item is not known before it starts.
- No audio track or subtitle selection.
- Folders play in this source only. The Directory Sorter script could also
  fill a VLC Video Source or an Image Slideshow; Playlister does not.
- Windows will not rename or delete a file while it plays (OBS holds it open);
  do that after it has played.

## Where Playlister comes from

Playlister is a third-party plugin for OBS Studio. It is not made, endorsed or
supported by the OBS Project. Before 0.3.1 its name began with "OBS"; its
plugin files, source ID and this repository keep the identifier
`obs-playlister`, so existing scenes, settings and hotkeys carry over.

**If you use Media Playlist Source or the Directory Sorter for OBS script (both
by Ian Rodriguez, CodeYan01 on GitHub), this is the one tool that does both
jobs.** Playlister is a fork of
[Media Playlist Source](https://github.com/CodeYan01/media-playlist-source), and
its watched folders take over what the
[Directory Sorter for OBS](https://github.com/CodeYan01/directory_sorter_for_obs)
script did for a playlist: sorted folders that update themselves (by date
modified, oldest first, by default), with no Stop needed. That part is written
from scratch, and folders play in Playlister itself (the script could also
fill a VLC Video Source or an Image Slideshow). Playlister is its
own source type and installs next to Media Playlist Source without either one
replacing the other; existing Media Playlist Source sources are left as they
are. It is a native plugin, so unlike the script it needs no Python. It also
runs on macOS and Linux. What changed in each version is in
[CHANGELOG.md](CHANGELOG.md); the design is in [PLAN.md](PLAN.md) and
[REQUIREMENTS.md](REQUIREMENTS.md).

From Media Playlist Source it keeps: editing the playlist without restarting
the video, even if rows are reordered; remembering the current item across an
OBS restart; playing a chosen item; and the shuffle based on VLC 4's (adding
or removing items does not reshuffle, Previous still works after you pick an
item, and it reshuffles at the end of a pass without losing history).

In the Fadester suite four tools play video at adjustable speeds; which one to
use when is in
[START-HERE.md](https://github.com/jbrick2070/streamdeck-fadester/blob/main/docs/START-HERE.md).
Playlister is the one for ordered lists of files, folders, playlists and
stream addresses; its Speed is a setting that restarts the item, not a live
control.

## For controllers and scripts

What plays now and what comes next can be read without the Properties window,
so a controller key can show it. Only names are published, never paths.

### obs-websocket

Playlister registers the vendor **`obs-playlister`** with obs-websocket (which
ships with OBS). Where obs-websocket is absent or off, nothing is registered.

Request `get_now_next`, sent as obs-websocket's `CallVendorRequest` with
`vendorName` `obs-playlister` and `requestType` `get_now_next`:

| Request field | Type | Meaning |
| :--- | :--- | :--- |
| `source_uuid` | string, optional | The Playlister source to ask, by its UUID |
| `source` | string, optional | The same by its name; used when `source_uuid` is absent |

With one of the two, the response has `found` (bool: whether that is a
Playlister source) and, when it is, the fields below. With neither, the
response has `sources`: an array with these fields for every Playlister source.

| Response field | Type | Meaning |
| :--- | :--- | :--- |
| `source` | string | The source's name |
| `source_uuid` | string | The source's UUID |
| `playing` | string | The name of the item that is loaded: a playlist entry's title, else the file name, else the last part of the address. Empty when nothing is loaded |
| `next` | string | The name of the item that follows; while nothing plays, the item Play starts. Empty when nothing follows or it is not decided |
| `next_decided` | bool | False when Shuffle is on and the next item is chosen only when it starts |
| `state` | string | `playing`, `paused`, `stopped`, `ended`, `opening`, `error` or `none` |
| `status` | string | The worst of the Status lines: `ok`, `warning` or `error` |
| `row` | number | The playing item's row in the Playlist, from 1; 0 when nothing is loaded |
| `item` | number | Its place within a folder or playlist row, from 1; 0 for a file row |
| `items` | number | How many items the whole list has to play |

Event `now_next_changed` (obs-websocket's `VendorEvent`, `vendorName`
`obs-playlister`, `eventType` `now_next_changed`): sent when `playing`, `next`,
`next_decided` or `status` of a source changes. Its `eventData` has the same
fields as the response for one source. Play, pause and stop alone send no
event; obs-websocket's own media events and `GetMediaInputStatus` report those.

### In OBS (scripts and plugins)

The same reading is a proc on the source:

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
calldata_t cd = {0};
proc_handler_call(ph, "get_now_next", &cd);
const char *playing = calldata_string(&cd, "playing"); /* also: next, state, status */
bool next_decided = calldata_bool(&cd, "next_decided");
long long row = calldata_int(&cd, "row"); /* also: item, items */
calldata_free(&cd);
```

and a signal on the source's signal handler when it changes:
`void playlister_now_next(ptr source, string playing, string next, bool next_decided, string status)`.

To select an item from a script or another plugin:

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

The source id is `obs_playlister_source`. To see the setting keys, use
[obs_data_get_json](https://docs.obsproject.com/reference-settings#c.obs_data_get_json)
on the source's settings or look at the scene collection JSON. The order of a
folder is saved as `sort_key` (0 date, 1 name, 2 as listed) and
`sort_descending`; the Order list of Properties follows them.

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

On macOS 13 or later with Xcode 16 or newer and CMake 3.28 or newer, the build is a universal (arm64 and x86_64)
Xcode project; the first configure downloads OBS and its dependencies into `.deps/` as on Windows:

```
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure --no-tests=error
```

On Linux the build uses the installed OBS (its development files) instead:
`obs-studio`, `cmake`, `ninja`, `simde` and libcurl's development files
(`libcurl4-openssl-dev` on Ubuntu). From the repository root:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error
```

## License and credits

GPL-2.0-or-later (see [LICENSE](LICENSE)).

- Media Playlist Source: Copyright (C) 2023 Ian Rodriguez. This project keeps
  his notices; his original project is at
  https://github.com/CodeYan01/media-playlist-source.
- The shuffler is based on VLC's playlist randomizer.
- obs-playlister changes: Copyright (C) 2026 Jeffrey A. Brick. Much of this
  code was written with an AI coding assistant (Claude, by Anthropic); every
  change is reviewed and tested before release, and the commits that used it
  say so.
- The folder watching (Phase 2, in 0.3.0) is written from scratch. It takes
  over the job of the Directory Sorter for OBS script by Ian Rodriguez
  (https://github.com/CodeYan01/directory_sorter_for_obs) but uses none of its
  code, as that script has no license.
- The build system and CI come from the OBS Plugin Template,
  https://github.com/obsproject/obs-plugintemplate (see PLUGIN-TEMPLATE.md).
