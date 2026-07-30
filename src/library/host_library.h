#pragma once

#include "itdb/itunesdb.h"
#include "library/fingerprint.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace podbox {

// A song in the Mac-side library.
//
// Reuses Track for its metadata so the dedupe keys, sorting and the track
// table all work on host and device songs alike.
struct HostTrack {
    std::uint64_t id = 0;              // stable within this library
    std::filesystem::path file;        // where the audio actually is
    Track meta;
    AudioFingerprint fp;

    // For skipping unchanged files on a rescan. Re-reading tags and hashing
    // every file every time would make a rescan of a large folder painful.
    std::int64_t mtime = 0;
    std::uint64_t size = 0;

    std::string origin;        // "watch" or "applemusic"
    bool missing = false;      // the file was not there on the last scan
};

// A folder PodBox indexes. The files stay where they are and are never
// modified — indexing a folder is not the same as importing from it.
struct WatchFolder {
    std::filesystem::path path;
    bool enabled = true;
};

struct ScanStats {
    int added = 0;
    int updated = 0;     // file changed since last scan
    int unchanged = 0;
    int missing = 0;     // previously indexed, now gone
    int failed = 0;      // unreadable or untaggable
    int skippedPartial = 0;  // inside a downloading/incomplete folder

    int total() const { return added + updated + unchanged; }
};

// PodBox's own library, entirely separate from Apple Music's.
//
// Lives in ~/Library/Application Support/PodBox/. Nothing here writes to the
// Apple Music library or to the folders it indexes.
class HostLibrary {
public:
    static std::filesystem::path configDir();
    static std::filesystem::path libraryPath();

    // Reads the library from disk. A missing file is not an error — it just
    // means this is a first run.
    bool load();
    bool save() const;

    const std::vector<HostTrack>& tracks() const { return tracks_; }
    std::vector<HostTrack>& tracks() { return tracks_; }
    const std::vector<WatchFolder>& watchFolders() const { return watch_; }

    void addWatchFolder(const std::filesystem::path& p);
    void removeWatchFolder(std::size_t index);
    void setWatchFolderEnabled(std::size_t index, bool enabled);

    // On a first run with no folders configured, offer the obvious one. Only
    // adds a folder that actually exists, so this is a no-op elsewhere.
    void seedDefaultWatchFolders();

    // Re-indexes every enabled watch folder. `fingerprintFiles` also hashes
    // audio content, which is what makes byte-identical duplicate detection
    // possible but roughly triples the cost of a cold scan.
    //
    // `cancelled` is polled between files so a long scan stays interruptible.
    ScanStats rescan(bool fingerprintFiles = true,
                     const bool* cancelled = nullptr,
                     std::string* currentFile = nullptr);

    // Marks every track whose file has gone, whatever it was indexed from.
    // Watch-folder tracks are handled by rescan(); this also covers copies
    // (from Apple Music, say) that were later deleted by hand. Returns how
    // many are now missing.
    int refreshMissing();

    // Drops the tracks currently flagged missing. Returns how many went.
    int removeMissing();

    // Adds a track, or updates the one already at that path. Matching on the
    // file path means re-importing a source updates play counts instead of
    // duplicating the library. Returns true when newly added.
    bool upsert(const std::filesystem::path& file, const Track& meta,
                const std::string& origin, const AudioFingerprint& fp = {});

    // Folder names that mark work in progress. A part-downloaded FLAC has
    // perfectly valid-looking tags and would index as a corrupt track.
    static bool isPartialFolder(const std::string& name);

private:
    std::vector<HostTrack> tracks_;
    std::vector<WatchFolder> watch_;
    std::uint64_t nextId_ = 1;
};

}  // namespace podbox
