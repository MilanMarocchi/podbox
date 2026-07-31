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

std::string usbSerialForMount(const fs::path& mountPoint) {
    const std::string bsdName = bsdNameForMount(mountPoint);
    if (bsdName.empty()) return {};

    CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault, 0,
                                                     bsdName.c_str());
    if (!match) return {};
    // Consumes `match`.
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!service) return {};

    // The partition knows nothing about USB; the serial lives on the USB
    // device several levels up, so walk the service plane towards the root
    // until something publishes one.
    std::string serial;
    io_service_t node = service;
    IOObjectRetain(node);
    for (int depth = 0; depth < 12 && serial.empty(); ++depth) {
        CFTypeRef prop = IORegistryEntryCreateCFProperty(
            node, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
        if (prop) {
            if (CFGetTypeID(prop) == CFStringGetTypeID())
                serial = cfStringToStd(static_cast<CFStringRef>(prop));
            CFRelease(prop);
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
    return serial;
}

}  // namespace podbox
