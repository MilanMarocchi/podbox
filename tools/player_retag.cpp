// Rewrites the tags of every MP3 on a folder player in the player-safe form
// PodBox now writes (ID3v2.3, UTF-16, displayed fields only), in place and
// without touching the audio. For libraries synced before that, whose
// ffmpeg-written ID3v2.4 tags crash the Snowsky Echo's firmware.

#include "device/filesystem_player.h"
#include "device/media_device.h"
#include "library/metadata.h"
#include "library/transcode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace podbox;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <mount-point>\n", argv[0]);
        return 2;
    }
    const auto device = describeFilesystemDevice(argv[1]);
    if (!device) {
        std::fprintf(stderr, "%s is not a folder-based player\n", argv[1]);
        return 1;
    }
    if (!device->writable) {
        std::fprintf(stderr, "%s is read-only\n", argv[1]);
        return 1;
    }

    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             device->mountPoint / device->musicDirectory, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file(ec) || path.filename().string().rfind("._", 0) == 0 ||
            isPartialImport(path))
            continue;
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".mp3") files.push_back(path);
    }
    if (ec) {
        std::fprintf(stderr, "could not scan the music folder: %s\n",
                     ec.message().c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());

    int failed = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string error;
        if (!writePlayerSafeMp3Tags(files[i], files[i], &error)) {
            ++failed;
            std::printf("FAILED %s\n", error.c_str());
        }
        if ((i + 1) % 100 == 0 || i + 1 == files.size())
            std::printf("%zu/%zu retagged\n", i + 1, files.size());
        std::fflush(stdout);
    }

    // Writing the tags makes macOS leave "._" companions on FAT and exFAT; a
    // save of the player sweeps them.
    FilesystemPlayerState state;
    const FilesystemPlayerLoad load =
        loadFilesystemPlayer(device->mountPoint, device->musicDirectory, &state);
    std::string error;
    if (!load.library ||
        !saveFilesystemPlayer(device->mountPoint, device->musicDirectory,
                              *load.library, &state, &error)) {
        std::fprintf(stderr, "retagged, but could not clean up: %s\n",
                     load.library ? error.c_str() : load.error.c_str());
        return 1;
    }
    std::printf("done: %zu MP3s, %d failed\n", files.size(), failed);
    return failed == 0 ? 0 : 1;
}
