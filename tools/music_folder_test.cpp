// Checks the library's music folder (src/library/music_folder.cpp) and the
// scan that fills it from import folders (HostLibrary::rescan).
//
// Everything happens in a scratch folder with generated WAV files. Nothing
// here touches ~/Music, the real library or any real import folder.

#include "library/host_dedupe.h"
#include "library/host_library.h"
#include "library/music_folder.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace podbox;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void checkEq(long long got, long long want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL  %s: got %lld, want %lld\n", what.c_str(), got, want);
        ++failures;
    }
}

// A short, valid, untagged 16-bit mono WAV. `samples` varies the size so two
// songs can be told apart by it.
void writeWav(const fs::path& p, std::uint32_t samples = 4410) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t data = samples * 2;
    out.write("RIFF", 4); u32(36 + data); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
    out.write("data", 4); u32(data);
    for (std::uint32_t i = 0; i < samples; ++i) u16(std::uint16_t(i * 37));
}

int countAudio(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".wav") ++n;
    return n;
}

const HostTrack* findByTitle(const HostLibrary& lib, const std::string& title) {
    for (const HostTrack& t : lib.tracks())
        if (t.meta.title == title) return &t;
    return nullptr;
}

fs::path scratch() {
    const fs::path root =
        fs::temp_directory_path() /
        ("podbox-musicfolder-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    return root;
}

void testPlacement() {
    std::printf("placement\n");
    const fs::path root = scratch();
    const fs::path lib = root / "PodBox";
    writeWav(root / "src" / "a.wav", 1000);
    writeWav(root / "src" / "b.wav", 2000);

    Track meta;
    meta.artist = "Band";
    meta.album = "Record: Deluxe";
    meta.title = "Song";
    meta.trackNumber = 3;

    fs::path dest;
    bool reused = true;
    std::string error;
    check(placeInMusicFolder(root / "src" / "a.wav", meta, lib, &dest, &reused, &error),
          "a song is copied in");
    check(dest == lib / "Band" / "Record_ Deluxe" / "03 Song.wav",
          "it lands at Artist/Album/NN Title");
    check(!reused, "a first copy is a real copy");
    check(fs::exists(root / "src" / "a.wav"), "the original stays");

    check(placeInMusicFolder(root / "src" / "a.wav", meta, lib, &dest, &reused, &error) &&
              reused && dest.filename() == "03 Song.wav",
          "the same song again reuses its copy");

    check(placeInMusicFolder(root / "src" / "b.wav", meta, lib, &dest, &reused, &error) &&
              !reused && dest.filename() == "03 Song (2).wav",
          "a different file wanting the name gets its own");
    checkEq((long long)fs::file_size(lib / "Band" / "Record_ Deluxe" / "03 Song.wav"),
            (long long)fs::file_size(root / "src" / "a.wav"),
            "and the first copy is never overwritten");

    bool partial = false;
    for (auto& e : fs::recursive_directory_iterator(lib))
        if (e.path().string().find(".podbox-partial") != std::string::npos) partial = true;
    check(!partial, "no temporary files are left behind");
    fs::remove_all(root);
}

void testPrune() {
    std::printf("empty folders\n");
    const fs::path root = scratch();
    fs::create_directories(root / "A" / "Empty");
    fs::create_directories(root / "B" / "Junk");
    std::ofstream(root / "B" / "Junk" / ".DS_Store") << "x";
    writeWav(root / "C" / "Kept" / "song.wav");
    pruneEmptyFolders(root);
    check(!fs::exists(root / "A"), "an emptied artist goes with its album");
    check(!fs::exists(root / "B"), "a folder holding only .DS_Store goes");
    check(fs::exists(root / "C" / "Kept" / "song.wav"), "folders with music stay");
    check(fs::exists(root), "the library folder itself stays");
    fs::remove_all(root);
}

void testImport() {
    std::printf("importing into the library\n");
    const fs::path root = scratch();
    const fs::path lib = root / "PodBox";
    const fs::path inbox = root / "Soulseek" / "complete";
    writeWav(inbox / "user1" / "Album" / "one.wav", 1000);
    writeWav(inbox / "user1" / "Album" / "two.wav", 1100);
    writeWav(inbox / "user2" / "incomplete" / "partial.wav", 1200);
    writeWav(root / "Soulseek" / "downloading" / "busy.wav", 1300);
    writeWav(root / "Legacy" / "old.wav", 1400);
    const auto inboxTime = fs::last_write_time(inbox / "user1" / "Album" / "one.wav");

    HostLibrary host;
    host.setMusicFolder(lib);
    host.addWatchFolder(inbox);
    host.addWatchFolder(root / "Soulseek" / "downloading" / "");
    host.addWatchFolder(root / "Legacy");
    host.setWatchFolderEnabled(2, false);

    // A song indexed in place by an older PodBox, with history worth keeping.
    host.upsert(root / "Legacy" / "old.wav", [] {
        Track t;
        t.title = "old";
        t.playCount = 7;
        t.rating = 80;
        return t;
    }(), "watch");

    ScanStats s = host.rescan(false);
    checkEq(s.imported, 3, "two new songs and the old one are copied in");
    checkEq((long long)host.tracks().size(), 3, "three songs in the library");
    bool allInside = true;
    for (const HostTrack& t : host.tracks())
        if (!isWithinFolder(t.file, lib) || !fs::exists(t.file)) allInside = false;
    check(allInside, "every track now plays from the library folder");
    const HostTrack* old = findByTitle(host, "old");
    check(old && old->meta.playCount == 7 && old->meta.rating == 80,
          "the old song keeps its play count and rating");
    check(!findByTitle(host, "partial") && !findByTitle(host, "busy"),
          "downloads in progress are never imported");

    check(fs::exists(inbox / "user1" / "Album" / "one.wav") &&
              fs::last_write_time(inbox / "user1" / "Album" / "one.wav") == inboxTime &&
              fs::exists(root / "Legacy" / "old.wav"),
          "import folders are left exactly as they were");

    s = host.rescan(false);
    checkEq(s.imported, 0, "a second scan imports nothing new");
    checkEq((long long)host.tracks().size(), 3, "and adds no duplicates");

    // What removing a duplicate does: the copy goes, the entry goes.
    const HostTrack* two = findByTitle(host, "two");
    check(two != nullptr, "the second song is there");
    if (two) {
        const fs::path copy = two->file;
        const std::uint64_t id = two->id;
        fs::remove(copy);
        std::erase_if(host.tracks(), [id](const HostTrack& t) { return t.id == id; });
        s = host.rescan(false);
        check(!findByTitle(host, "two") && !fs::exists(copy),
              "a song removed from the library is not imported again");
        check(fs::exists(inbox / "user1" / "Album" / "two.wav"),
              "while its original stays in the import folder");
    }

    writeWav(inbox / "user3" / "three.wav", 1500);
    s = host.rescan(false);
    checkEq(s.imported, 1, "a newly downloaded song is imported");

    writeWav(lib / "Dropped" / "In" / "direct.wav", 1600);
    s = host.rescan(false);
    checkEq(s.added, 1, "a file put straight into the library is indexed");

    const HostTrack* three = findByTitle(host, "three");
    if (three) fs::remove(three->file);
    s = host.rescan(false);
    three = findByTitle(host, "three");
    check(three && three->missing, "a library file deleted by hand is flagged missing");

    // The library round-trips what it has imported.
    checkEq(countAudio(lib), 3, "the library folder holds exactly its songs");
    fs::remove_all(root);
}

}  // namespace

int main() {
    testPlacement();
    testPrune();
    testImport();
    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
