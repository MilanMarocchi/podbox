#pragma once

#include "device/media_device.h"

namespace podbox {

using IpodInfo = DeviceInfo;  // source compatibility for command-line tools

// Scans mounted volumes for one containing an iPod_Control directory and
// returns its details, or nullopt when no iPod is connected.
std::optional<IpodInfo> findIpod();
std::vector<IpodInfo> findIpods();

}  // namespace podbox
