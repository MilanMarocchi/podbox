#include "library/transcode.h"

#include "library/metadata.h"

#include <cstdlib>

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
                   " -codec:a alac -sample_fmt s16p -ar " +
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

bool toMp3(const fs::path& src, const fs::path& dest) {
    if (haveTool("ffmpeg"))
        return run("ffmpeg -y -i " + shellQuote(src.string()) +
                   " -codec:a libmp3lame -b:a 320k " +
                   shellQuote(dest.string()));
    if (haveTool("lame"))
        return run("lame -b 320 " + shellQuote(src.string()) + " " +
                   shellQuote(dest.string()));
    return false;
}

}  // namespace

bool mp3EncoderAvailable() {
    static const bool available = haveTool("ffmpeg") || haveTool("lame");
    return available;
}

std::string importExtension(ImportFormat fmt, const fs::path& src) {
    switch (fmt) {
        case ImportFormat::Alac:
            return ".m4a";
        case ImportFormat::Mp3:
            return mp3EncoderAvailable() ? ".mp3" : ".m4a";  // AAC fallback
        case ImportFormat::Original:
        default:
            // Playable formats keep their extension; anything else (FLAC) is
            // converted to Apple Lossless so it plays on the iPod.
            return isSupportedAudioFile(src) ? src.extension().string()
                                             : ".m4a";
    }
}

bool importAudio(ImportFormat fmt, const fs::path& src, const fs::path& dest,
                 std::string* error) {
    std::error_code ec;
    bool ok = false;
    switch (fmt) {
        case ImportFormat::Alac:
            ok = toAlac(src, dest);
            break;
        case ImportFormat::Mp3:
            if (mp3EncoderAvailable())
                ok = toMp3(src, dest);
            else
                ok = afconvert(src, dest, "aac");  // guaranteed fallback
            break;
        case ImportFormat::Original:
        default:
            if (isSupportedAudioFile(src))
                ok = fs::copy_file(src, dest, ec) && !ec;
            else
                ok = toAlac(src, dest);
            break;
    }
    if (!ok && error)
        *error = src.filename().string() + ": import/conversion failed";
    return ok;
}

}  // namespace podbox
