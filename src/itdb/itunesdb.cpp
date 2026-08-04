#include "itdb/itunesdb.h"

#include "itdb/itunescdb.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace podbox {
namespace {

// Mac HFS epoch (1904) to Unix epoch (1970) offset in seconds.
constexpr std::int64_t kMacToUnixEpoch = 2082844800LL;

// Bounds-checked little-endian reader over the whole file.
struct Buf {
    std::vector<unsigned char> d;

    bool has(size_t off, size_t n) const {
        return off <= d.size() && n <= d.size() - off;
    }
    std::uint8_t u8(size_t off) const { return has(off, 1) ? d[off] : 0; }
    std::uint16_t u16(size_t off) const {
        if (!has(off, 2)) return 0;
        return std::uint16_t(d[off] | (d[off + 1] << 8));
    }
    std::uint32_t u32(size_t off) const {
        if (!has(off, 4)) return 0;
        return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) |
               (std::uint32_t(d[off + 3]) << 24);
    }
    bool tagIs(size_t off, const char* tag) const {
        return has(off, 4) && std::memcmp(&d[off], tag, 4) == 0;
    }
};

std::string utf16leToUtf8(const unsigned char* p, size_t bytes) {
    std::string out;
    const size_t n = bytes / 2;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::uint32_t c = p[2 * i] | (p[2 * i + 1] << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            const std::uint32_t lo = p[2 * i + 2] | (p[2 * i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// String mhods: 24-byte header, then position/length/unknown/unknown u32s,
// then UTF-16LE string data at +40.
std::string readMhodString(const Buf& b, size_t off, std::uint32_t totalLen) {
    if (totalLen < 40) return {};
    std::uint32_t len = b.u32(off + 28);
    if (len > totalLen - 40) len = totalLen - 40;
    const size_t strOff = off + 40;
    if (!b.has(strOff, len) || len == 0) return {};
    return utf16leToUtf8(&b.d[strOff], len);
}

void parseTracks(const Buf& b, size_t mhlt, Library& lib) {
    const std::uint32_t headerLen = b.u32(mhlt + 4);
    const std::uint32_t count = b.u32(mhlt + 8);
    if (headerLen < 12) return;
    size_t pos = mhlt + headerLen;
    // The count field is trusted by the loop below anyway, so the reserve is
    // only a hint — but a corrupt count must not be allowed to allocate
    // gigabytes. Cap it well above any real library.
    lib.tracks.reserve(std::min<std::uint32_t>(count, 1000000));
    for (std::uint32_t i = 0; i < count && b.tagIs(pos, "mhit"); ++i) {
        const std::uint32_t ihLen = b.u32(pos + 4);
        const std::uint32_t itLen = b.u32(pos + 8);
        const std::uint32_t nMhods = b.u32(pos + 12);
        if (ihLen < 0x60 || itLen < ihLen) break;

        Track t;
        if (b.has(pos, ihLen)) t.rawHeader.assign(&b.d[pos], &b.d[pos] + ihLen);
        t.id = b.u32(pos + 16);
        t.rating = b.u8(pos + 31);
        t.sizeBytes = b.u32(pos + 36);
        t.lengthMs = b.u32(pos + 40);
        t.trackNumber = b.u32(pos + 44);
        t.year = b.u32(pos + 52);
        t.bitrate = b.u32(pos + 56);
        t.sampleRate = b.u32(pos + 60) >> 16;
        t.playCount = b.u32(pos + 80);
        t.discNumber = b.u32(pos + 92);
        if (const std::uint32_t added = b.u32(pos + 104))
            t.dateAdded = std::int64_t(added) - kMacToUnixEpoch;
        t.dbid = std::uint64_t(b.u32(pos + 112)) |
                 (std::uint64_t(b.u32(pos + 116)) << 32);
        if (ihLen >= 212) t.mediaType = b.u32(pos + 208);

        size_t mpos = pos + ihLen;
        for (std::uint32_t m = 0; m < nMhods && b.tagIs(mpos, "mhod"); ++m) {
            const std::uint32_t mtLen = b.u32(mpos + 8);
            if (mtLen < 24) break;
            switch (b.u32(mpos + 12)) {
                case 1: t.title = readMhodString(b, mpos, mtLen); break;
                case 2: t.location = readMhodString(b, mpos, mtLen); break;
                case 3: t.album = readMhodString(b, mpos, mtLen); break;
                case 4: t.artist = readMhodString(b, mpos, mtLen); break;
                case 5: t.genre = readMhodString(b, mpos, mtLen); break;
                case 12: t.composer = readMhodString(b, mpos, mtLen); break;
                default:
                    // Not understood, so kept byte-for-byte rather than lost.
                    if (b.has(mpos, mtLen)) {
                        t.extraMhods.insert(t.extraMhods.end(), &b.d[mpos],
                                            &b.d[mpos] + mtLen);
                        ++t.extraMhodCount;
                    }
                    break;
            }
            mpos += mtLen;
        }
        lib.tracks.push_back(std::move(t));
        pos += itLen;
    }
}

void parsePlaylists(const Buf& b, size_t mhlp, Library& lib) {
    const std::uint32_t headerLen = b.u32(mhlp + 4);
    const std::uint32_t count = b.u32(mhlp + 8);
    if (headerLen < 12) return;
    size_t pos = mhlp + headerLen;
    for (std::uint32_t i = 0; i < count && b.tagIs(pos, "mhyp"); ++i) {
        const std::uint32_t phLen = b.u32(pos + 4);
        const std::uint32_t ptLen = b.u32(pos + 8);
        const std::uint32_t nMhods = b.u32(pos + 12);
        const std::uint32_t nItems = b.u32(pos + 16);
        if (phLen < 24 || ptLen < phLen) break;

        Playlist pl;
        pl.isMaster = b.u8(pos + 20) != 0;
        pl.dbid = std::uint64_t(b.u32(pos + 28)) |
                  (std::uint64_t(b.u32(pos + 32)) << 32);

        size_t p = pos + phLen;
        for (std::uint32_t m = 0; m < nMhods && b.tagIs(p, "mhod"); ++m) {
            const std::uint32_t mtLen = b.u32(p + 8);
            if (mtLen < 24) break;
            if (b.u32(p + 12) == 1) {
                pl.name = readMhodString(b, p, mtLen);
            } else if (b.has(p, mtLen)) {
                // Smart-playlist criteria live here; dropping them turns a
                // smart playlist into a frozen list of whatever it held today.
                pl.extraMhods.insert(pl.extraMhods.end(), &b.d[p],
                                     &b.d[p] + mtLen);
                ++pl.extraMhodCount;
            }
            p += mtLen;
        }
        pl.trackIds.reserve(std::min<std::uint32_t>(nItems, 1000000));
        for (std::uint32_t it = 0; it < nItems && b.tagIs(p, "mhip"); ++it) {
            const std::uint32_t ihLen = b.u32(p + 4);
            std::uint32_t itLen = b.u32(p + 8);
            if (ihLen < 28) break;
            pl.trackIds.push_back(b.u32(p + 24));
            if (itLen < ihLen) itLen = ihLen;
            p += itLen;
            // Some DB versions don't count child mhods in the mhip length;
            // skip any that follow before the next mhip.
            while (b.tagIs(p, "mhod")) {
                const std::uint32_t skip = b.u32(p + 8);
                if (skip < 24) break;
                p += skip;
            }
        }
        if (pl.isMaster) {
            lib.masterName = pl.name;
            lib.masterDbid = pl.dbid;
        } else {
            lib.playlists.push_back(std::move(pl));
        }
        pos += ptLen;
    }
}

}  // namespace

ParseResult parseItunesDb(const fs::path& path) {
    Buf b;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return {std::nullopt, "No music database found on this iPod"};
        const std::streamsize size = in.tellg();
        if (size < 48) return {std::nullopt, "Music database is empty"};
        in.seekg(0);
        b.d.resize(size_t(size));
        if (!in.read(reinterpret_cast<char*>(b.d.data()), size))
            return {std::nullopt, "Could not read the music database"};
    }
    if (!b.tagIs(0, "mhbd"))
        return {std::nullopt, "Not an iTunesDB file (bad header)"};

    Library lib;

    // Nano 5G and later store the database compressed as iTunesCDB. The mhbd
    // header stays plain; the flag at 0xA8 says the payload is a zlib stream,
    // and total_length then holds the compressed physical size, which must not
    // be trusted as the parse boundary.
    if (b.d.size() >= 0xAA && b.u16(0xA8) == 1) {
        if (auto plain = decompressItunesCdb(b.d)) {
            b.d = std::move(*plain);
            lib.compressed = true;
        } else {
            return {std::nullopt,
                    "This database is compressed (iTunesCDB) but will not "
                    "decompress"};
        }
    }

    lib.version = b.u32(16);
    if (b.d.size() >= 0x20)
        lib.databaseDbid = std::uint64_t(b.u32(24)) |
                           (std::uint64_t(b.u32(28)) << 32);
    // hashing_scheme lives at 0x30. (A second field at 0x70 is unmodelled and
    // looks temptingly scheme-like, but it is not the scheme.)
    if (b.d.size() >= 0x32) lib.hashingScheme = b.u16(0x30);
    const std::uint32_t mhbdHeaderLen = b.u32(4);

    const size_t fileEnd = std::min(b.d.size(), size_t(b.u32(8)));

    // Look ahead for a real playlist dataset before parsing anything. Older
    // databases put playlists in the type-3 slot, but modern ones use it for
    // podcasts and can emit it *before* type 2 — so deciding as we go would
    // misread 86 KB of podcast data as playlists and then discard it.
    bool haveType2 = false;
    for (size_t scan = mhbdHeaderLen;
         scan + 16 <= fileEnd && b.tagIs(scan, "mhsd");) {
        const std::uint32_t sHLen = b.u32(scan + 4);
        const std::uint32_t sTLen = b.u32(scan + 8);
        if (sHLen < 16 || sTLen < sHLen) break;
        if (b.u32(scan + 12) == 2 && b.tagIs(scan + sHLen, "mhlp"))
            haveType2 = true;
        scan += sTLen;
    }

    size_t pos = mhbdHeaderLen;
    while (pos + 16 <= fileEnd && b.tagIs(pos, "mhsd")) {
        const std::uint32_t hLen = b.u32(pos + 4);
        const std::uint32_t tLen = b.u32(pos + 8);
        const std::uint32_t type = b.u32(pos + 12);
        if (hLen < 16 || tLen < hLen) break;
        const size_t child = pos + hLen;
        if (type == 1 && b.tagIs(child, "mhlt")) {
            parseTracks(b, child, lib);
        } else if (type == 2 && b.tagIs(child, "mhlp")) {
            lib.playlists.clear();
            parsePlaylists(b, child, lib);
        } else if (type == 3 && !haveType2 && b.tagIs(child, "mhlp")) {
            // Only on databases old enough to keep playlists here.
            parsePlaylists(b, child, lib);
        } else if (type != 1 && type != 2 && b.has(child, tLen - hLen)) {
            // Podcasts, album lists, modern playlist datasets: not modelled,
            // so kept whole rather than dropped on the next write.
            Library::RawDataset raw;
            raw.type = type;
            raw.payload.assign(&b.d[child], &b.d[child] + (tLen - hLen));
            lib.extraDatasets.push_back(std::move(raw));
        }
        pos += tLen;
    }
    return {std::move(lib), {}};
}

std::string formatDuration(std::uint32_t ms) {
    const std::uint32_t total = ms / 1000;
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", total / 3600,
                      (total / 60) % 60, total % 60);
    else
        std::snprintf(buf, sizeof(buf), "%u:%02u", total / 60, total % 60);
    return buf;
}

}  // namespace podbox
