#include "library/artwork.h"

#include <attachedpictureframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <riff/wav/wavfile.h>
#include <riff/aiff/aifffile.h>
#include <tfilestream.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
namespace podbox {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::size_t kMaxArtBytes = 32 * 1024 * 1024;

ArtImage decode(const unsigned char* bytes, std::size_t size) {
    ArtImage out;
    if (!size || size > kMaxArtBytes)
        return out;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes, int(size), &w, &h, &comp) || w <= 0 || h <= 0 ||
        std::uint64_t(w) * h > 32 * 1024 * 1024)
        return out;
    unsigned char* pixels = stbi_load_from_memory(bytes, int(size), &w, &h, &comp, 4);
    if (!pixels)
        return out;
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + std::size_t(w) * h * 4);
    stbi_image_free(pixels);
    return out;
}
ArtImage decode(const TagLib::ByteVector& bytes) {
    return decode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}
TagLib::ID3v2::Tag* pictureTag(TagLib::File* file, bool create) {
    if (auto* f = dynamic_cast<TagLib::MPEG::File*>(file))
        return f->ID3v2Tag(create);
    if (auto* f = dynamic_cast<TagLib::RIFF::WAV::File*>(file))
        return f->ID3v2Tag();
    if (auto* f = dynamic_cast<TagLib::RIFF::AIFF::File*>(file))
        return f->tag();
    return nullptr;
}
ArtImage embedded(const fs::path& path) {
    TagLib::FileStream stream(path.c_str(), true);
    TagLib::FileRef file(&stream, false);
    if (file.isNull())
        return {};
    if (auto* tag = pictureTag(file.file(), false)) {
        const auto frames = tag->frameList("APIC");
        for (bool front : {true, false})
            for (const auto* frame : frames) {
                const auto* p =
                    dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame);
                if (!p ||
                    (p->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) != front)
                    continue;
                auto art = decode(p->picture());
                if (art.ok())
                    return art;
            }
    }
    if (auto* f = dynamic_cast<TagLib::FLAC::File*>(file.file())) {
        const auto pictures = f->pictureList();
        for (bool front : {true, false})
            for (const auto* p : pictures) {
                if ((p->type() == TagLib::FLAC::Picture::FrontCover) != front)
                    continue;
                auto art = decode(p->data());
                if (art.ok())
                    return art;
            }
    }
    if (auto* f = dynamic_cast<TagLib::MP4::File*>(file.file())) {
        if (f->tag() && f->tag()->contains("covr"))
            for (const auto& cover : f->tag()->item("covr").toCoverArtList()) {
                auto art = decode(cover.data());
                if (art.ok())
                    return art;
            }
    }
    return {};
}
std::vector<fs::path> sidecars(const fs::path& path) {
    std::vector<fs::path> candidates;
    std::error_code ec;
    const auto folder = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        const fs::path lower(name);
        const auto ext = lower.extension().string(), stem = lower.stem().string();
        if ((ext == ".jpg" || ext == ".jpeg" || ext == ".png") &&
            (stem == "cover" || stem == "folder" || stem == "front" || stem == "album") &&
            entry.is_regular_file(ec) && !ec)
            candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}
Bytes read(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > kMaxArtBytes)
        return {};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
void appendJpeg(void* context, void* data, int size) {
    auto& out = *static_cast<Bytes*>(context);
    auto* bytes = static_cast<unsigned char*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
}  // namespace

ArtImage loadEmbeddedArtwork(const fs::path& path) {
    return embedded(path);
}
ArtImage loadArtwork(const fs::path& path) {
    auto art = embedded(path);
    if (art.ok())
        return art;
    for (const auto& sidecar : sidecars(path)) {
        const auto bytes = read(sidecar);
        art = decode(bytes.data(), bytes.size());
        if (art.ok())
            return art;
    }
    return {};
}
ArtImage loadArtworkThumbnail(const fs::path& path, int maxEdge) {
    ArtImage art = loadArtwork(path);
    if (!art.ok() || maxEdge <= 0 || std::max(art.width, art.height) <= maxEdge)
        return art;
    const double scale = double(maxEdge) / std::max(art.width, art.height);
    ArtImage out;
    out.width = std::max(1, int(art.width * scale));
    out.height = std::max(1, int(art.height * scale));
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    // Box filter: every source pixel lands in exactly one destination pixel.
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const int x0 = x * art.width / out.width, x1 = (x + 1) * art.width / out.width;
            const int y0 = y * art.height / out.height, y1 = (y + 1) * art.height / out.height;
            std::uint64_t sums[4]{};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const auto* p = &art.rgba[(std::size_t(sy) * art.width + sx) * 4];
                    for (int c = 0; c < 4; ++c) sums[c] += p[c];
                }
            const std::uint64_t n = std::uint64_t(x1 - x0) * (y1 - y0);
            for (int c = 0; c < 4; ++c)
                out.rgba[(std::size_t(y) * out.width + x) * 4 + c] =
                    static_cast<unsigned char>(sums[c] / n);
        }
    return out;
}
std::vector<unsigned char> playerArtwork(const fs::path& path) {
    const auto art = loadArtwork(path);
    if (!art.ok())
        return {};
    const double scale = std::min(1.0, 600.0 / std::max(art.width, art.height));
    const int w = std::max(1, int(art.width * scale)),
              h = std::max(1, int(art.height * scale));
    Bytes rgb(std::size_t(w) * h * 3);
    // Average source pixels when reducing; composite transparency onto white.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = x * art.width / w, x1 = (x + 1) * art.width / w;
            const int y0 = y * art.height / h, y1 = (y + 1) * art.height / h;
            std::uint64_t sums[3]{};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const auto* p = &art.rgba[(std::size_t(sy) * art.width + sx) * 4];
                    for (int c = 0; c < 3; ++c)
                        sums[c] += (p[c] * p[3] + 255 * (255 - p[3]) + 127) / 255;
                }
            for (int c = 0; c < 3; ++c)
                rgb[(std::size_t(y) * w + x) * 3 + c] =
                    unsigned(sums[c] / ((x1 - x0) * (y1 - y0)));
        }
    Bytes jpeg;
    if (!stbi_write_jpg_to_func(appendJpeg, &jpeg, w, h, 3, rgb.data(), 90))
        return {};
    return jpeg;
}
bool copyArtwork(const fs::path& source, const fs::path& dest, std::string* error) {
    const auto jpeg = playerArtwork(source);
    if (jpeg.empty())
        return true;
    TagLib::FileRef file(dest.c_str(), false);
    bool supported = !file.isNull();
    const TagLib::ByteVector bytes(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
    if (supported) {
        if (auto* tag = pictureTag(file.file(), true)) {
            tag->removeFrames("APIC");
            auto* pic = new TagLib::ID3v2::AttachedPictureFrame;
            pic->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
            pic->setMimeType("image/jpeg");
            pic->setPicture(bytes);
            tag->addFrame(pic);
        } else if (auto* f = dynamic_cast<TagLib::MP4::File*>(file.file())) {
            TagLib::MP4::CoverArtList covers;
            covers.append(TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::JPEG, bytes));
            f->tag()->setItem("covr", TagLib::MP4::Item(covers));
        } else if (auto* f = dynamic_cast<TagLib::FLAC::File*>(file.file())) {
            f->removePictures();
            auto* pic = new TagLib::FLAC::Picture;
            pic->setType(TagLib::FLAC::Picture::FrontCover);
            pic->setMimeType("image/jpeg");
            pic->setData(bytes);
            f->addPicture(pic);
        } else
            supported = false;
    }
    if (supported) {
        if (auto* mp3 = dynamic_cast<TagLib::MPEG::File*>(file.file())) {
            if (mp3->save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone,
                          TagLib::ID3v2::v3, TagLib::File::DoNotDuplicate))
                return true;
        } else if (file.save())
            return true;
    }
    if (error)
        *error = dest.filename().string() + ": could not save album artwork";
    return false;
}
bool copySidecarArtwork(const fs::path& source, const fs::path& album, std::string* error) {
    if (embedded(source).ok())
        return true;
    for (const auto& candidate : sidecars(source)) {
        const auto bytes = read(candidate);
        if (!decode(bytes.data(), bytes.size()).ok())
            continue;
        std::string ext = candidate.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        const fs::path dest = album / ("cover" + ext);
        std::error_code ec;
        if (fs::equivalent(candidate, dest, ec) && !ec)
            return true;
        ec.clear();
        if (fs::exists(dest, ec) && !ec)
            return true;
        if (fs::copy_file(candidate, dest, ec) && !ec)
            return true;
        if (error)
            *error = "Could not copy album artwork: " + ec.message();
        return false;
    }
    return true;
}
}  // namespace podbox
