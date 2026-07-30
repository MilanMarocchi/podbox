#pragma once

#include "itdb/itunesdb.h"

#include <filesystem>

namespace podbox {

struct PlayCountsMerge {
    bool formatOk = false;
    int entries = 0;
    int applied = 0;  // tracks whose counts/ratings changed

    // The file parsed, but its entry count did not line up with the track
    // list, so nothing could be applied. The caller must keep the file: it
    // still holds real listening history, and deleting it destroys counts
    // that a later, matching load could have merged.
    bool mismatched = false;
    int trackCount = 0;  // what the library had, for the message
};

// Merges the firmware-written "Play Counts" file into the library. Entries
// are positional — they line up with the track order of the DB the iPod
// booted with — so nothing is applied unless the counts match exactly.
PlayCountsMerge mergePlayCounts(const std::filesystem::path& file,
                                Library& lib);

}  // namespace podbox
