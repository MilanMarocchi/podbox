#pragma once

#include <filesystem>
#include <string>

namespace podbox {

// How dragged-in files should be encoded when copied onto the iPod.
enum class ImportFormat {
    Original,  // copy playable files as-is; convert non-playable (FLAC) to ALAC
    Alac,      // convert everything to Apple Lossless
    Mp3,       // convert everything to MP3 (falls back to AAC without ffmpeg)
};

// The file extension (including the dot) an imported file will end up with
// under `fmt`, given its source path.
std::string importExtension(ImportFormat fmt,
                            const std::filesystem::path& src);

// Copies or transcodes `src` to `dest` (whose extension must equal
// importExtension(fmt, src)). Returns false with a message in `error`.
bool importAudio(ImportFormat fmt, const std::filesystem::path& src,
                 const std::filesystem::path& dest, std::string* error);

// True when an MP3 encoder (ffmpeg or lame) is available on this system.
bool mp3EncoderAvailable();

}  // namespace podbox
