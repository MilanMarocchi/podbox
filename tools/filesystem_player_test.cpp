// Synthetic tests for mounted, folder-based players such as non-Android
// Sony Walkmans. No physical device is needed.

#include "device/filesystem_player.h"
#include "device/media_device.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace podbox;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("  FAIL  %s\n", what.c_str());
    ++failures;
}

void put16(std::ofstream& out, std::uint16_t value) {
    const std::array<char, 2> bytes = {
        char(value & 0xff), char((value >> 8) & 0xff)};
    out.write(bytes.data(), bytes.size());
}

void put32(std::ofstream& out, std::uint32_t value) {
    const std::array<char, 4> bytes = {
        char(value & 0xff), char((value >> 8) & 0xff),
        char((value >> 16) & 0xff), char((value >> 24) & 0xff)};
    out.write(bytes.data(), bytes.size());
}

void writeWav(const fs::path& path) {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint32_t dataSize = sampleRate;  // one second, mono 8-bit
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    put32(out, 36 + dataSize);
    out.write("WAVEfmt ", 8);
    put32(out, 16);
    put16(out, 1);
    put16(out, 1);
    put32(out, sampleRate);
    put32(out, sampleRate);
    put16(out, 1);
    put16(out, 8);
    out.write("data", 4);
    put32(out, dataSize);
    std::array<char, 1024> silence{};
    silence.fill(char(0x80));
    for (std::uint32_t written = 0; written < dataSize;
         written += silence.size())
        out.write(silence.data(),
                  std::min<std::uint32_t>(silence.size(), dataSize - written));
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    const fs::path mount =
        fs::temp_directory_path() /
        ("podbox-fs-player-test-" + std::to_string(::getpid())) /
        "WALKMAN";
    std::error_code ec;
    fs::remove_all(mount.parent_path(), ec);
    fs::create_directories(mount / "MUSIC" / "Artist" / "Album", ec);

    std::printf("device profile\n");
    fs::create_directories(mount.parent_path() / "NOT-A-PLAYER", ec);
    check(!describeFilesystemDevice(mount.parent_path() / "NOT-A-PLAYER"),
          "a volume without a music root is ignored");
    const auto device = describeFilesystemDevice(mount);
    check(device.has_value(), "a WALKMAN volume with MUSIC is detected");
    if (device) {
        check(!device->isIpod(), "the folder device is not classified as iPod");
        check(device->musicDirectory == "MUSIC", "MUSIC root is recorded");
        check(device->modelName.find("Sony Walkman") != std::string::npos,
              "WALKMAN volume gets the Sony profile");
        check(device->originalExtensions.count(".flac") == 1,
              "Sony profile keeps FLAC unchanged");
        check(device->capabilities.playlists,
              "Sony profile advertises playlist support");
        check(!device->capabilities.ratings,
              "filesystem profile does not invent rating support");
    }

    std::printf("multiple device discovery\n");
    fs::create_directories(mount.parent_path() / "DAP" / "Music", ec);
    fs::create_directories(mount.parent_path() / "CLASSIC" / "iPod_Control",
                           ec);
    const std::vector<DeviceInfo> devices =
        findMediaDevicesAt(mount.parent_path());
    check(devices.size() == 3, "all mounted player volumes are returned");
    if (devices.size() == 3) {
        check(devices[0].isIpod(), "iPods sort before folder players");
        check(devices[1].volumeName == "WALKMAN",
              "a named Walkman sorts ahead of a generic DAP");
        check(devices[2].volumeName == "DAP",
              "the generic DAP remains independently visible");
    }

    std::printf("readable allocation\n");
    Track destinationMetadata;
    destinationMetadata.artist = "AC/DC";
    destinationMetadata.album = "A:B";
    destinationMetadata.title = "Song?";
    destinationMetadata.trackNumber = 3;
    std::string allocatedLocation;
    const fs::path allocated = allocateFilesystemMusicPath(
        mount, "MUSIC", destinationMetadata, ".flac", &allocatedLocation);
    check(allocatedLocation == "MUSIC/AC_DC/A_B/03 - Song_.flac",
          "metadata becomes a portable Artist/Album filename");
    check(allocated == mount / allocatedLocation,
          "allocated location is relative to the player mount");

    std::printf("scan, manifest and playlists\n");
    const fs::path song = mount / "MUSIC" / "Artist" / "Album" / "Song.wav";
    writeWav(song);
    fs::create_directories(mount / "MUSIC" / "Playlists", ec);
    const fs::path playlistPath =
        mount / "MUSIC" / "Playlists" / "Favourites.m3u8";
    const std::string originalPlaylist =
        "#EXTM3U\n# a comment PodBox must preserve\n"
        "/MUSIC/Artist/Album/Song.wav\n";
    std::ofstream(playlistPath) << originalPlaylist;

    FilesystemPlayerState state;
    FilesystemPlayerLoad loaded =
        loadFilesystemPlayer(mount, "MUSIC", &state);
    check(loaded.library.has_value(), "filesystem library loads");
    if (!loaded.library) {
        fs::remove_all(mount.parent_path(), ec);
        return 1;
    }
    check(loaded.library->tracks.size() == 1, "one WAV track is scanned");
    check(loaded.library->playlists.size() == 1, "one M3U playlist is scanned");
    check(loaded.library->playlists[0].trackIds.size() == 1,
          "relative M3U entry resolves to the scanned track");
    check(loaded.managedTrackIds.empty(),
          "pre-existing music is not silently claimed by PodBox");

    const Track scanned = loaded.library->tracks[0];
    state.managedTracks.insert(scanned.location);
    std::string error;
    check(saveFilesystemPlayer(mount, "MUSIC", *loaded.library, &state, &error),
          "filesystem state saves: " + error);
    check(readAll(playlistPath) == originalPlaylist,
          "an untouched existing playlist stays byte-for-byte unchanged");

    FilesystemPlayerState reloadedState;
    FilesystemPlayerLoad reloaded =
        loadFilesystemPlayer(mount, "MUSIC", &reloadedState);
    check(reloaded.library && reloaded.library->tracks.size() == 1,
          "saved state reloads with the track");
    check(reloaded.managedTrackIds.size() == 1,
          "managed track authority survives reconnect");
    if (reloaded.library) {
        check(reloaded.library->tracks[0].dbid == scanned.dbid,
              "filesystem track identity is stable across reconnects");
        reloaded.library->playlists[0].name = "Road Trip";
        check(saveFilesystemPlayer(mount, "MUSIC", *reloaded.library,
                                   &reloadedState, &error),
              "renamed playlist saves: " + error);
        check(!fs::exists(playlistPath),
              "renaming removes the old PodBox-tracked playlist path");
        check(fs::exists(mount / "MUSIC" / "Road Trip.m3u8"),
              "renaming writes the new M3U8 playlist");
    }

    std::printf("missing managed file\n");
    fs::remove(song, ec);
    FilesystemPlayerState missingState;
    const FilesystemPlayerLoad missing =
        loadFilesystemPlayer(mount, "MUSIC", &missingState);
    check(missing.library.has_value(), "player still loads after audio loss");
    check(missing.managedTrackIds.empty(),
          "a vanished file no longer grants managed-removal authority");
    check(missingState.managedTracks.empty(),
          "a vanished managed path is pruned for a replacement sync");

    fs::remove_all(mount.parent_path(), ec);
    if (failures == 0) std::printf("all filesystem player tests passed\n");
    return failures == 0 ? 0 : 1;
}
