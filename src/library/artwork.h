#pragma once

#include <filesystem>
#include <vector>

namespace podbox {

struct ArtImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;  // width * height * 4
    bool ok() const { return width > 0 && height > 0; }
};

// Extracts and decodes cover art embedded in an audio file (ID3v2 APIC for
// MP3, 'covr' atom for MP4/AAC). Returns an empty image when there is none.
ArtImage loadEmbeddedArtwork(const std::filesystem::path& audioFile);

}  // namespace podbox
