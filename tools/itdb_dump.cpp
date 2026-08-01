// Prints a summary of an iTunesDB file; parser smoke test.
#include "itdb/hash58.h"
#include "itdb/hash72.h"
#include "itdb/itunesdb.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Recomputes the hash58 of a database the device already accepts and compares
// it to the one stored inside. This is the only way to confirm the algorithm
// end to end without writing to an iPod: if the two agree, PodBox's hash is
// what the firmware expects for this device.
int checkHash58(const char* dbPath, const char* guidStr) {
    std::ifstream in(dbPath, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", dbPath);
        return 1;
    }
    std::vector<std::uint8_t> db((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

    const std::vector<std::uint8_t> guid = podbox::parseFirewireGuid(guidStr);
    if (guid.empty()) {
        std::fprintf(stderr,
                     "error: \"%s\" is not a 16-hex-digit FireWire GUID.\n"
                     "       Find it in iPod_Control/Device/SysInfoExtended "
                     "under <key>FireWireGUID</key>.\n",
                     guidStr);
        return 2;
    }

    auto hex = [](const std::vector<std::uint8_t>& v) {
        std::string s;
        char buf[3];
        for (std::uint8_t b : v) {
            std::snprintf(buf, sizeof(buf), "%02x", b);
            s += buf;
        }
        return s;
    };

    // Worth saying plainly: a database signed with a different scheme will
    // never match, however correct the hash58 code is. hashing_scheme is the
    // 16-bit field at 0x30.
    if (db.size() >= 0x32) {
        const std::uint16_t scheme =
            std::uint16_t(db[0x30] | (db[0x31] << 8));
        if (scheme != podbox::kChecksumHash58)
            std::printf(
                "note: this database declares hashing scheme %u, not hash58 "
                "(1).\n      A mismatch below says nothing about hash58.\n\n",
                scheme);
    }

    const std::vector<std::uint8_t> stored = podbox::storedHash58(db);
    const std::vector<std::uint8_t> computed =
        podbox::hash58OfDatabase(db, guid);
    if (computed.empty()) {
        std::fprintf(stderr, "error: not a usable iTunesDB image\n");
        return 1;
    }
    std::printf("stored:   %s\n", hex(stored).c_str());
    std::printf("computed: %s\n", hex(computed).c_str());
    if (stored == computed) {
        std::printf("\nMATCH — hash58 is correct for this device.\n");
        return 0;
    }
    std::printf(
        "\nMISMATCH. Either the GUID does not belong to this database, or\n"
        "PodBox's hash58 is wrong. Do not enable hash58 writes on this iPod.\n");
    return 1;
}

// Same idea for hash72: extract the (IV, random) pair hidden in the signature
// of a database the device already accepts, regenerate the signature with it,
// and compare. The file must be the database exactly as it sits on the device
// — for a nano 5G that is the compressed iTunesCDB, not an inflated copy.
int checkHash72(const char* dbPath, const char* guidStr) {
    std::ifstream in(dbPath, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", dbPath);
        return 1;
    }
    std::vector<std::uint8_t> db((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

    auto hex = [](const std::vector<std::uint8_t>& v) {
        std::string s;
        char buf[3];
        for (std::uint8_t b : v) {
            std::snprintf(buf, sizeof(buf), "%02x", b);
            s += buf;
        }
        return s;
    };

    std::vector<std::uint8_t> digest = db;
    const std::vector<std::uint8_t> sha1 = podbox::hash72Sha1(digest);
    const std::vector<std::uint8_t> stored = podbox::storedHash72(db);
    if (sha1.empty() || stored.empty()) {
        std::fprintf(stderr, "error: not a usable iTunesDB image\n");
        return 1;
    }
    const auto params = podbox::hash72Extract(stored, sha1);
    if (!params) {
        std::fprintf(stderr,
                     "error: the signature at 0x72 is not hash72-shaped for "
                     "this database.\n");
        return 1;
    }
    std::printf("sha1:     %s\n", hex(sha1).c_str());
    std::printf("iv:       %s\n", hex(params->iv).c_str());
    std::printf("rndpart:  %s\n", hex(params->rndpart).c_str());
    if (podbox::hash72Signature(sha1, params->iv, params->rndpart) == stored) {
        std::printf("\nMATCH — hash72 is correct for this device.\n");
        if (guidStr && *guidStr) {
            const std::vector<std::uint8_t> guid =
                podbox::parseFirewireGuid(guidStr);
            if (!guid.empty()) {
                std::printf(
                    "HashInfo for this device would be: HASHv0 + %s (uuid) + "
                    "%s (rndpart) + %s (iv)\n",
                    hex(podbox::hash72Uuid(guid)).c_str(),
                    hex(params->rndpart).c_str(), hex(params->iv).c_str());
            }
        }
        return 0;
    }
    std::printf("\nMISMATCH. The signature does not decrypt to this "
                "database's SHA1.\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--check-hash58") {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: itdb_dump --check-hash58 <iTunesDB> "
                         "<FireWireGUID>\n");
            return 2;
        }
        return checkHash58(argv[2], argv[3]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--check-hash72") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: itdb_dump --check-hash72 <iTunesCDB> "
                         "[<FireWireGUID>]\n");
            return 2;
        }
        return checkHash72(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: itdb_dump <iTunesDB> [<out> [+pl]]\n"
                     "       itdb_dump --check-hash58 <iTunesDB> "
                     "<FireWireGUID>\n"
                     "       itdb_dump --check-hash72 <iTunesCDB> "
                     "[<FireWireGUID>]\n");
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

    // What PodBox does not model but must not destroy.
    {
        std::size_t trackExtra = 0, trackExtraCount = 0, plExtra = 0;
        for (const auto& t : lib.tracks) {
            trackExtra += t.extraMhods.size();
            trackExtraCount += t.extraMhodCount;
        }
        for (const auto& pl : lib.playlists) plExtra += pl.extraMhods.size();
        std::size_t rawHeaders = 0, dsBytes = 0;
        for (const auto& t : lib.tracks) rawHeaders += t.rawHeader.size();
        std::string dsTypes;
        for (const auto& ds : lib.extraDatasets) {
            dsBytes += ds.payload.size();
            dsTypes += " " + std::to_string(ds.type);
        }
        std::printf(
            "preserved: %zu unmodelled track mhods (%zu bytes), %zu bytes of "
            "track headers, %zu bytes of playlist criteria, %zu extra "
            "datasets (types%s, %zu bytes), dbversion 0x%x\n",
            trackExtraCount, trackExtra, rawHeaders, plExtra,
            lib.extraDatasets.size(), dsTypes.empty() ? " none" : dsTypes.c_str(),
            dsBytes, lib.version);
    }

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
