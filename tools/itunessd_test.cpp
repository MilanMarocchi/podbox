#include "itdb/itunessd.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put16(Bytes& b, std::size_t off, std::uint16_t value) {
    b[off] = std::uint8_t(value);
    b[off + 1] = std::uint8_t(value >> 8);
}

void put32(Bytes& b, std::size_t off, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b[off + i] = std::uint8_t(value >> (8 * i));
}

std::uint32_t get32(const Bytes& b, std::size_t off) {
    return std::uint32_t(b[off]) | (std::uint32_t(b[off + 1]) << 8) |
           (std::uint32_t(b[off + 2]) << 16) |
           (std::uint32_t(b[off + 3]) << 24);
}

void tag(Bytes& b, std::size_t off, const char* value) {
    std::memcpy(b.data() + off, value, 4);
}

Bytes emptyAppleShuffleDb() {
    // Root (64), empty track header (20), modern playlist header (68 + one
    // offset), and the empty master playlist (44).
    Bytes b(64 + 20 + 72 + 44, 0);
    tag(b, 0, "bdhs");
    put32(b, 4, 0x02010001);
    put32(b, 8, 64);
    put32(b, 12, 0);
    put32(b, 16, 1);
    b[29] = 1;
    put32(b, 32, 0);
    put32(b, 36, 64);
    put32(b, 40, 84);

    tag(b, 64, "hths");
    put32(b, 68, 20);
    put32(b, 72, 0);

    tag(b, 84, "hphs");
    put32(b, 88, 72);
    put32(b, 92, 1);
    put32(b, 96, 1);   // one master playlist
    put32(b, 100, 0);  // no ordinary playlists
    for (std::size_t off : {104u, 112u, 120u, 128u})
        put32(b, off, 0xFFFFFFFFu);
    put32(b, 84 + 68, 156);

    tag(b, 156, "lphs");
    put32(b, 160, 44);
    put32(b, 164, 0);
    put32(b, 168, 0);
    put32(b, 180, 1);
    return b;
}

bool writeBytes(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return bool(out.write(reinterpret_cast<const char*>(bytes.data()),
                          std::streamsize(bytes.size())));
}

Bytes readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

podbox::Track track(std::uint32_t id, std::uint64_t dbid,
                    const std::string& file, const std::string& title,
                    const std::string& artist, const std::string& album) {
    podbox::Track t;
    t.id = id;
    t.dbid = dbid;
    t.location = ":iPod_Control:Music:F00:" + file;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.lengthMs = 123456 + id;
    t.trackNumber = id;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4) {
        auto library = podbox::parseItunesDb(argv[1]);
        if (!library.library) {
            std::fprintf(stderr, "iTunesDB: %s\n", library.error.c_str());
            return 1;
        }
        podbox::ItunesSdWriteOptions options;
        options.generateVoiceOver = false;
        std::string error;
        podbox::reconcileShufflePlaylistIds(*library.library, argv[2]);
        if (!podbox::writeItunesSd(*library.library, argv[2], argv[3], {},
                                   &error, options)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const fs::path stats = fs::path(argv[2]).parent_path() / "iTunesStats";
        const fs::path statsOut = std::string(argv[3]) + ".stats";
        if (!podbox::writeShuffleStats(*library.library, argv[2], stats,
                                       statsOut, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const auto generated = podbox::parseItunesSd(argv[3], &error);
        if (!generated) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (generated->tracks.size() != library.library->tracks.size() ||
            generated->playlists.size() != library.library->playlists.size() + 1) {
            std::fprintf(stderr, "generated Shuffle database counts do not match\n");
            return 1;
        }
        for (std::size_t i = 0; i < generated->tracks.size(); ++i) {
            std::string expected = library.library->tracks[i].location;
            std::replace(expected.begin(), expected.end(), ':', '/');
            if (expected.empty() || expected.front() != '/') expected.insert(0, 1, '/');
            if (generated->tracks[i].location != expected ||
                generated->tracks[i].dbid != library.library->tracks[i].dbid) {
                std::fprintf(stderr, "generated track %zu does not match iTunesDB\n",
                             i);
                return 1;
            }
        }
        for (std::size_t i = 0; i < library.library->playlists.size(); ++i) {
            if (generated->playlists[i + 1].dbid !=
                library.library->playlists[i].dbid) {
                std::fprintf(stderr,
                             "generated playlist %zu persistent id does not "
                             "match iTunesDB\n",
                             i);
                return 1;
            }
        }
        std::printf("validated %zu tracks and %zu playlists against real iTunesSD\n",
                    generated->tracks.size(), generated->playlists.size());
        return 0;
    }
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: itunessd_test [<iTunesDB> <existing-iTunesSD> "
                     "<output>]\n");
        return 2;
    }
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };

    const fs::path base =
        fs::temp_directory_path() /
        ("podbox-itunessd-test-" + std::to_string(std::random_device{}()));
    const fs::path itunes = base / "iPod_Control" / "iTunes";
    fs::create_directories(itunes);
    const fs::path existing = itunes / "iTunesSD";
    const fs::path output = itunes / "iTunesSD.out";
    expect(writeBytes(existing, emptyAppleShuffleDb()), "write seed database");
    expect(podbox::detectItunesSd(existing) == podbox::ItunesSdKind::Modern,
           "detect modern Shuffle database");

    podbox::Library lib;
    lib.tracks.push_back(track(1, 0x0123456789ABCDEFull, "AAAA.m4a",
                               "First", "Artist", "Album"));
    lib.tracks.push_back(track(2, 0xFFEEDDCCBBAA9988ull, "BBBB.mp3",
                               "Second", "Artist", "Album"));
    podbox::Playlist playlist;
    playlist.name = "Favourites";
    playlist.dbid = 0x1020304050607080ull;
    playlist.trackIds = {2};
    lib.playlists.push_back(playlist);

    std::string error;
    podbox::ItunesSdWriteOptions options;
    const bool testVoiceOver = std::getenv("PODBOX_TEST_VOICEOVER") != nullptr;
    options.generateVoiceOver = testVoiceOver;
    expect(podbox::writeItunesSd(lib, existing, output, base, &error, options),
           error.empty() ? "write database" : error.c_str());

    const auto parsed = podbox::parseItunesSd(output, &error);
    expect(parsed.has_value(), error.empty() ? "parse output" : error.c_str());
    if (parsed) {
        expect(parsed->version == 0x02010001, "preserve Apple format version");
        expect(parsed->voiceOverEnabled, "enable VoiceOver flag");
        expect(parsed->tracks.size() == 2, "write two tracks");
        expect(parsed->tracks.size() >= 2 &&
                   parsed->tracks[0].location ==
                       "/iPod_Control/Music/F00/AAAA.m4a",
               "write slash-separated track path");
        expect(parsed->tracks.size() >= 2 &&
                   parsed->tracks[0].dbid == 0x0123456789ABCDEFull &&
                   parsed->tracks[1].dbid == 0xFFEEDDCCBBAA9988ull,
               "preserve track persistent ids");
        expect(parsed->playlists.size() == 2, "write master and user playlist");
        expect(parsed->playlists.size() >= 2 &&
                   parsed->playlists[0].type == 1 &&
                   parsed->playlists[0].trackIndices ==
                       std::vector<std::uint32_t>({0, 1}),
               "master playlist contains every track");
        expect(parsed->playlists.size() >= 2 &&
                   parsed->playlists[1].dbid == playlist.dbid &&
                   parsed->playlists[1].trackIndices ==
                       std::vector<std::uint32_t>({1}),
               "user playlist uses persistent id and remapped track index");
    }

    podbox::Library mismatched = lib;
    mismatched.playlists[0].dbid = 7;
    expect(podbox::reconcileShufflePlaylistIds(mismatched, output) == 1 &&
               mismatched.playlists[0].dbid == playlist.dbid,
           "repair playlist id by matching membership");

    const Bytes bytes = readBytes(output);
    const std::uint32_t playlistHeader = get32(bytes, 40);
    expect(playlistHeader < bytes.size() &&
               get32(bytes, playlistHeader + 4) == 68 + 2 * 4,
           "preserve fourth-generation 68-byte playlist-header prefix");
    expect(podbox::shuffleVoiceOverName(0x0123456789ABCDEFull) ==
               "0123456789ABCDEF",
           "VoiceOver filename uses persistent-id hex");

    // Statistics follow track position, so removing/reordering tracks must
    // remap their 32-byte entries by location rather than leave counts on the
    // next song in line.
    Bytes stats(8 + 2 * 32, 0);
    put32(stats, 0, 2);
    put32(stats, 8, 32);
    put32(stats, 12, 111);       // first track bookmark marker
    put32(stats, 8 + 32, 32);
    put32(stats, 8 + 32 + 4, 222);  // second track bookmark marker
    const fs::path oldStats = itunes / "iTunesStats";
    const fs::path newStats = itunes / "iTunesStats.out";
    expect(writeBytes(oldStats, stats), "write seed statistics");
    podbox::Library changed;
    changed.tracks.push_back(lib.tracks[1]);
    changed.tracks.push_back(track(3, 0x8877665544332211ull, "CCCC.m4a",
                                   "Third", "Other", "Other Album"));
    expect(podbox::writeShuffleStats(changed, output, oldStats, newStats,
                                     &error),
           error.empty() ? "write remapped statistics" : error.c_str());
    const Bytes remapped = readBytes(newStats);
    expect(remapped.size() == 8 + 2 * 32 && get32(remapped, 0) == 2,
           "statistics count follows generated track count");
    expect(remapped.size() >= 44 && get32(remapped, 12) == 222,
           "statistics follow retained track after reorder");
    expect(remapped.size() >= 48 && get32(remapped, 8 + 32 + 4) == 0,
           "new track receives empty statistics");
    if (testVoiceOver) {
        expect(fs::exists(base / "iPod_Control" / "Speakable" / "Tracks" /
                          "0123456789ABCDEF.wav"),
               "generate first track VoiceOver WAV");
        expect(fs::exists(base / "iPod_Control" / "Speakable" / "Tracks" /
                          "FFEEDDCCBBAA9988.wav"),
               "generate second track VoiceOver WAV");
        expect(fs::exists(base / "iPod_Control" / "Speakable" / "Playlists" /
                          "1020304050607080.wav"),
               "generate playlist VoiceOver WAV");
    }

    std::error_code ec;
    fs::remove_all(base, ec);
    if (failures) return 1;
    std::printf("iTunesSD tests passed\n");
    return 0;
}
