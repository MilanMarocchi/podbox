#include "library/fingerprint_store.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr const char* kMagic = "podbox-fingerprints 1";

fs::path sidecarPath(const fs::path& mount) {
    return mount / "iPod_Control" / "iTunes" / "PodBoxFingerprints";
}

}  // namespace

void FingerprintStore::clear() {
    fingerprints_.clear();
    origins_.clear();
    dirty_ = false;
}

void FingerprintStore::load(const fs::path& mount) {
    clear();
    std::ifstream in(sidecarPath(mount));
    if (!in) return;

    std::string line;
    if (!std::getline(in, line) || line.rfind(kMagic, 0) != 0) return;

    // dbid<TAB>bytes<TAB>hash<TAB>origin
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::uint64_t dbid = 0, bytes = 0, hash = 0;
        std::string origin;
        if (!(ls >> dbid >> bytes >> hash >> origin)) continue;
        if (dbid == 0 || bytes == 0) continue;
        fingerprints_[dbid] = AudioFingerprint{bytes, hash};
        origins_[dbid] = origin == "dev" ? Origin::Device : Origin::Source;
    }
}

bool FingerprintStore::save(const fs::path& mount) {
    if (!dirty_) return true;

    const fs::path path = sidecarPath(mount);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << kMagic << "\n";
        for (const auto& [dbid, fp] : fingerprints_) {
            const auto it = origins_.find(dbid);
            const bool dev =
                it != origins_.end() && it->second == Origin::Device;
            out << dbid << '\t' << fp.bytes << '\t' << fp.hash << '\t'
                << (dev ? "dev" : "src") << '\n';
        }
        if (!out) return false;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    dirty_ = false;
    return true;
}

const AudioFingerprint* FingerprintStore::get(std::uint64_t dbid) const {
    const auto it = fingerprints_.find(dbid);
    return it == fingerprints_.end() ? nullptr : &it->second;
}

void FingerprintStore::put(std::uint64_t dbid, const AudioFingerprint& fp,
                           Origin origin) {
    if (dbid == 0 || !fp.ok()) return;
    // A source fingerprint is the better record: it survives transcoding,
    // which is exactly what a device-side hash cannot do. Never let the
    // verify pass overwrite one.
    const auto existing = origins_.find(dbid);
    if (origin == Origin::Device && existing != origins_.end() &&
        existing->second == Origin::Source)
        return;

    fingerprints_[dbid] = fp;
    origins_[dbid] = origin;
    dirty_ = true;
}

FingerprintStore::Origin FingerprintStore::origin(std::uint64_t dbid) const {
    const auto it = origins_.find(dbid);
    return it == origins_.end() ? Origin::Device : it->second;
}

void FingerprintStore::prune(const Library& lib) {
    std::unordered_set<std::uint64_t> live;
    live.reserve(lib.tracks.size());
    for (const Track& t : lib.tracks) live.insert(t.dbid);

    for (auto it = fingerprints_.begin(); it != fingerprints_.end();) {
        if (live.count(it->first)) {
            ++it;
            continue;
        }
        origins_.erase(it->first);
        it = fingerprints_.erase(it);
        dirty_ = true;
    }
}

}  // namespace podbox
