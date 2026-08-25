#include "device/device_watcher.h"

#include <algorithm>

namespace podbox {

void DeviceWatcher::update(double nowSeconds) {
    if (nowSeconds - lastScan_ < kScanIntervalSeconds) return;
    lastScan_ = nowSeconds;
    devices_ = findMediaDevices();
}

const DeviceInfo* DeviceWatcher::find(
    const std::filesystem::path& mount) const {
    for (const DeviceInfo& device : devices_)
        if (device.mountPoint == mount) return &device;
    return nullptr;
}

void DeviceWatcher::forget(const std::filesystem::path& mount) {
    std::erase_if(devices_, [&](const DeviceInfo& device) {
        return device.mountPoint == mount;
    });
}

}  // namespace podbox
