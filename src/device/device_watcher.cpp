#include "device/device_watcher.h"

#include <algorithm>

namespace podbox {

void DeviceWatcher::update(double nowSeconds, bool allowScan) {
    if (job_.ready()) {
        try {
            auto found = job_.take();
            // A scan started before an eject must not resurrect that volume.
            std::erase_if(*found, [&](const DeviceInfo& d) {
                return forgotten_.count(d.mountPoint.string()) != 0;
            });
            devices_ = std::move(*found);
        } catch (...) {
            // Preserve the last successful discovery on transient failures.
        }
        forgotten_.clear();
    }
    if (!allowScan || job_.busy() || nowSeconds - lastScan_ < kScanIntervalSeconds) return;
    lastScan_ = nowSeconds;
    job_.start(scan_);
}

const DeviceInfo* DeviceWatcher::find(
    const std::filesystem::path& mount) const {
    for (const DeviceInfo& device : devices_)
        if (device.mountPoint == mount) return &device;
    return nullptr;
}

void DeviceWatcher::forget(const std::filesystem::path& mount) {
    if (job_.busy()) forgotten_.insert(mount.string());
    std::erase_if(devices_, [&](const DeviceInfo& device) {
        return device.mountPoint == mount;
    });
}

}  // namespace podbox
