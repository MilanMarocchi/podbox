#pragma once

#include "device/media_device.h"
#include "itdb/itunesdb.h"

namespace podbox {

using IpodInfo = DeviceInfo;  // source compatibility for command-line tools

// Scans mounted volumes for one containing an iPod_Control directory and
// returns its details, or nullopt when no iPod is connected.
std::optional<IpodInfo> findIpod();
std::vector<IpodInfo> findIpods();

std::optional<IpodInfo> describeIpodMount(const std::filesystem::path& mount);
std::filesystem::path ipodDatabasePath(const std::filesystem::path& mount);

// Read-only: load the existing database, or an empty in-memory library for
// an identified, freshly restored iPod video. The first import/sync writes
// the database through the normal transaction. Never replaces corrupt data.
ParseResult loadIpodLibrary(const IpodInfo& device);

}  // namespace podbox
