// Verifies Play Counts parsing/merge against a DB + Play Counts pair.
// Read-only: merges in memory and reports, never writes anything.
#include "itdb/itunesdb.h"
#include "itdb/playcounts.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: playcounts_test <iTunesDB> <PlayCounts>\n");
        return 2;
    }
    auto res = podbox::parseItunesDb(argv[1]);
    if (!res.library) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }
    podbox::Library lib = std::move(*res.library);

    std::vector<std::uint32_t> before;
    before.reserve(lib.tracks.size());
    for (const auto& t : lib.tracks) before.push_back(t.playCount);

    const auto merge = podbox::mergePlayCounts(argv[2], lib);
    std::printf("formatOk=%d entries=%d tracks=%zu applied=%d\n",
                merge.formatOk, merge.entries, lib.tracks.size(),
                merge.applied);
    int shown = 0;
    for (size_t i = 0; i < lib.tracks.size() && shown < 5; ++i) {
        if (lib.tracks[i].playCount != before[i]) {
            std::printf("  \"%s\": %u -> %u plays\n",
                        lib.tracks[i].title.c_str(), before[i],
                        lib.tracks[i].playCount);
            ++shown;
        }
    }
    return merge.formatOk ? 0 : 1;
}
