#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace podbox {

// Values of Library::hashingScheme, read from the hashing_scheme field of the
// mhbd header at offset 0x30. Anything but None means the device checks a
// signature over its database and will show an empty library if it does not
// match.
//
// The three schemes are implemented in itdb/hash58, hash72 and hashab.
inline constexpr std::uint16_t kChecksumNone = 0;
inline constexpr std::uint16_t kChecksumHash58 = 1;
inline constexpr std::uint16_t kChecksumHash72 = 2;
inline constexpr std::uint16_t kChecksumHashAB = 3;

// Track::mediaType. The iPod files a track by this and plays it accordingly:
// audiobooks are kept out of shuffle and resume where you left off. The values
// are a bit field in the database, but a track carries exactly one.
inline constexpr std::uint32_t kMediaAudio = 1;
inline constexpr std::uint32_t kMediaVideo = 2;
inline constexpr std::uint32_t kMediaPodcast = 4;
inline constexpr std::uint32_t kMediaAudiobook = 8;

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
    std::uint32_t mediaType = 1;  // see kMedia* below
    std::int64_t dateAdded = 0;   // unix time
    std::uint64_t dbid = 0;       // persistent id; generated when 0

    // mhod records PodBox has no model for — sort keys, comments, album
    // artist, artwork references. Carried through a read/write cycle verbatim
    // so editing one song does not silently discard what iTunes or Apple Music
    // stored alongside it.
    std::vector<std::uint8_t> extraMhods;
    std::uint32_t extraMhodCount = 0;

    // The track's original fixed header, byte for byte. Apple Music writes a
    // 624-byte header where PodBox models 328 bytes of it; re-emitting this
    // and patching only the handful of fields PodBox edits keeps artwork
    // references, gapless data and sort keys intact. Empty for new tracks,
    // which get a freshly built header instead.
    std::vector<std::uint8_t> rawHeader;
};

struct Playlist {
    std::string name;
    bool isMaster = false;
    // Persistent playlist id. Shuffle 3G/4G uses this same value to locate
    // the spoken playlist name in iPod_Control/Speakable/Playlists.
    std::uint64_t dbid = 0;
    std::vector<std::uint32_t> trackIds;
    // Smart-playlist criteria (mhod 50/51) and anything else unmodelled.
    // Without this a smart playlist is flattened to a static snapshot the
    // first time PodBox writes.
    std::vector<std::uint8_t> extraMhods;
    std::uint32_t extraMhodCount = 0;
};

struct Library {
    std::vector<Track> tracks;
    std::vector<Playlist> playlists;  // master playlist excluded
    std::string masterName;
    // Persistent ids shared with the nano 6G/7G SQLite companion databases.
    // Keeping them stable is required for Library.itdb's db_info and master
    // container to continue describing the same library after a write.
    std::uint64_t databaseDbid = 0;
    std::uint64_t masterDbid = 0;
    std::uint32_t version = 0;

    // Whole mhsd datasets PodBox does not model: podcasts, the album list
    // (mhla/mhia), and the modern playlist dataset Apple Music writes. Kept in
    // their original order and handed straight back, because writing the file
    // without them discards real user data.
    struct RawDataset {
        std::uint32_t type = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<RawDataset> extraDatasets;
    // 0 = none, 1 = hash58, 2 = hash72, 3 = hashAB. Read from the mhbd
    // header; tells us whether this device will accept unhashed DB writes.
    std::uint16_t hashingScheme = 0;
    // True when the device stores its database compressed as iTunesCDB (nano
    // 5G and later): the library is parsed from the inflated image, and the
    // next write must deflate again or the device cannot read it.
    bool compressed = false;
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
// Extra requirements for a write. Empty means "none", which is what an iPod
// that needs no checksum wants.
struct WriteOptions {
    // The 8 raw bytes of the device's FireWire GUID. When set, the finished
    // image is checksummed with hash58 before it is written — which iPod
    // classic and nano 3G-4G require.
    std::vector<std::uint8_t> hash58Guid;
    // The (IV, random) pair recovered from the device's own database, kept in
    // its HashInfo file. When set (with hash58Guid empty), the finished image
    // is checksummed with hash72 instead — what the nano 5G requires.
    std::vector<std::uint8_t> hash72Iv;
    std::vector<std::uint8_t> hash72Rndpart;
    // The nano 6G/7G device UUID and 23-byte nonce used for hashAB.
    std::vector<std::uint8_t> hashAbUuid;
    std::vector<std::uint8_t> hashAbNonce;
    // Store the database compressed as iTunesCDB. hash72 devices always
    // arrive and leave compressed; the signature covers the compressed bytes.
    bool compressed = false;
};

bool writeItunesDb(const Library& lib, const std::filesystem::path& path,
                   std::string* error, const WriteOptions& opts = {});

// "3:07", "1:02:45"
std::string formatDuration(std::uint32_t ms);

}  // namespace podbox
