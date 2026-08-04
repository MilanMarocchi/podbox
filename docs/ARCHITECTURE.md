# PodBox architecture

Orientation for anyone changing the code. For what PodBox *does*, see the
[README](../README.md).

Roughly 11,000 lines of C++20 across seven subsystems, plus Dear ImGui, GLFW,
TagLib, stb, SQLite and the pinned hashAB implementation. No framework, no
plugin system, no dependency injection —
`main.cpp` builds an `App` and calls `frame()` in a loop.

## Module map

```
src/
  main.cpp        window + GL context + the frame loop
  app/            all UI and all mutation logic (app.cpp is the big one)
  ui/             theme, Aqua drawing, and native macOS window/media commands
  itdb/           iTunesDB/iTunesSD readers and writers, and play-count merges
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

`itunesdb.cpp` parses it, `itunesdb_writer.cpp` writes it back, and
`playcounts.cpp` folds in the separate `Play Counts` file the firmware writes.

A 3rd/4th-generation Shuffle additionally boots from `iTunesSD`, with one
positional `iTunesStats` record per indexed track. `itunessd.cpp` parses the
modern `bdhs` form, preserves Apple-written track and playlist records where
possible, adds records for new tracks, remaps the positional stats, and uses
macOS `say` to synthesize missing 22.05 kHz mono VoiceOver WAV files. The
legacy 1G/2G format is detected but stays read-only. `App::writeDatabase()`
stages and validates `iTunesDB`, `iTunesSD`, and `iTunesStats`, then installs or
rolls back the three as a matched set.

A nano 6G/7G additionally boots from the device-initialized SQLite bundle at
`iTunes Library.itlp`. `itunessqlite.cpp` copies that bundle into staging—so
model-specific schema, indexes and triggers are retained—then updates
`Library.itdb`, `Dynamic.itdb`, `Locations.itdb` and relevant `Extras.itdb`
rows. It registers the nano sort-key functions preserved triggers can call,
regenerates the hashAB-signed `Locations.itdb.cbk`, runs `PRAGMA
integrity_check`, and checks row counts against the in-memory library. The app
installs this directory and the signed `iTunesCDB` as one rollback unit.

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

**Checksums.** Newer devices sign the database. `hash58.cpp` implements the
scheme iPod classic and nano 3G/4G use — HMAC-SHA1 over the image with three
header fields zeroed, keyed by a value derived from the device's FireWire GUID.
The algorithm was reverse-engineered by wtbw and first implemented by Christophe
Fergeau for libgpod under a 3-clause BSD licence, which is why that notice is
reproduced at the top of the file.

`hash72.cpp` implements the scheme the nano 5G uses: SHA-1 over the image with
four header fields zeroed, then a 46-byte signature — marker, 12 random bytes,
and AES-128-CBC of SHA1||random under a fixed key. It was reverse-engineered by
Chris Lee and first implemented for libgpod's LGPL `itdb_hash72.c`, and is an
independent implementation of the same algorithm. hash72 is why the nano 5G has
a `HashInfo` file (`iPod_Control/Device/HashInfo`): the device verifies
signatures with its (UUID, random, IV), so the first write recovers that
(IV, random) pair from the database the device already accepts — the signature
decrypts back to the database's own SHA1, which reveals the IV — and records it
in `HashInfo`. It also stores its database compressed as `iTunesCDB`
(`itunescdb.cpp`): a zlib stream after the mhbd header, flagged at 0xA8, with
`total_length` counting the compressed physical size. The hash72 signature
covers the compressed bytes exactly as they sit on disk, so the writer deflates
before signing and the parser inflates before reading.

The GUID hash58 is keyed on comes from `iPod_Control/Device/SysInfoExtended`,
and failing that from the device's USB serial number via IOKit
(`device/usb_serial.h`) — plenty of iPods have no SysInfoExtended at all, and
without a GUID a signing device can never be verified and so can never be
written.

`hashab.cpp` wraps the open, Unlicense-licensed
[`dstaley/hashab`](https://github.com/dstaley/hashab) C implementation. CMake
and Nix pin the audited revision; do not unpin the large white-box tables or
silently track its default branch. The wrapper zeroes/restores the `iTunesCDB`
header ranges, recovers the embedded 23-byte nonce from an accepted signature,
and recalculates the complete 57-byte signature before allowing writes. HashAB
alone is insufficient: `App::verifyChecksum()` also validates the SQLite
bundle and CBK before a nano 6G/7G becomes writable.

The `podbox_add` command applies the same gate before copying a source file.
It derives signing parameters only from the database already accepted by that
device, stages and reads back the new iTunesDB/iTunesCDB, stages the nano SQLite
bundle or Shuffle indexes when applicable, and swaps the complete set together.
If staging or backup fails, newly copied audio and VoiceOver files are removed.
If installation fails, the previous live database set is restored as a unit;
if the filesystem also refuses that rollback, the audio is deliberately kept
so the partially installed database cannot point at files PodBox just deleted.

Correctness is established in two halves. SHA-1, HMAC-SHA1, AES-128 and the
generated S-boxes are covered by `hash58_test` and `hash72_test` against
published vectors (FIPS 180-1/RFC 2202, FIPS 197) — and the S-boxes are
*generated* rather than tabulated, so there is no page of magic constants to
mistype. `hashab_test` pins published hashAB vectors, nonce inversion and the
physical compressed writer; `itunessqlite_test` exercises a device-like
bundle, preserved triggers, CBK signing and repeat writes. The composition is
proven per-device at runtime by
`App::verifyChecksum()`, which recomputes the checksum of the database the iPod
is already using and compares it to the stored one. Writes to a hash58,
hash72 or hashAB device are refused until that matches; for hash72 the recovery of the
(IV, random) pair is what makes later writes possible at all.

**What is not modelled:** artwork on the device,
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

On macOS, `macos_window.mm` registers Play, Pause, and Play/Pause with
`MPRemoteCommandCenter` and publishes the current state through
`MPNowPlayingInfoCenter`. MediaPlayer callbacks only enqueue commands and wake
GLFW; `main.cpp` consumes them on the UI thread and calls the same `App`
playback methods as the toolbar.

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
  temp-and-rename. Rows are read positionally against a minimum column count:
  columns past it are read when present and defaulted when absent, so the
  format can gain a column without making every older file unreadable. If you
  add one, append it — never reorder, and never raise `kMinTrackFields`, which
  is what makes an older file load rather than silently count as zero tracks.
- `PodBoxFingerprints` — `podbox-fingerprints 1`, keyed by `Track::dbid`. Lives
  on the device so it travels with it.

## Building and testing

See the README for build instructions. `nix build` is hermetic and reproducible;
plain CMake fetches dependencies at configure time. Both build the same pinned
revisions — **if you bump one, bump the other**: the versions in
`CMakeLists.txt` and in `nix/podbox.nix` must stay in step or the two paths stop
building the same program.

The assertion-based tests are wired into CTest; run
`ctest --test-dir build --output-on-failure` before committing. They cover
deduplication/import metadata, UI helpers, hash58/hash72/hashAB cryptographic
vectors, nano SQLite/CBK composition, and modern Shuffle `iTunesSD`
parsing/writing.

The remaining device-facing checks need local data:

- `itdb_dump <in> <out>` is a round-trip regression check, but needs a real
  `iTunesDB`.
- Most other tools need a mounted iPod or a live Apple Music library.
- `testdata/` is gitignored, so fixtures are not shared. Point the tools at
  your own device.

GitHub Actions builds and runs CTest. There is no `.clang-format` or code
signing; both remain on the roadmap.
