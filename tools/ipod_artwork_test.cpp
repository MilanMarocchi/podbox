#include "itdb/artworkdb.h"
#include "library/metadata.h"
#include <stb_image_write.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdio>
namespace fs = std::filesystem;
using namespace podbox;
namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
void write(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary).write(bytes.data(), bytes.size());
}
std::uint32_t get(const std::string& b, std::size_t p) {
    if (p > b.size() || b.size() - p < 4)
        return 0;
    return (unsigned char)b[p] | ((unsigned char)b[p + 1] << 8) |
           ((unsigned char)b[p + 2] << 16) | (std::uint32_t((unsigned char)b[p + 3]) << 24);
}
std::uint32_t get(const std::vector<std::uint8_t>& b, std::size_t p) {
    return get(std::string(b.begin(), b.end()), p);
}
void append(void* p, void* data, int n) {
    static_cast<std::string*>(p)->append(static_cast<char*>(data), n);
}
void cover(const fs::path& p) {
    unsigned char rgb[8 * 4 * 3];
    for (int i = 0; i < 8 * 4; ++i) {
        rgb[i * 3] = 255;
        rgb[i * 3 + 1] = rgb[i * 3 + 2] = 0;
    }
    std::string png;
    stbi_write_png_to_func(append, &png, 8, 4, 3, rgb, 8 * 3);
    write(p, png);
}
void wav(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t n) { out.write(reinterpret_cast<char*>(&n), 4); };
    auto u16 = [&](std::uint16_t n) { out.write(reinterpret_cast<char*>(&n), 2); };
    out.write("RIFF", 4);
    u32(88236);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(88200);
    std::string samples(88200, 0);
    out.write(samples.data(), samples.size());
}
std::string firstImage(const std::string& db) {
    auto start = db.find("mhii");
    return start == std::string::npos ? "" : db.substr(start, get(db, start + 8));
}
}  // namespace
int main() {
    const auto root =
        fs::temp_directory_path() /
        ("podbox-ipodart-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto mount = root / "iPod";
    fs::create_directories(mount / "iPod_Control/iTunes");
    fs::create_directories(root / "host");
    wav(root / "host/song.wav");
    cover(root / "host/cover.png");
    DeviceInfo device;
    device.mountPoint = mount;
    device.modelNumber = "MA146";
    device.modelName = "iPod 5th gen (30 GB, black)";
    Library library;
    Track song;
    song.id = 100;
    song.dbid = 0x123456789abcdef0;
    song.title = "Song";
    song.artist = "Band";
    song.album = "Record";
    song.location = ":iPod_Control:Music:F00:ABCD.WAV";
    song.artworkSource = root / "host/song.wav";
    library.tracks.push_back(song);
    int added = 0;
    std::string error;
    check(writeIpodArtwork(device, library, &added, &error) && added == 1,
          "create iPod 5G artwork database");
    const auto folder = mount / "iPod_Control/Artwork", index = folder / "ArtworkDB";
    const auto initial = read(index);
    check(initial.substr(0, 4) == "mhfd" && get(initial, 8) == initial.size() &&
              get(initial, 20) == 3,
          "ArtworkDB lengths and section count");
    const auto image = firstImage(initial);
    check(get(image, 4) == 0x98 && get(image, 12) == 2, "5G image has both thumbnail sizes");
    check(get(image, 20) == 0x9abcdef0 && get(image, 24) == 0x12345678,
          "artwork uses the persistent song id");
    const auto small = read(folder / "F1028_1.ithmb"), large = read(folder / "F1029_1.ithmb");
    check(small.size() == 100 * 100 * 2 && large.size() == 200 * 200 * 2,
          "5G RGB565 thumbnails have exact sizes");
    check(small[0] == 0 && small[1] == 0 && small[100 * 25 * 2] == 0 &&
              (unsigned char)small[100 * 25 * 2 + 1] == 0xf8,
          "non-square artwork is centered, not stretched; red is RGB565 little endian");
    const auto ni = image.find("mhni");
    check(
        ni != std::string::npos && get(image, ni + 16) == 1028 && get(image, ni + 24) == 20000,
        "mhni links its format and byte range");
    check(library.tracks[0].artworkUpdate &&
              library.tracks[0].artworkUpdate->imageId == get(image, 16),
          "track links to generated artwork id");
    const auto musicDb = mount / "iPod_Control/iTunes/iTunesDB";
    check(writeItunesDb(library, musicDb, &error), "write paired music database");
    const auto parsed = parseItunesDb(musicDb);
    check(parsed.library && parsed.library->tracks.size() == 1,
          "paired music database parses");
    if (parsed.library) {
        const auto& h = parsed.library->tracks[0].rawHeader;
        check(h.size() >= 356 && h[124] == 1 && h[164] == 1 && get(h, 352) == get(image, 16),
              "iTunesDB contains artwork count, flag and mhii link");
    }
    check(writeIpodArtwork(device, library, &added, &error) && added == 0 &&
              read(index) == initial,
          "repeat save does not duplicate or rewrite valid art");
    Track second = song;
    second.id = 101;
    second.dbid++;
    library.tracks.push_back(second);
    check(writeIpodArtwork(device, library, &added, &error) && added == 1,
          "append another track's artwork");
    const auto appended = read(index);
    check(firstImage(appended) == image && read(folder / "F1028_1.ithmb") == small,
          "appending preserves existing artwork bytes and ids");
    check(read(index.string() + ".podbox-bak.1") == initial,
          "previous artwork database backed up");
    // Simulate the failure that produces blank/wrong iPod covers: a database
    // offset still exists but its thumbnail file has been truncated.
    write(folder / "F1028_1.ithmb", small.substr(0, 10));
    check(writeIpodArtwork(device, library, &added, &error) && added == 1,
          "truncated thumbnail repaired from the Mac original");
    check(get(firstImage(read(index)), 16) == get(image, 16),
          "repair preserves the artwork id used by old music databases");
    const auto good = read(index);
    write(index, good.substr(0, good.size() - 1));
    const auto corrupt = read(index);
    check(!writeIpodArtwork(device, library, &added, &error) && read(index) == corrupt,
          "a damaged ArtworkDB is rejected and preserved");
    write(index, good);
    DeviceInfo classic = device;
    classic.mountPoint = root / "classic";
    classic.modelNumber = "MB147";
    classic.modelName = "iPod classic (80 GB, black)";
    Library classicLib;
    classicLib.tracks.push_back(song);
    check(writeIpodArtwork(classic, classicLib, &added, &error) && added == 1,
          "classic artwork generated");
    const auto classicDir = classic.mountPoint / "iPod_Control/Artwork";
    check(get(firstImage(read(classicDir / "ArtworkDB")), 12) == 4 &&
              fs::file_size(classicDir / "F1060_1.ithmb") == 320 * 320 * 2,
          "classic has its four display formats");
    DeviceInfo unknown;
    unknown.mountPoint = root / "unknown";
    check(writeIpodArtwork(unknown, library, &added, &error) && added == 0 &&
              !fs::exists(unknown.mountPoint),
          "unknown models never receive guessed thumbnail formats");
    fs::remove_all(root);
    std::printf("iPod artwork tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
