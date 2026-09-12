#pragma once

#include "device/media_device.h"
#include "util/background_job.h"
#include <functional>
#include <unordered_set>

#include <filesystem>
#include <vector>

namespace podbox {

// Polls mounted volumes for a supported player. Safe to call every frame;
// filesystem scans run on a worker and are rate-limited internally.
class DeviceWatcher {
public:
    explicit DeviceWatcher(std::function<std::vector<DeviceInfo>()> scan = findMediaDevices)
        : scan_(std::move(scan)) {}
    bool busy() const { return job_.busy(); }
    void update(double nowSeconds, bool allowScan = true);
    const std::vector<DeviceInfo>& devices() const { return devices_; }
    const DeviceInfo* find(const std::filesystem::path& mount) const;
    void forget(const std::filesystem::path& mount);

private:
    static constexpr double kScanIntervalSeconds = 2.0;
    double lastScan_ = -kScanIntervalSeconds;
    std::vector<DeviceInfo> devices_;
    std::function<std::vector<DeviceInfo>()> scan_;
    BackgroundJob<std::vector<DeviceInfo>> job_;
    std::unordered_set<std::string> forgotten_;
};

}  // namespace podbox
