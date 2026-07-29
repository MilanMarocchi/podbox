#include "itdb/itunesdb.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr std::int64_t kMacToUnixEpoch = 2082844800LL;

using Bytes = std::vector<unsigned char>;

void set8(Bytes& b, size_t off, std::uint8_t v) { b[off] = v; }
void set16(Bytes& b, size_t off, std::uint16_t v) {
    b[off] = v & 0xFF;
    b[off + 1] = v >> 8;
}
void set32(Bytes& b, size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
}
void set64(Bytes& b, size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
}

// Zero-filled chunk with its tag and header length set. Unknown fields stay
// zero — the same policy libgpod shipped for years; firmwares tolerate it.
Bytes chunk(const char* tag, size_t headerLen) {
    Bytes b(headerLen, 0);
    std::memcpy(b.data(), tag, 4);
    set32(b, 4, std::uint32_t(headerLen));
    return b;
}

void append(Bytes& dst, const Bytes& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void appendUtf16le(Bytes& out, const std::string& s) {
    auto push16 = [&](std::uint32_t u) {
        out.push_back(u & 0xFF);
        out.push_back((u >> 8) & 0xFF);
    };
    size_t i = 0;
    while (i < s.size()) {
        std::uint32_t c = (unsigned char)s[i];
        int extra = 0;
        if (c >= 0xF0) {
            c &= 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            c &= 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            c &= 0x1F;
            extra = 1;
        }
        ++i;
        while (extra-- > 0 && i < s.size()) {
            c = (c << 6) | (s[i] & 0x3F);
            ++i;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            push16(0xD800 + (c >> 10));
            push16(0xDC00 + (c & 0x3FF));
        } else {
            push16(c);
        }
    }
}

Bytes mhodString(std::uint32_t type, const std::string& utf8) {
    Bytes str;
    appendUtf16le(str, utf8);
    Bytes b = chunk("mhod", 24);
    b.resize(40, 0);
    set32(b, 8, std::uint32_t(40 + str.size()));
    set32(b, 12, type);
    set32(b, 24, 1);  // position
    set32(b, 28, std::uint32_t(str.size()));
    append(b, str);
    return b;
}

// mhod type 100: playlist item position marker.
Bytes mhodPosition(std::uint32_t position) {
    Bytes b = chunk("mhod", 24);
    b.resize(44, 0);
    set32(b, 8, 44);
    set32(b, 12, 100);
    set32(b, 24, position);
    return b;
}

std::uint32_t filetypeMarker(const std::string& location) {
    std::string ext;
    if (const auto dot = location.rfind('.'); dot != std::string::npos)
        ext = location.substr(dot + 1);
    for (char& c : ext)
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    char four[4] = {' ', ' ', ' ', ' '};
    std::memcpy(four, ext.c_str(), std::min<size_t>(ext.size(), 4));
    return (std::uint32_t(four[0]) << 24) | (std::uint32_t(four[1]) << 16) |
           (std::uint32_t(four[2]) << 8) | std::uint32_t(four[3]);
}

std::uint32_t toMacTime(std::int64_t unixTime) {
    return unixTime > 0 ? std::uint32_t(unixTime + kMacToUnixEpoch) : 0;
}

Bytes mhit(const Track& t, std::uint64_t fallbackDbid) {
    Bytes b = chunk("mhit", 0x148);
    set32(b, 16, t.id);
    set32(b, 20, 1);  // visible
    set32(b, 24, filetypeMarker(t.location));
    const bool mp3 = filetypeMarker(t.location) == 0x4D503320;  // "MP3 "
    set8(b, 29, mp3 ? 1 : 0);
    set8(b, 31, t.rating);
    set32(b, 32, toMacTime(t.dateAdded));  // date modified
    set32(b, 36, t.sizeBytes);
    set32(b, 40, t.lengthMs);
    set32(b, 44, t.trackNumber);
    set32(b, 52, t.year);
    set32(b, 56, t.bitrate);
    set32(b, 60, t.sampleRate << 16);
    set32(b, 80, t.playCount);
    set32(b, 84, t.playCount);
    set32(b, 92, t.discNumber);
    set32(b, 104, toMacTime(t.dateAdded));
    set64(b, 112, t.dbid ? t.dbid : fallbackDbid);
    set32(b, 208, t.mediaType ? t.mediaType : 1);

    std::uint32_t nMhods = 0;
    Bytes body;
    auto add = [&](std::uint32_t type, const std::string& s) {
        if (s.empty()) return;
        append(body, mhodString(type, s));
        ++nMhods;
    };
    add(1, t.title);
    add(2, t.location);
    add(3, t.album);
    add(4, t.artist);
    add(5, t.genre);
    add(12, t.composer);
    set32(b, 8, std::uint32_t(b.size() + body.size()));
    set32(b, 12, nMhods);
    append(b, body);
    return b;
}

Bytes mhip(std::uint32_t trackId, std::uint32_t position, std::uint32_t now) {
    Bytes b = chunk("mhip", 0x4C);
    const Bytes pos = mhodPosition(position);
    set32(b, 8, std::uint32_t(b.size() + pos.size()));
    set32(b, 12, 1);              // one child mhod
    set32(b, 20, 1000 + position);  // group id, just needs uniqueness
    set32(b, 24, trackId);
    set32(b, 28, now);
    append(b, pos);
    return b;
}

Bytes mhyp(const std::string& name, bool master,
           const std::vector<std::uint32_t>& trackIds, std::uint32_t now,
           std::uint64_t ppid) {
    Bytes b = chunk("mhyp", 0x6C);
    set8(b, 20, master ? 1 : 0);
    set32(b, 24, now);
    set64(b, 28, ppid);

    Bytes body;
    std::uint32_t nMhods = 0;
    if (!name.empty()) {
        append(body, mhodString(1, name));
        ++nMhods;
    }
    std::uint32_t position = 1;
    for (const std::uint32_t id : trackIds)
        append(body, mhip(id, position++, now));

    set32(b, 8, std::uint32_t(b.size() + body.size()));
    set32(b, 12, nMhods);
    set32(b, 16, std::uint32_t(trackIds.size()));
    append(b, body);
    return b;
}

Bytes mhsd(std::uint32_t type, const Bytes& child) {
    Bytes b = chunk("mhsd", 0x60);
    set32(b, 8, std::uint32_t(b.size() + child.size()));
    set32(b, 12, type);
    append(b, child);
    return b;
}

}  // namespace

bool writeItunesDb(const Library& lib, const fs::path& path,
                   std::string* error) {
    std::mt19937_64 rng{std::random_device{}()};
    const std::uint32_t now = toMacTime(std::time(nullptr));

    // Track list.
    Bytes mhlt = chunk("mhlt", 0x5C);
    set32(mhlt, 8, std::uint32_t(lib.tracks.size()));
    for (const Track& t : lib.tracks) append(mhlt, mhit(t, rng()));

    // Playlists: regenerated master first, then user playlists.
    std::vector<std::uint32_t> allIds;
    allIds.reserve(lib.tracks.size());
    for (const Track& t : lib.tracks) allIds.push_back(t.id);

    Bytes mhlp = chunk("mhlp", 0x5C);
    set32(mhlp, 8, std::uint32_t(1 + lib.playlists.size()));
    const std::string masterName =
        lib.masterName.empty() ? "iPod" : lib.masterName;
    append(mhlp, mhyp(masterName, true, allIds, now, rng()));
    for (const Playlist& pl : lib.playlists)
        append(mhlp, mhyp(pl.name, false, pl.trackIds, now, rng()));

    // Empty podcast dataset keeps the firmware's Podcasts menu happy.
    Bytes podcastMhlp = chunk("mhlp", 0x5C);

    Bytes db = chunk("mhbd", 0xF4);
    set32(db, 12, 1);
    set32(db, 16, 0x19);  // dbversion, iTunes 7.x era
    set32(db, 20, 3);     // child mhsd count
    set64(db, 24, rng());
    set16(db, 32, 2);
    append(db, mhsd(1, mhlt));
    append(db, mhsd(2, mhlp));
    append(db, mhsd(3, podcastMhlp));
    set32(db, 8, std::uint32_t(db.size()));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(db.data()),
                           std::streamsize(db.size()))) {
        if (error) *error = "Could not write " + path.string();
        return false;
    }
    return true;
}

}  // namespace podbox
