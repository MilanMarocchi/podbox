#pragma once

#include "itdb/itunesdb.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace podbox {

// One song as Apple Music describes it.
struct AppleMusicTrack {
    std::filesystem::path file;  // empty for streaming-only entries
    Track meta;
};

struct AppleMusicRead {
    bool ok = false;
    std::string error;
    std::vector<AppleMusicTrack> tracks;

    // Reported rather than silently dropped: songs that exist in Apple Music
    // but have nothing on this Mac to copy.
    int streamingOnly = 0;
    int fileMissing = 0;  // has a path, but nothing is there
    int drmProtected = 0;  // .m4p — cannot be transcoded or played on an iPod
};

// True when Music.app is installed and scriptable.
bool appleMusicAvailable();

// Reads the Apple Music library through AppleScript.
//
// This is the only route that works: the library folder is TCC-protected, so
// PodBox cannot enumerate it, but Music.app will hand over each track's real
// path and reading that path directly is permitted. Nothing here writes to
// Apple Music.
//
// `limit` caps how many tracks are returned (0 = all), which keeps a dry run
// cheap.
AppleMusicRead readAppleMusicLibrary(int limit = 0);

// Progress for the copy step. Return false to cancel.
using CopyProgress =
    std::function<bool(int done, int total, const std::string& name)>;

struct CopyResult {
    int copied = 0;
    int skipped = 0;  // already present at the destination
    int failed = 0;
    std::uint64_t bytes = 0;
    bool cancelled = false;
};

// The folder PodBox copies Apple Music songs into, so the library keeps
// working regardless of what Music.app later does to its own media folder.
std::filesystem::path appleMusicCopyRoot();

// Copies each track's file to `root`/Artist/Album/NN Title.ext, filling in
// `dest`. Existing identical-size files are left alone, so this is resumable.
CopyResult copyAppleMusicFiles(std::vector<AppleMusicTrack>& tracks,
                               const std::filesystem::path& root,
                               const CopyProgress& progress);

}  // namespace podbox
