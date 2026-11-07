#pragma once
#include "device/media_device.h"
#include "itdb/itunesdb.h"
#include <string>
#include <vector>
namespace podbox {
struct IpodArtworkFormat {
    std::uint32_t id;
    int width;
    int height;
};
std::vector<IpodArtworkFormat> ipodArtworkFormats(const DeviceInfo& device);
// Adds missing artwork without renumbering/replacing existing artwork records
// or thumbnail files. New immutable thumbnails are flushed before ArtworkDB,
// and ArtworkDB before iTunesDB, so an interrupted music save leaves the old
// music database's artwork references valid. Unknown models are a no-op.
bool writeIpodArtwork(const DeviceInfo& device, Library& library, int* added,
                      std::string* error);
}  // namespace podbox
