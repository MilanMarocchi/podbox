#pragma once

#include "itdb/itunesdb.h"

#include <filesystem>
#include <string>

namespace podbox {

// True for audio formats a classic iPod can play (by extension).
bool isSupportedAudioFile(const std::filesystem::path& path);

struct FileMeta {
    bool ok = false;
    std::string error;
    Track track;  // metadata only; id/location left for the caller
};

// Reads tags and audio properties from a local file via TagLib.
FileMeta readFileMetadata(const std::filesystem::path& path);

}  // namespace podbox
