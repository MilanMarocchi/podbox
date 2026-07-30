#pragma once

#include <cstdint>
#include <filesystem>

namespace podbox {

// A content fingerprint for an audio file.
//
// Computed over the audio stream only, with container metadata (ID3 tags,
// FLAC metadata blocks, the MP4 moov atom) excluded — so retagging a file
// leaves its fingerprint unchanged. That matters because the same song grabbed
// from two sources usually differs in tags and nothing else.
//
// Sampled rather than exhaustive: three windows plus the stream length, which
// costs the same for a 3 MB MP3 and a 60 MB FLAC. That constant cost is what
// makes hashing a whole iPod over USB tolerable. Two copies of one song
// reliably share a fingerprint; two different songs sharing one is vanishingly
// unlikely.
//
// A fingerprint match means "certainly the same file". A mismatch means very
// little on its own — a transcode of a song is a different file by this
// measure — so callers pair it with the metadata key in dedupe.h.
//
// Verified against real files: an MP3 whose ID3v2 tag shrank from 292 KB to
// 547 bytes fingerprints identically, as does a retagged FLAC. Remuxing an MP3
// (e.g. `ffmpeg -c copy`) does change it, because that rewrites the leading
// Xing/LAME header frame, which is genuinely part of the audio stream.
struct AudioFingerprint {
    std::uint64_t bytes = 0;  // audio stream length, container metadata excluded
    std::uint64_t hash = 0;

    bool ok() const { return bytes != 0; }
    bool operator==(const AudioFingerprint&) const = default;
};

// Reads `path` and fingerprints its audio stream. Never throws; an unreadable
// or empty file yields a default (!ok()) fingerprint.
AudioFingerprint fingerprintFile(const std::filesystem::path& path);

}  // namespace podbox
