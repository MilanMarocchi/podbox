// CLI add: copies audio files onto a mounted iPod and updates its iTunesDB.
// Same pipeline as the GUI drag-and-drop; used for headless testing.
#include "itdb/itunesdb.h"
#include "library/metadata.h"
#include "sync/sync_engine.h"

#include <cstdio>
#include <random>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: podbox_add <mount-point> <file>...\n");
        return 2;
    }
    const fs::path mount = argv[1];
    const fs::path dbPath = mount / "iPod_Control" / "iTunes" / "iTunesDB";

    auto res = podbox::parseItunesDb(dbPath);
    if (!res.library) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }
    podbox::Library lib = std::move(*res.library);
    if (lib.hashingScheme != 0) {
        std::fprintf(stderr,
                     "error: this iPod requires a hashed database "
                     "(scheme %u); writes not yet supported\n",
                     lib.hashingScheme);
        return 1;
    }

    std::uint32_t nextId = 100;
    for (const auto& t : lib.tracks) nextId = std::max(nextId, t.id + 1);
    std::mt19937_64 rng{std::random_device{}()};

    int added = 0;
    for (int i = 2; i < argc; ++i) {
        const fs::path src = argv[i];
        podbox::FileMeta meta = podbox::readFileMetadata(src);
        if (!meta.ok) {
            std::fprintf(stderr, "skip: %s\n", meta.error.c_str());
            continue;
        }
        std::string location;
        const fs::path dest = podbox::allocateMusicPath(
            mount, src.extension().string(), &location);
        std::error_code ec;
        if (dest.empty() || !fs::copy_file(src, dest, ec) || ec) {
            std::fprintf(stderr, "skip: %s: copy failed\n",
                         src.filename().string().c_str());
            continue;
        }
        meta.track.id = nextId++;
        meta.track.dbid = rng();
        meta.track.location = location;
        lib.tracks.push_back(std::move(meta.track));
        std::printf("added \"%s\" -> %s\n",
                    lib.tracks.back().title.c_str(), location.c_str());
        ++added;
    }
    if (!added) {
        std::fprintf(stderr, "nothing added\n");
        return 1;
    }

    // Back up the original DB once, then write atomically.
    const fs::path backup = dbPath.string() + ".podbox-backup";
    std::error_code ec;
    if (!fs::exists(backup, ec)) fs::copy_file(dbPath, backup, ec);
    const fs::path tmp = dbPath.string() + ".podbox-tmp";
    std::string err;
    if (!podbox::writeItunesDb(lib, tmp, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    fs::rename(tmp, dbPath, ec);
    if (ec) {
        std::fprintf(stderr, "error: rename failed: %s\n",
                     ec.message().c_str());
        return 1;
    }
    std::printf("database updated: %zu tracks\n", lib.tracks.size());
    return 0;
}
