#pragma once

// The iTunesCDB container: on the iPod nano 5G (and the 6G/7G, which PodBox
// cannot write anyway) the music database is stored compressed. The mhbd
// header stays uncompressed; the payload after it — everything from the first
// mhsd dataset on — is a zlib stream, the flag at 0xA8 says so, and the
// total_length field at 0x08 counts the compressed file's physical size
// rather than the database's real one.
//
// The hash72 checksum a nano 5G requires covers the compressed bytes exactly
// as they sit on disk, so reading and writing go through these helpers in
// both directions: the parser inflates to see the library, the writer deflates
// before signing.

#include <cstdint>
#include <optional>
#include <vector>

namespace podbox {

// True when the image carries the compressed-payload marker (u16 0xA8 == 1).
bool isCompressedCdb(const std::vector<std::uint8_t>& image);

// Inflates the payload of a compressed iTunesCDB and returns a parse-ready
// plain image with total_length patched to the uncompressed size (the parser
// trusts that field as the data boundary, and it cannot be trusted to come
// with the file). Nothing when the image is not a compressed CDB or the zlib
// stream is corrupt.
std::optional<std::vector<std::uint8_t>> decompressItunesCdb(
    const std::vector<std::uint8_t>& cdb);

// Compresses a plain iTunesDB image into the iTunesCDB format: the payload
// after the mhbd header is deflated, total_length and the 0xA8 flag are
// updated. Returns empty on failure. The output is what must be hashed and
// written for a hash72 device.
std::vector<std::uint8_t> compressItunesCdb(
    const std::vector<std::uint8_t>& image);

}  // namespace podbox
