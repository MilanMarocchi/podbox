// CLI add: copies (or transcodes) audio files onto a mounted iPod and updates
// its iTunesDB. Same pipeline as the GUI drag-and-drop; used for testing.
//   podbox_add <mount> [--alac|--mp3] <file>...
#include "itdb/itunesdb.h"
#include "itdb/itunessd.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include "sync/sync_engine.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: podbox_add <mount-point> [--alac|--mp3] <file>...\n");
        return 2;
    }
    const fs::path mount = argv[1];
    const fs::path dbPath = mount / "iPod_Control" / "iTunes" / "iTunesDB";
    const fs::path sdPath = mount / "iPod_Control" / "iTunes" / "iTunesSD";
    const fs::path statsPath =
        mount / "iPod_Control" / "iTunes" / "iTunesStats";
    const podbox::ItunesSdKind sdKind = podbox::detectItunesSd(sdPath);
    if (sdKind == podbox::ItunesSdKind::Legacy) {
        std::fprintf(stderr,
                     "error: this older iPod shuffle uses an unsupported "
                     "iTunesSD format\n");
        return 1;
    }

    podbox::ImportFormat fmt = podbox::ImportFormat::Original;
    int firstFile = 2;
    if (argc >= 3 && std::strcmp(argv[2], "--alac") == 0) {
        fmt = podbox::ImportFormat::Alac;
        firstFile = 3;
    } else if (argc >= 3 && std::strcmp(argv[2], "--mp3") == 0) {
        fmt = podbox::ImportFormat::Mp3;
        firstFile = 3;
    }

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
    for (int i = firstFile; i < argc; ++i) {
        const fs::path src = argv[i];
        podbox::FileMeta meta = podbox::readFileMetadata(src);
        if (!meta.ok) {
            std::fprintf(stderr, "skip: %s\n", meta.error.c_str());
            continue;
        }
        std::string location;
        const fs::path dest = podbox::allocateMusicPath(
            mount, podbox::importExtension(fmt, src), &location);
        std::string err;
        if (dest.empty() || !podbox::importAudio(fmt, src, dest, &err)) {
            std::fprintf(stderr, "skip: %s\n",
                         err.empty() ? src.filename().string().c_str()
                                     : err.c_str());
            continue;
        }
        std::error_code ec;
        if (const std::uint32_t sz = std::uint32_t(fs::file_size(dest, ec));
            !ec)
            meta.track.sizeBytes = sz;
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
    if (sdKind == podbox::ItunesSdKind::Modern) {
        const fs::path sdBackup = sdPath.string() + ".podbox-backup";
        if (!fs::exists(sdBackup, ec)) fs::copy_file(sdPath, sdBackup, ec);
        const fs::path statsBackup = statsPath.string() + ".podbox-backup";
        if (fs::exists(statsPath, ec) && !fs::exists(statsBackup, ec))
            fs::copy_file(statsPath, statsBackup, ec);
    }
    const fs::path tmp = dbPath.string() + ".podbox-tmp";
    std::string err;
    if (!podbox::writeItunesDb(lib, tmp, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const fs::path sdTmp = sdPath.string() + ".podbox-tmp";
    const fs::path statsTmp = statsPath.string() + ".podbox-tmp";
    if (sdKind == podbox::ItunesSdKind::Modern &&
        (!podbox::writeItunesSd(lib, sdPath, sdTmp, mount, &err) ||
         !podbox::writeShuffleStats(lib, sdPath, statsPath, statsTmp, &err))) {
        fs::remove(tmp, ec);
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    fs::rename(tmp, dbPath, ec);
    if (ec) {
        std::fprintf(stderr, "error: rename failed: %s\n",
                     ec.message().c_str());
        return 1;
    }
    if (sdKind == podbox::ItunesSdKind::Modern) {
        fs::rename(sdTmp, sdPath, ec);
        if (ec) {
            std::fprintf(stderr, "error: Shuffle database rename failed: %s\n",
                         ec.message().c_str());
            return 1;
        }
        fs::rename(statsTmp, statsPath, ec);
        if (ec) {
            std::fprintf(stderr, "error: Shuffle statistics rename failed: %s\n",
                         ec.message().c_str());
            return 1;
        }
    }
    std::printf("database updated: %zu tracks\n", lib.tracks.size());
    return 0;
}
