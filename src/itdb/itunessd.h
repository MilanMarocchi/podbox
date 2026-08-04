#pragma once

#include "itdb/itunesdb.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace podbox {

// The third- and fourth-generation Shuffle database starts with "bdhs".
// First/second-generation Shuffles also have an iTunesSD, but use a different
// format and must not be handed this writer's output.
enum class ItunesSdKind { None, Legacy, Modern };

struct ShuffleTrackRecord {
    std::string location;  // slash-separated, rooted at /iPod_Control
    std::uint64_t dbid = 0;
};

struct ShufflePlaylistRecord {
    std::uint64_t dbid = 0;
    std::uint32_t type = 0;  // 1 = master, 2 = ordinary playlist
    std::vector<std::uint32_t> trackIndices;
};

struct ShuffleDatabase {
    std::uint32_t version = 0;
    bool voiceOverEnabled = false;
    std::vector<ShuffleTrackRecord> tracks;
    std::vector<ShufflePlaylistRecord> playlists;
};

ItunesSdKind detectItunesSd(const std::filesystem::path& path);

// Read-only parser used for format validation and tests.
std::optional<ShuffleDatabase> parseItunesSd(
    const std::filesystem::path& path, std::string* error = nullptr);

// Recover playlist ids from iTunesSD when an older PodBox build changed them
// only in iTunesDB. Matching membership proves which playlist is which.
int reconcileShufflePlaylistIds(Library& library,
                                const std::filesystem::path& existingSdPath);

struct ItunesSdWriteOptions {
    // Generate only missing announcements; existing Apple-generated files are
    // retained. The database is not replaced if speech generation fails.
    bool generateVoiceOver = true;
    // A renamed playlist keeps its persistent id, so its old announcement
    // exists but says the wrong name. These ids are regenerated atomically.
    std::vector<std::uint64_t> refreshPlaylistVoiceOver;
};

// Writes a modern (Shuffle 3G/4G) iTunesSD. `existingPath` supplies the exact
// device dialect and the raw records whose unknown fields must survive.
// `outputPath` may be a temporary sibling for an atomic replace.
bool writeItunesSd(const Library& library,
                   const std::filesystem::path& existingPath,
                   const std::filesystem::path& outputPath,
                   const std::filesystem::path& mountPoint,
                   std::string* error,
                   const ItunesSdWriteOptions& options = {});

// iTunesStats is positional: entry N belongs to iTunesSD track N. Remap the
// existing entries by location so additions/removals do not attach play counts
// or bookmarks to the wrong song. New tracks receive an empty 32-byte entry.
bool writeShuffleStats(const Library& library,
                       const std::filesystem::path& existingSdPath,
                       const std::filesystem::path& existingStatsPath,
                       const std::filesystem::path& outputPath,
                       std::string* error);

// The firmware treats the persistent id as a little-endian 64-bit value in
// iTunesSD and its conventional big-endian hex form as the spoken-file name.
std::string shuffleVoiceOverName(std::uint64_t dbid);

}  // namespace podbox
