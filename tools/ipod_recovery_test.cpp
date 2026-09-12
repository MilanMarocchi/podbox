#include "device/ipod_recovery.h"
#include "sync/sync_plan.h"
#include "sync/sync_engine.h"
#include <cstdio>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace podbox;
namespace {
int failures = 0;
void check(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); ++failures; }
}
std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
void wav(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](unsigned n) { out.put(n & 255); out.put((n >> 8) & 255); };
    auto u32 = [&](unsigned n) { u16(n & 65535); u16(n >> 16); };
    out.write("RIFF", 4); u32(36 + 44100 * 2);
    out.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
    u32(44100); u32(88200); u16(2); u16(16);
    out.write("data", 4); u32(88200);
    out << std::string(88200, '\0');
}
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--scan") {
        auto device = describeIpodMount(argv[2]);
        if (!device) return 1;
        const auto result = scanIpodRecovery(*device);
        std::printf("Read-only recovery scan: %zu songs, %llu bytes, %zu skipped\n%s\n",
            result.library.tracks.size(), static_cast<unsigned long long>(result.musicBytes),
            result.skipped.size(), result.error.c_str());
        for (const auto& skipped : result.skipped) std::puts(skipped.c_str());
        return result.error.empty() ? 0 : 1;
    }
    const fs::path root = fs::temp_directory_path() / ("podbox-recovery-test-" + std::to_string(getpid()));
    const fs::path mount = root / "Milan’s iPod";
    const fs::path dir = mount / "iPod_Control" / "iTunes";
    const fs::path music = mount / "iPod_Control" / "Music" / "F00";
    fs::create_directories(dir);
    fs::create_directories(music);
    IpodInfo device;
    device.mountPoint = mount; device.volumeName = "Milan’s iPod";
    device.usbVendorId = 0x05ac; device.usbProductId = 0x1209;
    std::ofstream(dir / "iTunesControl") << "restore metadata";
    const fs::path song = music / "Déjà vu.wav";
    wav(song);
    const auto originalAudio = bytes(song);
    std::ofstream(music / "broken.mp3") << "not audio";
    std::ofstream(music / "unsupported.flac") << "not copied or removed";
    auto scan = scanIpodRecovery(device);
    check(scan.error.empty() && scan.library.tracks.size() == 1 && scan.skipped.size() == 2,
          "preview indexes real audio and reports invalid/unsupported files");
    check(!fs::exists(dir / "iTunesDB"), "scan never creates the database");
    if (!scan.library.tracks.empty())
        check(scan.library.tracks[0].title == "Déjà vu" &&
              scan.library.tracks[0].location == ":iPod_Control:Music:F00:Déjà vu.wav",
              "Unicode title and existing on-device location survive");
    auto unknown = device; unknown.usbProductId = 0x1261;
    check(!scanIpodRecovery(unknown).error.empty(), "signed model blocked");
    unknown = device; unknown.modelNumber = "MC293";
    check(!scanIpodRecovery(unknown).error.empty(), "conflicting model blocked");
    unknown = device; unknown.writable = false;
    check(!scanIpodRecovery(unknown).error.empty(), "read-only device blocked");
    std::atomic<bool> cancel{true};
    check(!scanIpodRecovery(device, &cancel).error.empty(), "scan can be cancelled");
    for (const char* name : {"iTunesCDB", "iTunesSD", "iTunes Library.itlp"}) {
        std::ofstream(dir / name);
        check(!scanIpodRecovery(device).error.empty(), std::string("companion protected: ") + name);
        fs::remove(dir / name);
    }
    fs::create_symlink(song, music / "link.wav");
    check(!scanIpodRecovery(device).error.empty(), "music symlink rejected");
    fs::remove(music / "link.wav");
    scan = scanIpodRecovery(device);
    std::ofstream(music / "new.mp3") << "another writer";
    std::string error; fs::path archive;
    check(!installIpodRecovery(scan, &archive, &error) && !fs::exists(dir / "iTunesDB"),
          "changed music invalidates preview without installing anything");
    fs::remove(music / "new.mp3");
    scan = scanIpodRecovery(device);
    check(installIpodRecovery(scan, &archive, &error), "missing database recovery: " + error);
    const auto loaded = loadIpodLibrary(device);
    check(loaded.library && loaded.library->tracks.size() == 1, "recovered song survives reconnect");
    check(bytes(song) == originalAudio && bytes(dir / "iTunesControl") == "restore metadata",
          "audio and restore metadata unchanged");
    check(fs::exists(dir / ("iTunesDB.podbox-recovered." + archive.filename().string().substr(16))), "first recovered database has a backup");
    check(!scanIpodRecovery(device).error.empty(), "healthy database is never overwritten by rebuilding");
    if (loaded.library) {
        HostLibrary host;
        host.upsert(song, scan.library.tracks[0], "test");
        FingerprintStore fp;
        const auto plan = planSync(host, *loaded.library, fp, mount, {});
        check(plan.toCopy.empty() && plan.alreadyOnDevice == 1, "next sync recognizes recovered song");
    }
    std::ofstream(dir / "iTunesDB", std::ios::trunc) << "damaged database";
    std::ofstream(dir / "Play Counts") << "original history";
    scan = scanIpodRecovery(device);
    check(installIpodRecovery(scan, &archive, &error), "damaged database recovery: " + error);
    check(bytes(archive / "iTunesDB.original") == "damaged database", "damaged original archived exactly");
    check(bytes(archive / "Play Counts") == "original history" && !fs::exists(dir / "Play Counts"),
          "old positional play counts are preserved without misassigning them");
    check(bytes(song) == originalAudio && fs::exists(music / "broken.mp3"), "recovery never alters audio, including skipped files");
    // Database close/flush failures must not be reported as success.
    if (fs::exists("/dev/full")) {
        Library empty;
        check(!writeItunesDb(empty, "/dev/full", &error), "buffered write failure reported");
    }
    // The shutdown barrier must retain completed metadata for the UI to save.
    SyncEngine engine;
    ImportTarget target;
    target.mount = mount; target.layout = DeviceMusicLayout::IpodFolders;
    target.originalExtensions = {".wav"};
    engine.queueAdds({song}, target, ImportFormat::Original);
    while (engine.batchDone() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    engine.stopAndWait();
    const auto completed = engine.takeCompleted();
    check(!engine.busy() && completed.size() == 1 && completed[0].error.empty(),
          "shutdown retains completed copy metadata and becomes idle");
    fs::remove_all(root);
    if (!failures) std::puts("All iPod recovery tests passed");
    return failures ? 1 : 0;
}
