// Synthetic tests for mounted, folder-based players such as non-Android
// Sony Walkmans. No physical device is needed.

#include "device/filesystem_player.h"
#include "device/media_device.h"
#include "library/metadata.h"
#include "library/transcode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
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

// Silent PCM. The defaults keep fixtures tiny; conversion tests need a
// realistic stereo, 16-bit, hi-res source.
void writeWav(const fs::path& path, std::uint32_t sampleRate = 8000,
              std::uint16_t channels = 1, std::uint16_t bits = 8) {
    const std::uint32_t blockAlign = channels * (bits / 8);
    const std::uint32_t dataSize = sampleRate * blockAlign;  // one second
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    put32(out, 36 + dataSize);
    out.write("WAVEfmt ", 8);
    put32(out, 16);
    put16(out, 1);
    put16(out, channels);
    put32(out, sampleRate);
    put32(out, sampleRate * blockAlign);
    put16(out, std::uint16_t(blockAlign));
    put16(out, bits);
    out.write("data", 4);
    put32(out, dataSize);
    std::array<char, 1024> silence{};
    silence.fill(bits == 8 ? char(0x80) : char(0));
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

    std::printf("FiiO Snowsky Echo profile\n");
    const fs::path echo = mount.parent_path() / "echo-volumes" / "ECHO";
    fs::create_directories(echo, ec);
    std::ofstream(echo / "About FIIO.txt") << "About FIIO\n";
    writeWav(echo / "loose.wav");
    const auto echoDevice = describeFilesystemDevice(echo);
    check(echoDevice.has_value(),
          "an Echo is detected before it has a music folder");
    if (echoDevice) {
        check(echoDevice->modelName == "FiiO Snowsky Echo",
              "the Echo gets its own profile");
        check(echoDevice->musicDirectory == "Music",
              "the Echo defaults to a Music folder");
        check(echoDevice->writable,
              "a missing music folder does not make the Echo read-only");
        check(echoDevice->originalExtensions.count(".flac") == 1,
              "the Echo keeps FLAC unchanged");
        check(echoDevice->originalExtensions.count(".m4b") == 0,
              "the Echo does not receive unrecognised .m4b files");
        check(echoDevice->maxSampleRate == 48000,
              "the Echo converts anything above 48 kHz");
    }
    FilesystemPlayerState echoState;
    const FilesystemPlayerLoad echoLoad =
        loadFilesystemPlayer(echo, "Music", &echoState);
    check(echoLoad.library.has_value() && echoLoad.error.empty(),
          "a missing music folder loads as an empty library");
    if (echoLoad.library)
        check(echoLoad.library->tracks.empty(),
              "audio outside the music folder is not claimed");

    fs::create_directories(echo / "Music", ec);
    writeWav(echo / "Music" / "song.wav");
    std::ofstream(echo / "Music" / "._song.wav") << "AppleDouble";
    writeWav(echo / "Music" / ".podbox-partial.cut off.wav");
    FilesystemPlayerState echoReloadState;
    const FilesystemPlayerLoad echoReload =
        loadFilesystemPlayer(echo, "Music", &echoReloadState);
    check(echoReload.error.empty(),
          "AppleDouble companions are not reported as unreadable");
    if (echoReload.library)
        check(echoReload.library->tracks.size() == 1,
              "AppleDouble companions and partial imports are not indexed");
    std::ofstream(echo / "Music" / "._orphan") << "not a companion";
    if (echoReload.library) {
        check(saveFilesystemPlayer(echo, "Music", *echoReload.library,
                                   &echoReloadState, &error),
              "Echo saves: " + error);
        check(!fs::exists(echo / "Music" / "._song.wav"),
              "saving removes AppleDouble companions from FAT players");
        check(fs::exists(echo / "Music" / "._orphan"),
              "a ._ file without a companion target is left alone");
        check(!fs::exists(echo / "Music" / ".podbox-partial.cut off.wav"),
              "saving removes an import abandoned mid-write");
    }

    std::printf("imports land whole or not at all\n");
    {
        const fs::path source = mount.parent_path() / "import source.wav";
        writeWav(source);
        const fs::path dest = echo / "Music" / "imported.wav";
        std::string importError;
        check(importAudio(ImportFormat::Original, source, dest, &importError,
                          true),
              "an import copies: " + importError);
        check(readAll(dest) == readAll(source), "the import is complete");
        check(!fs::exists(echo / "Music" / ".podbox-partial.imported.wav"),
              "a finished import leaves no partial file");

        const fs::path failedDest = echo / "Music" / "failed.wav";
        check(!importAudio(ImportFormat::Original,
                           mount.parent_path() / "missing.wav", failedDest,
                           &importError, true),
              "an import of a missing file fails");
        check(!fs::exists(failedDest) &&
                  !fs::exists(echo / "Music" / ".podbox-partial.failed.wav"),
              "a failed import leaves nothing behind");
    }

    std::printf("case-insensitive volumes\n");
    const auto echoWithMusic = describeFilesystemDevice(echo);
    check(echoWithMusic && echoWithMusic->musicDirectory == "Music",
          "the music folder is recorded with its on-disk spelling");
    std::ofstream(echo / "._Music") << "AppleDouble";
    FilesystemPlayerState ownedState;
    FilesystemPlayerLoad owned = loadFilesystemPlayer(echo, "Music", &ownedState);
    ownedState.managedTracks.insert("Music/song.wav");
    if (owned.library)
        check(saveFilesystemPlayer(echo, "Music", *owned.library, &ownedState,
                                   &error),
              "owned Echo track saves: " + error);
    check(!fs::exists(echo / "._Music"),
          "the music folder's own AppleDouble companion is removed");
    // The temporary directory is case-insensitive on a default macOS
    // install, like the FAT volumes players use.
    if (fs::is_directory(echo / "MUSIC")) {
        FilesystemPlayerState upperState;
        const FilesystemPlayerLoad upper =
            loadFilesystemPlayer(echo, "MUSIC", &upperState);
        check(upper.managedTrackIds.size() == 1,
              "a differently cased path keeps managed-track authority");
        check(upperState.managedTracks.count("MUSIC/song.wav") == 1,
              "managed paths adopt the scanned spelling");
    }

    std::printf("lossless to AAC\n");
    check(importExtension(ImportFormat::LosslessToAac, "a.mp3", true) ==
              ".mp3",
          "lossy files keep their format");
    check(importExtension(ImportFormat::LosslessToAac, "a.flac", true) ==
              ".m4a",
          "lossless files become AAC");
    const fs::path hiRes = mount.parent_path() / "hires.wav";
    writeWav(hiRes, 96000, 2, 16);
    const fs::path aac = mount.parent_path() / "hires.m4a";
    std::string convertError;
    check(importAudio(ImportFormat::LosslessToAac, hiRes, aac, &convertError,
                      true),
          "hi-res WAV converts to AAC: " + convertError);
    const FileMeta converted = readFileMetadata(aac);
    check(converted.ok && !isLosslessAudioFile(aac),
          "the converted file is lossy AAC");
    check(converted.ok && converted.track.sampleRate <= 48000,
          "the converted file is at most 48 kHz");
    check(isLosslessAudioFile(hiRes), "WAV counts as lossless");

    std::printf("sync size estimates\n");
    Track fourMinutes;
    fourMinutes.lengthMs = 240000;
    fourMinutes.bitrate = 1000;
    fourMinutes.sampleRate = 96000;
    const std::uint64_t flacBytes = 30'000'000;
    const std::uint64_t asAac = estimateImportBytes(
        ImportFormat::LosslessToAac, "a.flac", fourMinutes, flacBytes, true);
    check(asAac > 7'000'000 && asAac < 9'000'000,
          "four minutes of lossless estimates as AAC 256 kbps");
    Track mp3 = fourMinutes;
    mp3.bitrate = 320;
    check(estimateImportBytes(ImportFormat::LosslessToAac, "a.mp3", mp3,
                              9'600'000, true) == 9'600'000,
          "lossy files are counted at their real size");
    Track speech = fourMinutes;
    speech.bitrate = 64;
    check(!losslessToAacConverts("a.wav", speech.bitrate, true),
          "a low-bitrate WAV is not inflated to AAC 256");
    check(estimateImportBytes(ImportFormat::LosslessToAac, "a.wav", speech,
                              1'920'000, true) == 1'920'000,
          "a low-bitrate WAV is counted as copied");
    if (echoDevice) {
        const bool playsHiRes = devicePlaysOriginal(
            echoDevice->originalExtensions, echoDevice->maxSampleRate,
            "a.flac", 96000);
        check(!playsHiRes, "96 kHz FLAC is not copied to the Echo as-is");
        check(estimateImportBytes(ImportFormat::Original, "a.flac",
                                  fourMinutes, flacBytes, playsHiRes) <
                  flacBytes,
              "Keep original counts hi-res files at their converted size");
        check(devicePlaysOriginal(echoDevice->originalExtensions,
                                  echoDevice->maxSampleRate, "A.FLAC", 48000),
              "48 kHz FLAC is copied to the Echo as-is");
    }

    std::printf("import formats per device\n");
    const std::unordered_set<std::string> mp3Only = {".mp3"};
    check(importFormatPlayable(ImportFormat::Original, mp3Only),
          "keeping the original format is always offered");
    check(!importFormatPlayable(ImportFormat::Alac, mp3Only),
          "ALAC is hidden from a player without .m4a");
    check(!importFormatPlayable(ImportFormat::LosslessToAac, mp3Only),
          "AAC is hidden from a player without .m4a");
    if (echoDevice)
        for (ImportFormat format :
             {ImportFormat::Original, ImportFormat::Alac, ImportFormat::Mp3,
              ImportFormat::LosslessToAac})
            check(importFormatPlayable(format, echoDevice->originalExtensions),
                  "every conversion is offered for the Echo");

    fs::remove_all(mount.parent_path(), ec);
    if (failures == 0) std::printf("all filesystem player tests passed\n");
    return failures == 0 ? 0 : 1;
}
