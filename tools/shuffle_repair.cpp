// Repairs a modern Shuffle's paired firmware databases from its iTunesDB.
// Live files are replaced only after all proposed files parse successfully;
// a matched backup set is retained beside them.

#include "itdb/itunesdb.h"
#include "itdb/itunessd.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::uint32_t statsCount(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 4> b{};
    if (!in.read(reinterpret_cast<char*>(b.data()), b.size())) return 0;
    return b[0] | (std::uint32_t(b[1]) << 8) |
           (std::uint32_t(b[2]) << 16) |
           (std::uint32_t(b[3]) << 24);
}

bool copy(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec) return true;
    if (error) *error = "Could not copy " + from.string() + ": " + ec.message();
    return false;
}

void removeTemps(const std::array<fs::path, 3>& paths) {
    std::error_code ec;
    for (const fs::path& path : paths) fs::remove(path, ec);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shuffle_repair <ipod-mount>\n");
        return 2;
    }
    const fs::path mount = fs::absolute(argv[1]);
    const fs::path itunes = mount / "iPod_Control" / "iTunes";
    const fs::path dbPath = itunes / "iTunesDB";
    const fs::path sdPath = itunes / "iTunesSD";
    const fs::path statsPath = itunes / "iTunesStats";
    if (podbox::detectItunesSd(sdPath) != podbox::ItunesSdKind::Modern) {
        std::fprintf(stderr, "error: %s is not a modern Shuffle database\n",
                     sdPath.c_str());
        return 1;
    }

    auto parsed = podbox::parseItunesDb(dbPath);
    if (!parsed.library) {
        std::fprintf(stderr, "error: %s\n", parsed.error.c_str());
        return 1;
    }
    podbox::Library library = std::move(*parsed.library);
    const auto oldSd = podbox::parseItunesSd(sdPath);
    if (!oldSd) {
        std::fprintf(stderr, "error: could not parse existing iTunesSD\n");
        return 1;
    }
    const int repairedPlaylists =
        podbox::reconcileShufflePlaylistIds(library, sdPath);

    std::unordered_set<std::string> oldLocations;
    for (const auto& track : oldSd->tracks) oldLocations.insert(track.location);

    const std::array<fs::path, 3> tmp = {
        dbPath.string() + ".podbox-repair-tmp",
        sdPath.string() + ".podbox-repair-tmp",
        statsPath.string() + ".podbox-repair-tmp"};
    removeTemps(tmp);

    std::string error;
    if (!podbox::writeItunesDb(library, tmp[0], &error) ||
        !podbox::writeItunesSd(library, sdPath, tmp[1], mount, &error) ||
        !podbox::writeShuffleStats(library, sdPath, statsPath, tmp[2], &error)) {
        removeTemps(tmp);
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const auto checkDb = podbox::parseItunesDb(tmp[0]);
    const auto checkSd = podbox::parseItunesSd(tmp[1], &error);
    if (!checkDb.library || !checkSd ||
        checkDb.library->tracks.size() != library.tracks.size() ||
        checkSd->tracks.size() != library.tracks.size() ||
        statsCount(tmp[2]) != library.tracks.size()) {
        removeTemps(tmp);
        std::fprintf(stderr, "error: staged Shuffle files failed validation\n");
        return 1;
    }

    std::string suffix = ".podbox-shuffle-repair-backup";
    std::error_code ec;
    for (int i = 2; fs::exists(dbPath.string() + suffix, ec) ||
                    fs::exists(sdPath.string() + suffix, ec) ||
                    fs::exists(statsPath.string() + suffix, ec);
         ++i)
        suffix = ".podbox-shuffle-repair-backup." + std::to_string(i);
    const std::array<fs::path, 3> backup = {
        dbPath.string() + suffix, sdPath.string() + suffix,
        statsPath.string() + suffix};
    if (!copy(dbPath, backup[0], &error) || !copy(sdPath, backup[1], &error) ||
        !copy(statsPath, backup[2], &error)) {
        removeTemps(tmp);
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    auto rollback = [&] {
        std::string ignored;
        copy(backup[0], dbPath, &ignored);
        copy(backup[1], sdPath, &ignored);
        copy(backup[2], statsPath, &ignored);
    };
    for (std::size_t i = 0; i < tmp.size(); ++i) {
        fs::rename(tmp[i], std::array<fs::path, 3>{dbPath, sdPath, statsPath}[i],
                   ec);
        if (ec) {
            rollback();
            removeTemps(tmp);
            std::fprintf(stderr, "error: install failed and was rolled back: %s\n",
                         ec.message().c_str());
            return 1;
        }
    }

#ifndef _WIN32
    ::sync();
#endif

    const auto liveDb = podbox::parseItunesDb(dbPath);
    const auto liveSd = podbox::parseItunesSd(sdPath, &error);
    if (!liveDb.library || !liveSd ||
        liveDb.library->tracks.size() != library.tracks.size() ||
        liveSd->tracks.size() != library.tracks.size() ||
        statsCount(statsPath) != library.tracks.size()) {
        rollback();
        std::fprintf(stderr, "error: live verification failed; backup restored\n");
        return 1;
    }

    std::size_t newTracks = 0;
    for (const podbox::Track& track : library.tracks) {
        std::string location = track.location;
        for (char& c : location)
            if (c == ':') c = '/';
        if (location.empty() || location.front() != '/') location.insert(0, 1, '/');
        if (oldLocations.count(location)) continue;
        ++newTracks;
        const fs::path voice = mount / "iPod_Control" / "Speakable" / "Tracks" /
            (podbox::shuffleVoiceOverName(track.dbid) + ".wav");
        if (!fs::exists(voice, ec)) {
            rollback();
            std::fprintf(stderr,
                         "error: missing VoiceOver for %s; backup restored\n",
                         track.title.c_str());
            return 1;
        }
    }

    std::printf("Shuffle repair complete: %zu tracks, %zu playlists, "
                "%zu newly indexed track(s), %d playlist id(s) repaired\n",
                liveSd->tracks.size(), liveSd->playlists.size(), newTracks,
                repairedPlaylists);
    std::printf("Backup suffix: %s\n", suffix.c_str());
    return 0;
}
