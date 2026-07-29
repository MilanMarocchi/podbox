#include "device/device_watcher.h"

namespace podbox {

void DeviceWatcher::update(double nowSeconds) {
    if (nowSeconds - lastScan_ < kScanIntervalSeconds) return;
    lastScan_ = nowSeconds;
    device_ = findIpod();
}

}  // namespace podbox
