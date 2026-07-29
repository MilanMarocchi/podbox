#pragma once

#include "device/ipod_device.h"

#include <optional>

namespace podbox {

// Polls mounted volumes for an iPod. Safe to call every frame; actual
// filesystem scans are rate-limited internally.
class DeviceWatcher {
public:
    void update(double nowSeconds);
    const std::optional<IpodInfo>& device() const { return device_; }

private:
    static constexpr double kScanIntervalSeconds = 2.0;
    double lastScan_ = -kScanIntervalSeconds;
    std::optional<IpodInfo> device_;
};

}  // namespace podbox
