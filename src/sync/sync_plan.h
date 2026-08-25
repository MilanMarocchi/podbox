#pragma once

#include "itdb/itunesdb.h"
#include "library/fingerprint_store.h"
#include "library/host_library.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace podbox {

struct SyncOptions {
    // Off by default and deliberately so: taking music off a device is the
    // one step here that cannot be undone by running sync again.
    bool removeFromDevice = false;
};

struct SyncPlan {
    std::vector<std::uint64_t> toCopy;    // host track ids
    std::vector<std::uint32_t> toRemove;  // device track ids
    // Database entries whose audio file has disappeared from the device.
    // These are always dropped before copying so they cannot make either the
    // planner or the import duplicate guard mistake a missing song for one
    // that is still playable.
    std::vector<std::uint32_t> missingDeviceFiles;

    std::uint64_t bytesToCopy = 0;
    std::uint64_t bytesToFree = 0;

    int alreadyOnDevice = 0;
    // Of toRemove, how many exist nowhere else. Removing these is the only
    // irreversible thing sync does, so it is surfaced rather than buried.
    int deviceOnly = 0;
    int skippedMissing = 0;    // host file is gone
    int skippedDuplicate = 0;  // another copy of the same song is already queued

    bool empty() const {
        return toCopy.empty() && toRemove.empty() &&
               missingDeviceFiles.empty();
    }
};

// Works out what would have to happen to make the device match the Mac
// library. Read-only: in addition to the two libraries, it checks that every
// device track's referenced audio file is still present below `deviceMount`.
//
// A host song counts as already present when a device track shares its
// fingerprint (certain) or its metadata key (near-certain, and the only thing
// that still works once a FLAC has been transcoded to ALAC on the way over).
SyncPlan planSync(const HostLibrary& host, const Library& device,
                  const FingerprintStore& fingerprints,
                  const std::filesystem::path& deviceMount,
                  const SyncOptions& options,
                  const std::unordered_set<std::uint32_t>* removableIds =
                      nullptr);

}  // namespace podbox
