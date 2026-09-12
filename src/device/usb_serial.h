#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace podbox {

struct UsbDeviceIdentity {
    std::string serial;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
};

// Identity of the USB device backing this particular volume, never another
// attached iPod. Product/vendor IDs survive a restore that empties SysInfo.
UsbDeviceIdentity usbIdentityForMount(const std::filesystem::path& mountPoint);

// The 16-hex-digit FireWire GUID of the USB device backing `mountPoint`, taken
// from its USB serial number, or empty when it cannot be determined.
//
// Devices normally publish this in iPod_Control/Device/SysInfoExtended, but
// that file is absent on plenty of iPods — restored ones, flash-modded ones,
// and every unit whose SysInfo was written empty. The USB serial carries the
// same value, so a device with no SysInfoExtended can still be identified.
//
// This matters because the GUID is what keys the database checksum: without
// it PodBox cannot verify, and therefore cannot enable, writes to any model
// that signs its database.
std::string usbSerialForMount(const std::filesystem::path& mountPoint);

}  // namespace podbox
