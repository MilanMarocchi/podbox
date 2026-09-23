#include "library/transcode.h"

#include "library/metadata.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace podbox {
namespace {

std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += (c == '\'') ? "'\\''" : std::string(1, c);
    return q + "'";
}

bool run(const std::string& cmd) {
    return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

// Whether a tool exists on PATH (result cached per tool).
bool haveTool(const char* tool) {
    return run(std::string("command -v ") + tool);
}

bool afconvert(const fs::path& src, const fs::path& dest, const char* codec) {
    // -f m4af: MPEG-4 audio container (.m4a), which holds alac or aac.
    return run("/usr/bin/afconvert -f m4af -d " + std::string(codec) + " " +
               shellQuote(src.string()) + " " + shellQuote(dest.string()));
}

// The sample rate to give the iPod. Its ALAC decoder tops out at 48 kHz, so
// hi-res sources have to come down — to 44.1 kHz when they are a multiple of
// it (88.2/176.4), otherwise to 48 kHz, which keeps the conversion a clean
// integer ratio and avoids a needless resampling artefact.
int alacSampleRate(std::uint32_t sourceRate) {
    if (sourceRate == 0) return 44100;
    if (sourceRate <= 48000) return int(sourceRate);
    return (sourceRate % 44100 == 0) ? 44100 : 48000;
}

// Apple Lossless the iPod can actually decode: 16-bit, at most 48 kHz.
//
// This matters more than it looks. Left alone, afconvert copies the source's
// bit depth straight through, so a 24-bit FLAC becomes a 24-bit ALAC that a
// classic iPod silently refuses to play — and which is larger than the FLAC
// it came from.
bool toAlac(const fs::path& src, const fs::path& dest) {
    const FileMeta meta = readFileMetadata(src);
    const int rate = alacSampleRate(meta.ok ? meta.track.sampleRate : 0);

    if (haveTool("ffmpeg"))
        return run("ffmpeg -y -v error -i " + shellQuote(src.string()) +
                   " -map 0:a:0 -codec:a alac -sample_fmt s16p -ar " +
                   std::to_string(rate) + " " + shellQuote(dest.string()));

    // afconvert has no way to set ALAC bit depth directly, so go via a 16-bit
    // intermediate rather than emitting something unplayable.
    const fs::path tmp = dest.string() + ".16bit.caf";
    std::error_code ec;
    const bool ok =
        run("/usr/bin/afconvert -f caff -d LEI16@" + std::to_string(rate) +
            " " + shellQuote(src.string()) + " " + shellQuote(tmp.string())) &&
        run("/usr/bin/afconvert -f m4af -d alac " + shellQuote(tmp.string()) +
            " " + shellQuote(dest.string()));
    fs::remove(tmp, ec);
    return ok;
}

bool aacAudioToolboxAvailable() {
    static const bool available =
        haveTool("ffmpeg") &&
        run("ffmpeg -hide_banner -encoders | grep -q ' aac_at '");
    return available;
}

// AAC at 256 kbps constrained VBR, the iTunes Plus setting. Tags and cover
// art come across so the player's own library can file the song, and hi-res
// sources come down to 48 kHz or below, which every player decodes.
bool toAac(const fs::path& src, const fs::path& dest) {
    const FileMeta meta = readFileMetadata(src);
    const std::uint32_t sourceRate = meta.ok ? meta.track.sampleRate : 0;
    const int rate = alacSampleRate(sourceRate);
    const std::string resample =
        sourceRate > 48000 ? " -ar " + std::to_string(rate) : "";

    if (haveTool("ffmpeg")) {
        // Apple's encoder is noticeably better than ffmpeg's native one at
        // the same bitrate; the native one is the fallback off macOS.
        const std::string encoder = aacAudioToolboxAvailable()
                                        ? "aac_at -aac_at_mode cvbr"
                                        : "aac";
        const std::string head = "ffmpeg -y -v error -i " +
                                 shellQuote(src.string()) + " -map 0:a:0";
        const std::string audio =
            " -codec:a " + encoder + " -b:a 256k" + resample;
        if (run(head + " -map '0:v:0?'" + audio +
                " -codec:v copy -disposition:v:0 attached_pic " +
                shellQuote(dest.string())))
            return true;
        // MP4 only carries JPEG/PNG cover art; drop anything else.
        return run(head + audio + " -vn " + shellQuote(dest.string()));
    }

    if (!run("/usr/bin/afconvert -f m4af -d aac@" + std::to_string(rate) +
             " -b 256000 -s 2 " + shellQuote(src.string()) + " " +
             shellQuote(dest.string())))
        return false;
    // afconvert carries no tags over; without them every song would be
    // filed under its filename.
    return !meta.ok || writeFileTags(dest, meta.track, nullptr);
}

bool toMp3(const fs::path& src, const fs::path& dest) {
    // ffmpeg defaults to ID3v2.4 tags, which crash the Snowsky Echo's
    // firmware on playback; v2.3 is what iTunes writes and every player reads.
    if (haveTool("ffmpeg"))
        return run("ffmpeg -y -i " + shellQuote(src.string()) +
                   " -map 0:a:0 -codec:a libmp3lame -b:a 320k"
                   " -id3v2_version 3 " +
                   shellQuote(dest.string()));
    if (haveTool("lame"))
        return run("lame -b 320 " + shellQuote(src.string()) + " " +
                   shellQuote(dest.string()));
    return false;
}

constexpr std::uint32_t kAacKbps = 256;
// Above this a lossless file is worth re-encoding; below it (speech, mono
// or low-rate recordings) AAC 256 would not save anything.
constexpr std::uint32_t kLosslessToAacMinKbps = 320;

std::uint32_t sourceBitrateKbps(const fs::path& src) {
    const FileMeta meta = readFileMetadata(src);
    return meta.ok ? meta.track.bitrate : 0;
}

// Hidden, and the real extension stays last so ffmpeg and afconvert still
// pick the container from it.
constexpr const char* kPartialPrefix = ".podbox-partial.";

fs::path partialPath(const fs::path& dest) {
    return dest.parent_path() / (kPartialPrefix + dest.filename().string());
}

// Pushes a file's data out to the device. On macOS plain fsync() only
// reaches the drive's cache; F_FULLFSYNC asks the drive to commit it, and
// not every USB bridge supports it, so fall back to fsync().
bool flushToDisk(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = false;
#ifdef F_FULLFSYNC
    ok = ::fcntl(fd, F_FULLFSYNC) == 0;
#endif
    if (!ok) ok = ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}

bool writeImport(ImportFormat fmt, const fs::path& src, const fs::path& dest,
                 bool originalSupported) {
    std::error_code ec;
    switch (fmt) {
        case ImportFormat::Alac:
            return toAlac(src, dest);
        case ImportFormat::Mp3:
            if (mp3EncoderAvailable()) return toMp3(src, dest);
            return afconvert(src, dest, "aac");  // guaranteed fallback
        case ImportFormat::LosslessToAac:
            if (losslessToAacConverts(src, sourceBitrateKbps(src),
                                      originalSupported))
                return toAac(src, dest);
            return fs::copy_file(src, dest, ec) && !ec;
        case ImportFormat::Original:
        default:
            if (originalSupported) return fs::copy_file(src, dest, ec) && !ec;
            return toAlac(src, dest);
    }
}

}  // namespace

bool isPartialImport(const fs::path& path) {
    return path.filename().string().rfind(kPartialPrefix, 0) == 0;
}

bool mp3EncoderAvailable() {
    static const bool available = haveTool("ffmpeg") || haveTool("lame");
    return available;
}

bool devicePlaysOriginal(const std::unordered_set<std::string>& playable,
                         std::uint32_t maxSampleRate, const fs::path& src,
                         std::uint32_t sampleRate) {
    std::string ext = src.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return playable.count(ext) > 0 &&
           (maxSampleRate == 0 || sampleRate <= maxSampleRate);
}

bool losslessToAacConverts(const fs::path& src, std::uint32_t bitrateKbps,
                           bool originalSupported) {
    if (!originalSupported) return true;
    // An unknown bitrate is treated as worth converting, as for any FLAC.
    return isLosslessAudioFile(src) &&
           (bitrateKbps == 0 || bitrateKbps > kLosslessToAacMinKbps);
}

std::uint64_t estimateImportBytes(ImportFormat fmt, const fs::path& src,
                                  const Track& meta, std::uint64_t sourceBytes,
                                  bool originalSupported) {
    const double seconds = meta.lengthMs / 1000.0;
    auto atKbps = [&](double kbps) -> std::uint64_t {
        return seconds > 0 ? std::uint64_t(seconds * kbps * 1000 / 8)
                           : sourceBytes;
    };
    // 16-bit stereo at the capped rate; ALAC typically lands near 60% of PCM.
    auto alac = [&] {
        const double rate = alacSampleRate(meta.sampleRate);
        return atKbps(rate * 2 * 16 / 1000 * 0.6);
    };
    switch (fmt) {
        case ImportFormat::Alac:
            return alac();
        case ImportFormat::Mp3:
            return atKbps(mp3EncoderAvailable() ? 322 : kAacKbps + 8);
        case ImportFormat::LosslessToAac:
            // Constrained VBR runs a little over its target, plus container.
            return losslessToAacConverts(src, meta.bitrate, originalSupported)
                       ? atKbps(kAacKbps + 8)
                       : sourceBytes;
        case ImportFormat::Original:
        default:
            return originalSupported ? sourceBytes : alac();
    }
}

bool importFormatPlayable(ImportFormat fmt,
                          const std::unordered_set<std::string>& playable) {
    switch (fmt) {
        case ImportFormat::Alac:
        case ImportFormat::LosslessToAac:
            return playable.count(".m4a") > 0;
        case ImportFormat::Mp3:
            return playable.count(mp3EncoderAvailable() ? ".mp3" : ".m4a") > 0;
        case ImportFormat::Original:
        default:
            return true;
    }
}

std::string importExtension(ImportFormat fmt, const fs::path& src) {
    return importExtension(fmt, src, isSupportedAudioFile(src));
}

std::string importExtension(ImportFormat fmt, const fs::path& src,
                            bool originalSupported) {
    switch (fmt) {
        case ImportFormat::Alac:
            return ".m4a";
        case ImportFormat::Mp3:
            return mp3EncoderAvailable() ? ".mp3" : ".m4a";  // AAC fallback
        case ImportFormat::LosslessToAac:
            return losslessToAacConverts(src, sourceBitrateKbps(src),
                                         originalSupported)
                       ? ".m4a"
                       : src.extension().string();
        case ImportFormat::Original:
        default:
            // Playable formats keep their extension; anything else (FLAC) is
            // converted to Apple Lossless so it plays on the iPod.
            return originalSupported ? src.extension().string() : ".m4a";
    }
}

bool importAudio(ImportFormat fmt, const fs::path& src, const fs::path& dest,
                 std::string* error) {
    return importAudio(fmt, src, dest, error, isSupportedAudioFile(src));
}

bool importAudio(ImportFormat fmt, const fs::path& src, const fs::path& dest,
                 std::string* error, bool originalSupported) {
    const fs::path partial = partialPath(dest);
    std::error_code ec;
    fs::remove(partial, ec);  // left by an earlier, interrupted import
    bool ok = writeImport(fmt, src, partial, originalSupported) &&
              flushToDisk(partial);
    if (ok) {
        fs::rename(partial, dest, ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(partial, ec);
        if (error)
            *error = src.filename().string() + ": import/conversion failed";
    }
    return ok;
}

}  // namespace podbox
