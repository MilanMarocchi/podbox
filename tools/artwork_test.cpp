// Generated media only: exercises the actual encoders and container writers.
#include "library/artwork.h"
#include "library/fingerprint.h"
#include "library/metadata.h"
#include "library/music_folder.h"
#include "library/transcode.h"
#include <attachedpictureframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mp4coverart.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <riff/wav/wavfile.h>
#include <riff/aiff/aifffile.h>
#include <stb_image_write.h>
#include <stb_image.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstdio>
namespace fs = std::filesystem;
using namespace podbox;
namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}
std::string quote(const fs::path& p) {
    std::string out = "'";
    for (char c : p.string())
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
void write(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary).write(bytes.data(), bytes.size());
}
void append(void* context, void* bytes, int size) {
    static_cast<std::string*>(context)->append(static_cast<char*>(bytes), size);
}
std::string png(int r, int g, int b, int a = 255, int w = 8, int h = 8) {
    std::vector<unsigned char> pixels(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    std::string bytes;
    stbi_write_png_to_func(append, &bytes, w, h, 4, pixels.data(), w * 4);
    return bytes;
}
bool red(const ArtImage& art) {
    return art.ok() && art.rgba[0] > 220 && art.rgba[1] < 30 && art.rgba[2] < 30;
}
TagLib::ID3v2::Tag* id3(TagLib::File* f) {
    if (auto* p = dynamic_cast<TagLib::MPEG::File*>(f))
        return p->ID3v2Tag(true);
    if (auto* p = dynamic_cast<TagLib::RIFF::WAV::File*>(f))
        return p->ID3v2Tag();
    if (auto* p = dynamic_cast<TagLib::RIFF::AIFF::File*>(f))
        return p->tag();
    return nullptr;
}
void pictures(const fs::path& p) {
    const auto front = png(255, 0, 0), back = png(0, 0, 255);
    TagLib::FileRef file(p.c_str(), false);
    check(!file.isNull(), "generated audio opens: " + p.string());
    if (file.isNull())
        return;
    if (auto* tag = id3(file.file())) {
        for (int type : {4, 3, 3}) {
            auto* pic = new TagLib::ID3v2::AttachedPictureFrame;
            pic->setType(static_cast<TagLib::ID3v2::AttachedPictureFrame::Type>(type));
            pic->setMimeType("image/png");
            const auto& bytes = type == 4 ? back : front;
            // A corrupt first front cover must not hide a later valid one.
            pic->setPicture(TagLib::ByteVector(bytes.data(), bytes.size()));
            tag->addFrame(pic);
        }
        auto* firstFront =
            dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(tag->frameList("APIC")[1]);
        firstFront->setPicture(TagLib::ByteVector("broken"));
    } else if (auto* f = dynamic_cast<TagLib::FLAC::File*>(file.file())) {
        for (int type : {4, 3, 3}) {
            auto* pic = new TagLib::FLAC::Picture;
            pic->setType(static_cast<TagLib::FLAC::Picture::Type>(type));
            pic->setMimeType("image/png");
            const auto& bytes = type == 4 ? back : front;
            pic->setData(TagLib::ByteVector(bytes.data(), bytes.size()));
            if (type == 3 && f->pictureList().size() == 1)
                pic->setData(TagLib::ByteVector("broken"));
            f->addPicture(pic);
        }
    } else if (auto* f = dynamic_cast<TagLib::MP4::File*>(file.file())) {
        TagLib::MP4::CoverArtList covers;
        covers.append(
            TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::PNG, TagLib::ByteVector("broken")));
        covers.append(TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::PNG,
                                            TagLib::ByteVector(front.data(), front.size())));
        f->tag()->setItem("covr", TagLib::MP4::Item(covers));
    }
    check(file.save(), "write artwork fixture");
}
}  // namespace
int main() {
    if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
        std::printf("Artwork conversion tests require ffmpeg; skipped\n");
        return 77;
    }
    const auto root =
        fs::temp_directory_path() /
        ("podbox-artwork-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::vector<std::pair<std::string, std::string>> formats = {{"mp3", "libmp3lame"},
                                                                      {"flac", "flac"},
                                                                      {"m4a", "aac"},
                                                                      {"wav", "pcm_s16le"},
                                                                      {"aiff", "pcm_s16be"}};
    for (const auto& [ext, codec] : formats) {
        const auto src = root / ("source." + ext);
        const std::string command =
            "ffmpeg -v error -y -f lavfi -i anullsrc=r=44100:cl=stereo -t 1 -codec:a " +
            codec + " -metadata title=Song -metadata artist=Band -metadata album=Record " +
            quote(src);
        check(std::system(command.c_str()) == 0, "generate " + ext);
        pictures(src);
        if (ext == "aiff") {
            Track tags;
            tags.title = "Song";
            tags.artist = "Band";
            tags.album = "Record";
            check(writeFileTags(src, tags, nullptr), "AIFF text fixture");
        }
        check(red(loadEmbeddedArtwork(src)),
              ext + " chooses valid front cover over back/corrupt covers");
        const auto original = readAll(src);
        for (auto fmt : {ImportFormat::Alac, ImportFormat::Mp3, ImportFormat::LosslessToAac}) {
            const auto dest = root / (ext + "-" + std::to_string(int(fmt)) +
                                      importExtension(fmt, src, false));
            std::string error;
            check(importAudio(fmt, src, dest, &error, false),
                  ext + " conversion succeeds: " + error);
            check(
                red(loadEmbeddedArtwork(dest)),
                ext + " retains the front cover after conversion " + std::to_string(int(fmt)));
            const auto meta = readFileMetadata(dest);
            check(meta.ok && meta.track.title == "Song" && meta.track.artist == "Band",
                  ext + " preserves text tags");
            if (dest.extension() == ".mp3") {
                const auto bytes = readAll(dest);
                check(bytes.substr(0, 4) == std::string("ID3\x03", 4),
                      "MP3 artwork does not change ID3v2.3 to v2.4");
                check(bytes.find("TIT2") < bytes.find("APIC"),
                      "text appears before the artwork");
            }
        }
        check(readAll(src) == original,
              "all conversions leave " + ext + " source byte-for-byte unchanged");
        const auto dest = root / ("copy." + ext);
        std::string error;
        check(importAudio(ImportFormat::Original, src, dest, &error, true) &&
                  readAll(dest) == original,
              "original " + ext + " copies preserve existing embedded art unchanged");
    }
    const auto wav = root / "source.wav";
    check(writePlayerSafeMp3Tags(root / "source.mp3", root / "source.mp3", nullptr),
          "in-place MP3 retag succeeds");
    check(red(loadEmbeddedArtwork(root / "source.mp3")),
          "in-place MP3 retag preserves artwork");
    const auto inbox = root / "inbox";
    fs::create_directories(inbox);
    // Re-encode only audio to make a WAV with no embedded picture.
    const auto sidecarSong = inbox / "sidecar.wav";
    check(std::system(("ffmpeg -v error -y -i " + quote(wav) +
                       " -map 0:a:0 -map_metadata -1 " + quote(sidecarSong))
                          .c_str()) == 0,
          "create sidecar-only song");
    write(inbox / "Cover.PNG", png(255, 0, 0));
    check(!loadEmbeddedArtwork(sidecarSong).ok() && red(loadArtwork(sidecarSong)),
          "case-insensitive sidecar fallback");
    Track meta;
    meta.artist = "Band";
    meta.album = "Sidecar";
    meta.title = "Song";
    fs::path placed;
    bool reused = false;
    std::string error;
    check(placeInMusicFolder(sidecarSong, meta, root / "library", &placed, &reused, &error) &&
              red(loadArtwork(placed)),
          "library import keeps sidecar artwork");
    const auto mp3 = root / "sidecar.mp3";
    check(importAudio(ImportFormat::Mp3, placed, mp3, &error) && red(loadEmbeddedArtwork(mp3)),
          "sidecar survives library import then MP3 conversion");
    const auto originalCopy = root / "sidecar-copy.wav";
    check(importAudio(ImportFormat::Original, placed, originalCopy, &error, true) &&
              red(loadEmbeddedArtwork(originalCopy)),
          "copying original embeds sidecar for the player");
    check(fingerprintFile(originalCopy) == fingerprintFile(placed),
          "adding a sidecar cover does not change the WAV audio");
    const auto bare = root / "bare";
    fs::create_directories(bare);
    fs::copy_file(sidecarSong, bare / "plain.wav");
    check(!loadArtwork(bare / "plain.wav").ok(),
          "no art is handled without borrowing a different album's cover");
    write(bare / "cover.png", png(255, 0, 0, 0, 1200, 800));
    const auto jpeg = playerArtwork(bare / "plain.wav");
    int w = 0, h = 0, c = 0;
    auto* pixels = stbi_load_from_memory(jpeg.data(), jpeg.size(), &w, &h, &c, 3);
    check(pixels && w == 600 && h == 400 && pixels[0] > 245 && pixels[1] > 245 &&
              pixels[2] > 245,
          "large transparent PNG becomes a bounded white-composited JPEG");
    stbi_image_free(pixels);
    fs::remove_all(root);
    std::printf("Artwork tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
