#pragma once

#include "device/ipod_device.h"
#include <atomic>
#include <map>

namespace podbox {

struct RecoveryFile {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    bool operator==(const RecoveryFile&) const = default;
};

struct IpodRecovery {
    IpodInfo device;
    Library library;
    std::map<std::filesystem::path, RecoveryFile> files;
    std::uint64_t volumeDevice = 0, volumeInode = 0;
    std::uint64_t musicBytes = 0;
    std::vector<std::string> skipped;
    std::string error;
};

// Only identified video models can reconstruct an unsigned database. Existing
// compressed/Shuffle/SQLite companions and symlinked control paths fail closed.
std::string ipodRecoveryBlockReason(const IpodInfo& device);
// Read-only; suitable for a background thread. Never follows music symlinks.
IpodRecovery scanIpodRecovery(const IpodInfo& device,
                              const std::atomic<bool>* cancel = nullptr);
// Rechecks the preview's device and files, archives damaged metadata, validates
// and atomically installs the index. Does not copy, retag or remove any audio.
bool installIpodRecovery(const IpodRecovery& recovery,
                         std::filesystem::path* archive, std::string* error);

}  // namespace podbox
