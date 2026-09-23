#pragma once

#include "itdb/itunesdb.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace podbox {

// How dragged-in files should be encoded when copied onto the iPod.
enum class ImportFormat {
    Original,  // copy playable files as-is; convert non-playable (FLAC) to ALAC
    Alac,      // convert everything to Apple Lossless
    Mp3,       // convert everything to MP3 (falls back to AAC without ffmpeg)
    // Space saver: lossless sources become AAC 256 kbps (iTunes Plus
    // quality); files that are already lossy are copied unchanged.
    LosslessToAac,
};

// The file extension (including the dot) an imported file will end up with
// under `fmt`, given its source path.
std::string importExtension(ImportFormat fmt,
                            const std::filesystem::path& src);
std::string importExtension(ImportFormat fmt,
                            const std::filesystem::path& src,
                            bool originalSupported);

// Copies or transcodes `src` to `dest` (whose extension must equal
// importExtension(fmt, src)). Returns false with a message in `error`.
//
// The output is written beside `dest` under a partial-import name, flushed
// to the device, then renamed into place, so an interrupted write (unplug,
// forced unmount, crash) never leaves a truncated song at `dest`. An MP4's
// index is written last, so a cut-off .m4a would not play at all.
bool importAudio(ImportFormat fmt, const std::filesystem::path& src,
                 const std::filesystem::path& dest, std::string* error);
bool importAudio(ImportFormat fmt, const std::filesystem::path& src,
                 const std::filesystem::path& dest, std::string* error,
                 bool originalSupported);

// Whether `path` is an import still being written (or abandoned mid-write).
bool isPartialImport(const std::filesystem::path& path);

// True when an MP3 encoder (ffmpeg or lame) is available on this system.
bool mp3EncoderAvailable();

// Whether a device plays `src` unchanged: its extension is one the device
// plays (lowercase, with the dot) and its sample rate is within the device's
// limit (0: none known).
bool devicePlaysOriginal(const std::unordered_set<std::string>& playable,
                         std::uint32_t maxSampleRate,
                         const std::filesystem::path& src,
                         std::uint32_t sampleRate);

// Whether LosslessToAac re-encodes this file: when the device cannot play it
// as-is, or when it is lossless and AAC would actually be smaller. A
// low-bitrate recording stays as it is rather than growing.
bool losslessToAacConverts(const std::filesystem::path& src,
                           std::uint32_t bitrateKbps, bool originalSupported);

// Roughly how many bytes importing `src` under `fmt` writes to the device,
// so a sync plan can check capacity before anything is converted.
std::uint64_t estimateImportBytes(ImportFormat fmt,
                                  const std::filesystem::path& src,
                                  const Track& meta, std::uint64_t sourceBytes,
                                  bool originalSupported);

// Whether `fmt` only ever produces files a device can play, given the
// lowercase extensions it plays (DeviceInfo::originalExtensions). Original is
// always offered; the conversions are offered only when their output is.
bool importFormatPlayable(ImportFormat fmt,
                          const std::unordered_set<std::string>& playable);

}  // namespace podbox
