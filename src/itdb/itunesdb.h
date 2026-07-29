#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace podbox {

struct Track {
    std::uint32_t id = 0;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string composer;
    std::string location;  // iPod-style path, ':' separated
    std::uint32_t lengthMs = 0;
    std::uint32_t sizeBytes = 0;
    std::uint32_t trackNumber = 0;
    std::uint32_t discNumber = 0;
    std::uint32_t year = 0;
    std::uint32_t bitrate = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t playCount = 0;
    std::uint8_t rating = 0;      // 0-100, 20 per star
    std::uint32_t mediaType = 1;  // 1 audio, 2 video, 4 podcast, 8 audiobook
    std::int64_t dateAdded = 0;   // unix time
    std::uint64_t dbid = 0;       // persistent id; generated when 0
};

struct Playlist {
    std::string name;
    bool isMaster = false;
    std::vector<std::uint32_t> trackIds;
};

struct Library {
    std::vector<Track> tracks;
    std::vector<Playlist> playlists;  // master playlist excluded
    std::string masterName;
    std::uint32_t version = 0;
    // 0 = none (pre-2007 iPods), 1 = hash58, 2 = hash72. Read from the mhbd
    // header; tells us whether this device will accept unhashed DB writes.
    std::uint16_t hashingScheme = 0;
};

struct ParseResult {
    std::optional<Library> library;
    std::string error;
};

// Parses an iTunesDB file (read-only). Never throws; malformed input yields
// an error or a best-effort partial library.
ParseResult parseItunesDb(const std::filesystem::path& path);

// Serializes a library as an iTunes-7-era iTunesDB (dbversion 0x19, no
// hash) — the dialect classic iPod firmwares accept. The master playlist is
// regenerated from the track list. Returns false and sets `error` on failure.
bool writeItunesDb(const Library& lib, const std::filesystem::path& path,
                   std::string* error);

// "3:07", "1:02:45"
std::string formatDuration(std::uint32_t ms);

}  // namespace podbox
