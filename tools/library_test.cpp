// Exercises the Mac-side library (src/library/host_library.cpp).
//
//   library_test              show the current library and its watch folders
//   library_test scan         rescan every watch folder and save
//   library_test add <dir>    add a watch folder
//   library_test health       flag library entries whose file has gone
//   library_test dupes        report duplicates across the whole library
//
// Reads the folders it watches and writes only to
// ~/Library/Application Support/PodBox/. It never touches an iPod, the Apple
// Music library, or the files it indexes.

#include "library/dedupe.h"
#include "library/host_library.h"

#include <chrono>
#include <cstdio>
#include <map>
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
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(n) / (1024.0 * 1024.0));
    return buf;
}

void showSummary(const HostLibrary& lib) {
    std::printf("watch folders:\n");
    if (lib.watchFolders().empty()) std::printf("  (none)\n");
    for (const WatchFolder& w : lib.watchFolders())
        std::printf("  [%s] %s\n", w.enabled ? "on " : "off",
                    w.path.c_str());

    std::uint64_t bytes = 0;
    int missing = 0;
    std::map<std::string, int> byExt;
    for (const HostTrack& t : lib.tracks()) {
        bytes += t.size;
        if (t.missing) ++missing;
        byExt[toLower(t.file.extension().string())]++;
    }
    std::printf("\n%zu tracks, %s", lib.tracks().size(), humanBytes(bytes).c_str());
    if (missing) std::printf(", %d missing", missing);
    std::printf("\n  ");
    for (const auto& [ext, n] : byExt) std::printf("%s:%d  ", ext.c_str(), n);
    std::printf("\n");
}

// Duplicate reporting reuses the device-side grouping by borrowing a Library.
Library asLibrary(const HostLibrary& host) {
    Library lib;
    lib.tracks.reserve(host.tracks().size());
    for (const HostTrack& t : host.tracks()) {
        Track copy = t.meta;
        copy.id = std::uint32_t(t.id);
        copy.dbid = t.id;
        lib.tracks.push_back(std::move(copy));
    }
    return lib;
}

}  // namespace

int main(int argc, char** argv) {
    HostLibrary lib;
    const bool loaded = lib.load();
    std::printf("library: %s%s\n", HostLibrary::libraryPath().c_str(),
                loaded ? "" : "  (new)");
    if (!loaded) lib.seedDefaultWatchFolders();

    const std::string cmd = argc > 1 ? argv[1] : "show";

    if (cmd == "add" && argc > 2) {
        lib.addWatchFolder(argv[2]);
        lib.save();
        std::printf("added watch folder: %s\n", argv[2]);
        showSummary(lib);
        return 0;
    }

    if (cmd == "scan") {
        const auto t0 = std::chrono::steady_clock::now();
        std::string current;
        const ScanStats s = lib.rescan(true, nullptr, &current);
        const double secs = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
        std::printf(
            "scanned in %.1fs: %d added, %d updated, %d unchanged, %d missing, "
            "%d unreadable, %d partial folders skipped\n",
            secs, s.added, s.updated, s.unchanged, s.missing, s.failed,
            s.skippedPartial);
        if (!lib.save()) {
            std::fprintf(stderr, "error: could not save the library\n");
            return 1;
        }
        showSummary(lib);
        return 0;
    }

    if (cmd == "health") {
        const int missing = lib.refreshMissing();
        std::printf("%d missing of %zu tracks\n", missing, lib.tracks().size());
        for (const HostTrack& t : lib.tracks())
            if (t.missing)
                std::printf("   ! %s\n", t.file.c_str());
        lib.save();
        return 0;
    }

    if (cmd == "dupes") {
        const Library asLib = asLibrary(lib);
        FingerprintMap fps;
        for (const HostTrack& t : lib.tracks())
            if (t.fp.ok()) fps[t.id] = t.fp;

        for (const auto mode : {MatchMode::Exact, MatchMode::Loose}) {
            const auto groups = findDuplicates(asLib, mode, fps);
            std::uint64_t copies = 0, bytes = 0;
            int identical = 0;
            for (const auto& g : groups) {
                copies += g.trackIds.size() - 1;
                bytes += g.reclaimBytes;
                if (g.allIdenticalFiles) ++identical;
            }
            std::printf("%-6s %3zu groups, %3llu duplicates, %s, %d byte-identical\n",
                        mode == MatchMode::Exact ? "Exact" : "Loose",
                        groups.size(), (unsigned long long)copies,
                        humanBytes(bytes).c_str(), identical);
        }
        return 0;
    }

    showSummary(lib);
    if (!loaded)
        std::printf("\n(run \"library_test scan\" to index these folders)\n");
    return 0;
}
