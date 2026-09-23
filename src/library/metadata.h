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

// True when the audio is stored losslessly: FLAC, WAV, AIFF, or ALAC in an
// MP4 container. Lossy files are never worth re-encoding to save space.
bool isLosslessAudioFile(const std::filesystem::path& path);

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

// Replaces the tags of the MP3 at `dest` with the ones players display
// (title, artists, album, track, disc, year, genre) read from `src`, which may
// be the same file or any format TagLib reads. Written as ID3v2.3 with UTF-16
// text and nothing else: budget player firmware (the Snowsky Echo line) has no
// UTF-8 decoder, only reads the first few KB of a tag, and crashes on frames
// it does not expect. Drops artwork. An existing tag is overwritten in place
// when the new one fits, so this is cheap on a slow device.
bool writePlayerSafeMp3Tags(const std::filesystem::path& src,
                            const std::filesystem::path& dest,
                            std::string* error);

}  // namespace podbox
