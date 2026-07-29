// Prints a summary of an iTunesDB file; parser smoke test.
#include "itdb/itunesdb.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: itdb_dump <iTunesDB>\n");
        return 2;
    }
    auto res = podbox::parseItunesDb(argv[1]);
    if (!res.library) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }
    podbox::Library lib = std::move(*res.library);
    std::printf("version=0x%X hashingScheme=%u tracks=%zu playlists=%zu master=\"%s\"\n",
                lib.version, lib.hashingScheme, lib.tracks.size(),
                lib.playlists.size(), lib.masterName.c_str());
    for (size_t i = 0; i < lib.tracks.size() && i < 5; ++i) {
        const auto& t = lib.tracks[i];
        std::printf("track id=%u \"%s\" — %s — %s [%s] %s plays=%u loc=%s\n",
                    t.id, t.title.c_str(), t.artist.c_str(), t.album.c_str(),
                    t.genre.c_str(), podbox::formatDuration(t.lengthMs).c_str(),
                    t.playCount, t.location.c_str());
    }
    for (const auto& pl : lib.playlists)
        std::printf("playlist \"%s\" (%zu tracks)\n", pl.name.c_str(),
                    pl.trackIds.size());

    // Round-trip test: write the library back out, re-parse, compare.
    if (argc >= 3) {
        // Optional: synthesize a playlist to exercise the playlist writer.
        if (argc >= 4 && std::string(argv[3]) == "+pl" && lib.tracks.size() >= 3) {
            podbox::Playlist pl;
            pl.name = "PodBox Test Playlist";
            pl.trackIds = {lib.tracks[2].id, lib.tracks[0].id,
                           lib.tracks[1].id};
            lib.playlists.push_back(pl);
            std::printf("injected test playlist with 3 tracks\n");
        }
        std::string err;
        if (!podbox::writeItunesDb(lib, argv[2], &err)) {
            std::fprintf(stderr, "write error: %s\n", err.c_str());
            return 1;
        }
        auto res2 = podbox::parseItunesDb(argv[2]);
        if (!res2.library) {
            std::fprintf(stderr, "re-parse error: %s\n", res2.error.c_str());
            return 1;
        }
        const auto& a = lib;
        const auto& b = *res2.library;
        int mismatches = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) {
                std::fprintf(stderr, "MISMATCH: %s\n", what);
                ++mismatches;
            }
        };
        check(a.tracks.size() == b.tracks.size(), "track count");
        check(a.playlists.size() == b.playlists.size(), "playlist count");
        check(a.masterName == b.masterName, "master name");
        for (size_t i = 0; i < a.tracks.size() && i < b.tracks.size(); ++i) {
            const auto& x = a.tracks[i];
            const auto& y = b.tracks[i];
            if (x.id != y.id || x.title != y.title || x.artist != y.artist ||
                x.album != y.album || x.genre != y.genre ||
                x.location != y.location || x.lengthMs != y.lengthMs ||
                x.sizeBytes != y.sizeBytes || x.playCount != y.playCount ||
                x.rating != y.rating || x.trackNumber != y.trackNumber) {
                std::fprintf(stderr, "MISMATCH: track %zu (id=%u \"%s\")\n", i,
                             x.id, x.title.c_str());
                ++mismatches;
            }
        }
        for (size_t i = 0;
             i < a.playlists.size() && i < b.playlists.size(); ++i)
            check(a.playlists[i].name == b.playlists[i].name &&
                      a.playlists[i].trackIds == b.playlists[i].trackIds,
                  "playlist contents");
        std::printf(mismatches ? "ROUNDTRIP FAIL (%d mismatches)\n"
                               : "ROUNDTRIP OK%.0d\n",
                    mismatches);
        return mismatches ? 1 : 0;
    }
    return 0;
}
