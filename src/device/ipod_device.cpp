#include "device/ipod_device.h"

#include "device/usb_serial.h"

#include <array>
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

std::optional<IpodInfo> findIpod() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kVolumesRoot, ec)) {
        const fs::path control = entry.path() / "iPod_Control";
        if (!fs::is_directory(control, ec)) continue;

        IpodInfo info;
        info.mountPoint = entry.path();
        info.volumeName = entry.path().filename().string();

        auto sysinfo = parseSysInfo(control / "Device" / "SysInfo");
        if (auto it = sysinfo.find("ModelNumStr"); it != sysinfo.end())
            info.modelNumber = cleanModelNumber(it->second);
        if (auto it = sysinfo.find("pszSerialNumber"); it != sysinfo.end())
            info.serialNumber = it->second;
        if (auto it = sysinfo.find("visibleBuildID"); it != sysinfo.end())
            info.firmwareVersion = cleanFirmwareVersion(it->second);

        // SysInfo can be empty (common after flash mods/restores); fall back
        // to SysInfoExtended, which also carries the FireWire GUID needed to
        // hash DBs for 6th-gen+ devices.
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
        // Plenty of iPods have no SysInfoExtended at all — restored units,
        // flash mods, anything whose SysInfo was written empty. The USB serial
        // is the same value, and without it a device that signs its database
        // can never be verified and so can never be written.
        if (info.firewireGuid.empty())
            info.firewireGuid = usbSerialForMount(entry.path());

        info.modelName = lookupModelName(info.modelNumber);
        info.filesystem = detectFilesystem(entry.path());

        const fs::space_info space = fs::space(entry.path(), ec);
        if (!ec) {
            info.capacityBytes = space.capacity;
            info.freeBytes = space.free;
        }
        return info;
    }
    return std::nullopt;
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
