#pragma once

#include "device/media_device.h"

#include <filesystem>
#include <vector>

namespace podbox {

// Polls mounted volumes for a supported player. Safe to call every frame;
// actual
// filesystem scans are rate-limited internally.
class DeviceWatcher {
public:
    void update(double nowSeconds);
    const std::vector<DeviceInfo>& devices() const { return devices_; }
    const DeviceInfo* find(const std::filesystem::path& mount) const;
    void forget(const std::filesystem::path& mount);

private:
    static constexpr double kScanIntervalSeconds = 2.0;
    double lastScan_ = -kScanIntervalSeconds;
    std::vector<DeviceInfo> devices_;
};

}  // namespace podbox
