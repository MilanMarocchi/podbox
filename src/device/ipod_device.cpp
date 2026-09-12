#include "device/ipod_device.h"

#include "device/usb_serial.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <string_view>

#include <unistd.h>  // sync

#ifdef __APPLE__
#include <sys/mount.h>
#endif

namespace fs = std::filesystem;

namespace podbox {
namespace {

#ifdef __APPLE__
constexpr const char* kVolumesRoot = "/Volumes";
#else
constexpr const char* kVolumesRoot = "/media";
#endif

struct ModelEntry {
    const char* code;
    const char* name;
};

// Classic-line iPod model numbers (matched by prefix). Cosmetic only — the
// database format/hash requirements are read from the iTunesDB header itself.
constexpr ModelEntry kModels[] = {
    // 1st-4th gen and photo
    {"M8513", "iPod 1st gen (5 GB)"},
    {"M8541", "iPod 1st gen (5 GB)"},
    {"M8697", "iPod 1st gen (5 GB)"},
    {"M8709", "iPod 1st gen (10 GB)"},
    {"M8737", "iPod 2nd gen (10 GB)"},
    {"M8740", "iPod 2nd gen (20 GB)"},
    {"M8946", "iPod 3rd gen (10 GB)"},
    {"M8948", "iPod 3rd gen (15 GB)"},
    {"M8976", "iPod 3rd gen (30 GB)"},
    {"M9244", "iPod 3rd gen (20 GB)"},
    {"M9245", "iPod 3rd gen (40 GB)"},
    {"M9282", "iPod 4th gen (20 GB)"},
    {"M9268", "iPod 4th gen (40 GB)"},
    {"M9787", "iPod U2 (20 GB)"},
    {"M9585", "iPod photo (40 GB)"},
    {"M9586", "iPod photo (60 GB)"},
    {"M9829", "iPod photo (60 GB)"},
    {"MA079", "iPod photo (30 GB)"},
    {"MA127", "iPod U2 photo (20 GB)"},
    // mini
    {"M9160", "iPod mini (4 GB, silver)"},
    {"M9434", "iPod mini (4 GB, gold)"},
    {"M9435", "iPod mini (4 GB, pink)"},
    {"M9436", "iPod mini (4 GB, blue)"},
    {"M9437", "iPod mini (4 GB, green)"},
    {"M9800", "iPod mini 2nd gen (4 GB, silver)"},
    {"M9802", "iPod mini 2nd gen (4 GB, blue)"},
    {"M9804", "iPod mini 2nd gen (4 GB, pink)"},
    {"M9806", "iPod mini 2nd gen (4 GB, green)"},
    {"M9801", "iPod mini 2nd gen (6 GB, silver)"},
    {"M9803", "iPod mini 2nd gen (6 GB, blue)"},
    {"M9805", "iPod mini 2nd gen (6 GB, pink)"},
    {"M9807", "iPod mini 2nd gen (6 GB, green)"},
    // nano 1st/2nd gen
    {"MA350", "iPod nano (1 GB, white)"},
    {"MA352", "iPod nano (1 GB, black)"},
    {"MA004", "iPod nano (2 GB, white)"},
    {"MA099", "iPod nano (2 GB, black)"},
    {"MA005", "iPod nano (4 GB, white)"},
    {"MA107", "iPod nano (4 GB, black)"},
    {"MA477", "iPod nano 2nd gen (2 GB, silver)"},
    {"MA426", "iPod nano 2nd gen (4 GB, silver)"},
    {"MA428", "iPod nano 2nd gen (4 GB, blue)"},
    {"MA487", "iPod nano 2nd gen (4 GB, green)"},
    {"MA489", "iPod nano 2nd gen (4 GB, pink)"},
    {"MA725", "iPod nano 2nd gen (4 GB, red)"},
    {"MA497", "iPod nano 2nd gen (8 GB, black)"},
    {"MA726", "iPod nano 2nd gen (8 GB, red)"},
    // 5th/5.5th gen ("iPod with video")
    {"MA002", "iPod 5th gen (30 GB, white)"},
    {"MA146", "iPod 5th gen (30 GB, black)"},
    {"MA003", "iPod 5th gen (60 GB, white)"},
    {"MA147", "iPod 5th gen (60 GB, black)"},
    {"MA444", "iPod 5.5th gen (30 GB, white)"},
    {"MA446", "iPod 5.5th gen (30 GB, black)"},
    {"MA448", "iPod 5.5th gen (80 GB, white)"},
    {"MA450", "iPod 5.5th gen (80 GB, black)"},
    // nano 3rd-4th gen (writes need hash58)
    {"MA978", "iPod nano 3rd gen (4 GB, silver)"},
    {"MB249", "iPod nano 3rd gen (8 GB, silver)"},
    {"MB253", "iPod nano 3rd gen (8 GB, blue)"},
    {"MB255", "iPod nano 3rd gen (8 GB, green)"},
    {"MB257", "iPod nano 3rd gen (8 GB, black)"},
    {"MB261", "iPod nano 3rd gen (8 GB, red)"},
    {"MB598", "iPod nano 4th gen (8 GB, silver)"},
    {"MB732", "iPod nano 4th gen (8 GB, blue)"},
    {"MB735", "iPod nano 4th gen (8 GB, pink)"},
    {"MB739", "iPod nano 4th gen (8 GB, purple)"},
    // nano 5th gen (writes need hash72, iTunesCDB)
    {"MC02", "iPod nano 5th gen (8 GB)"},
    {"MC03", "iPod nano 5th gen (8 GB)"},
    {"MC04", "iPod nano 5th gen (8 GB)"},
    {"MC05", "iPod nano 5th gen (16 GB)"},
    {"MC06", "iPod nano 5th gen (16 GB)"},
    {"MC07", "iPod nano 5th gen (16 GB)"},
    // classic (writes need hash58)
    {"MB029", "iPod classic (80 GB, silver)"},
    {"MB147", "iPod classic (80 GB, black)"},
    {"MB145", "iPod classic (160 GB, silver)"},
    {"MB150", "iPod classic (160 GB, black)"},
    {"MB562", "iPod classic (120 GB, silver)"},
    {"MB565", "iPod classic (120 GB, black)"},
    {"MC293", "iPod classic (160 GB, silver)"},
    {"MC297", "iPod classic (160 GB, black)"},
    // nano 6th/7th gen (hashAB plus the SQLite companion database set)
    {"MC52", "iPod nano 6th gen"},
    {"MC68", "iPod nano 6th gen"},
    {"MC69", "iPod nano 6th gen"},
    {"MD47", "iPod nano 7th gen"},
    {"MKN", "iPod nano 7th gen"},
};

std::string trim(std::string_view s) {
    const char* ws = " \t\r\n";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(ws);
    return std::string(s.substr(begin, end - begin + 1));
}

// SysInfo is a plain "Key: value" text file written by the iPod firmware at
// /iPod_Control/Device/SysInfo.
std::map<std::string, std::string> parseSysInfo(const fs::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        out[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }
    return out;
}

// ModelNumStr looks like "xMA146" or "MA146LL"; strip the leading 'x' and
// match against known codes.
std::string cleanModelNumber(std::string value) {
    if (!value.empty() && (value[0] == 'x' || value[0] == 'X'))
        value.erase(0, 1);
    return value;
}

std::string lookupModelName(const std::string& modelNumber) {
    for (const auto& entry : kModels) {
        if (modelNumber.rfind(entry.code, 0) == 0) return entry.name;
    }
    // SysInfo can be empty on restored or flash-modded iPods.
    return modelNumber.empty() ? "iPod" : "iPod (" + modelNumber + ")";
}

// SysInfoExtended is an XML plist; grab a simple <key>K</key><string>V</string>
// pair without dragging in a plist library.
std::string plistString(const std::string& xml, const std::string& key) {
    const auto kpos = xml.find("<key>" + key + "</key>");
    if (kpos == std::string::npos) return {};
    const auto spos = xml.find("<string>", kpos);
    if (spos == std::string::npos) return {};
    const auto vpos = spos + 8;
    const auto epos = xml.find("</string>", vpos);
    if (epos == std::string::npos) return {};
    return xml.substr(vpos, epos - vpos);
}

std::string readWholeFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string detectFilesystem(const fs::path& mountPoint) {
#ifdef __APPLE__
    struct statfs sb;
    if (statfs(mountPoint.c_str(), &sb) == 0) {
        const std::string type = sb.f_fstypename;
        if (type == "msdos") return "FAT32 (Windows format)";
        if (type == "hfs") return "HFS+ (Mac format)";
        return type;
    }
#else
    (void)mountPoint;
#endif
    return {};
}

// visibleBuildID values look like "0x05008000 (5.0)"; prefer the readable
// part in parentheses when present.
std::string cleanFirmwareVersion(const std::string& value) {
    auto open = value.find('(');
    auto close = value.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open)
        return value.substr(open + 1, close - open - 1);
    return value;
}

}  // namespace

std::optional<IpodInfo> describeIpodMount(const fs::path& mount) {
    std::error_code ec;
    const fs::path control = mount / "iPod_Control";
    if (!fs::is_directory(control, ec)) return std::nullopt;

    IpodInfo info;
    info.kind = DeviceKind::Ipod;
    info.mountPoint = mount;
    info.musicDirectory = fs::path("iPod_Control") / "Music";
    info.volumeName = mount.filename().string();
    info.capabilities = {/*playlists=*/true, /*ratings=*/true,
                         /*playCounts=*/true,
                         /*databaseBackups=*/true};
    info.originalExtensions = {".mp3", ".m4a", ".m4b", ".aac",
                               ".wav", ".aif", ".aiff"};

    auto sysinfo = parseSysInfo(control / "Device" / "SysInfo");
    if (auto it = sysinfo.find("ModelNumStr"); it != sysinfo.end())
        info.modelNumber = cleanModelNumber(it->second);
    if (auto it = sysinfo.find("pszSerialNumber"); it != sysinfo.end())
        info.serialNumber = it->second;
    if (auto it = sysinfo.find("visibleBuildID"); it != sysinfo.end())
        info.firmwareVersion = cleanFirmwareVersion(it->second);

    if (info.modelNumber.empty() || info.serialNumber.empty()) {
        const std::string xml =
            readWholeFile(control / "Device" / "SysInfoExtended");
        if (!xml.empty()) {
            if (info.modelNumber.empty())
                info.modelNumber =
                    cleanModelNumber(plistString(xml, "ModelNumStr"));
            if (info.serialNumber.empty())
                info.serialNumber = plistString(xml, "SerialNumber");
            info.firewireGuid = plistString(xml, "FireWireGUID");
        }
    }
    const auto usb = usbIdentityForMount(mount);
    info.usbVendorId = usb.vendorId;
    info.usbProductId = usb.productId;
    if (info.firewireGuid.empty()) info.firewireGuid = usb.serial;

    info.modelName = lookupModelName(info.modelNumber);
    if (info.modelNumber.empty() && usb.vendorId == 0x05ac &&
        usb.productId == 0x1209)
        info.modelName = "iPod video (5th/5.5th gen)";
    info.filesystem = detectFilesystem(mount);
    info.writable = ::access((control / "iTunes").c_str(), W_OK) == 0 &&
                    ::access((control / "Music").c_str(), W_OK) == 0;
    const fs::space_info space = fs::space(mount, ec);
    if (!ec) {
        info.capacityBytes = space.capacity;
        info.freeBytes = space.free;
    }
    return info;
}

fs::path ipodDatabasePath(const fs::path& mount) {
    const fs::path dir = mount / "iPod_Control" / "iTunes";
    std::error_code ec;
    const fs::path cdb = dir / "iTunesCDB";
    if (fs::exists(cdb, ec) && fs::file_size(cdb, ec) > 0) return cdb;
    return dir / "iTunesDB";
}

bool isIpodVideo(const IpodInfo& device) {
    bool video = device.usbVendorId == 0x05ac && device.usbProductId == 0x1209;
    if (!device.modelNumber.empty()) {
        video = false;
        for (const char* code : {"MA002", "MA146", "MA003", "MA147",
                                 "MA444", "MA446", "MA448", "MA450"})
            if (device.modelNumber.rfind(code, 0) == 0) video = true;
    }
    return device.isIpod() && video;
}

ParseResult loadIpodLibrary(const IpodInfo& device) {
    const fs::path dir = device.mountPoint / "iPod_Control" / "iTunes";
    const fs::path music = device.mountPoint / "iPod_Control" / "Music";
    // An empty, unreadable or corrupt database is still an existing database.
    // In particular, a nano's zero-byte iTunesDB must not invite an unsigned
    // replacement for its compressed/signed library.
    for (const char* name : {"iTunesDB", "iTunesCDB"}) {
        std::error_code ec;
        const auto status = fs::symlink_status(dir / name, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return {std::nullopt, "Could not inspect this iPod's database: " +
                                      ec.message()};
        if (fs::exists(status)) return parseItunesDb(ipodDatabasePath(device.mountPoint));
    }

    // Apple USB 05ac:1209 identifies the video family (also documented by
    // Rockbox's firmware/export/config/ipodvideo.h). These models use the
    // unhashed database dialect our writer can create without an Apple seed.
    if (!isIpodVideo(device))
        return {std::nullopt,
                "No music database found. Set up this iPod in Finder or "
                "iTunes once, then reconnect it to PodBox"};
    if (!device.writable)
        return {std::nullopt, "This iPod is mounted read-only"};

    // Restore leaves empty Fxx music folders and may leave iTunesControl,
    // which is restore metadata, not an iTunesDB. Keep it untouched. Refuse
    // initialization when songs, backups or companion databases remain.
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || !fs::is_directory(music, ec))
        return {std::nullopt, "The restored iPod's music folders are missing"};
    fs::directory_iterator entry(dir, ec), end;
    for (; !ec && entry != end; entry.increment(ec)) {
        const auto name = entry->path().filename().string();
        if (name != "iTunesControl" && name != ".DS_Store")
            return {std::nullopt,
                    "No music database found, but iPod database files remain. "
                    "Restore a database backup or set up the iPod in Finder"};
    }
    if (ec) return {std::nullopt, "Could not inspect this iPod: " + ec.message()};
    fs::recursive_directory_iterator song(music, ec), songsEnd;
    for (; !ec && song != songsEnd; song.increment(ec)) {
        const auto status = song->symlink_status(ec);
        if (ec) break;
        if (!fs::is_directory(status) &&
            !(fs::is_regular_file(status) &&
              song->path().filename() == ".DS_Store"))
            return {std::nullopt,
                    "No music database found, but music files remain. "
                    "Use Recover Music to rebuild the song list, or restore "
                    "a database backup before syncing"};
    }
    if (ec) return {std::nullopt, "Could not inspect this iPod: " + ec.message()};
    Library library;
    library.masterName = device.volumeName;
    library.version = 0x19;
    return {std::move(library), {}};
}

std::vector<IpodInfo> findIpods() {
    std::vector<IpodInfo> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kVolumesRoot, ec))
        if (auto info = describeIpodMount(entry.path()))
            result.push_back(std::move(*info));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.volumeName < b.volumeName;
    });
    return result;
}

std::optional<IpodInfo> findIpod() {
    auto devices = findIpods();
    if (devices.empty()) return std::nullopt;
    return std::move(devices.front());
}

std::optional<DeviceInfo> describeFilesystemDevice(
    const fs::path& mountPoint) {
    std::error_code ec;
    if (!fs::is_directory(mountPoint, ec) ||
        fs::is_directory(mountPoint / "iPod_Control", ec))
        return std::nullopt;

    fs::path musicDirectory;
    for (const fs::path& candidate :
         {fs::path("MUSIC"), fs::path("Music"), fs::path("music"),
          fs::path("Storage Media") / "Music"}) {
        ec.clear();
        if (fs::is_directory(mountPoint / candidate, ec)) {
            musicDirectory = candidate;
            break;
        }
    }
    if (musicDirectory.empty()) return std::nullopt;

    DeviceInfo info;
    info.kind = DeviceKind::Filesystem;
    info.mountPoint = mountPoint;
    info.musicDirectory = musicDirectory;
    info.volumeName = mountPoint.filename().string();

    std::string lowerName = info.volumeName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    info.modelName = lowerName.find("walkman") != std::string::npos
                         ? "Sony Walkman (USB storage)"
                         : "Filesystem media player";
    info.filesystem = detectFilesystem(mountPoint);
    info.writable = ::access((mountPoint / musicDirectory).c_str(), W_OK) == 0;
    info.capabilities = {/*playlists=*/true, /*ratings=*/false,
                         /*playCounts=*/false,
                         /*databaseBackups=*/false};

    // These are the formats PodBox can currently inspect and transfer. Sony
    // A30/A40/A50-class players support all of them, including FLAC, so the
    // filesystem profile does not perform the iPod-only FLAC conversion.
    info.originalExtensions = {".mp3", ".m4a", ".m4b", ".aac", ".wav",
                               ".aif", ".aiff", ".flac"};

    const fs::space_info space = fs::space(mountPoint, ec);
    if (!ec) {
        info.capacityBytes = space.capacity;
        info.freeBytes = space.free;
    }
    return info;
}

std::vector<DeviceInfo> findMediaDevicesAt(const fs::path& volumesRoot) {
    std::vector<DeviceInfo> devices;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(volumesRoot, ec))
        if (auto ipod = describeIpodMount(entry.path()))
            devices.push_back(std::move(*ipod));
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        return a.volumeName < b.volumeName;
    });

    std::vector<std::pair<int, DeviceInfo>> filesystemDevices;
    ec.clear();
    for (const auto& entry : fs::directory_iterator(volumesRoot, ec)) {
        auto device = describeFilesystemDevice(entry.path());
        if (!device) continue;
        // Prefer a player we have managed before, then an explicitly named
        // Walkman, then the conventional all-caps MUSIC layout. This avoids
        // an unrelated external archive with a `Music` folder winning merely
        // because directory iteration happened to return it first.
        int score = 0;
        std::error_code markerError;
        if (fs::exists(entry.path() / ".podbox" /
                           "filesystem-player.tsv",
                       markerError))
            score += 100;
        if (device->modelName.find("Sony Walkman") != std::string::npos)
            score += 50;
        if (device->musicDirectory == fs::path("MUSIC")) score += 10;
        filesystemDevices.emplace_back(score, std::move(*device));
    }
    std::sort(filesystemDevices.begin(), filesystemDevices.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second.volumeName < b.second.volumeName;
              });
    for (auto& [score, device] : filesystemDevices) {
        (void)score;
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<DeviceInfo> findMediaDevices() {
    return findMediaDevicesAt(kVolumesRoot);
}

std::optional<DeviceInfo> findMediaDevice() {
    auto devices = findMediaDevices();
    if (devices.empty()) return std::nullopt;
    return std::move(devices.front());
}

namespace {

// Runs a command, capturing stderr, and returns its exit status. Arguments
// are shell-quoted so paths with spaces are safe.
int runCapture(const std::string& cmd, std::string* out) {
    std::array<char, 256> buf{};
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return -1;
    while (std::fgets(buf.data(), int(buf.size()), pipe))
        if (out) *out += buf.data();
    return pclose(pipe);
}

std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    return q + "'";
}

}  // namespace

bool ejectDevice(const fs::path& mountPoint, std::string* error) {
    ::sync();  // flush pending writes before we detach
    std::string output;
#ifdef __APPLE__
    const int rc =
        runCapture("diskutil eject " + shellQuote(mountPoint.string()), &output);
#else
    const int rc =
        runCapture("umount " + shellQuote(mountPoint.string()), &output);
#endif
    if (rc != 0) {
        if (error) {
            *error = output.empty() ? "Eject failed" : output;
            while (!error->empty() &&
                   (error->back() == '\n' || error->back() == '\r'))
                error->pop_back();
        }
        return false;
    }
    return true;
}

std::string formatBytes(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1000ull * 1000 * 1000)
        std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / 1e9);
    else if (bytes >= 1000ull * 1000)
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(bytes / 1000));
    return buf;
}

}  // namespace podbox
