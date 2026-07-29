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
            ok = afconvert(src, dest, "alac");
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
                ok = afconvert(src, dest, "alac");
            break;
    }
    if (!ok && error)
        *error = src.filename().string() + ": import/conversion failed";
    return ok;
}

}  // namespace podbox
