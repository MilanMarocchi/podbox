// iTunesCDB, the zlib-compressed database container used by the iPod nano 5G
// and later. The format was documented from real devices in the iOpenPod
// project (its checksum notes describe the compressed file layout field by
// field); the details here match that analysis:
//
//   - "mhbd" header, uncompressed, with the total_length at 0x08 holding the
//     compressed file's physical size and the flag at 0xA8 set to 1;
//   - everything from the header length (0x04) to total_length is a zlib
//     stream of the database body;
//   - on devices that use it, the iTunesDB file itself exists but is empty.

#include "itdb/itunescdb.h"

#include <cstring>
#include <zlib.h>

namespace podbox {
namespace {

std::uint16_t u16le(const std::vector<std::uint8_t>& v, std::size_t off) {
    return std::uint16_t(v[off] | (std::uint16_t(v[off + 1]) << 8));
}

std::uint32_t u32le(const std::vector<std::uint8_t>& v, std::size_t off) {
    return std::uint32_t(v[off]) | (std::uint32_t(v[off + 1]) << 8) |
           (std::uint32_t(v[off + 2]) << 16) | (std::uint32_t(v[off + 3]) << 24);
}

void set32le(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) v[off + i] = std::uint8_t(n >> (8 * i));
}

void set16le(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t n) {
    v[off] = std::uint8_t(n);
    v[off + 1] = std::uint8_t(n >> 8);
}

bool isMhbd(const std::vector<std::uint8_t>& v) {
    return v.size() >= 4 && std::memcmp(v.data(), "mhbd", 4) == 0;
}

}  // namespace

bool isCompressedCdb(const std::vector<std::uint8_t>& image) {
    return isMhbd(image) && image.size() >= 0xAA && u16le(image, 0xA8) == 1;
}

std::optional<std::vector<std::uint8_t>> decompressItunesCdb(
    const std::vector<std::uint8_t>& cdb) {
    if (!isCompressedCdb(cdb)) return std::nullopt;
    const std::uint32_t headerLen = u32le(cdb, 4);
    const std::uint32_t totalLen = u32le(cdb, 8);
    if (headerLen < 0xF4 || totalLen <= headerLen || totalLen > cdb.size())
        return std::nullopt;

    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return std::nullopt;
    strm.next_in =
        const_cast<Bytef*>(cdb.data() + headerLen);
    strm.avail_in = uInt(totalLen - headerLen);

    std::vector<std::uint8_t> body;
    std::uint8_t chunk[64 * 1024];
    int ret;
    do {
        strm.next_out = chunk;
        strm.avail_out = sizeof(chunk);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) break;
        body.insert(body.end(), chunk, chunk + (sizeof(chunk) - strm.avail_out));
    } while (ret != Z_STREAM_END);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END) return std::nullopt;

    // The parser trusts total_length as the boundary between the header and
    // the datasets; in a compressed file it holds the physical size, which
    // would truncate the parse. Patch it to the real length, and clear the
    // compressed marker so the result is a plain image again — the two flags
    // describe the container, and this no longer is one.
    std::vector<std::uint8_t> image(cdb.begin(), cdb.begin() + headerLen);
    image.insert(image.end(), body.begin(), body.end());
    set32le(image, 8, std::uint32_t(image.size()));
    set16le(image, 0xA8, 0);
    return image;
}

std::vector<std::uint8_t> compressItunesCdb(
    const std::vector<std::uint8_t>& image) {
    if (!isMhbd(image)) return {};
    const std::uint32_t headerLen = u32le(image, 4);
    if (headerLen < 0xF4 || headerLen >= image.size()) return {};

    // Level 1: the payload is a megabyte of tracks, compressed once; a
    // firmware-mandated "zlib" stream has no level semantics, so speed wins.
    uLongf bound = compressBound(uLong(image.size() - headerLen));
    std::vector<std::uint8_t> payload(bound);
    if (compress2(payload.data(), &bound, image.data() + headerLen,
                  uLong(image.size() - headerLen), 1) != Z_OK)
        return {};
    payload.resize(bound);

    std::vector<std::uint8_t> cdb(image.begin(), image.begin() + headerLen);
    cdb.insert(cdb.end(), payload.begin(), payload.end());
    set32le(cdb, 8, std::uint32_t(cdb.size()));  // physical size
    set16le(cdb, 0xA8, 1);                       // compressed marker
    return cdb;
}

}  // namespace podbox
