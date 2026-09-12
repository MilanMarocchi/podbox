#include "device/usb_serial.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <sys/mount.h>
#include <sys/param.h>

namespace fs = std::filesystem;

namespace podbox {
namespace {

std::string cfStringToStd(CFStringRef s) {
    if (!s) return {};
    char buf[256] = {};
    if (!CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        return {};
    return buf;
}

// The BSD name of the whole disk behind a mount point: /dev/disk6s2 -> disk6s2.
std::string bsdNameForMount(const fs::path& mountPoint) {
    struct statfs fsInfo = {};
    if (statfs(mountPoint.c_str(), &fsInfo) != 0) return {};
    std::string dev = fsInfo.f_mntfromname;  // e.g. /dev/disk6s2
    const std::string prefix = "/dev/";
    if (dev.rfind(prefix, 0) != 0) return {};
    return dev.substr(prefix.size());
}

}  // namespace

UsbDeviceIdentity usbIdentityForMount(const fs::path& mountPoint) {
    const std::string bsdName = bsdNameForMount(mountPoint);
    if (bsdName.empty()) return {};

    // MACH_PORT_NULL requests IOKit's default port on every supported macOS
    // release. kIOMainPortDefault is only exported from macOS 12 onward.
    CFMutableDictionaryRef match = IOBSDNameMatching(MACH_PORT_NULL, 0,
                                                     bsdName.c_str());
    if (!match) return {};
    // Consumes `match`.
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, match);
    if (!service) return {};

    // The partition knows nothing about USB; the serial lives on the USB
    // device several levels up, so walk the service plane towards the root
    // until something publishes one.
    UsbDeviceIdentity identity;
    io_service_t node = service;
    IOObjectRetain(node);
    for (int depth = 0; depth < 12; ++depth) {
        auto number = [node](CFStringRef key) -> std::uint16_t {
            CFTypeRef value = IORegistryEntryCreateCFProperty(
                node, key, kCFAllocatorDefault, 0);
            int result = 0;
            if (value) {
                if (CFGetTypeID(value) == CFNumberGetTypeID())
                    CFNumberGetValue(static_cast<CFNumberRef>(value),
                                     kCFNumberIntType, &result);
                CFRelease(value);
            }
            return result > 0 && result <= 0xffff ? result : 0;
        };
        const auto vendor = number(CFSTR("idVendor"));
        const auto product = number(CFSTR("idProduct"));
        CFTypeRef prop = IORegistryEntryCreateCFProperty(
            node, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
        if (prop) {
            if (CFGetTypeID(prop) == CFStringGetTypeID())
                identity.serial = cfStringToStd(static_cast<CFStringRef>(prop));
            CFRelease(prop);
        }
        if (vendor && product) {
            identity.vendorId = vendor;
            identity.productId = product;
            break;  // Do not climb into the hub and use its identity.
        }
        io_service_t parent = 0;
        if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent) !=
            KERN_SUCCESS)
            break;
        IOObjectRelease(node);
        node = parent;
    }
    IOObjectRelease(node);
    IOObjectRelease(service);
    return identity;
}

std::string usbSerialForMount(const fs::path& mountPoint) {
    return usbIdentityForMount(mountPoint).serial;
}

}  // namespace podbox
