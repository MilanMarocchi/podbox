#pragma once

#include "itdb/itunesdb.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace podbox {

// PodBox's small amount of bookkeeping for a folder-based player. Audio and
// playlists remain ordinary files understood by the firmware; this state only
// records which files PodBox is allowed to remove during a mirror sync and
// keeps playlist identities stable across reconnects.
struct FilesystemPlayerState {
    std::unordered_set<std::string> managedTracks;  // mount-relative paths
    std::unordered_map<std::uint64_t, std::string> playlistLocations;
    // Runtime snapshots keep unrelated writes from normalizing or stripping
    // comments out of M3U files that have not actually changed.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
        playlistContents;
    std::unordered_map<std::uint64_t, std::string> playlistNames;
};

struct FilesystemPlayerLoad {
    std::optional<Library> library;
    std::unordered_set<std::uint32_t> managedTrackIds;
    std::string error;
};

FilesystemPlayerLoad loadFilesystemPlayer(
    const std::filesystem::path& mount,
    const std::filesystem::path& musicDirectory,
    FilesystemPlayerState* state);

// Writes M3U8 playlists and the managed-file manifest. Existing audio files
// are not retagged here; explicit Get Info edits handle that separately.
bool saveFilesystemPlayer(const std::filesystem::path& mount,
                          const std::filesystem::path& musicDirectory,
                          const Library& library,
                          FilesystemPlayerState* state,
                          std::string* error);

// Allocates a readable Artist/Album/track filename below the player's music
// directory. `location` receives a slash-separated path relative to `mount`.
std::filesystem::path allocateFilesystemMusicPath(
    const std::filesystem::path& mount,
    const std::filesystem::path& musicDirectory, const Track& metadata,
    const std::string& extension, std::string* location);

std::uint64_t filesystemTrackDbid(const std::string& location);

}  // namespace podbox
