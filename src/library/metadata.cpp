#include "library/metadata.h"

#include <fileref.h>
#include <tag.h>

#include <algorithm>
#include <ctime>

namespace fs = std::filesystem;

namespace podbox {
namespace {

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

}  // namespace

bool isSupportedAudioFile(const fs::path& path) {
    static const char* kExts[] = {".mp3", ".m4a", ".m4b", ".aac",
                                  ".wav", ".aif", ".aiff"};
    const std::string ext = lowerExt(path);
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

bool isImportableAudioFile(const fs::path& path) {
    return isSupportedAudioFile(path) || lowerExt(path) == ".flac";
}

FileMeta readFileMetadata(const fs::path& path) {
    FileMeta out;
    if (!isImportableAudioFile(path)) {
        out.error = path.filename().string() +
                    ": unsupported format (use MP3/AAC/ALAC/WAV/AIFF/FLAC)";
        return out;
    }

    TagLib::FileRef f(path.c_str(), true,
                      TagLib::AudioProperties::Average);
    if (f.isNull() || !f.audioProperties()) {
        out.error = path.filename().string() + ": could not read audio file";
        return out;
    }

    Track& t = out.track;
    if (const TagLib::Tag* tag = f.tag()) {
        t.title = tag->title().to8Bit(true);
        t.artist = tag->artist().to8Bit(true);
        t.album = tag->album().to8Bit(true);
        t.genre = tag->genre().to8Bit(true);
        t.year = tag->year();
        t.trackNumber = tag->track();
    }
    if (t.title.empty()) t.title = path.stem().string();

    const TagLib::AudioProperties* props = f.audioProperties();
    t.lengthMs = std::uint32_t(props->lengthInMilliseconds());
    t.bitrate = std::uint32_t(props->bitrate());
    t.sampleRate = std::uint32_t(props->sampleRate());

    std::error_code ec;
    t.sizeBytes = std::uint32_t(fs::file_size(path, ec));
    t.dateAdded = std::time(nullptr);
    out.ok = true;
    return out;
}

}  // namespace podbox
