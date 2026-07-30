#pragma once

#include "itdb/itunesdb.h"

#include <filesystem>
#include <string>

namespace podbox {

// True for audio formats a classic iPod can play directly (by extension).
bool isSupportedAudioFile(const std::filesystem::path& path);

// True for anything PodBox can import: iPod-playable formats plus those it can
// transcode to a playable one (FLAC).
bool isImportableAudioFile(const std::filesystem::path& path);

struct FileMeta {
    bool ok = false;
    std::string error;
    Track track;  // metadata only; id/location left for the caller
};

// Reads tags and audio properties from a local file via TagLib.
FileMeta readFileMetadata(const std::filesystem::path& path);

// Writes title/artist/album/genre/year/track number back into the file's
// tags. Only for files PodBox owns a copy of — it rewrites the user's file,
// so callers must be sure that is what was asked for. Returns false and sets
// `error` on failure.
bool writeFileTags(const std::filesystem::path& path, const Track& meta,
                   std::string* error);

}  // namespace podbox
