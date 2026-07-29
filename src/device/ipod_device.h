#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace podbox {

struct IpodInfo {
    std::filesystem::path mountPoint;
    std::string volumeName;
    std::string modelNumber;     // e.g. "MA146"
    std::string modelName;       // e.g. "iPod 5th gen (30 GB, black)"
    std::string serialNumber;
    std::string firmwareVersion;
    std::string filesystem;      // e.g. "FAT32 (Windows format)"
    std::string firewireGuid;    // needed to hash DBs for 6th-gen+ devices
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
};

// Scans mounted volumes for one containing an iPod_Control directory and
// returns its details, or nullopt when no iPod is connected.
std::optional<IpodInfo> findIpod();

// Flushes filesystem buffers and unmounts/ejects the device so it can be
// safely unplugged. Returns false (with a message in `error`) on failure.
bool ejectDevice(const std::filesystem::path& mountPoint, std::string* error);

// Formats a byte count as a human-readable string, e.g. "27.8 GB".
std::string formatBytes(std::uint64_t bytes);

}  // namespace podbox
