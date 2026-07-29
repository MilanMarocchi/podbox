# PodBox

A standalone iPod manager for macOS — manage the music library on a classic
iPod the way the old iTunes and apps like PodCenter let you: **drag songs on,
delete songs off, build playlists — no full-library sync, no iTunes
required.** Built in C++ with [Dear ImGui](https://github.com/ocornut/imgui),
styled after iTunes 7–9.

![PodBox](docs/screenshot.png)

## Features

- **Browse** the iPod's library in a sortable, searchable track list, plus
  its playlists — in a faithful iTunes 7–9 look (Lucida Grande, source-list
  sidebar, brushed-metal toolbar with an "LCD" readout, capacity bar).
- **Add songs** by dragging files or folders onto the window. Metadata is
  read with TagLib and files are copied into the iPod's music folders with
  iTunes-style scrambled names. Copies run on a background thread with
  progress shown in the toolbar.
- **Delete songs** from the device (right-click → *Remove from iPod*, or
  select and press Delete), with a confirmation dialog.
- **Playlists** — create, rename, delete, add/remove tracks (right-click a
  song → *Add to Playlist*), and drag to reorder within a playlist.
- **Play counts & ratings** written by the iPod are merged back into the
  library automatically on connect.
- **Album artwork** embedded in your files is shown for the selected track.
- **Safe eject** — click the eject button next to the device to flush and
  unmount before you unplug.

## Supported devices

PodBox reads the library of **any classic-line iPod** — 1st through 5.5th
generation, iPod mini, iPod photo, and iPod nano/classic — by parsing the
`iTunesDB` directly.

**Writing** (adding/removing songs and playlists) currently works on iPods
whose database needs no checksum: **iPod 1st–5.5th generation, iPod mini,
iPod photo, and iPod nano 1st/2nd generation.** PodBox reads the required
hashing scheme from the database header and will refuse to write — rather
than risk corrupting the library — on models that need a checksum it does not
yet produce:

| Model | Read | Write |
|---|---|---|
| iPod 1st–5.5th gen, mini, photo, nano 1G/2G | ✅ | ✅ |
| iPod classic (6G/7G), nano 3G–5G | ✅ | ⛔ needs *hash58* (planned) |
| iPod nano 6G/7G | ✅ | ⛔ needs *hash72* |
| iPod shuffle | — | — separate `iTunesSD` format (planned) |
| iPod touch / iPhone | — | — different sync protocol, out of scope |

The iPod must be mounted as a disk. Windows-formatted (FAT32) iPods mount
automatically on macOS; a Mac-formatted (HFS+) iPod works too. If your iPod
only appears in Finder's device sync view, enable "disk use" once (or boot it
into disk mode with Select+Play) so it mounts as a volume.

## Building

Requires **CMake 3.24+** and a **C++20** compiler. On macOS that's the
Command Line Tools (`xcode-select --install`). All other dependencies — Dear
ImGui, GLFW, TagLib, stb — are fetched automatically by CMake at configure
time; nothing needs to be installed via Homebrew.

```sh
git clone https://github.com/MilanMarocchi/podbox.git
cd podbox
cmake -S . -B build
cmake --build build -j
./build/podbox
```

Linux support is planned; the device-detection layer is already isolated
behind platform guards, but the app is currently tested only on macOS.

## Usage

1. Plug in your iPod and wait for it to mount. PodBox detects it and shows the
   model, serial, firmware and capacity under the **device** entry.
2. Click **Music** to see every song, or a **playlist** to see its contents.
   Sort by clicking a column header; filter with the search box.
3. **Add music**: drag audio files or folders onto the window. Supported
   formats are the ones the iPod itself plays: MP3, AAC/ALAC (`.m4a`/`.m4b`),
   WAV and AIFF.
4. **Remove music**: right-click a song → *Remove from iPod*.
5. **Playlists**: click *+ New Playlist*, or right-click a song →
   *Add to Playlist*. Right-click a playlist to rename or delete it. Drag rows
   to reorder a playlist.
6. **Eject** with the button on the device row before unplugging.

## Safety

PodBox writes directly to your iPod's database, so it is careful:

- The **first time it writes**, it backs up the original iTunes-written
  database to `iPod_Control/iTunes/iTunesDB.podbox-backup` on the device. To
  restore, copy that file back over `iTunesDB`.
- Every database write goes to a temporary file and is then atomically
  renamed into place, so an interrupted write cannot corrupt the library.
- Writes are refused outright on iPods that need a checksum PodBox cannot yet
  produce (see the table above).

Back up anything you care about before using pre-release software on it.

## Command-line tools

The build also produces small utilities used for testing:

- `itdb_dump <iTunesDB> [<out> [+pl]]` — print a database summary; with an
  output path, round-trip it through the writer and verify nothing changed.
- `podbox_add <mount-point> <file>...` — add files to a mounted iPod from the
  shell (same pipeline as the GUI).
- `playcounts_test <iTunesDB> <Play Counts>` — verify the play-count merge.

## How it works

The `iTunesDB` is a binary tree of tagged records (`mhbd` → `mhsd` → track and
playlist lists). PodBox has its own reader and writer for this format
(`src/itdb/`) rather than depending on the venerable but hard-to-build
[libgpod](http://www.gtkpod.org/libgpod/). Songs live in `iPod_Control/Music/F00…F49`
under obfuscated names; the play counts, ratings and on-the-go playlists the
device records live in a separate `Play Counts` file that is merged on connect.

## Roadmap

- `hash58` checksum so iPod classic and nano 3G–5G can be written
- Album artwork **on** the device (`ArtworkDB` + `.ithmb` thumbnails)
- Podcasts and audiobooks as first-class media types
- Optional transcoding (e.g. FLAC → ALAC) on import
- iPod shuffle (`iTunesSD`) support
- Linux build

## License

[MIT](LICENSE) © 2026 Milan Marocchi. Contributions welcome.

This project is not affiliated with or endorsed by Apple. iPod and iTunes are
trademarks of Apple Inc.
