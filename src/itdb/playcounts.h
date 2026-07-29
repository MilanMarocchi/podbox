#pragma once

#include "itdb/itunesdb.h"

#include <filesystem>

namespace podbox {

struct PlayCountsMerge {
    bool formatOk = false;
    int entries = 0;
    int applied = 0;  // tracks whose counts/ratings changed
};

// Merges the firmware-written "Play Counts" file into the library. Entries
// are positional — they line up with the track order of the DB the iPod
// booted with — so nothing is applied unless the counts match exactly.
PlayCountsMerge mergePlayCounts(const std::filesystem::path& file,
                                Library& lib);

}  // namespace podbox
