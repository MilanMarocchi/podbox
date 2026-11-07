#include "itdb/artworkdb.h"
#include "device/ipod_device.h"
#include "library/artwork.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace podbox {
namespace {
using Bytes = std::vector<unsigned char>;
std::uint32_t get(const Bytes& b, std::size_t p) {
    if (p > b.size() || b.size() - p < 4)
        throw std::runtime_error("truncated artwork record");
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) |
           (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
}
std::uint64_t get64(const Bytes& b, std::size_t p) {
    return get(b, p) | (std::uint64_t(get(b, p + 4)) << 32);
}
void put(Bytes& b, std::size_t p, std::uint32_t v, int n = 4) {
    for (int i = 0; i < n; ++i)
        b.at(p + i) = v >> (8 * i);
}
void put64(Bytes& b, std::size_t p, std::uint64_t v) {
    put(b, p, v);
    put(b, p + 4, v >> 32);
}
Bytes chunk(const char* name, std::size_t size) {
    Bytes b(size);
    std::memcpy(b.data(), name, 4);
    put(b, 4, size);
    return b;
}
void append(Bytes& a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
}
bool tag(const Bytes& b, std::size_t p, const char* name) {
    return p <= b.size() && b.size() - p >= 4 && std::memcmp(b.data() + p, name, 4) == 0;
}
Bytes read(const fs::path& p) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec || size > 256 * 1024 * 1024)
        throw std::runtime_error("artwork database is unreadable or too large");
    std::ifstream in(p, std::ios::binary);
    Bytes b(size);
    if (!in.read(reinterpret_cast<char*>(b.data()), b.size()))
        throw std::runtime_error("cannot read artwork database");
    return b;
}
bool write(const fs::path& p, const Bytes& b) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    std::size_t pos = 0;
    bool ok = true;
    while (pos < b.size()) {
        auto n = ::write(fd, b.data() + pos, b.size() - pos);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        pos += std::size_t(n);
    }
    if (ok) {
#ifdef F_FULLFSYNC
        ok = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
        ok = ::fsync(fd) == 0;
#endif
    }
    if (::close(fd) != 0)
        ok = false;
    return ok;
}
struct Section {
    Bytes header;
    Bytes list;
    std::vector<Bytes> records;
};
struct Database {
    Bytes header;
    std::vector<Section> sections;
    std::uint32_t next = 100;
};
Database parse(const Bytes& bytes) {
    if (!tag(bytes, 0, "mhfd") || get(bytes, 4) < 0x34 || get(bytes, 8) != bytes.size() ||
        get(bytes, 4) > bytes.size())
        throw std::runtime_error("invalid ArtworkDB header");
    Database out;
    out.header = Bytes(bytes.begin(), bytes.begin() + get(bytes, 4));
    out.next = std::max(100u, get(bytes, 28));
    std::size_t pos = out.header.size();
    std::unordered_set<unsigned> types;
    for (unsigned n = 0; n < get(bytes, 20); ++n) {
        if (!tag(bytes, pos, "mhsd"))
            throw std::runtime_error("invalid artwork section");
        const auto header = get(bytes, pos + 4), length = get(bytes, pos + 8),
                   type = get(bytes, pos + 12) & 0xffff;
        if (header < 16 || length < header || pos > bytes.size() ||
            length > bytes.size() - pos || !types.insert(type).second)
            throw std::runtime_error("invalid artwork section bounds");
        Section section;
        section.header = Bytes(bytes.begin() + pos, bytes.begin() + pos + header);
        std::size_t at = pos + header;
        const auto end = pos + length;
        const char* list = type == 1   ? "mhli"
                           : type == 2 ? "mhla"
                           : type == 3 ? "mhlf"
                                       : nullptr;
        if (!list || !tag(bytes, at, list))
            throw std::runtime_error("unrecognized artwork section");
        const auto listSize = get(bytes, at + 4), count = get(bytes, at + 8);
        if (listSize < 12 || listSize > end - at || count > 1000000)
            throw std::runtime_error("invalid artwork list");
        section.list = Bytes(bytes.begin() + at, bytes.begin() + at + listSize);
        at += listSize;
        for (unsigned i = 0; i < count; ++i) {
            if (at > end || end - at < 12)
                throw std::runtime_error("truncated artwork list");
            const auto h = get(bytes, at + 4), size = get(bytes, at + 8);
            if (h < 12 || size < h || size > end - at)
                throw std::runtime_error("invalid artwork record bounds");
            section.records.emplace_back(bytes.begin() + at, bytes.begin() + at + size);
            if (type == 1) {
                if (!tag(bytes, at, "mhii") || h < 52)
                    throw std::runtime_error("invalid artwork image");
                const auto id = get(bytes, at + 16);
                if (id == std::numeric_limits<std::uint32_t>::max())
                    throw std::runtime_error("artwork ids exhausted");
                out.next = std::max(out.next, id + 1);
            }
            at += size;
        }
        if (at != end)
            throw std::runtime_error("unexpected artwork data at end of section");
        out.sections.push_back(std::move(section));
        pos = end;
    }
    if (pos != bytes.size() || types != std::unordered_set<unsigned>{1, 2, 3})
        throw std::runtime_error("incomplete ArtworkDB");
    return out;
}
Database empty() {
    Database db;
    db.header = chunk("mhfd", 0x84);
    put(db.header, 16, 2);
    db.header[48] = 2;
    for (int i = 1; i <= 3; ++i) {
        Section s;
        s.header = chunk("mhsd", 0x60);
        put(s.header, 12, i);
        s.list = chunk(i == 1 ? "mhli" : i == 2 ? "mhla" : "mhlf", 0x5c);
        db.sections.push_back(std::move(s));
    }
    return db;
}
Bytes serialize(Database db) {
    Bytes out = db.header;
    put(out, 20, db.sections.size());
    put(out, 28, db.next);
    for (auto& s : db.sections) {
        Bytes body = s.list;
        put(body, 8, s.records.size());
        for (const auto& r : s.records)
            append(body, r);
        put(s.header, 8, s.header.size() + body.size());
        append(out, s.header);
        append(out, body);
    }
    put(out, 8, out.size());
    return out;
}
Section& section(Database& db, int type) {
    for (auto& s : db.sections)
        if ((get(s.header, 12) & 0xffff) == unsigned(type))
            return s;
    throw std::runtime_error("missing artwork section");
}
std::string filename(const Bytes& mhni) {
    const auto at = get(mhni, 4);
    if (!tag(mhni, at, "mhod") || get(mhni, at + 12) % 65536 != 3 || at > mhni.size() ||
        mhni.size() - at < 36 || mhni[at + 28] != 2)
        throw std::runtime_error("invalid thumbnail filename");
    const auto size = get(mhni, at + 24);
    if (size % 2 || size > mhni.size() - at - 36)
        throw std::runtime_error("invalid thumbnail filename length");
    std::string out;
    for (std::size_t p = at + 36; p < at + 36 + size; p += 2) {
        if (mhni[p + 1] || mhni[p] < 32 || mhni[p] > 126)
            throw std::runtime_error("unsupported thumbnail filename");
        out += char(mhni[p]);
    }
    if (out.empty() || out.front() != ':' || out.find('/', 1) != std::string::npos ||
        out.find(':', 1) != std::string::npos || out.find("..") != std::string::npos)
        throw std::runtime_error("unsafe thumbnail filename");
    return out.substr(1);
}
bool validImage(const Bytes& image, const fs::path& folder) {
    try {
        std::size_t at = get(image, 4);
        const auto count = get(image, 12);
        if (!count)
            return false;
        for (unsigned i = 0; i < count; ++i) {
            if (!tag(image, at, "mhod"))
                return false;
            const auto size = get(image, at + 8), h = get(image, at + 4);
            if (size < h || at > image.size() || size > image.size() - at)
                return false;
            if ((get(image, at + 12) & 0xffff) == 2) {
                Bytes ni(image.begin() + at + h, image.begin() + at + size);
                if (!tag(ni, 0, "mhni") || get(ni, 4) < 36 || get(ni, 8) != ni.size())
                    return false;
                std::error_code ec;
                const auto n = fs::file_size(folder / filename(ni), ec);
                const std::uint64_t offset = get(ni, 20), bytes = get(ni, 24);
                if (ec || !bytes || offset > n || bytes > n - offset)
                    return false;
            } else
                return false;
            at += size;
        }
        return at == image.size();
    } catch (const std::exception&) {
        return false;
    }
}
Bytes pathRecord(const std::string& filename) {
    const std::string path = ":" + filename;
    Bytes out = chunk("mhod", 36);
    put(out, 4, 24);
    put(out, 12, 3);
    out[28] = 2;
    put(out, 24, path.size() * 2);
    for (unsigned char c : path) {
        out.push_back(c);
        out.push_back(0);
    }
    const auto pad = (4 - out.size() % 4) % 4;
    out[15] = pad;
    out.resize(out.size() + pad);
    put(out, 8, out.size());
    return out;
}
struct Thumbnail {
    Bytes pixels;
    int width, height, padx, pady;
};
Thumbnail render(const ArtImage& image, const IpodArtworkFormat& fmt) {
    const double scale =
        std::min(double(fmt.width) / image.width, double(fmt.height) / image.height);
    const int w = std::max(1, int(std::round(image.width * scale))),
              h = std::max(1, int(std::round(image.height * scale)));
    Thumbnail out{Bytes(std::size_t(fmt.width) * fmt.height * 2, 0), w, h, (fmt.width - w) / 2,
                  (fmt.height - h) / 2};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = std::min(image.width - 1, int((x + 0.5) * image.width / w)),
                      sy = std::min(image.height - 1, int((y + 0.5) * image.height / h));
            const auto* p = &image.rgba[(std::size_t(sy) * image.width + sx) * 4];
            auto color = [&](int c) { return (p[c] * p[3] + 255 * (255 - p[3]) + 127) / 255; };
            const unsigned rgb =
                ((color(0) >> 3) << 11) | ((color(1) >> 2) << 5) | (color(2) >> 3);
            const auto off = (std::size_t(y + out.pady) * fmt.width + x + out.padx) * 2;
            out.pixels[off] = rgb & 255;
            out.pixels[off + 1] = rgb >> 8;
        }
    return out;
}
Bytes thumbnailRecord(const IpodArtworkFormat& format, const Thumbnail& image,
                      const std::string& file, std::uint32_t offset) {
    Bytes ni = chunk("mhni", 0x4c);
    put(ni, 12, 1);
    put(ni, 16, format.id);
    put(ni, 20, offset);
    put(ni, 24, image.pixels.size());
    put(ni, 28, image.pady, 2);
    put(ni, 30, image.padx, 2);
    put(ni, 32, image.height + image.pady, 2);
    put(ni, 34, image.width + image.padx, 2);
    append(ni, pathRecord(file));
    put(ni, 8, ni.size());
    Bytes out = chunk("mhod", 24);
    put(out, 12, 2);
    append(out, ni);
    put(out, 8, out.size());
    return out;
}
fs::path audioPath(const DeviceInfo& device, const Track& track) {
    std::string path = track.location;
    std::replace(path.begin(), path.end(), ':', '/');
    if (!path.empty() && path.front() == '/')
        path.erase(0, 1);
    const fs::path relative = fs::path(path).lexically_normal();
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return {};
    return device.mountPoint / relative;
}
}  // namespace

std::vector<IpodArtworkFormat> ipodArtworkFormats(const DeviceInfo& d) {
    if (!d.isIpod())
        return {};
    if (isIpodVideo(d))
        return {{1028, 100, 100}, {1029, 200, 200}};
    if (d.modelName.find("classic") != std::string::npos ||
        d.modelName.find("nano 3rd") != std::string::npos)
        return {{1061, 56, 56}, {1055, 128, 128}, {1068, 128, 128}, {1060, 320, 320}};
    if (d.modelName.find("nano 4th") != std::string::npos)
        return {{1055, 128, 128}, {1068, 128, 128}, {1071, 240, 240},
                {1074, 50, 50},   {1078, 80, 80},   {1084, 240, 240}};
    if (d.modelName.find("nano 5th") != std::string::npos)
        return {{1056, 128, 128}, {1078, 80, 80}, {1073, 240, 240}, {1074, 50, 50}};
    if (d.modelName.find("nano (") != std::string::npos ||
        d.modelName.find("nano 2nd gen") != std::string::npos)
        return {{1031, 42, 42}, {1027, 100, 100}};
    if (d.modelName.find("photo") != std::string::npos)
        return {{1017, 56, 56}, {1016, 140, 140}};
    return {};
}
bool writeIpodArtwork(const DeviceInfo& device, Library& library, int* added,
                      std::string* error) {
    if (added)
        *added = 0;
    const auto formats = ipodArtworkFormats(device);
    if (formats.empty())
        return true;
    try {
        const auto folder = device.mountPoint / "iPod_Control" / "Artwork",
                   index = folder / "ArtworkDB";
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec)
            throw std::runtime_error(ec.message());
        const int lockFd =
            ::open((folder / ".podbox-artwork.lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (lockFd < 0)
            throw std::runtime_error("cannot lock artwork database");
        struct Close {
            int fd;
            ~Close() {
                ::close(fd);
            }
        } close{lockFd};
        if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("another PodBox is updating artwork");
        std::optional<Bytes> original;
        if (fs::exists(index, ec))
            original = read(index);
        if (ec)
            throw std::runtime_error(ec.message());
        auto db = original ? parse(*original) : empty();
        auto& images = section(db, 1);
        auto& files = section(db, 3);
        std::unordered_map<std::uint64_t, std::size_t> bySong;
        std::unordered_map<std::uint32_t, std::size_t> byId;
        for (std::size_t i = 0; i < images.records.size(); ++i) {
            bySong.emplace(get64(images.records[i], 20), i);
            byId.emplace(get(images.records[i], 16), i);
        }
        struct Output {
            std::string name;
            Bytes bytes;
        };
        std::map<unsigned, Output> outputs;
        int count = 0;
        std::unordered_set<std::uint64_t> seenSongs;
        for (auto& track : library.tracks) {
            if (!track.dbid)
                continue;
            if (!seenSongs.insert(track.dbid).second)
                throw std::runtime_error("duplicate song id in library");
            auto existing = bySong.find(track.dbid);
            std::optional<std::size_t> record;
            if (existing != bySong.end())
                record = existing->second;
            else if (track.rawHeader.size() >= 356) {
                const auto linked = byId.find(get(track.rawHeader, 352));
                if (linked != byId.end())
                    record = linked->second;
            }
            if (record && validImage(images.records[*record], folder)) {
                const auto& image = images.records[*record];
                track.artworkUpdate = TrackArtwork{get(image, 16), get(image, 48)};
                continue;
            }
            auto art =
                track.artworkSource.empty() ? ArtImage{} : loadArtwork(track.artworkSource);
            if (!art.ok())
                art = loadArtwork(audioPath(device, track));
            if (!art.ok())
                continue;
            // Replace a broken record under its existing id; other valid
            // records and all existing thumbnail bytes remain untouched.
            const auto id = record ? get(images.records[*record], 16) : db.next++;
            if (!id || id == std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("artwork ids exhausted");
            Bytes image = chunk("mhii", 0x98);
            put(image, 12, formats.size());
            put(image, 16, id);
            put64(image, 20, track.dbid);
            const unsigned originalSize = art.rgba.size();
            put(image, 48, originalSize);
            for (const auto& format : formats) {
                auto& output = outputs[format.id];
                if (output.name.empty()) {
                    for (unsigned i = 1; i < 100000; ++i) {
                        const auto name = "F" + std::to_string(format.id) + "_" +
                                          std::to_string(i) + ".ithmb";
                        if (!fs::exists(folder / name, ec) && !ec) {
                            output.name = name;
                            break;
                        }
                    }
                    if (output.name.empty())
                        throw std::runtime_error("no free thumbnail filename");
                }
                auto thumb = render(art, format);
                if (output.bytes.size() >
                    std::numeric_limits<std::uint32_t>::max() - thumb.pixels.size())
                    throw std::runtime_error("thumbnail file too large");
                append(image,
                       thumbnailRecord(format, thumb, output.name, output.bytes.size()));
                append(output.bytes, thumb.pixels);
            }
            put(image, 8, image.size());
            if (record)
                images.records[*record] = std::move(image);
            else
                images.records.push_back(std::move(image));
            track.artworkUpdate = TrackArtwork{id, originalSize};
            ++count;
        }
        if (!count)
            return true;
        for (const auto& format : formats) {
            const bool known =
                std::any_of(files.records.begin(), files.records.end(), [&](const Bytes& r) {
                    return tag(r, 0, "mhif") && r.size() >= 24 && get(r, 16) == format.id;
                });
            if (!known) {
                auto info = chunk("mhif", 0x7c);
                put(info, 8, info.size());
                put(info, 16, format.id);
                put(info, 20, format.width * format.height * 2);
                files.records.push_back(std::move(info));
            }
        }
        static std::atomic<unsigned> serial{};
        const auto stage = folder / (".podbox-stage-" + std::to_string(::getpid()) + "-" +
                                     std::to_string(serial++));
        if (!fs::create_directory(stage))
            throw std::runtime_error("cannot stage artwork update");
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code e;
                fs::remove_all(path, e);
            }
        } cleanup{stage};
        const auto bytes = serialize(db);
        for (const auto& [id, output] : outputs)
            if (!write(stage / output.name, output.bytes))
                throw std::runtime_error("cannot write thumbnail file");
        if (!write(stage / "ArtworkDB", bytes))
            throw std::runtime_error("cannot stage ArtworkDB");
        if (fs::exists(index, ec) != (bool)original || (original && read(index) != *original))
            throw std::runtime_error("artwork changed during the save");
        // Immutable new files first. A failed/aborted commit leaves at worst
        // unreferenced new thumbnails, never truncated existing images.
        for (const auto& [id, output] : outputs) {
            fs::create_hard_link(stage / output.name, folder / output.name, ec);
            if (ec) {
                // FAT does not support hard links; copy_file fails if the
                // destination exists and therefore cannot overwrite iTunes.
                ec.clear();
                if (!fs::copy_file(stage / output.name, folder / output.name, ec) || ec)
                    throw std::runtime_error("cannot install thumbnail file");
                const int fd = ::open((folder / output.name).c_str(), O_RDONLY);
                if (fd < 0)
                    throw std::runtime_error("cannot flush thumbnail file");
                bool ok = false;
#ifdef F_FULLFSYNC
                ok = ::fcntl(fd, F_FULLFSYNC) == 0;
#endif
                if (!ok)
                    ok = ::fsync(fd) == 0;
                ::close(fd);
                if (!ok)
                    throw std::runtime_error("cannot flush thumbnail file");
            }
        }
        if (original) {
            for (int i = 4; i >= 1; --i) {
                const auto prev = index.string() + ".podbox-bak." + std::to_string(i);
                if (fs::exists(prev, ec)) {
                    fs::rename(prev, index.string() + ".podbox-bak." + std::to_string(i + 1),
                               ec);
                    if (ec)
                        throw std::runtime_error("cannot rotate artwork backups");
                }
            }
            if (!write(stage / "backup", *original))
                throw std::runtime_error("cannot back up artwork database");
            fs::rename(stage / "backup", index.string() + ".podbox-bak.1", ec);
            if (ec)
                throw std::runtime_error("cannot install artwork backup");
        }
        fs::rename(stage / "ArtworkDB", index, ec);
        if (ec)
            throw std::runtime_error("cannot install artwork database");
        if (added)
            *added = count;
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("iPod artwork was not saved: ") + e.what();
        return false;
    }
}
}  // namespace podbox
