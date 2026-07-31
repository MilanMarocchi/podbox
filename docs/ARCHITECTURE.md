# PodBox architecture

Orientation for anyone changing the code. For what PodBox *does*, see the
[README](../README.md).

Roughly 8,500 lines of C++20 across seven subsystems, plus Dear ImGui, GLFW,
TagLib and stb. No framework, no plugin system, no dependency injection —
`main.cpp` builds an `App` and calls `frame()` in a loop.

## Module map

```
src/
  main.cpp        window + GL context + the frame loop
  app/            all UI and all mutation logic (app.cpp is the big one)
  ui/             theme (palette, fonts, ImGui style) and Aqua drawing
  itdb/           iTunesDB reader, writer, and the Play Counts merge
  device/         mount detection, the model table, eject
  library/        everything about audio files and the Mac-side collection
  sync/           diffing the two libraries and copying between them
  audio/          playback (AVFoundation on macOS, a no-op stub elsewhere)
tools/            command-line utilities; see the README table
packaging/        Info.plist template and the app icon
nix/              the Nix derivation
```

Dependencies point one way: `app/` may use anything; `sync/` uses `library/`
and `itdb/`; `library/` and `itdb/` use nothing above them. `ui/` is pure
drawing and knows nothing about iPods. Nothing below `app/` calls ImGui.

### `itdb/` — the database

`iTunesDB` is a tree of tagged, length-prefixed records:

```
mhbd                        database
 └ mhsd                     dataset (type 1 = tracks, 2/3/… = playlists, podcasts, albums)
    ├ mhlt → mhit           track list → track
    │         └ mhod        one string or blob per field (title, artist, …)
    └ mhlp → mhyp           playlist list → playlist
              ├ mhod        playlist name, and smart-playlist criteria
              └ mhip        one entry per track in the playlist
```

`itunesdb.cpp` parses it, `itunesdb_writer.cpp` writes it back,
`playcounts.cpp` folds in the separate `Play Counts` file the firmware writes.

**The central design decision is preservation.** PodBox models a small subset
of what Apple writes, and throwing away the rest would degrade a library a
little more on every write. So three things are carried through verbatim:

- **Raw track headers.** Music.app writes a 624-byte `mhit` where PodBox models
  328. The original bytes are re-emitted and only the edited fields patched, so
  artwork references, gapless data and sort keys survive.
- **Unmodelled `mhod` records** — sort keys, album artist, comments, artwork
  refs — are kept as opaque blobs per track and re-emitted in place.
- **Whole unmodelled datasets** — podcasts, the album list, the modern playlist
  dataset — are kept as byte ranges and written back in their original order.
  Smart-playlist criteria are preserved the same way, so a smart playlist is
  not flattened into a static snapshot on first write.

`itdb_dump <in> <out>` is the regression harness for exactly this: it parses,
writes, re-parses and compares field by field, reporting how much was
preserved. Run it against a real database after touching anything in `itdb/`.

**What is not modelled:** the hash58/hash72 checksums (the scheme is read from
the header and writes are refused when one is required), artwork on the device,
Soundcheck, podcast episode metadata, audiobook resume positions, and the
on-the-go playlists in the `Play Counts` file.

Media type *is* modelled — one `mediaType` field per track, set on import by
`classifyMediaType` in `library/metadata.cpp` and used to split the source list
into Music, Podcasts and Audiobooks. A track with no type set counts as music,
because that is what everything written before PodBox recorded it looks like.

### `library/` — files and the Mac collection

| File | Responsibility |
|---|---|
| `metadata.cpp` | read/write tags via TagLib; decide what is an importable audio file |
| `artwork.cpp` | extract embedded cover art (ID3v2 `APIC`, MP4 `covr`) |
| `transcode.cpp` | shell out to `ffmpeg`/`lame`/`afconvert` |
| `fingerprint.cpp` | identity hash over the audio stream only |
| `fingerprint_store.cpp` | the device-side sidecar of source fingerprints |
| `host_library.cpp` | watch folders, the on-disk index, incremental rescan |
| `applemusic.cpp` | read Music.app via AppleScript, copy files out |
| `dedupe.cpp` | grouping and keeper ranking |

**Fingerprints** hash three windows of the audio stream plus its length,
skipping ID3 tags, FLAC metadata blocks and the MP4 `moov` atom. Cost is
constant regardless of file size. Retagging does not change the result;
re-encoding changes it completely. Treat a match as proof of sameness and a
mismatch as no information at all — that asymmetry is why the sidecar exists
(see the README).

**`host_library` never modifies the files it indexes.** Indexing is not
importing. Rescans skip files whose (mtime, size) are unchanged; vanished files
are flagged rather than dropped; a watch folder on an unmounted volume is
"unavailable this time", not "everything in it is gone".

### `sync/`

`sync_plan.cpp` computes what would change; `sync_engine.cpp` does the copying;
`verify_job.cpp` fingerprints files already on the device.

The important property is that **the two directions carry opposite risk and are
deliberately not symmetric**. For copying, a false "already there" means a song
silently doesn't get added — annoying. For removal, a false "not in the
library" means a song is deleted off the device — unrecoverable. So a device
track must fail the tolerant metadata test *and* the loose artist+title test
*and* the fingerprint test before it is eligible for removal, and duration
bucket boundaries are probed on both sides. Keep that asymmetry if you touch
this code.

## The threading contract

**One rule: workers own copies; the UI thread owns all mutation.**

Three things run off the UI thread — the folder rescan, the sync/import copier
(`SyncEngine`), and the fingerprint verifier (`VerifyJob`). Each is handed an
immutable copy of what it needs, produces results into its own buffer, and sets
an atomic flag. The UI thread polls a `take*()` method each frame and is the
only place that mutates `library_`, `host_`, or anything on disk.

This is why there are no locks around the library, and why a cold scan of a
large folder does not stall the frame loop. If you add background work, follow
the same shape: copy in, results out, no shared mutable state.

`App::writeDatabase()` is the single funnel for every database write. All
thirteen mutation sites go through it, and the guards live there: the hashing
scheme, the Apple Music mid-sync interlock, backup rotation, the atomic
temp-and-rename, and keeping the fingerprint sidecar in step. Add a mutation
and you get all of that for free — do not write the database any other way.

## `app/`

Four files, split by what each has a reason to change for:

| File | Holds |
|---|---|
| `app.cpp` | lifecycle, the frame loop, library loading, every mutation path, selection, playback |
| `app_chrome.cpp` | toolbar, transport, source list, column browser, track table, device pane, status bar |
| `app_modals.cpp` | the eight sheets and the work each drives |
| `app_util.{h,cpp}` | layout constants, and the drawing and text helpers the other three share |

Most state lives in `App`, grouped into a struct per feature (`apple_`,
`dupes_`, `getInfo_`, `syncUi_`, `plEdit_`, `art_`, `scan_`, `browser_`) so a
dialog's state travels with the dialog. The track-view state — `view_`,
`search_`, `sortCol_`, `selection_` — stays flat because nearly every function
here reads it.

Three abstractions worth knowing before you read any of it:

- **`shownLibrary()` / `shownIndex()`** return either the iPod's library or a
  `Library`-shaped view of the Mac one, so a single track table, search, sort
  and dedupe implementation serves both.
- **`rebuildVisible()`** is the one funnel for filtering and sorting. It
  produces `visible_`, a vector of `(display position, track index)`. Anything
  that changes what should be on screen sets `visibleDirty_` and lets this
  function do the work; nothing else filters or sorts. The display position is
  load-bearing — shift-click ranges and playlist drag-reorder both index
  through it.
- **The column browser reads a set upstream of the one it constrains.**
  `rebuildVisible()` runs in stages: the source's rows, narrowed by search,
  then the Genres/Artists/Albums facet lists, then the browser's own predicate,
  then the sort. The facet lists come out of the search stage and the table out
  of the predicate stage, so nothing reads what it writes. There is exactly one
  dirty flag for all of it, deliberately: a second cache would have to be
  invalidated at every one of `visibleDirty_`'s eighteen set-sites.

## `ui/`

`theme.cpp` sets the ImGui style and loads Lucida Grande (falling back to
Helvetica, then DejaVu Sans) at 13px and 11px, regular and bold. Fonts are
rasterised at `size × contentScale` with `FontGlobalScale = 1/contentScale`, so
all layout code works in logical pixels and stays crisp on Retina.

There is **no icon font**. Every glyph in the chrome — transport arrows, the
eject symbol, the speaker, the stars, the iPod — is drawn with `ImDrawList`
primitives. Hairlines are drawn at `+0.5` offsets to land on a pixel.

`aqua.cpp` provides the period drawing primitives: `gradientRect` (which fakes
a rounded gradient with three stacked fills, since ImGui cannot round a
multicolour rect), pill `button`, `roundButton`, and the `beginSheet`/`endSheet`
pair that makes every modal look like a Mac OS X sheet.

## On-disk formats PodBox owns

Both are line-oriented text with a magic first line, chosen so they can be
inspected and repaired by hand.

- `library.tsv` — `podbox-library 1`, then one `W` row per watch folder and one
  `T` row per track, tab-separated with escaping. Written atomically via
  temp-and-rename. **It has no version field beyond the magic, and rows with
  too few columns are silently dropped** — so adding a column breaks old files
  quietly. Fix that properly if you extend it.
- `PodBoxFingerprints` — `podbox-fingerprints 1`, keyed by `Track::dbid`. Lives
  on the device so it travels with it.

## Building and testing

See the README for build instructions. `nix build` is hermetic and reproducible;
plain CMake fetches dependencies at configure time. Both build the same pinned
revisions — **if you bump one, bump the other**: the versions in
`CMakeLists.txt` and in `nix/podbox.nix` must stay in step or the two paths stop
building the same program.

Testing is thin and honest about it:

- `dedupe_test` is the only tool with real assertions and a meaningful exit
  code. It is wired into CTest — `ctest --test-dir build`. Run it before
  committing.
- `itdb_dump <in> <out>` is a round-trip regression check, but needs a real
  `iTunesDB`.
- Everything else in `tools/` is a smoke tool that prints and returns 0, and
  most need a mounted iPod or a live Apple Music library.
- `testdata/` is gitignored, so fixtures are not shared. Point the tools at
  your own device.

There is no CI, no `.clang-format`, and no code signing. All three are on the
roadmap.
