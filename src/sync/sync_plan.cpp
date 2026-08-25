#include "sync/sync_plan.h"

#include "library/dedupe.h"

#include <algorithm>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {
namespace {

fs::path deviceTrackPath(const fs::path& mount,
                         const std::string& location) {
    std::string relative = location;
    std::replace(relative.begin(), relative.end(), ':', '/');
    // Both iTunesDB spellings occur in the wild: colon locations generally
    // start with ':' while SQLite-derived ones can start with '/'. In either
    // case the path is rooted at the device mount, not the Mac filesystem.
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    return mount / relative;
}

bool deviceFilePresent(const fs::path& mount, const Track& track) {
    if (track.location.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(deviceTrackPath(mount, track.location), ec);
}

}  // namespace

SyncPlan planSync(const HostLibrary& host, const Library& device,
                  const FingerprintStore& fingerprints,
                  const fs::path& deviceMount,
                  const SyncOptions& options) {
    SyncPlan plan;

    // What the device actually holds, by both measures. Finder/iTunes can
    // remove audio while leaving a database entry behind; that entry must not
    // suppress the replacement copy.
    std::unordered_set<std::string> deviceKeys;
    std::unordered_set<std::uint64_t> deviceHashes;
    std::unordered_set<std::uint32_t> presentDeviceIds;
    deviceKeys.reserve(device.tracks.size());
    presentDeviceIds.reserve(device.tracks.size());
    for (const Track& t : device.tracks) {
        if (!deviceFilePresent(deviceMount, t)) {
            plan.missingDeviceFiles.push_back(t.id);
            continue;
        }
        presentDeviceIds.insert(t.id);
        const std::string key = duplicateKey(t, MatchMode::Exact);
        if (!key.empty()) deviceKeys.insert(key);
        if (const AudioFingerprint* fp = fingerprints.get(t.dbid))
            if (fp->ok()) deviceHashes.insert(fp->hash);
    }

    // Songs queued so far, so two copies of one song in the Mac library do
    // not both get sent over.
    std::unordered_set<std::string> queuedKeys;
    std::unordered_set<std::uint64_t> queuedHashes;

    std::error_code ec;
    for (const HostTrack& h : host.tracks()) {
        if (h.missing || !fs::exists(h.file, ec)) {
            ++plan.skippedMissing;
            continue;
        }

        const bool onDevice = (h.fp.ok() && deviceHashes.count(h.fp.hash)) ||
                              matchesAny(h.meta, deviceKeys, MatchMode::Exact);
        if (onDevice) {
            ++plan.alreadyOnDevice;
            continue;
        }

        const bool queued = (h.fp.ok() && queuedHashes.count(h.fp.hash)) ||
                            matchesAny(h.meta, queuedKeys, MatchMode::Exact);
        if (queued) {
            ++plan.skippedDuplicate;
            continue;
        }

        const std::string key = duplicateKey(h.meta, MatchMode::Exact);
        if (!key.empty()) queuedKeys.insert(key);
        if (h.fp.ok()) queuedHashes.insert(h.fp.hash);
        plan.toCopy.push_back(h.id);
        plan.bytesToCopy += h.size;
    }

    if (options.removeFromDevice) {
        // Everything the Mac library can account for, so a device track is
        // only removed when nothing on the Mac corresponds to it.
        //
        // Deliberately more generous than the copy test above. "No match"
        // here means "delete this song", so the two directions have opposite
        // risk: missing a match costs a needless re-copy, a false one costs
        // the user their music. A track must fail the tolerant Exact test AND
        // the artist+title test before it is touched.
        std::unordered_set<std::string> hostExact, hostLoose;
        std::unordered_set<std::uint64_t> hostHashes;
        for (const HostTrack& h : host.tracks()) {
            if (h.missing) continue;
            const std::string exact = duplicateKey(h.meta, MatchMode::Exact);
            if (!exact.empty()) hostExact.insert(exact);
            const std::string loose = duplicateKey(h.meta, MatchMode::Loose);
            if (!loose.empty()) hostLoose.insert(loose);
            if (h.fp.ok()) hostHashes.insert(h.fp.hash);
        }
        for (const Track& t : device.tracks) {
            // Missing audio is handled unconditionally by
            // missingDeviceFiles. It cannot be a device-only copy and has no
            // bytes to free.
            if (!presentDeviceIds.count(t.id)) continue;
            const AudioFingerprint* fp = fingerprints.get(t.dbid);
            const bool known = (fp && fp->ok() && hostHashes.count(fp->hash)) ||
                               matchesAny(t, hostExact, MatchMode::Exact) ||
                               matchesAny(t, hostLoose, MatchMode::Loose);
            if (known) continue;
            // Nothing on the Mac corresponds to this song, so removing it
            // destroys the only copy. Counted separately so the UI can say so.
            ++plan.deviceOnly;
            plan.toRemove.push_back(t.id);
            plan.bytesToFree += t.sizeBytes;
        }
    }

    return plan;
}

}  // namespace podbox
