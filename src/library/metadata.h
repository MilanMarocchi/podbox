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

// How a file should be filed on the iPod, from its extension and genre.
// Separate from readFileMetadata so it can be tested without an audio file,
// and so Get Info can re-derive it after a genre edit.
//
// .m4b is the only unambiguous signal a file carries — it is the extension
// Apple defined for audiobooks. Genre is a fallback because it is what
// podcast and audiobook publishers actually fill in, and it is a guess.
std::uint32_t classifyMediaType(const std::filesystem::path& path,
                                const std::string& genre);

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
