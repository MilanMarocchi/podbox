#pragma once
#include "device/filesystem_player.h"
#include "device/ipod_recovery.h"
#include "itdb/itunessd.h"
#include "library/fingerprint_store.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace podbox {
// Copyable device state. Disk operations run on an owned snapshot; App
// installs completed results on the UI thread.
struct DeviceSession {
    const DeviceInfo* activeDevice() const { return loadedDeviceInfo_ ? &*loadedDeviceInfo_ : nullptr; }
    bool connectedIpod() const { return activeDevice() && activeDevice()->isIpod(); }
    void setStatus(const std::string& message) { status = message; }
    void load(const DeviceInfo& device);
    bool writeDatabase();
    int deleteTracks(const std::vector<std::uint32_t>& ids,
        const std::unordered_map<std::uint32_t, std::uint32_t>* remap);
    std::vector<std::pair<std::filesystem::path, Track>> pendingFileTags_;

    bool restoreDatabase(const std::filesystem::path& chosen);
    bool writesSupported() const;
    std::string writeBlockReason() const;
    bool appleMusicSyncing() const;
    void verifyChecksum();
    std::filesystem::path dbFilePath() const;
    void rotateBackups(const std::filesystem::path& path);
    std::vector<std::filesystem::path> availableBackups() const;
    std::string status;
    std::string recoveryBlockReason_ = "Player library is still loading";
    std::optional<Library> library_;
    bool playCountsUnmatched_ = false;
    bool hash58Verified_ = false;
    std::vector<std::uint8_t> hash58Guid_;
    bool hash72Verified_ = false;
    std::vector<std::uint8_t> hash72Iv_;
    std::vector<std::uint8_t> hash72Rndpart_;
    bool hashAbVerified_ = false;
    std::vector<std::uint8_t> hashAbUuid_;
    std::vector<std::uint8_t> hashAbNonce_;
    ItunesSdKind itunesSdKind_ = ItunesSdKind::None;
    std::string libraryError_;
    std::filesystem::path loadedMount_;
    std::optional<DeviceInfo> loadedDeviceInfo_;
    FilesystemPlayerState filesystemState_;
    std::unordered_set<std::uint32_t> managedFilesystemTrackIds_;
    std::unordered_map<std::uint32_t, int> trackIndexById_;
    FingerprintStore fingerprints_;
    std::uint32_t nextTrackId_ = 100;
    std::filesystem::file_time_type ownWriteTime_{};
    std::unordered_set<std::uint64_t> refreshPlaylistVoiceOver_;
    std::unordered_set<std::uint64_t> removedPlaylistVoiceOver_;
};
} // namespace podbox
