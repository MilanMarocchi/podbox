#include "device/ipod_device.h"
#include "sync/sync_engine.h"
#include "sync/sync_plan.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace podbox;

namespace {
int failures = 0;
void check(bool ok, const std::string& message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
}

int main(int argc, char** argv) {
    // Optional read-only probe uses the same discovery and loading as the GUI.
    if (argc == 3 && std::string(argv[1]) == "--inspect") {
        const auto device = describeIpodMount(argv[2]);
        if (!device) return 1;
        const auto loaded = loadIpodLibrary(*device);
        std::printf("%s; %s; USB %04x:%04x; writable=%d\n",
                    device->modelName.c_str(), device->filesystem.c_str(),
                    device->usbVendorId, device->usbProductId, device->writable);
        if (!loaded.library) {
            std::fprintf(stderr, "%s\n", loaded.error.c_str());
            return 1;
        }
        std::printf("Library ready: %zu tracks, checksum scheme %u\n",
                    loaded.library->tracks.size(), loaded.library->hashingScheme);
        return 0;
    }

    const fs::path root = fs::temp_directory_path() /
        ("podbox-restored-ipod-test-" + std::to_string(::getpid()));
    const fs::path mount = root / "Milan’s iPod";
    const fs::path dir = mount / "iPod_Control" / "iTunes";
    const fs::path music = mount / "iPod_Control" / "Music";
    fs::create_directories(dir);
    fs::create_directories(music / "F00");
    fs::create_directories(mount / "iPod_Control" / "Device");
    std::ofstream(mount / "iPod_Control" / "Device" / "SysInfo");
    std::ofstream(dir / "iTunesControl") << "restore metadata";
    auto device = *describeIpodMount(mount);
    check(!loadIpodLibrary(device).library,
          "empty SysInfo alone does not authorize an unsigned database");
    device.usbVendorId = 0x05ac;
    device.usbProductId = 0x1209;
    for (const char* format : {"HFS+ (Mac format)", "FAT32 (Windows format)"}) {
        device.filesystem = format;
        const auto loaded = loadIpodLibrary(device);
        check(loaded.library && loaded.library->tracks.empty() &&
                  loaded.library->hashingScheme == kChecksumNone &&
                  loaded.library->masterName == "Milan’s iPod",
              std::string("fresh video library loads on ") + format);
    }
    check(!fs::exists(dir / "iTunesDB"), "discovery/loading never writes a database");
    device.usbVendorId = 0x1234;
    check(!loadIpodLibrary(device).library, "non-Apple USB ID is not accepted");
    device.usbVendorId = 0x05ac;
    device.usbProductId = 0x1261;
    check(!loadIpodLibrary(device).library, "restored signed models require a seed database");
    device.usbProductId = 0x1209;
    device.modelNumber = "MC293";
    check(!loadIpodLibrary(device).library, "conflicting known model fails closed");
    device.modelNumber = "MA448LL";
    device.usbVendorId = device.usbProductId = 0;
    check(loadIpodLibrary(device).library.has_value(), "SysInfo video model works without USB");
    device.writable = false;
    check(!loadIpodLibrary(device).library, "read-only restored iPod stays blocked");
    device.writable = true;

    const fs::path existingSong = music / "F00" / "existing.mp3";
    std::ofstream(existingSong) << "existing music";
    check(!loadIpodLibrary(device).library, "orphaned music is not treated as a restore");
    fs::remove(existingSong);
    fs::create_symlink(root / "missing.mp3", existingSong);
    check(!loadIpodLibrary(device).library, "dangling song symlink blocks initialization");
    fs::remove(existingSong);
    for (const char* file : {"iTunesDB", "iTunesCDB", "iTunesSD",
                             "iTunesDB.podbox-backup"}) {
        std::ofstream(dir / file);
        check(!loadIpodLibrary(device).library,
              std::string("existing empty database/companion is protected: ") + file);
        fs::remove(dir / file);
    }
    std::ofstream(dir / "iTunesDB") << "damaged database";
    check(!loadIpodLibrary(device).library, "corrupt database is not replaced");
    fs::remove(dir / "iTunesDB");
    fs::create_symlink(root / "missing-db", dir / "iTunesDB");
    check(!loadIpodLibrary(device).library, "dangling database symlink is not replaced");
    fs::remove(dir / "iTunesDB");
    fs::create_directory(dir / "iTunes Library.itlp");
    check(!loadIpodLibrary(device).library, "SQLite companion directory blocks initialization");
    fs::remove(dir / "iTunes Library.itlp");

    // Exercise the first write, Unicode metadata, playlist and reconnect.
    auto loaded = loadIpodLibrary(device);
    check(loaded.library.has_value(), "restored device is ready for its first import");
    if (loaded.library) {
        Track track;
        track.id = 100;
        track.title = "Déjà vu 🎵";
        track.artist = "Björk";
        const fs::path source = root / "source.mp3";
        std::ofstream(source) << "fixture audio";
        HostLibrary host;
        host.upsert(source, track, "test");
        FingerprintStore fingerprints;
        const auto firstSync = planSync(host, *loaded.library, fingerprints, mount, {});
        check(firstSync.toCopy.size() == 1 && firstSync.toRemove.empty(),
              "first sync queues host music for the restored empty library");
        const fs::path destination = allocateMusicPath(mount, ".mp3", &track.location);
        fs::create_directories(destination.parent_path());
        std::ofstream(destination) << "fixture audio";
        loaded.library->tracks.push_back(track);
        Playlist playlist;
        playlist.name = "Favourites";
        playlist.trackIds = {track.id};
        loaded.library->playlists.push_back(playlist);
        std::string error;
        check(writeItunesDb(*loaded.library, dir / "iTunesDB", &error),
              "first database write: " + error);
        const auto reloaded = loadIpodLibrary(device);
        check(reloaded.library && reloaded.library->tracks.size() == 1 &&
                  reloaded.library->tracks[0].title == track.title &&
                  reloaded.library->tracks[0].artist == track.artist &&
                  reloaded.library->playlists.size() == 1 &&
                  reloaded.library->playlists[0].trackIds == playlist.trackIds,
              "first import survives database write and reconnect");
        if (reloaded.library) {
            const auto nextSync = planSync(host, *reloaded.library, fingerprints, mount, {});
            check(nextSync.missingDeviceFiles.empty() && nextSync.toCopy.empty() &&
                      nextSync.alreadyOnDevice == 1,
                  "reconnect finds the copied song on a Unicode volume without recopying");
        }
        // A valid existing compressed database must keep taking precedence.
        WriteOptions compressed;
        compressed.compressed = true;
        check(writeItunesDb(*loaded.library, dir / "iTunesCDB", &error, compressed),
              "compressed fixture writes");
        std::ofstream(dir / "iTunesDB", std::ios::trunc);
        const auto cdb = loadIpodLibrary(device);
        check(cdb.library && cdb.library->compressed && cdb.library->tracks.size() == 1,
              "compressed library wins over a zero-byte iTunesDB");
    }
    std::string restoreMetadata;
    std::getline(std::ifstream(dir / "iTunesControl"), restoreMetadata);
    check(restoreMetadata == "restore metadata", "restore metadata stays untouched");
    fs::remove_all(root);
    if (!failures) std::puts("All restored iPod tests passed");
    return failures ? 1 : 0;
}
