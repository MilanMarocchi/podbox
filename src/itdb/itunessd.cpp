#include "itdb/itunessd.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <unordered_map>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace podbox {
namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint16_t get16(const Bytes& b, std::size_t off) {
    if (off + 2 > b.size()) return 0;
    return std::uint16_t(b[off] | (b[off + 1] << 8));
}

std::uint32_t get32(const Bytes& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return std::uint32_t(b[off]) | (std::uint32_t(b[off + 1]) << 8) |
           (std::uint32_t(b[off + 2]) << 16) |
           (std::uint32_t(b[off + 3]) << 24);
}

std::uint64_t get64(const Bytes& b, std::size_t off) {
    return std::uint64_t(get32(b, off)) |
           (std::uint64_t(get32(b, off + 4)) << 32);
}

void set16(Bytes& b, std::size_t off, std::uint16_t value) {
    if (off + 2 > b.size()) b.resize(off + 2);
    b[off] = std::uint8_t(value);
    b[off + 1] = std::uint8_t(value >> 8);
}

void set32(Bytes& b, std::size_t off, std::uint32_t value) {
    if (off + 4 > b.size()) b.resize(off + 4);
    for (int i = 0; i < 4; ++i) b[off + i] = std::uint8_t(value >> (8 * i));
}

void set64(Bytes& b, std::size_t off, std::uint64_t value) {
    if (off + 8 > b.size()) b.resize(off + 8);
    for (int i = 0; i < 8; ++i) b[off + i] = std::uint8_t(value >> (8 * i));
}

bool tagIs(const Bytes& b, std::size_t off, const char* tag) {
    return off + 4 <= b.size() && std::memcmp(b.data() + off, tag, 4) == 0;
}

void setTag(Bytes& b, std::size_t off, const char* tag) {
    if (off + 4 > b.size()) b.resize(off + 4);
    std::memcpy(b.data() + off, tag, 4);
}

bool readFile(const fs::path& path, Bytes* out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out->resize(std::size_t(size));
    return size == 0 || in.read(reinterpret_cast<char*>(out->data()), size);
}

bool writeFile(const fs::path& path, const Bytes& data, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(data.data()),
                           std::streamsize(data.size()))) {
        if (error) *error = "Could not write " + path.string();
        return false;
    }
    return true;
}

std::string nulString(const Bytes& b, std::size_t off, std::size_t max) {
    if (off >= b.size()) return {};
    max = std::min(max, b.size() - off);
    std::size_t n = 0;
    while (n < max && b[off + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(b.data() + off), n);
}

std::string slashLocation(std::string location) {
    std::replace(location.begin(), location.end(), ':', '/');
    if (location.empty() || location.front() != '/') location.insert(0, 1, '/');
    return location;
}

std::string lookupKey(std::string location) {
    location = slashLocation(std::move(location));
    std::transform(location.begin(), location.end(), location.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return location;
}

std::uint32_t shuffleFileType(const std::string& location) {
    std::string ext;
    if (const auto dot = location.rfind('.'); dot != std::string::npos)
        ext = location.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".m4a" || ext == ".m4b" || ext == ".m4p" || ext == ".aa"
               ? 2
               : 1;
}

struct ExistingPlaylist {
    std::uint64_t dbid = 0;
    std::uint32_t type = 0;
    std::vector<std::uint32_t> trackIndices;
};

struct ExistingSd {
    Bytes rootHeader;
    Bytes trackHeaderPrefix;
    Bytes playlistHeaderPrefix;
    std::unordered_map<std::string, Bytes> tracks;
    std::vector<ExistingPlaylist> playlists;
    std::uint32_t version = 0x02010001;
    std::uint8_t maxVolume = 0;
    bool voiceOver = true;
};

bool parseExisting(const Bytes& data, ExistingSd* db, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (data.size() < 64 || !tagIs(data, 0, "bdhs"))
        return fail("Not a modern iTunesSD file (missing bdhs header)");

    const std::uint32_t rootLen = get32(data, 8);
    if (rootLen < 44 || rootLen > data.size())
        return fail("Malformed iTunesSD root header");
    db->rootHeader.assign(data.begin(), data.begin() + rootLen);
    db->version = get32(data, 4);
    db->maxVolume = data[28];
    db->voiceOver = data[29] != 0;

    const std::uint32_t trackHeader = get32(data, 36);
    if (!tagIs(data, trackHeader, "hths"))
        return fail("Malformed iTunesSD track header");
    const std::uint32_t trackHeaderLen = get32(data, trackHeader + 4);
    const std::uint32_t trackCount = get32(data, trackHeader + 8);
    if (trackHeaderLen < 20 || trackHeaderLen > data.size() - trackHeader ||
        trackCount > (trackHeaderLen - 20) / 4)
        return fail("Malformed iTunesSD track index");
    const std::size_t trackPrefix = trackHeaderLen - std::size_t(trackCount) * 4;
    if (trackPrefix < 20)
        return fail("Malformed iTunesSD track-header prefix");
    db->trackHeaderPrefix.assign(data.begin() + trackHeader,
                                 data.begin() + trackHeader + trackPrefix);

    for (std::uint32_t i = 0; i < trackCount; ++i) {
        const std::uint32_t off = get32(data, trackHeader + trackPrefix + i * 4);
        if (!tagIs(data, off, "rths")) return fail("Malformed iTunesSD track");
        const std::uint32_t len = get32(data, off + 4);
        if (len < 340 || len > data.size() - off)
            return fail("Malformed iTunesSD track length");
        Bytes record(data.begin() + off, data.begin() + off + len);
        db->tracks[lookupKey(nulString(record, 24, 256))] = std::move(record);
    }

    const std::uint32_t playlistHeader = get32(data, 40);
    if (!tagIs(data, playlistHeader, "hphs"))
        return fail("Malformed iTunesSD playlist header");
    const std::uint32_t playlistHeaderLen = get32(data, playlistHeader + 4);
    const std::uint32_t playlistCount = get32(data, playlistHeader + 8);
    if (playlistHeaderLen < 20 ||
        playlistHeaderLen > data.size() - playlistHeader ||
        playlistCount > (playlistHeaderLen - 20) / 4)
        return fail("Malformed iTunesSD playlist index");
    const std::size_t playlistPrefix =
        playlistHeaderLen - std::size_t(playlistCount) * 4;
    if (playlistPrefix < 20)
        return fail("Malformed iTunesSD playlist-header prefix");
    db->playlistHeaderPrefix.assign(data.begin() + playlistHeader,
                                    data.begin() + playlistHeader + playlistPrefix);

    for (std::uint32_t i = 0; i < playlistCount; ++i) {
        const std::uint32_t off =
            get32(data, playlistHeader + playlistPrefix + i * 4);
        if (!tagIs(data, off, "lphs"))
            return fail("Malformed iTunesSD playlist");
        const std::uint32_t len = get32(data, off + 4);
        const std::uint32_t count = get32(data, off + 8);
        if (len < 44 || len > data.size() - off || count > (len - 44) / 4)
            return fail("Malformed iTunesSD playlist length");
        ExistingPlaylist playlist;
        playlist.dbid = get64(data, off + 16);
        playlist.type = get32(data, off + 24);
        playlist.trackIndices.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j)
            playlist.trackIndices.push_back(get32(data, off + 44 + j * 4));
        db->playlists.push_back(std::move(playlist));
    }
    return true;
}

Bytes newRootHeader() {
    Bytes b(64, 0);
    setTag(b, 0, "bdhs");
    set32(b, 4, 0x02010001);
    set32(b, 8, 64);
    b[29] = 1;
    set32(b, 36, 64);
    return b;
}

Bytes newTrackHeaderPrefix() {
    Bytes b(20, 0);
    setTag(b, 0, "hths");
    return b;
}

Bytes newPlaylistHeaderPrefix() {
    // Apple's later Shuffle 3G/4G dialect has 48 bytes of counters/reserved
    // fields between the common 20-byte header and the absolute offset list.
    Bytes b(68, 0);
    setTag(b, 0, "hphs");
    for (std::size_t off : {20u, 28u, 36u, 44u}) set32(b, off, 0xFFFFFFFFu);
    return b;
}

Bytes newTrackRecord(const Track& track) {
    Bytes b(372, 0);
    setTag(b, 0, "rths");
    set32(b, 4, 372);
    set32(b, 12, track.lengthMs);
    set32(b, 20, shuffleFileType(track.location));
    b[284] = 1;  // included when shuffling
    set32(b, 288, 0x200);
    set32(b, 292, 0x200);
    set16(b, 316, std::uint16_t(track.trackNumber ? track.trackNumber : 1));
    set16(b, 318, std::uint16_t(track.discNumber));
    return b;
}

void patchTrackRecord(Bytes& record, const Track& track, std::uint32_t albumId,
                      std::uint32_t artistId) {
    if (record.size() < 372) record.resize(372, 0);
    setTag(record, 0, "rths");
    set32(record, 4, std::uint32_t(record.size()));
    set32(record, 12, track.lengthMs);
    set32(record, 20, shuffleFileType(track.location));
    std::fill(record.begin() + 24, record.begin() + 280, 0);
    const std::string filename = slashLocation(track.location);
    const std::size_t copy = std::min<std::size_t>(filename.size(), 255);
    std::memcpy(record.data() + 24, filename.data(), copy);
    set32(record, 312, albumId);
    set16(record, 316, std::uint16_t(track.trackNumber ? track.trackNumber : 1));
    set16(record, 318, std::uint16_t(track.discNumber));
    set64(record, 328, track.dbid);
    set32(record, 336, artistId);
}

Bytes playlistRecord(std::uint32_t type, std::uint64_t dbid,
                     const std::vector<std::uint32_t>& indices) {
    Bytes b(44 + indices.size() * 4, 0);
    setTag(b, 0, "lphs");
    set32(b, 4, std::uint32_t(b.size()));
    set32(b, 8, std::uint32_t(indices.size()));
    set32(b, 12, std::uint32_t(indices.size()));
    set64(b, 16, dbid);
    set32(b, 24, type);
    for (std::size_t i = 0; i < indices.size(); ++i)
        set32(b, 44 + i * 4, indices[i]);
    return b;
}

std::uint64_t randomDbid() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t id = rng();
    return id ? id : 1;
}

bool validVoiceWave(const fs::path& path) {
    Bytes b;
    if (!readFile(path, &b) || b.size() < 44 || !tagIs(b, 0, "RIFF") ||
        !tagIs(b, 8, "WAVE"))
        return false;
    bool formatOk = false;
    bool audioOk = false;
    for (std::size_t off = 12; off + 8 <= b.size();) {
        const std::uint32_t len = get32(b, off + 4);
        if (tagIs(b, off, "fmt ") && len >= 16 && off + 8 + len <= b.size()) {
            formatOk = get16(b, off + 8) == 1 && get16(b, off + 10) == 1 &&
                       get32(b, off + 12) == 22050 &&
                       get16(b, off + 22) == 16;
        } else if (tagIs(b, off, "data")) {
            audioOk = len > 0 && off + 8 + len <= b.size();
        }
        const std::size_t advance = 8 + std::size_t(len) + (len & 1u);
        if (advance < 8 || advance > b.size() - off) break;
        off += advance;
    }
    return formatOk && audioOk;
}

#ifndef _WIN32
bool runSpeechProcess(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

bool generateVoiceWave(const fs::path& output, const std::string& text,
                       std::string* error) {
#ifdef _WIN32
    (void)output;
    (void)text;
    if (error) *error = "VoiceOver generation is not available on Windows";
    return false;
#else
    std::vector<std::string> args;
    if (fs::exists("/usr/bin/say")) {
        args = {"/usr/bin/say", "-o", output.string(),
                "--data-format=LEI16@22050", "--file-format=WAVE", "--", text};
    } else if (fs::exists("/usr/bin/espeak-ng")) {
        args = {"/usr/bin/espeak-ng", "-s", "150", "-w", output.string(),
                "--", text};
    } else if (fs::exists("/usr/bin/espeak")) {
        args = {"/usr/bin/espeak", "-s", "150", "-w", output.string(),
                "--", text};
    } else {
        if (error) *error = "No speech synthesizer is available for Shuffle VoiceOver";
        return false;
    }
    if (!runSpeechProcess(args) || !validVoiceWave(output)) {
        if (error) *error = "Could not generate a valid Shuffle VoiceOver file";
        return false;
    }
    return true;
#endif
}

bool ensureAnnouncement(const fs::path& directory, std::uint64_t dbid,
                        const std::string& text, std::string* error) {
    if (!dbid || text.empty()) return true;
    const std::string stem = shuffleVoiceOverName(dbid);
    const fs::path wav = directory / (stem + ".wav");
    const fs::path aiff = directory / (stem + ".aiff");
    std::error_code ec;
    if (fs::exists(wav, ec) || fs::exists(aiff, ec)) return true;
    fs::create_directories(directory, ec);
    if (ec) {
        if (error) *error = "Could not create " + directory.string();
        return false;
    }
    const fs::path tmp = directory / (stem + ".podbox-tmp.wav");
    fs::remove(tmp, ec);
    if (!generateVoiceWave(tmp, text, error)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, wav, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "Could not install Shuffle VoiceOver file: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

ItunesSdKind detectItunesSd(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ItunesSdKind::None;
    std::array<char, 4> magic{};
    if (!in.read(magic.data(), magic.size())) return ItunesSdKind::Legacy;
    return std::memcmp(magic.data(), "bdhs", 4) == 0 ? ItunesSdKind::Modern
                                                      : ItunesSdKind::Legacy;
}

std::optional<ShuffleDatabase> parseItunesSd(const fs::path& path,
                                             std::string* error) {
    Bytes data;
    if (!readFile(path, &data)) {
        if (error) *error = "Could not read " + path.string();
        return std::nullopt;
    }
    ExistingSd raw;
    if (!parseExisting(data, &raw, error)) return std::nullopt;
    ShuffleDatabase db;
    db.version = raw.version;
    db.voiceOverEnabled = raw.voiceOver;

    const std::uint32_t trackHeader = get32(data, 36);
    const std::uint32_t count = get32(data, trackHeader + 8);
    const std::uint32_t len = get32(data, trackHeader + 4);
    const std::size_t prefix = len - std::size_t(count) * 4;
    db.tracks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t off = get32(data, trackHeader + prefix + i * 4);
        db.tracks.push_back({nulString(data, off + 24, 256),
                             get64(data, off + 328)});
    }
    for (const ExistingPlaylist& pl : raw.playlists)
        db.playlists.push_back({pl.dbid, pl.type, pl.trackIndices});
    return db;
}

int reconcileShufflePlaylistIds(Library& library,
                                const fs::path& existingSdPath) {
    const auto sd = parseItunesSd(existingSdPath);
    if (!sd || sd->playlists.empty()) return 0;
    std::unordered_map<std::uint32_t, std::uint32_t> indexByTrackId;
    for (std::size_t i = 0; i < library.tracks.size(); ++i)
        indexByTrackId[library.tracks[i].id] = std::uint32_t(i);

    int repaired = 0;
    for (std::size_t i = 0;
         i < library.playlists.size() && i + 1 < sd->playlists.size(); ++i) {
        std::vector<std::uint32_t> indices;
        for (const std::uint32_t id : library.playlists[i].trackIds)
            if (const auto found = indexByTrackId.find(id);
                found != indexByTrackId.end())
                indices.push_back(found->second);
        const ShufflePlaylistRecord& old = sd->playlists[i + 1];
        if (old.type != 2 || indices != old.trackIndices || !old.dbid ||
            library.playlists[i].dbid == old.dbid)
            continue;
        library.playlists[i].dbid = old.dbid;
        ++repaired;
    }
    return repaired;
}

std::string shuffleVoiceOverName(std::uint64_t dbid) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX",
                  static_cast<unsigned long long>(dbid));
    return out;
}

bool writeItunesSd(const Library& library, const fs::path& existingPath,
                   const fs::path& outputPath, const fs::path& mountPoint,
                   std::string* error, const ItunesSdWriteOptions& options) {
    ExistingSd existing;
    Bytes original;
    if (!readFile(existingPath, &original) ||
        !parseExisting(original, &existing, error))
        return false;

    Bytes root = existing.rootHeader.empty() ? newRootHeader()
                                             : existing.rootHeader;
    Bytes trackPrefix = existing.trackHeaderPrefix.empty()
                            ? newTrackHeaderPrefix()
                            : existing.trackHeaderPrefix;
    Bytes playlistPrefix = existing.playlistHeaderPrefix.empty()
                               ? newPlaylistHeaderPrefix()
                               : existing.playlistHeaderPrefix;

    std::unordered_map<std::string, std::uint32_t> albumIds, artistIds;
    std::uint32_t nextAlbum = 0, nextArtist = 0;
    for (const Track& track : library.tracks) {
        const auto found = existing.tracks.find(lookupKey(track.location));
        if (found == existing.tracks.end() || found->second.size() < 340) continue;
        const std::uint32_t album = get32(found->second, 312);
        const std::uint32_t artist = get32(found->second, 336);
        if (!track.album.empty()) albumIds.emplace(track.album, album);
        if (!track.artist.empty()) artistIds.emplace(track.artist, artist);
        nextAlbum = std::max(nextAlbum, album + 1);
        nextArtist = std::max(nextArtist, artist + 1);
    }
    auto groupedId = [](const std::string& value,
                        std::unordered_map<std::string, std::uint32_t>& ids,
                        std::uint32_t* next) {
        if (const auto it = ids.find(value); it != ids.end()) return it->second;
        const std::uint32_t id = (*next)++;
        ids.emplace(value, id);
        return id;
    };

    std::vector<Bytes> trackRecords;
    trackRecords.reserve(library.tracks.size());
    std::unordered_map<std::uint32_t, std::uint32_t> indexByTrackId;
    for (std::size_t i = 0; i < library.tracks.size(); ++i) {
        const Track& track = library.tracks[i];
        if (!track.dbid) {
            if (error) *error = "A Shuffle track is missing its persistent id";
            return false;
        }
        Bytes record;
        bool fresh = false;
        if (const auto found = existing.tracks.find(lookupKey(track.location));
            found != existing.tracks.end())
            record = found->second;
        else {
            record = newTrackRecord(track);
            fresh = true;
        }
        if (fresh) {
            patchTrackRecord(record, track,
                             groupedId(track.album, albumIds, &nextAlbum),
                             groupedId(track.artist, artistIds, &nextArtist));
        } else {
            // Apple stores playback/bookmark/gapless details here that the
            // main iTunesDB model cannot fully express. An existing track's
            // location and persistent id are its identity, so retain every
            // other byte exactly as the device received it.
            set64(record, 328, track.dbid);
        }
        trackRecords.push_back(std::move(record));
        indexByTrackId[track.id] = std::uint32_t(i);
    }

    std::vector<std::uint32_t> allIndices;
    allIndices.reserve(library.tracks.size());
    for (std::uint32_t i = 0; i < library.tracks.size(); ++i)
        allIndices.push_back(i);
    std::vector<Bytes> playlistRecords;
    playlistRecords.push_back(playlistRecord(1, 0, allIndices));

    std::vector<std::uint64_t> playlistDbids;
    std::vector<bool> playlistNeedsVoice;
    playlistDbids.reserve(library.playlists.size());
    playlistNeedsVoice.reserve(library.playlists.size());
    for (std::size_t i = 0; i < library.playlists.size(); ++i) {
        const Playlist& playlist = library.playlists[i];
        std::vector<std::uint32_t> indices;
        indices.reserve(playlist.trackIds.size());
        for (const std::uint32_t id : playlist.trackIds)
            if (const auto found = indexByTrackId.find(id);
                found != indexByTrackId.end())
                indices.push_back(found->second);
        std::uint64_t dbid = playlist.dbid;
        bool retainedExisting = false;
        if (i + 1 < existing.playlists.size()) {
            const ExistingPlaylist& old = existing.playlists[i + 1];
            // PodBox versions before Shuffle support rewrote iTunesDB playlist
            // ids without updating iTunesSD. Membership is the only bridge
            // left in that case; using it repairs the pair while retaining
            // Apple's already-generated spoken playlist name.
            if (!dbid || dbid == old.dbid || indices == old.trackIndices) {
                dbid = old.dbid;
                retainedExisting = true;
            }
        }
        if (!dbid) dbid = randomDbid();
        playlistDbids.push_back(dbid);
        playlistNeedsVoice.push_back(!retainedExisting);
        playlistRecords.push_back(playlistRecord(2, dbid, indices));
    }

    if (options.generateVoiceOver) {
        const fs::path speakable = mountPoint / "iPod_Control" / "Speakable";
        for (const Track& track : library.tracks) {
            // Existing iTunesSD records already had their VoiceOver pass. Do
            // not make an ordinary metadata/rating write synthesize hundreds
            // of historical announcements that happen to be absent; only a
            // newly indexed track needs a new spoken file here.
            if (existing.tracks.count(lookupKey(track.location))) continue;
            std::string speech = track.title;
            if (!track.artist.empty()) speech += ", " + track.artist;
            if (!ensureAnnouncement(speakable / "Tracks", track.dbid, speech,
                                    error))
                return false;
        }
        for (std::size_t i = 0; i < library.playlists.size(); ++i) {
            if (!playlistNeedsVoice[i]) continue;
            if (!ensureAnnouncement(speakable / "Playlists", playlistDbids[i],
                                    library.playlists[i].name, error))
                return false;
        }
    }

    const std::uint32_t trackCount = std::uint32_t(trackRecords.size());
    const std::uint32_t playlistCount = std::uint32_t(playlistRecords.size());
    setTag(root, 0, "bdhs");
    set32(root, 4, existing.version);
    set32(root, 8, std::uint32_t(root.size()));
    set32(root, 12, trackCount);
    set32(root, 16, playlistCount);
    root[28] = existing.maxVolume;
    root[29] = 1;
    std::uint32_t musicCount = 0;
    for (const Track& track : library.tracks)
        if (track.mediaType == kMediaAudio) ++musicCount;
    set32(root, 32, musicCount);
    set32(root, 36, std::uint32_t(root.size()));

    setTag(trackPrefix, 0, "hths");
    set32(trackPrefix, 4,
          std::uint32_t(trackPrefix.size() + trackRecords.size() * 4));
    set32(trackPrefix, 8, trackCount);

    Bytes output = root;
    const std::size_t trackHeaderOffset = output.size();
    output.insert(output.end(), trackPrefix.begin(), trackPrefix.end());
    const std::size_t trackOffsets = output.size();
    output.resize(output.size() + trackRecords.size() * 4);
    for (std::size_t i = 0; i < trackRecords.size(); ++i) {
        set32(output, trackOffsets + i * 4, std::uint32_t(output.size()));
        output.insert(output.end(), trackRecords[i].begin(), trackRecords[i].end());
    }
    (void)trackHeaderOffset;
    set32(output, 40, std::uint32_t(output.size()));

    setTag(playlistPrefix, 0, "hphs");
    set32(playlistPrefix, 4,
          std::uint32_t(playlistPrefix.size() + playlistRecords.size() * 4));
    set32(playlistPrefix, 8, playlistCount);
    if (playlistPrefix.size() >= 68) {
        set32(playlistPrefix, 12, 1);
        set32(playlistPrefix, 16, std::uint32_t(library.playlists.size()));
    } else if (playlistPrefix.size() >= 20) {
        set16(playlistPrefix, 14, 1);
    }
    output.insert(output.end(), playlistPrefix.begin(), playlistPrefix.end());
    const std::size_t playlistOffsets = output.size();
    output.resize(output.size() + playlistRecords.size() * 4);
    for (std::size_t i = 0; i < playlistRecords.size(); ++i) {
        set32(output, playlistOffsets + i * 4, std::uint32_t(output.size()));
        output.insert(output.end(), playlistRecords[i].begin(),
                      playlistRecords[i].end());
    }

    return writeFile(outputPath, output, error);
}

bool writeShuffleStats(const Library& library, const fs::path& existingSdPath,
                       const fs::path& existingStatsPath,
                       const fs::path& outputPath, std::string* error) {
    const auto oldDb = parseItunesSd(existingSdPath, error);
    if (!oldDb) return false;

    Bytes oldStats;
    const bool haveStats = readFile(existingStatsPath, &oldStats);
    std::unordered_map<std::string, Bytes> entryByLocation;
    Bytes header(8, 0);
    if (haveStats && oldStats.size() >= 8) {
        std::copy_n(oldStats.begin(), 8, header.begin());
        const std::uint32_t count = get32(oldStats, 0);
        std::size_t off = 8;
        bool valid = count == oldDb->tracks.size();
        for (std::uint32_t i = 0; valid && i < count; ++i) {
            const std::uint32_t len = get32(oldStats, off);
            if (len < 4 || len > oldStats.size() - off) {
                valid = false;
                break;
            }
            entryByLocation.emplace(
                lookupKey(oldDb->tracks[i].location),
                Bytes(oldStats.begin() + off, oldStats.begin() + off + len));
            off += len;
        }
        if (!valid) entryByLocation.clear();
    }

    set32(header, 0, std::uint32_t(library.tracks.size()));
    Bytes output = std::move(header);
    for (const Track& track : library.tracks) {
        if (const auto found = entryByLocation.find(lookupKey(track.location));
            found != entryByLocation.end()) {
            output.insert(output.end(), found->second.begin(), found->second.end());
        } else {
            const std::size_t off = output.size();
            output.resize(off + 32, 0);
            set32(output, off, 32);
        }
    }
    return writeFile(outputPath, output, error);
}

}  // namespace podbox
