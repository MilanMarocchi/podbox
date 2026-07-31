#include "library/artwork.h"

#include <attachedpictureframe.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <tbytevector.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <string>

namespace fs = std::filesystem;

namespace podbox {
namespace {

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

TagLib::ByteVector extractArtBytes(const fs::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".mp3") {
        TagLib::MPEG::File f(path.c_str(), false);
        if (!f.isValid()) return {};
        TagLib::ID3v2::Tag* tag = f.ID3v2Tag();
        if (!tag) return {};
        const auto& frames = tag->frameListMap()["APIC"];
        if (frames.isEmpty()) return {};
        auto* apic =
            static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
        return apic->picture();
    }
    if (ext == ".m4a" || ext == ".m4b" || ext == ".aac") {
        TagLib::MP4::File f(path.c_str(), false);
        if (!f.isValid() || !f.tag()) return {};
        TagLib::MP4::Tag* tag = f.tag();
        if (!tag->contains("covr")) return {};
        const TagLib::MP4::CoverArtList covers =
            tag->item("covr").toCoverArtList();
        if (covers.isEmpty()) return {};
        return covers.front().data();
    }
    if (ext == ".flac") {
        // FLAC keeps cover art in PICTURE metadata blocks. Prefer the one
        // typed as the front cover; a file can carry several — back cover,
        // liner notes, the artist's photo — and taking whichever came first
        // shows the wrong one often enough to matter.
        TagLib::FLAC::File f(path.c_str(), false);
        if (!f.isValid()) return {};
        const TagLib::List<TagLib::FLAC::Picture*> pics = f.pictureList();
        if (pics.isEmpty()) return {};
        for (const TagLib::FLAC::Picture* p : pics)
            if (p->type() == TagLib::FLAC::Picture::FrontCover)
                return p->data();
        return pics.front()->data();
    }
    return {};
}

}  // namespace

ArtImage loadEmbeddedArtwork(const fs::path& audioFile) {
    ArtImage out;
    const TagLib::ByteVector bytes = extractArtBytes(audioFile);
    if (bytes.isEmpty()) return out;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()),
        int(bytes.size()), &w, &h, &comp, 4);
    if (!pixels) return out;
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    return out;
}

}  // namespace podbox
