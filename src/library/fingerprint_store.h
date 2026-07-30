#pragma once

#include "itdb/itunesdb.h"
#include "library/dedupe.h"
#include "library/fingerprint.h"

#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace podbox {

// Remembers what each track on a device was made from.
//
// The iTunesDB has nowhere safe to record this, so it lives in a sidecar next
// to it and travels with the iPod. Keyed by Track::dbid, the only per-track
// identifier the DB persists across sessions.
//
// The point is transcoding. When a FLAC is imported as ALAC, the file on the
// device can never byte-match the source, so the source's fingerprint is
// recorded here at import time. A later drop of that same FLAC then matches
// exactly, and sync sees no work to do — without this, every sync would
// re-copy the entire lossless library forever.
class FingerprintStore {
public:
    enum class Origin {
        Source,  // hashed from the file that was imported
        Device,  // hashed from the file on the iPod, by the verify pass
    };

    // Replaces the contents with whatever is stored for `mount`. A missing or
    // malformed sidecar is not an error; it just yields an empty store.
    void load(const std::filesystem::path& mount);

    // Writes the sidecar via a temp file and a rename. No-op when unchanged.
    bool save(const std::filesystem::path& mount);

    const AudioFingerprint* get(std::uint64_t dbid) const;
    void put(std::uint64_t dbid, const AudioFingerprint& fp, Origin origin);
    Origin origin(std::uint64_t dbid) const;

    // Drops entries whose track is no longer in the library, so the sidecar
    // does not grow forever as songs come and go.
    void prune(const Library& lib);

    const FingerprintMap& all() const { return fingerprints_; }
    std::size_t size() const { return fingerprints_.size(); }
    void clear();

private:
    FingerprintMap fingerprints_;
    std::unordered_map<std::uint64_t, Origin> origins_;
    bool dirty_ = false;
};

}  // namespace podbox
