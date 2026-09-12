#include "device/usb_serial.h"

namespace podbox {

UsbDeviceIdentity usbIdentityForMount(const std::filesystem::path&) { return {}; }

// Only macOS has an IOKit registry to ask. Elsewhere the GUID has to come
// from SysInfoExtended, as it always did.
std::string usbSerialForMount(const std::filesystem::path&) { return {}; }

}  // namespace podbox
