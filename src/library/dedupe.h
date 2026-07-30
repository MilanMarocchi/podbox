#pragma once

#include "itdb/itunesdb.h"
#include "library/fingerprint.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace podbox {

// ASCII-lowercases a string. Shared by the dedupe keys and the UI's search
// filter so the two agree on what "same text" means.
std::string toLower(const std::string& s);

enum class MatchMode {
    // artist + title + album + duration. The default: conservative enough to
    // trust with a delete button.
    Exact,
    // artist + title only. Catches one song appearing on both an album and a
    // compilation, at the cost of grouping live and studio recordings.
    Loose,
};

// True when a track carries enough tagging to be compared at all. Tracks with
// neither an artist nor a title are never grouped: untagged rips would
// otherwise all collide on the empty key and be "deduplicated" into one.
bool hasIdentity(const Track& t);

// The identity key for `t` under `mode`. Two tracks are candidate duplicates
// exactly when their keys match. Empty when !hasIdentity(t).
std::string duplicateKey(const Track& t, MatchMode mode);

// The keys to probe when testing whether `t` is present in a set built from
// duplicateKey(). Duration is bucketed, so two rips of one song that differ by
// a few milliseconds can land either side of a boundary and miss each other.
// Grouping tolerates that (it just yields two groups), but a membership test
// must not: callers use "no match" to mean "this song is not on the Mac", and
// act on it by deleting. Probing the neighbouring buckets makes the tolerance
// real. Empty when !hasIdentity(t).
std::vector<std::string> lookupKeys(const Track& t, MatchMode mode);

// True when `t` matches something in `keys` (built with duplicateKey) under
// `mode`, allowing for that boundary case.
bool matchesAny(const Track& t, const std::unordered_set<std::string>& keys,
                MatchMode mode);

// True when the track is in a lossless format. Extension decides it for source
// files; on an iPod, ALAC and AAC are both ".m4a", so bitrate breaks the tie
// (lossless stereo audio does not fit under ~400 kbps).
bool isLossless(const Track& t);

// Strict weak ordering: true when `a` is the better copy of a song to keep.
// Lossless first, because that is the choice this is really being asked to
// make; then listening history, then encode quality.
bool betterCopy(const Track& a, const Track& b);

// Fingerprints of on-device files, keyed by Track::dbid.
using FingerprintMap = std::unordered_map<std::uint64_t, AudioFingerprint>;

struct DuplicateGroup {
    std::vector<std::uint32_t> trackIds;  // best copy first; the rest are dupes
    // Every member has a known fingerprint and they all agree, i.e. these are
    // byte-identical files rather than merely the same song.
    bool allIdenticalFiles = false;
    std::uint64_t reclaimBytes = 0;  // freed by removing all but the keeper
};

// Groups `lib` into sets of duplicates, largest space saving first. Groups of
// one are omitted. Deterministic for a given library.
std::vector<DuplicateGroup> findDuplicates(const Library& lib, MatchMode mode,
                                           const FingerprintMap& fingerprints);

// Maps a removed track id to the copy being kept in its place.
using KeeperRemap = std::unordered_map<std::uint32_t, std::uint32_t>;

// Rewrites one playlist's track ids for a batch removal, in place.
//
// Entries naming a removed track become its keeper when `remap` has one, and
// are dropped otherwise. Only the repeats the remap itself introduces are
// collapsed — a playlist that already listed the same song twice keeps both,
// because playlists are allowed to and that is the user's arrangement.
void applyRemovalToPlaylist(const std::unordered_set<std::uint32_t>& removed,
                            const KeeperRemap* remap,
                            std::vector<std::uint32_t>* trackIds);

}  // namespace podbox
