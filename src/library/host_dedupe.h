#pragma once

#include "library/dedupe.h"
#include "library/host_library.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace podbox {

// Duplicate review for the Mac library. Grouping and the choice of which copy
// to keep are exactly the player's (dedupe.h). Removal is what differs: a
// removed duplicate goes to the Trash rather than being deleted, and only
// ever out of the library's music folder. The folders PodBox imports from are
// never touched.

// Groups the duplicates in the library's music folder. Track ids in the
// result are host ids narrowed to 32 bits, as in the Library-shaped view the
// UI shows. Songs outside the music folder are left out, and so are songs
// whose file is gone: a missing file cannot be the copy that is kept, and
// there is nothing of it left to remove.
std::vector<DuplicateGroup> findHostDuplicates(const HostLibrary& host,
                                               MatchMode mode);

// One duplicate to remove, and the copy being kept in its place.
struct HostRemoval {
    std::uint64_t id = 0;
    std::filesystem::path file;
    std::filesystem::path keeper;
};

struct HostRemovalResult {
    std::vector<std::uint64_t> removed;             // drop these from the library
    std::vector<std::filesystem::path> removedFiles;  // their paths, same order
    int trashed = 0;   // moved to the Trash
    int unlisted = 0;  // dropped from the library; no file was touched
    int kept = 0;      // left alone entirely
    std::string error;  // the first thing that refused, for the status bar
};

// Moves one file to the Trash. Injected so tests never touch the real one.
using TrashFn = std::function<bool(const std::filesystem::path&, std::string*)>;

// Removes each duplicate from the Mac library, re-checking everything on disk
// first because the review may be minutes old:
//
//   - If the keeper has gone, the duplicate is left alone: it may now be the
//     only copy of the song.
//   - If the duplicate is the keeper under another name (a symlink, or a
//     watch folder added twice in different case), only the library entry
//     goes. Trashing it would trash the keeper.
//   - If the duplicate lives outside `managedRoots` (the music folder), only
//     the library entry goes. Nothing outside the library is PodBox's to
//     delete.
//   - Otherwise the file goes to the Trash, and the entry with it. If the
//     Trash refuses, both stay.
HostRemovalResult removeHostDuplicates(
    const std::vector<HostRemoval>& items,
    const std::vector<std::filesystem::path>& managedRoots,
    const TrashFn& trash);

}  // namespace podbox
