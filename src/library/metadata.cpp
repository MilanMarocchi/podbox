#include "library/metadata.h"

#include <fileref.h>
#include <mp4file.h>
#include <tag.h>
#include <tfilestream.h>

#include <algorithm>
#include <ctime>

namespace fs = std::filesystem;

namespace podbox {
namespace {

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string lowerExt(const fs::path& path) {
    return lowerAscii(path.extension().string());
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

bool isLosslessAudioFile(const fs::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".flac" || ext == ".wav" || ext == ".aif" || ext == ".aiff")
        return true;
    if (ext != ".m4a" && ext != ".m4b") return false;
    TagLib::FileStream stream(path.c_str(), /*openReadOnly=*/true);
    TagLib::MP4::File f(&stream, true, TagLib::MP4::Properties::Fast);
    return f.isValid() && f.audioProperties() &&
           f.audioProperties()->codec() == TagLib::MP4::Properties::ALAC;
}

std::uint32_t classifyMediaType(const fs::path& path,
                                const std::string& genre) {
    if (lowerExt(path) == ".m4b") return kMediaAudiobook;
    const std::string g = lowerAscii(genre);
    if (g == "podcast" || g == "podcasts") return kMediaPodcast;
    if (g == "audiobook" || g == "audiobooks" || g == "spoken word" ||
        g == "books & spoken")
        return kMediaAudiobook;
    return kMediaAudio;
}

FileMeta readFileMetadata(const fs::path& path) {
    FileMeta out;
    if (!isImportableAudioFile(path)) {
        out.error = path.filename().string() +
                    ": unsupported format (use MP3/AAC/ALAC/WAV/AIFF/FLAC)";
        return out;
    }

    // Read-only on purpose: opening for write makes macOS tag the file with
    // com.apple.provenance, which on a FAT player becomes a "._" companion
    // file the firmware lists as a broken track. The stream must outlive f.
    TagLib::FileStream stream(path.c_str(), /*openReadOnly=*/true);
    TagLib::FileRef f(&stream, true, TagLib::AudioProperties::Average);
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

    t.mediaType = classifyMediaType(path, t.genre);

    std::error_code ec;
    t.sizeBytes = std::uint32_t(fs::file_size(path, ec));
    t.dateAdded = std::time(nullptr);
    out.ok = true;
    return out;
}

bool writeFileTags(const fs::path& path, const Track& meta,
                   std::string* error) {
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.tag()) {
        if (error)
            *error = path.filename().string() + ": cannot write tags here";
        return false;
    }
    TagLib::Tag* tag = f.tag();
    tag->setTitle(TagLib::String(meta.title, TagLib::String::UTF8));
    tag->setArtist(TagLib::String(meta.artist, TagLib::String::UTF8));
    tag->setAlbum(TagLib::String(meta.album, TagLib::String::UTF8));
    tag->setGenre(TagLib::String(meta.genre, TagLib::String::UTF8));
    tag->setYear(meta.year);
    tag->setTrack(meta.trackNumber);
    if (!f.save()) {
        if (error) *error = path.filename().string() + ": could not save tags";
        return false;
    }
    return true;
}

}  // namespace podbox
