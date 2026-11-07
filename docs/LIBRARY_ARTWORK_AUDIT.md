# Library preservation and artwork audit — 5 November 2026

This audit covers Mac imports and rescans, index persistence, duplicate removal,
iPod database saves, file conversion, artwork extraction and iPod thumbnail
writing. The preceding recovery restored 1,126 absent songs; the resulting Mac
library contains 1,904 tracks. The historical import ledger identifies the
originals, but does not record deletion reasons. The cause of that particular
mass disappearance therefore cannot be established from the surviving files.

## Findings and fixes

| Finding | Resulting behavior | Regression coverage |
|---|---|---|
| Existing names were reused solely because file sizes matched. | Compare complete file contents. Different recordings receive separate filenames. | Equal-size WAVs with different sample data. |
| Concurrent importers used shared staging names and overwriting renames. | Unique staging files, exclusive final installation, and no deletion of another importer's destination after failure. | Concurrent Mac imports and an occupied player destination. |
| Mac path strings treated filesystem case aliases as separate songs. | Match filesystem identity, retain stable ids, ratings and play counts. | Case alias rescan and upsert. |
| The imported-file ledger permanently skipped missing copies. | Restore still-indexed missing songs from originals with matching audio fingerprints. Deliberately unlisted duplicates stay removed. | Lost copy restoration with history, deliberate removal, stale missing flags. |
| Invalid headers/rows could become an empty or partial index at the next save. | Transactional parsing rejects damage without clearing the live library. New saves have a checksum and record-count footer. Legacy valid indexes remain readable. | Damaged legacy row, truncated footer, changed field, escaped fields. |
| Index saves lacked backups and protection against another process's newer revision. | Five backups, unique temporary files, flush/atomic replacement, advisory locking and revision checks. | Backup rotation, stale independent writer, shared worker snapshots. |
| Local save errors were transient and did not prevent quitting. | Failure remains visible; quitting requires retrying or explicitly discarding unsaved index changes. | Failed save and successful retry. |
| Duplicate reviews could become stale; overlapping removals could name each other's keepers. | Recheck reviewed metadata/fingerprints, protect every keeper in the batch, and resolve ownership through directory symlinks. Matching audio fingerprints are the default review filter. | Stale review, cyclic removals, missing keeper and symlink escape. |
| The iPod's active-sync check expired after 20 seconds. | Save/deletion also checks the database revision loaded by the session; another app's change remains protected after the busy window. Recheck immediately before replacing the database. | An externally changed database timestamped one minute ago remains untouched. |
| MP3 exports discarded APIC artwork; ALAC omitted pictures; encoder fallbacks differed. | Normalize a valid front cover to a bounded baseline JPEG. Preserve it through MP3, ALAC and AAC conversions, including fallback paths. Keep MP3 text before artwork in ID3v2.3. | Actual encoders across MP3/FLAC/M4A/WAV/AIFF source containers. |
| Artwork extraction ignored WAV/AIFF, chose the first MP3 image, and stopped at malformed pictures. | Support ID3 APIC in WAV/AIFF; prefer decodable front covers, then other decodable embedded images. | Back cover first, corrupt front cover first, corrupt MP4 covr first. |
| Folder-only covers disappeared when copying audio into the library or onto a player. | Preserve recognized JPEG/PNG sidecars in the Mac library and embed them when exporting audio. | Sidecar → Mac library → MP3 and original WAV. |
| WAV INFO text could be hidden by a picture-only ID3 tag; WAV/AIFF fingerprints included metadata. | Fall back to WAV INFO text, and fingerprint data/SSND sample chunks independently of tags and pictures. Failed fingerprint reads yield no fingerprint. | WAV conversion text and unchanged audio identity after adding a cover. |
| iPod imports never created ArtworkDB/ithmb entries. | Generate model-specific RGB565 thumbnails, ArtworkDB records and iTunesDB artwork flags/links; provide “Fill Missing Album Artwork” in the device pane. Both GUI and CLI imports use the writer. | 5G/video and Classic formats, persistent ids, padding/pixel order, paired iTunesDB links, repeated saves, append preservation, missing/truncated thumbnails and malformed ArtworkDB. |

## iPod artwork storage and compatibility

Known profiles include iPod photo, video/5G/5.5G, Classic and nano 1G–5G when
identified by the device profile. Video uses 100×100 and 200×200 RGB565LE;
Classic uses its 56×56, two 128×128 and 320×320 formats. Unknown models and
nano 6G/7G receive no guessed artwork layout. Existing artwork on those devices
continues to pass through unchanged.

Existing image ids, valid image records and old thumbnail bytes are retained.
Each update creates new thumbnail files; files are flushed before the
ArtworkDB is atomically installed, and ArtworkDB is installed before the music
database. If a later music database save fails, the old music database's
artwork references remain valid. Broken thumbnail records are repaired under
their existing image ids. Previous ArtworkDB versions are retained in five
rolling backups. Existing valid artwork is not replaced by the repair button.

The implementation uses the binary layouts and format tables documented in
[libgpod's format definitions](https://github.com/gtkpod/libgpod/blob/master/src/db-itunes-parser.h),
[device format tables](https://github.com/gtkpod/libgpod/blob/master/src/itdb_device.c),
and [artwork writer](https://github.com/gtkpod/libgpod/blob/master/src/db-artwork-writer.c).
No libgpod source implementation is included as a dependency.

## Validation and remaining limits

Build with CMake and run `ctest --test-dir build-audit --output-on-failure`.
The 15 suites include real encoding/container round trips and isolated synthetic
iPod volumes. No test writes to the user's music library or a physical player.
The actual Mac index is separately checked read-only with `library_test show`.

A physical iPod was not connected during this audit, so firmware display and
USB interruption behavior have not been tested on hardware. Classic Cover Flow
album-catalog creation is outside this change; the writer sets the track artwork
links and preserves existing album datasets. Artwork records and thumbnail files
are intentionally not compacted during ordinary saves, to preserve references
from existing music-database backups. There may be harmless unreferenced new
thumbnail files after a failed update.

The safeguards cannot replace an independent backup against drive failure or
manual deletion. A missing song with no surviving indexed entry, original or
backup still requires explicit recovery; the old imported-file ledger alone
cannot distinguish intentional removals from accidental ones.
