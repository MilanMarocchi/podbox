#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace podbox {

enum class DeviceKind {
    Ipod,
    Filesystem,
};

struct DeviceCapabilities {
    bool playlists = false;
    bool ratings = false;
    bool playCounts = false;
    bool databaseBackups = false;
};

// Everything the UI and sync pipeline need to know about a connected player.
// An iPod owns a private database; a filesystem player asks its firmware to
// index tagged files below musicDirectory after it is disconnected.
struct DeviceInfo {
    DeviceKind kind = DeviceKind::Ipod;
    std::filesystem::path mountPoint;
    std::filesystem::path musicDirectory;  // relative to mountPoint
    std::string volumeName;
    std::string modelNumber;
    std::string modelName;
    std::string serialNumber;
    std::string firmwareVersion;
    std::string filesystem;
    std::string firewireGuid;  // iPod database signing only
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
    bool writable = true;
    DeviceCapabilities capabilities;

    // Lowercase extensions including the dot. ImportFormat::Original copies
    // these unchanged; an unsupported source follows the device's fallback.
    std::unordered_set<std::string> originalExtensions;

    bool isIpod() const { return kind == DeviceKind::Ipod; }
};

// Describes a mounted, folder-based player. Public so tests and a future
// manual "use this volume" picker can probe one mount without scanning the
// whole machine.
std::optional<DeviceInfo> describeFilesystemDevice(
    const std::filesystem::path& mountPoint);

// All mounted players, with iPods first and likely dedicated filesystem
// players ahead of generic volumes. MTP devices deliberately do not pretend
// to be paths; they will use a later transport.
std::vector<DeviceInfo> findMediaDevices();

// Same discovery against an explicit volumes directory. This keeps mounted
// volume enumeration testable without real hardware.
std::vector<DeviceInfo> findMediaDevicesAt(
    const std::filesystem::path& volumesRoot);

// Compatibility helper for command-line tools that only operate on one
// device. The GUI uses findMediaDevices().
std::optional<DeviceInfo> findMediaDevice();

// Flushes filesystem buffers and unmounts/ejects the device so it can be
// safely unplugged. Returns false (with a message in `error`) on failure.
bool ejectDevice(const std::filesystem::path& mountPoint, std::string* error);

// Formats a byte count as a human-readable string, e.g. "27.8 GB".
std::string formatBytes(std::uint64_t bytes);

}  // namespace podbox
