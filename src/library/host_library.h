#pragma once

#include "itdb/itunesdb.h"
#include "library/fingerprint.h"
#include "library/music_folder.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
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

    std::string origin;        // "watch", "import" or "applemusic"
    bool missing = false;      // the file was not there on the last scan
};

// A folder PodBox imports from, such as a Soulseek downloads folder. New songs
// found in it are copied into the library folder; nothing in it is ever
// changed, moved or deleted.
struct WatchFolder {
    std::filesystem::path path;
    bool enabled = true;
};

// True when `file` lies somewhere below `root`. Compared lexically, so
// "/Music/A" does not contain "/Music/AB/x.mp3", and a trailing slash on the
// root makes no difference.
bool isWithinFolder(const std::filesystem::path& file,
                    const std::filesystem::path& root);

struct ScanStats {
    int imported = 0;    // copied in from an import folder
    int added = 0;       // found already in the library folder
    int updated = 0;     // file changed since last scan
    int unchanged = 0;
    int missing = 0;     // previously indexed, now gone
    int failed = 0;      // unreadable or untaggable
    int skippedPartial = 0;  // inside a downloading/incomplete folder

    int total() const { return added + updated + unchanged; }
};

// PodBox's own library, entirely separate from Apple Music's.
//
// The library is the music folder (~/Music/PodBox, music_folder.h): every
// track's file is there, and it is the only thing indexed, synced or
// deduplicated. The index itself lives in ~/Library/Application Support/PodBox/.
// Nothing here writes to the Apple Music library or to the folders it imports
// from.
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

    // Where the library's files live. Only tests point it anywhere else.
    const std::filesystem::path& musicFolder() const { return musicFolder_; }
    void setMusicFolder(const std::filesystem::path& p) { musicFolder_ = p; }

    // Every file ever copied in from an import folder. A file here is never
    // copied again, even once its copy has been deleted from the library —
    // that is what keeps a removed duplicate removed.
    const std::unordered_set<std::string>& importedFiles() const {
        return imported_;
    }

    // On a first run with no folders configured, offer the obvious one. Only
    // adds a folder that actually exists, so this is a no-op elsewhere.
    void seedDefaultWatchFolders();

    // Brings the library up to date:
    //   1. indexes the music folder, the library itself;
    //   2. copies in any track still played from outside it (libraries from
    //      before the music folder indexed their folders in place), keeping
    //      its play count and rating;
    //   3. copies in every song in an enabled import folder that has not been
    //      imported before. Folders marking downloads in progress are skipped.
    // `fingerprintFiles` also hashes audio content, which is what makes
    // byte-identical duplicate detection possible but roughly triples the
    // cost of a cold scan.
    //
    // `cancelled` is polled between files so a long scan stays interruptible.
    ScanStats rescan(bool fingerprintFiles = true,
                     const std::atomic<bool>* cancelled = nullptr,
                     std::string* currentFile = nullptr);

    // Marks every track whose file has gone, without the rest of a rescan.
    // Returns how many are now missing.
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
    std::filesystem::path musicFolder_ = defaultMusicFolder();
    std::unordered_set<std::string> imported_;
};

}  // namespace podbox
