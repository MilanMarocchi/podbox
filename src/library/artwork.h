#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace podbox {

struct ArtImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;  // width * height * 4
    bool ok() const { return width > 0 && height > 0 && rgba.size() == std::size_t(width) * height * 4; }
};

// MP3/WAV/AIFF APIC, FLAC PICTURE or MP4 covr; prefers a valid front cover.
ArtImage loadEmbeddedArtwork(const std::filesystem::path& audioFile);

// Falls back to cover/folder/front/album JPEG or PNG in the same directory.
ArtImage loadArtwork(const std::filesystem::path& audioFile);
// loadArtwork reduced so its longest edge is at most `maxEdge`, for the
// album grid, which holds hundreds of covers at once.
ArtImage loadArtworkThumbnail(const std::filesystem::path& audioFile, int maxEdge);
// Baseline JPEG, at most 600 pixels on its longest edge, for portable players.
std::vector<unsigned char> playerArtwork(const std::filesystem::path& audioFile);
// No art is a successful no-op. A failure to save available art fails import.
bool copyArtwork(const std::filesystem::path& source,
                 const std::filesystem::path& destination, std::string* error);
// Preserve sidecar-only art when copying audio into the Mac library.
bool copySidecarArtwork(const std::filesystem::path& source,
                       const std::filesystem::path& albumDirectory, std::string* error);

}  // namespace podbox
