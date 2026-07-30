#include "library/fingerprint.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace podbox {
namespace {

// Each sampled window. Three of these bound the per-file cost regardless of
// how large the file is.
constexpr std::uint64_t kWindow = 256 * 1024;

std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t be64(const unsigned char* p) {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

bool readAt(std::ifstream& in, std::uint64_t off, unsigned char* buf,
            std::size_t n) {
    in.clear();
    in.seekg(std::streamoff(off));
    if (!in) return false;
    in.read(reinterpret_cast<char*>(buf), std::streamsize(n));
    return in.gcount() == std::streamsize(n);
}

// The half-open byte range holding the audio itself.
struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

// An ID3v2 tag at the head of the file. Size is stored "syncsafe": four bytes
// carrying seven bits each.
std::uint64_t id3v2Length(std::ifstream& in, std::uint64_t size) {
    unsigned char h[10];
    if (size < 10 || !readAt(in, 0, h, 10)) return 0;
    if (std::memcmp(h, "ID3", 3) != 0) return 0;
    const std::uint64_t body = (std::uint64_t(h[6] & 0x7f) << 21) |
                               (std::uint64_t(h[7] & 0x7f) << 14) |
                               (std::uint64_t(h[8] & 0x7f) << 7) |
                               std::uint64_t(h[9] & 0x7f);
    // Bit 4 of the flags marks a 10-byte footer after the tag body.
    const std::uint64_t total = 10 + body + ((h[5] & 0x10) ? 10 : 0);
    return total < size ? total : 0;
}

// ID3v1 (fixed 128 bytes) and APEv2 (self-describing) tags live at the tail.
// Trimming them keeps the fingerprint stable across taggers that append rather
// than prepend.
void trimTrailingTags(std::ifstream& in, std::uint64_t begin,
                      std::uint64_t* end) {
    for (bool trimmed = true; trimmed;) {
        trimmed = false;
        unsigned char buf[32];

        if (*end >= begin + 128 && readAt(in, *end - 128, buf, 3) &&
            std::memcmp(buf, "TAG", 3) == 0) {
            *end -= 128;
            trimmed = true;
            continue;
        }
        if (*end >= begin + 32 && readAt(in, *end - 32, buf, 32) &&
            std::memcmp(buf, "APETAGEX", 8) == 0) {
            // Little-endian tag size, covering the footer but not the header.
            const std::uint64_t sz = std::uint64_t(buf[12]) |
                                     (std::uint64_t(buf[13]) << 8) |
                                     (std::uint64_t(buf[14]) << 16) |
                                     (std::uint64_t(buf[15]) << 24);
            if (sz >= 32 && *end - begin >= sz) {
                *end -= sz;
                trimmed = true;
            }
        }
    }
}

// FLAC: "fLaC" then a chain of metadata blocks, the last one flagged, then the
// audio frames. Vorbis comments and embedded art all live in that chain, so
// skipping it is what makes a retagged FLAC fingerprint identically.
bool flacAudioStart(std::ifstream& in, std::uint64_t begin, std::uint64_t end,
                    std::uint64_t* out) {
    std::uint64_t pos = begin + 4;
    for (int block = 0; block < 128; ++block) {
        unsigned char h[4];
        if (pos + 4 > end || !readAt(in, pos, h, 4)) return false;
        const std::uint64_t len =
            (std::uint64_t(h[1]) << 16) | (std::uint64_t(h[2]) << 8) | h[3];
        pos += 4 + len;
        if (pos > end) return false;
        if (h[0] & 0x80) {  // last-metadata-block flag
            *out = pos;
            return true;
        }
    }
    return false;
}

// MP4/M4A: top-level atoms, of which `mdat` holds the audio and `moov` holds
// the metadata. Hash only mdat.
bool mp4MdatRange(std::ifstream& in, std::uint64_t end, Range* out) {
    std::uint64_t pos = 0;
    while (pos + 8 <= end) {
        unsigned char h[16];
        if (!readAt(in, pos, h, 8)) return false;
        std::uint64_t size = be32(h);
        std::uint64_t header = 8;
        if (size == 1) {  // 64-bit extended size follows the type
            if (!readAt(in, pos, h, 16)) return false;
            size = be64(h + 8);
            header = 16;
        } else if (size == 0) {
            size = end - pos;  // extends to end of file
        }
        if (size < header || pos + size > end) return false;
        if (std::memcmp(h + 4, "mdat", 4) == 0) {
            out->begin = pos + header;
            out->end = pos + size;
            return out->end > out->begin;
        }
        pos += size;
    }
    return false;
}

// Narrows the whole file down to just its audio stream.
Range audioRange(std::ifstream& in, std::uint64_t size) {
    Range r{0, size};

    // An ID3v2 tag can precede any container, not just MP3.
    r.begin = id3v2Length(in, size);

    unsigned char magic[12] = {};
    const bool haveMagic = readAt(in, r.begin, magic, sizeof(magic));

    if (haveMagic && std::memcmp(magic, "fLaC", 4) == 0) {
        std::uint64_t start = 0;
        if (flacAudioStart(in, r.begin, r.end, &start)) r.begin = start;
        return r;
    }
    if (haveMagic && std::memcmp(magic + 4, "ftyp", 4) == 0) {
        Range mdat;
        if (mp4MdatRange(in, size, &mdat)) return mdat;
        return r;
    }

    // MP3 and anything unrecognised: strip tail tags and hash the rest.
    trimTrailingTags(in, r.begin, &r.end);
    return r;
}

void hashWindow(std::ifstream& in, std::uint64_t off, std::uint64_t len,
                std::uint64_t* h) {
    std::vector<unsigned char> buf(std::size_t(len), 0);
    if (!readAt(in, off, buf.data(), buf.size())) return;

    std::size_t i = 0;
    for (; i + 8 <= buf.size(); i += 8) {
        std::uint64_t word;
        std::memcpy(&word, buf.data() + i, 8);
        *h = mix(*h ^ mix(word));
    }
    std::uint64_t tail = 0;
    for (std::size_t k = 0; i < buf.size(); ++i, ++k)
        tail |= std::uint64_t(buf[i]) << (k * 8);
    *h = mix(*h ^ mix(tail));
}

}  // namespace

AudioFingerprint fingerprintFile(const fs::path& path) {
    AudioFingerprint fp;

    std::error_code ec;
    const std::uintmax_t fileSize = fs::file_size(path, ec);
    if (ec || fileSize == 0) return fp;

    std::ifstream in(path, std::ios::binary);
    if (!in) return fp;

    const Range r = audioRange(in, std::uint64_t(fileSize));
    if (r.end <= r.begin) return fp;
    const std::uint64_t len = r.end - r.begin;

    // Seed with the stream length so same-length-different-content and
    // different-length files diverge even if a window read fails.
    std::uint64_t h = mix(len);
    if (len <= kWindow * 3) {
        hashWindow(in, r.begin, len, &h);
    } else {
        hashWindow(in, r.begin, kWindow, &h);
        hashWindow(in, r.begin + len / 2 - kWindow / 2, kWindow, &h);
        hashWindow(in, r.end - kWindow, kWindow, &h);
    }

    fp.bytes = len;
    fp.hash = h;
    return fp;
}

}  // namespace podbox
