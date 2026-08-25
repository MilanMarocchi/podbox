// Dry-runs a sync between the Mac library and a mounted iPod.
//
//   sync_test <mount>            what would be copied
//   sync_test <mount> --remove   also what would be removed
//   sync_test --self-test        synthetic planner assertions
//
// Read-only: it computes the plan and prints it. Nothing is written to the
// device, the library, or your files.

#include "itdb/itunesdb.h"
#include "library/fingerprint_store.h"
#include "library/host_library.h"
#include "sync/sync_plan.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace podbox;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("  FAIL  %s\n", what);
    ++failures;
}

Track track(std::uint32_t id) {
    Track result;
    result.id = id;
    result.dbid = 1000 + id;
    result.artist = "Artist";
    result.title = "Song";
    result.album = "Album";
    result.lengthMs = 180000;
    result.sizeBytes = 1024;
    result.location = ":iPod_Control:Music:F00:AAAA.mp3";
    return result;
}

int selfTest() {
    std::printf("missing device files\n");
    const fs::path mount = fs::temp_directory_path() /
                           "podbox-sync-missing-device-file-test";
    std::error_code ec;
    fs::remove_all(mount, ec);
    fs::create_directories(mount / "iPod_Control" / "Music" / "F00", ec);

    const fs::path source = mount / "source.mp3";
    std::ofstream(source) << "source audio";

    HostLibrary host;
    HostTrack hostTrack;
    hostTrack.id = 42;
    hostTrack.file = source;
    hostTrack.meta = track(9);
    hostTrack.meta.location = source.string();
    hostTrack.size = 12;
    hostTrack.fp = {12, 0x1234};
    host.tracks().push_back(hostTrack);

    Library device;
    device.tracks.push_back(track(7));
    FingerprintStore fingerprints;
    fingerprints.put(device.tracks[0].dbid, hostTrack.fp,
                     FingerprintStore::Origin::Source);

    SyncOptions options;
    SyncPlan plan = planSync(host, device, fingerprints, mount, options);
    check(plan.toCopy.size() == 1 && plan.toCopy[0] == 42,
          "a stale matching entry does not suppress the replacement copy");
    check(plan.missingDeviceFiles.size() == 1 &&
              plan.missingDeviceFiles[0] == 7,
          "the stale database entry is queued for cleanup");
    check(plan.alreadyOnDevice == 0,
          "a song with no device file is not counted as already present");

    std::ofstream(mount / "iPod_Control" / "Music" / "F00" / "AAAA.mp3")
        << "device audio";
    plan = planSync(host, device, fingerprints, mount, options);
    check(plan.toCopy.empty(), "a present device file is not recopied");
    check(plan.missingDeviceFiles.empty(),
          "a present device file is not queued for cleanup");
    check(plan.alreadyOnDevice == 1,
          "a present matching song is counted as already on the device");

    std::printf("managed removal boundary\n");
    Track other = track(8);
    other.artist = "Someone Else";
    other.title = "Device Only";
    other.location = ":iPod_Control:Music:F00:BBBB.mp3";
    device.tracks.push_back(other);
    std::ofstream(mount / "iPod_Control" / "Music" / "F00" / "BBBB.mp3")
        << "other device audio";
    options.removeFromDevice = true;
    std::unordered_set<std::uint32_t> managed = {7};
    plan = planSync(host, device, fingerprints, mount, options, &managed);
    check(plan.toRemove.empty(),
          "mirror sync leaves an unmanaged device-only file alone");
    managed.insert(8);
    plan = planSync(host, device, fingerprints, mount, options, &managed);
    check(plan.toRemove.size() == 1 && plan.toRemove[0] == 8,
          "mirror sync may remove a managed device-only file");

    fs::remove_all(mount, ec);
    if (failures == 0) std::printf("all sync tests passed\n");
    return failures == 0 ? 0 : 1;
}

std::string humanBytes(std::uint64_t n) {
    char buf[32];
    const double gb = double(n) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0)
        std::snprintf(buf, sizeof(buf), "%.2f GB", gb);
    else
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      double(n) / (1024.0 * 1024.0));
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
        return selfTest();
    if (argc < 2) {
        std::fprintf(stderr, "usage: sync_test <ipod-mount> [--remove]\n");
        return 2;
    }
    const fs::path mount = argv[1];
    SyncOptions opt;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--remove") == 0) opt.removeFromDevice = true;

    HostLibrary host;
    if (!host.load()) {
        std::fprintf(stderr, "no Mac library yet — run library_test scan\n");
        return 1;
    }

    auto res = parseItunesDb(mount / "iPod_Control" / "iTunes" / "iTunesDB");
    if (!res.library) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }

    FingerprintStore fps;
    fps.load(mount);

    const SyncPlan plan = planSync(host, *res.library, fps, mount, opt);

    std::printf("Mac library: %zu songs\niPod: %zu songs (%zu fingerprinted)\n\n",
                host.tracks().size(), res.library->tracks.size(), fps.size());
    std::printf("would copy   : %zu songs, %s\n", plan.toCopy.size(),
                humanBytes(plan.bytesToCopy).c_str());
    std::printf("would remove : %zu songs, %s\n", plan.toRemove.size(),
                humanBytes(plan.bytesToFree).c_str());
    std::printf("already there: %d\n", plan.alreadyOnDevice);
    std::printf("dup skipped  : %d\n", plan.skippedDuplicate);
    std::printf("missing      : %d\n", plan.skippedMissing);
    std::printf("device files missing: %zu\n", plan.missingDeviceFiles.size());

    // A few examples, so the numbers are checkable by eye.
    int shown = 0;
    for (std::uint64_t id : plan.toCopy) {
        if (shown++ >= 5) break;
        for (const HostTrack& h : host.tracks())
            if (h.id == id)
                std::printf("   + %.28s — %.32s\n", h.meta.artist.c_str(),
                            h.meta.title.c_str());
    }
    return 0;
}
