#include "library/host_dedupe.h"

#include <algorithm>
#include <limits>

namespace fs = std::filesystem;

namespace podbox {

std::vector<DuplicateGroup> findHostDuplicates(const HostLibrary& host,
                                               MatchMode mode) {
    Library lib;
    FingerprintMap fingerprints;
    lib.tracks.reserve(host.tracks().size());
    std::error_code ec;
    for (const HostTrack& h : host.tracks()) {
        // Only the library folder is the library. Anything elsewhere is not
        // PodBox's to judge, let alone remove.
        if (!isWithinFolder(h.file, host.musicFolder())) continue;
        if (h.missing || !fs::is_regular_file(h.file, ec)) continue;
        Track t = h.meta;
        t.id = std::uint32_t(h.id);
        t.dbid = h.id;
        t.location = h.file.string();
        // The size on disk is what removing a copy frees.
        if (h.size)
            t.sizeBytes = std::uint32_t(std::min<std::uint64_t>(
                h.size, std::numeric_limits<std::uint32_t>::max()));
        if (h.fp.ok()) fingerprints[h.id] = h.fp;
        lib.tracks.push_back(std::move(t));
    }
    return findDuplicates(lib, mode, fingerprints);
}

HostRemovalResult removeHostDuplicates(const std::vector<HostRemoval>& items,
                                       const std::vector<fs::path>& managedRoots,
                                       const TrashFn& trash) {
    HostRemovalResult out;
    auto drop = [&out](const HostRemoval& item) {
        out.removed.push_back(item.id);
        out.removedFiles.push_back(item.file);
    };

    for (const HostRemoval& item : items) {
        std::error_code ec;
        if (!fs::is_regular_file(item.keeper, ec)) {
            ++out.kept;
            continue;
        }

        const bool present = fs::exists(item.file, ec);
        if (ec) {  // unreadable rather than absent: cannot tell, so leave it
            ++out.kept;
            continue;
        }
        if (!present) {
            drop(item);
            ++out.unlisted;
            continue;
        }

        const bool same = fs::equivalent(item.file, item.keeper, ec);
        if (ec) {
            ++out.kept;
            continue;
        }
        const bool managed = std::any_of(
            managedRoots.begin(), managedRoots.end(),
            [&](const fs::path& root) { return isWithinFolder(item.file, root); });
        if (same || !managed) {
            drop(item);
            ++out.unlisted;
            continue;
        }

        std::string error;
        if (trash(item.file, &error)) {
            drop(item);
            ++out.trashed;
        } else {
            ++out.kept;
            if (out.error.empty())
                out.error = error.empty() ? "could not move a file to the Trash"
                                          : error;
        }
    }
    return out;
}

}  // namespace podbox
