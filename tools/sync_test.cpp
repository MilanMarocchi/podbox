// Dry-runs a sync between the Mac library and a mounted iPod.
//
//   sync_test <mount>            what would be copied
//   sync_test <mount> --remove   also what would be removed
//
// Read-only: it computes the plan and prints it. Nothing is written to the
// device, the library, or your files.

#include "itdb/itunesdb.h"
#include "library/fingerprint_store.h"
#include "library/host_library.h"
#include "sync/sync_plan.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace fs = std::filesystem;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sync_test <ipod-mount> [--remove]\n");
        return 2;
    }
    const fs::path mount = argv[1];
    SyncOptions opt;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--remove") == 0) opt.removeFromDevice = true;

    HostLibrary host;
    if (!host.load()) {
        std::fprintf(stderr, "no Mac library yet — run library_test scan\n");
        return 1;
    }

    auto res = parseItunesDb(mount / "iPod_Control" / "iTunes" / "iTunesDB");
    if (!res.library) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }

    FingerprintStore fps;
    fps.load(mount);

    const SyncPlan plan = planSync(host, *res.library, fps, opt);

    std::printf("Mac library: %zu songs\niPod: %zu songs (%zu fingerprinted)\n\n",
                host.tracks().size(), res.library->tracks.size(), fps.size());
    std::printf("would copy   : %zu songs, %s\n", plan.toCopy.size(),
                humanBytes(plan.bytesToCopy).c_str());
    std::printf("would remove : %zu songs, %s\n", plan.toRemove.size(),
                humanBytes(plan.bytesToFree).c_str());
    std::printf("already there: %d\n", plan.alreadyOnDevice);
    std::printf("dup skipped  : %d\n", plan.skippedDuplicate);
    std::printf("missing      : %d\n", plan.skippedMissing);

    // A few examples, so the numbers are checkable by eye.
    int shown = 0;
    for (std::uint64_t id : plan.toCopy) {
        if (shown++ >= 5) break;
        for (const HostTrack& h : host.tracks())
            if (h.id == id)
                std::printf("   + %.28s — %.32s\n", h.meta.artist.c_str(),
                            h.meta.title.c_str());
    }
    return 0;
}
