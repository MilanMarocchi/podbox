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
#include <sstream>
#include <stdexcept>
#include <future>

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

    writeWav(root / "src" / "same-size.wav", 1000);
    {
        std::fstream changed(root / "src" / "same-size.wav", std::ios::in | std::ios::out | std::ios::binary);
        changed.seekp(-1, std::ios::end);
        changed.put('\x55');
    }
    fs::path collision;
    check(placeInMusicFolder(root / "src" / "same-size.wav", meta, lib, &collision, &reused, &error) &&
              !reused && collision.filename() == "03 Song (2).wav",
          "equal-size files with different audio are both preserved");

    check(placeInMusicFolder(root / "src" / "a.wav", meta, lib, &dest, &reused, &error) &&
              reused && dest.filename() == "03 Song.wav",
          "the same song again reuses its copy");

    check(placeInMusicFolder(root / "src" / "b.wav", meta, lib, &dest, &reused, &error) &&
              !reused && dest.filename() == "03 Song (3).wav",
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

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out; out << in.rdbuf(); return out.str();
}

void testPersistence() {
    std::printf("safe index persistence\n");
    const auto root = scratch();
    const auto index = root / "config" / "library.tsv";
    HostLibrary host;
    check(!host.load(index), "absent index is a first run");
    Track song; song.title = "Song\twith\\escapes\nand a newline"; song.playCount = 8;
    writeWav(root / "song.wav");
    host.upsert(root / "song.wav", song, "import");
    check(host.save(index), "save a new isolated library");
    const std::string first = readAll(index);
    HostLibrary stale;
    check(stale.load(index), "a second app loads the same revision");
    auto snapshot = host;
    host.tracks()[0].meta.playCount = 9;
    check(host.save(index), "save a new revision");
    check(readAll(index.string() + ".bak.1") == first, "previous index backed up intact");
    snapshot.tracks()[0].meta.playCount = 10;
    check(snapshot.save(index), "serialized worker snapshots share the saved revision");
    const std::string latest = readAll(index);
    check(!stale.save(index) && readAll(index) == latest, "a competing app cannot overwrite newer changes");
    HostLibrary fresh;
    check(!fresh.save(index), "an uninitialized library cannot replace an existing index");
    for (int i=0;i<7;++i) { host.tracks()[0].meta.playCount=20+i; check(host.save(index), "rotate saved revisions"); }
    for (int i=1;i<=5;++i) {
        HostLibrary backup;
        check(backup.load(index.string()+".bak."+std::to_string(i)), "each rolling backup is readable");
    }
    HostLibrary loaded;
    check(loaded.load(index) && loaded.tracks().size()==1 && loaded.tracks()[0].meta.title==song.title,
          "escaped paths and metadata round-trip");
    const auto original = readAll(index);
    auto damaged = original;
    const auto titleAt = damaged.find("Song");
    if (titleAt != std::string::npos) damaged[titleAt] = 'X';
    std::ofstream(index, std::ios::binary) << damaged;
    bool checksumRejected=false;
    try { HostLibrary broken; broken.load(index); } catch (const std::runtime_error&) { checksumRejected=true; }
    check(checksumRejected, "checksum detects a changed field even when row counts still match");
    const auto cut = original.rfind("C\t");
    std::ofstream(index, std::ios::binary) << original.substr(0,cut);
    bool rejected=false;
    try { loaded.load(index); } catch (const std::runtime_error&) { rejected=true; }
    check(rejected && loaded.tracks().size()==1, "truncation is rejected without clearing the live library");
    check(!loaded.save(index), "a damaged index is not overwritten after failed load");
    std::ofstream(index) << "podbox-library 1\nT\tbroken\n";
    rejected=false;
    try { loaded.load(index); } catch (const std::runtime_error&) { rejected=true; }
    check(rejected, "a malformed legacy row cannot silently disappear");
    fs::remove_all(root);
}

void testRecoveryAndCase() {
    std::printf("missing originals and filesystem identity\n");
    const auto root=scratch(), inbox=root/"complete", music=root/"PodBox";
    writeWav(inbox/"song.wav");
    HostLibrary host; host.setMusicFolder(music);host.addWatchFolder(inbox);
    host.rescan(true);
    check(host.tracks().size()==1, "fixture imported");
    if (host.tracks().empty()) { fs::remove_all(root); return; }
    const auto id=host.tracks()[0].id;
    host.tracks()[0].meta.playCount=42;
    host.tracks()[0].meta.rating=80;
    fs::remove(host.tracks()[0].file);
    auto result=host.rescan(true);
    check(result.restored==1 && host.tracks().size()==1 && host.tracks()[0].id==id &&
          host.tracks()[0].meta.playCount==42 && host.tracks()[0].meta.rating==80 && !host.tracks()[0].missing &&
          fs::exists(host.tracks()[0].file), "rescan restores a missing song from its original and preserves history");
    auto actual=host.tracks()[0].file;
    auto alias=actual.parent_path()/"SONG.WAV";
    if (fs::exists(alias)) {
        host.tracks()[0].file=alias;
        result=host.rescan(true);
        check(host.tracks().size()==1 && host.tracks()[0].id==id && !host.tracks()[0].missing &&
              host.tracks()[0].meta.playCount==42, "case aliases keep the same id and play history after a scan");
        host.upsert(alias,host.tracks()[0].meta,"import");
        check(host.tracks().size()==1,"upserting a case alias does not invent another track");
    }
    host.tracks()[0].missing=true;
    check(host.removeMissing()==0,"stale missing flags cannot unlist a returned file");
    fs::remove(actual);
    host.tracks().clear();
    result=host.rescan(true);
    check(host.tracks().empty() && result.restored==0,"deliberately removed entries are not resurrected");
    fs::remove_all(root);
}

void testConcurrentPlacement() {
    const auto root=scratch();
    writeWav(root/"a.wav", 100000);
    writeWav(root/"b.wav", 100000);
    { std::fstream out(root/"b.wav",std::ios::in|std::ios::out|std::ios::binary);out.seekp(-1,std::ios::end);out.put('\x55'); }
    Track meta;meta.artist="A";meta.album="B";meta.title="Same name";
    std::promise<void> ready;auto gate=ready.get_future().share();
    auto copy=[&](const fs::path& src) {
        gate.wait();fs::path dest;bool reused=false;std::string error;
        const bool ok=placeInMusicFolder(src,meta,root/"library",&dest,&reused,&error);
        return std::pair(ok,dest);
    };
    auto a=std::async(std::launch::async,copy,root/"a.wav");
    auto b=std::async(std::launch::async,copy,root/"b.wav");
    ready.set_value();const auto first=a.get(),second=b.get();
    check(first.first&&second.first&&first.second!=second.second,
          "concurrent imports with a shared name both get their own file");
    if(first.first&&second.first)
        check(readAll(first.second)==readAll(root/"a.wav")&&readAll(second.second)==readAll(root/"b.wav"),
              "a concurrent importer cannot overwrite the other song");
    fs::remove_all(root);
}

}  // namespace

int main() {
    testPlacement();
    testPrune();
    testImport();
    testPersistence();
    testRecoveryAndCase();
    testConcurrentPlacement();
    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
