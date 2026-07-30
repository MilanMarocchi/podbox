// Reads the Apple Music library and, optionally, copies its files into
// PodBox's own folder.
//
//   applemusic_test                 read and summarise (nothing is written)
//   applemusic_test list <n>        read and print the first n tracks
//   applemusic_test copy <n>        copy n tracks into ~/Music/PodBox
//   applemusic_test copy all        copy the whole library
//
// Apple Music is only ever read from.

#include "library/applemusic.h"
#include "library/fingerprint.h"
#include "library/host_library.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace podbox;

namespace {

std::string humanBytes(std::uint64_t n) {
    char buf[32];
    const double gb = double(n) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0)
        std::snprintf(buf, sizeof(buf), "%.2f GB", gb);
    else
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      double(n) / (1024.0 * 1024.0));
    return buf;
}

void summarise(const AppleMusicRead& r) {
    std::printf("%zu importable tracks\n", r.tracks.size());
    if (r.streamingOnly)
        std::printf("  %d skipped: streaming-only, no file on this Mac\n",
                    r.streamingOnly);
    if (r.fileMissing)
        std::printf("  %d skipped: file listed but not on disk\n",
                    r.fileMissing);
    if (r.drmProtected)
        std::printf("  %d skipped: DRM-protected (.m4p), unplayable on an iPod\n",
                    r.drmProtected);

    std::uint64_t bytes = 0;
    int rated = 0, played = 0;
    for (const auto& t : r.tracks) {
        bytes += t.meta.sizeBytes;
        if (t.meta.rating > 0) ++rated;
        if (t.meta.playCount > 0) ++played;
    }
    std::printf("  %s, %d rated, %d with play counts\n", humanBytes(bytes).c_str(),
                rated, played);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "summary";
    int limit = 0;
    if (argc > 2 && std::strcmp(argv[2], "all") != 0)
        limit = std::atoi(argv[2]);

    if (!appleMusicAvailable()) {
        std::fprintf(stderr, "Music.app is not installed\n");
        return 1;
    }

    AppleMusicRead r = readAppleMusicLibrary(cmd == "summary" ? 0 : limit);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    summarise(r);

    if (cmd == "list") {
        for (const auto& t : r.tracks) {
            std::printf("  %-28.28s %-24.24s %-24.24s %4u kbps %2u/5  %s\n",
                        t.meta.artist.c_str(), t.meta.title.c_str(),
                        t.meta.album.c_str(), t.meta.bitrate,
                        t.meta.rating / 20, t.file.c_str());
        }
        return 0;
    }

    if (cmd == "copy") {
        const auto root = appleMusicCopyRoot();
        std::printf("\ncopying %zu tracks to %s\n", r.tracks.size(),
                    root.c_str());
        const CopyResult res = copyAppleMusicFiles(
            r.tracks, root, [](int done, int total, const std::string& name) {
                if (done % 25 == 0 || done == total - 1)
                    std::printf("  %d/%d  %.40s\n", done, total, name.c_str());
                return true;  // no cancellation from the CLI
            });
        std::printf("copied %d, skipped %d already present, %d failed, %s\n",
                    res.copied, res.skipped, res.failed,
                    humanBytes(res.bytes).c_str());
        if (!r.tracks.empty())
            std::printf("first track now at: %s\n", r.tracks[0].file.c_str());

        // Fold the copies into PodBox's library so they are actually usable.
        HostLibrary lib;
        lib.load();
        int added = 0;
        for (const AppleMusicTrack& t : r.tracks) {
            if (t.file.empty()) continue;
            if (lib.upsert(t.file, t.meta, "applemusic", fingerprintFile(t.file)))
                ++added;
        }
        lib.save();
        std::printf("library: %d added, %zu tracks total\n", added,
                    lib.tracks().size());
        return res.failed ? 1 : 0;
    }

    return 0;
}
