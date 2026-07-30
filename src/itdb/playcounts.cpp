#include "itdb/playcounts.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace podbox {

PlayCountsMerge mergePlayCounts(const fs::path& file, Library& lib) {
    PlayCountsMerge out;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return out;
    const std::streamsize size = in.tellg();
    if (size < 16) return out;
    in.seekg(0);
    std::vector<unsigned char> d(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(d.data()), size)) return out;

    auto u32 = [&](size_t off) -> std::uint32_t {
        if (off + 4 > d.size()) return 0;
        return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) |
               (std::uint32_t(d[off + 3]) << 24);
    };

    if (std::memcmp(d.data(), "mhdp", 4) != 0) return out;
    const std::uint32_t headerLen = u32(4);
    const std::uint32_t entryLen = u32(8);
    const std::uint32_t numEntries = u32(12);
    if (headerLen < 16 || entryLen < 12) return out;
    out.formatOk = true;
    out.entries = int(numEntries);

    // Positional format: only safe to apply when it matches our track list.
    out.trackCount = int(lib.tracks.size());
    if (numEntries != lib.tracks.size()) {
        out.mismatched = true;
        return out;
    }
    if (headerLen + std::uint64_t(numEntries) * entryLen > d.size()) return out;

    for (std::uint32_t i = 0; i < numEntries; ++i) {
        const size_t off = headerLen + size_t(i) * entryLen;
        Track& t = lib.tracks[i];
        bool changed = false;
        if (const std::uint32_t plays = u32(off)) {
            t.playCount += plays;
            changed = true;
        }
        if (entryLen >= 16) {
            const std::uint32_t rating = u32(off + 12);
            if (rating > 0 && rating <= 100 &&
                rating != std::uint32_t(t.rating)) {
                t.rating = std::uint8_t(rating);
                changed = true;
            }
        }
        if (changed) ++out.applied;
    }
    return out;
}

}  // namespace podbox
