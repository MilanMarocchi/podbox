#include "library/dedupe.h"

#include <algorithm>
#include <unordered_map>

namespace podbox {
namespace {

// Lowercased extension of the track's file, without the dot. The location is
// an iPod-style ':'-separated path, but only the tail after the last '.'
// matters here so the separator is irrelevant.
std::string trackExtension(const Track& t) {
    const std::size_t dot = t.location.rfind('.');
    if (dot == std::string::npos) return {};
    return toLower(t.location.substr(dot + 1));
}

// Duration rounded to the nearest 2 seconds, so encoders that disagree by a
// frame or two still match.
//
// Being a bucket rather than a true tolerance, this splits a pair straddling a
// boundary (e.g. 4.999s and 5.001s land either side). Those escape as separate
// groups, which fails safe: a missed duplicate is a nuisance, a false one is a
// deleted song. Loose mode drops duration entirely and catches the stragglers.
std::uint32_t durationBucket(std::uint32_t lengthMs) {
    return (lengthMs + 1000) / 2000;
}

}  // namespace

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}

bool hasIdentity(const Track& t) {
    return !t.artist.empty() || !t.title.empty();
}

std::string duplicateKey(const Track& t, MatchMode mode) {
    if (!hasIdentity(t)) return {};
    std::string key = toLower(t.artist) + '|' + toLower(t.title);
    if (mode == MatchMode::Exact) {
        key += '|' + toLower(t.album) + '|' +
               std::to_string(durationBucket(t.lengthMs));
    }
    return key;
}

std::vector<std::string> lookupKeys(const Track& t, MatchMode mode) {
    if (!hasIdentity(t)) return {};
    if (mode == MatchMode::Loose) return {duplicateKey(t, mode)};

    const std::string stem = toLower(t.artist) + '|' + toLower(t.title) + '|' +
                             toLower(t.album) + '|';
    const std::uint32_t b = durationBucket(t.lengthMs);
    std::vector<std::string> keys;
    keys.reserve(3);
    keys.push_back(stem + std::to_string(b));
    if (b > 0) keys.push_back(stem + std::to_string(b - 1));
    keys.push_back(stem + std::to_string(b + 1));
    return keys;
}

bool matchesAny(const Track& t, const std::unordered_set<std::string>& keys,
                MatchMode mode) {
    for (const std::string& k : lookupKeys(t, mode))
        if (keys.count(k)) return true;
    return false;
}

bool isLossless(const Track& t) {
    const std::string ext = trackExtension(t);
    if (ext == "flac" || ext == "wav" || ext == "aif" || ext == "aiff" ||
        ext == "alac")
        return true;
    // ALAC and AAC share the .m4a/.m4b container on the device; only the rate
    // tells them apart.
    if (ext == "m4a" || ext == "m4b" || ext == "mp4") return t.bitrate >= 400;
    return false;
}

bool betterCopy(const Track& a, const Track& b) {
    // The decision this mostly exists to make: keep the FLAC, drop the MP3.
    if (isLossless(a) != isLossless(b)) return isLossless(a);
    if (a.playCount != b.playCount) return a.playCount > b.playCount;
    if (a.rating != b.rating) return a.rating > b.rating;
    if (a.bitrate != b.bitrate) return a.bitrate > b.bitrate;
    if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
    // Prefer the copy that has been in the library longest; 0 means unknown.
    if ((a.dateAdded != 0) != (b.dateAdded != 0)) return a.dateAdded != 0;
    if (a.dateAdded != b.dateAdded && a.dateAdded != 0)
        return a.dateAdded < b.dateAdded;
    return a.id < b.id;  // total order, so results are reproducible
}

void applyRemovalToPlaylist(const std::unordered_set<std::uint32_t>& removed,
                            const KeeperRemap* remap,
                            std::vector<std::uint32_t>* trackIds) {
    std::vector<std::uint32_t> kept;
    kept.reserve(trackIds->size());
    for (std::uint32_t id : *trackIds) {
        std::uint32_t mapped = id;
        if (removed.count(id)) {
            if (!remap) continue;
            const auto r = remap->find(id);
            if (r == remap->end()) continue;
            mapped = r->second;
        }
        if (mapped != id && !kept.empty() && kept.back() == mapped) continue;
        kept.push_back(mapped);
    }
    *trackIds = std::move(kept);
}

std::vector<DuplicateGroup> findDuplicates(const Library& lib, MatchMode mode,
                                           const FingerprintMap& fingerprints) {
    // Bucket by key, preserving first-seen order so equal-saving groups come
    // out in a stable sequence.
    std::unordered_map<std::string, std::vector<int>> byKey;
    std::vector<std::string> order;
    for (int i = 0; i < int(lib.tracks.size()); ++i) {
        const std::string key = duplicateKey(lib.tracks[i], mode);
        if (key.empty()) continue;
        auto [it, inserted] = byKey.try_emplace(key);
        if (inserted) order.push_back(key);
        it->second.push_back(i);
    }

    std::vector<DuplicateGroup> groups;
    for (const std::string& key : order) {
        std::vector<int>& members = byKey[key];
        if (members.size() < 2) continue;

        std::sort(members.begin(), members.end(), [&](int x, int y) {
            return betterCopy(lib.tracks[x], lib.tracks[y]);
        });

        DuplicateGroup g;
        g.trackIds.reserve(members.size());
        for (int idx : members) g.trackIds.push_back(lib.tracks[idx].id);
        for (std::size_t i = 1; i < members.size(); ++i)
            g.reclaimBytes += lib.tracks[members[i]].sizeBytes;

        // Byte-identical only if every member has a fingerprint and they agree.
        g.allIdenticalFiles = true;
        const AudioFingerprint* first = nullptr;
        for (int idx : members) {
            const auto it = fingerprints.find(lib.tracks[idx].dbid);
            if (it == fingerprints.end() || !it->second.ok()) {
                g.allIdenticalFiles = false;
                break;
            }
            if (!first)
                first = &it->second;
            else if (!(*first == it->second)) {
                g.allIdenticalFiles = false;
                break;
            }
        }

        groups.push_back(std::move(g));
    }

    std::stable_sort(groups.begin(), groups.end(),
                     [](const DuplicateGroup& a, const DuplicateGroup& b) {
                         return a.reclaimBytes > b.reclaimBytes;
                     });
    return groups;
}

}  // namespace podbox
