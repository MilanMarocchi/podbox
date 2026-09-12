#include "app/app.h"
#include "app/app_util.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include <imgui.h>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
namespace podbox {

void App::saveHost() { hostSavePending_ = true; }

void App::applyBackgroundWork() {
    if (encoderJob_.ready()) {
        try { mp3Available_ = *encoderJob_.take(); } catch (...) {}
        encoderChecked_ = true;
    }
    if (!encoderChecked_ && !encoderJob_.busy() && !closeRequested_)
        encoderJob_.start([] { return mp3EncoderAvailable(); });
    if (tagJob_.ready()) {
        try {
            const int failed = *tagJob_.take();
            if (failed) setStatus(std::to_string(failed) + " file tags could not be saved");
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (!pendingHostTags_.empty() && !tagJob_.busy()) {
        auto tags = std::move(pendingHostTags_);
        pendingHostTags_.clear();
        tagJob_.start([tags = std::move(tags)] {
            int failures = 0;
            for (const auto& [path, track] : tags) {
                std::string error;
                if (!writeFileTags(path, track, &error)) ++failures;
            }
            return failures;
        });
    }
    if (syncPlanJob_.ready()) {
        try {
            auto plan = *syncPlanJob_.take();
            if (!syncUi_.dirty) syncUi_.plan = std::move(plan);
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (duplicateJob_.ready()) {
        try {
            auto groups = *duplicateJob_.take();
            if (!dupes_.dirty) {
                dupes_.groups = std::move(groups);
                dupes_.enabled.assign(dupes_.groups.size(), 1);
            }
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (fingerprintSaveJob_.ready()) {
        try {
            if (!*fingerprintSaveJob_.take()) setStatus("Could not save song fingerprints");
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (fingerprintSavePending_ && !fingerprintSaveJob_.busy() &&
        !deviceJob_.busy() && !dupes_.verify.running()) {
        fingerprintSavePending_ = false;
        fingerprintSaveJob_.start([store = fingerprints_, mount = loadedMount_]() mutable { return store.save(mount); });
    }
    if (folderCheckJob_.ready()) {
        try { folderExists_ = std::move(*folderCheckJob_.take()); }
        catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (foldersOpen_ && !closeRequested_ && !folderCheckJob_.busy() && ImGui::GetTime() - lastFolderCheck_ >= 2) {
        lastFolderCheck_ = ImGui::GetTime();
        folderCheckJob_.start([folders = host_.watchFolders()] {
            std::unordered_map<std::string, bool> result;
            for (const auto& folder : folders) {
                std::error_code ec;
                result[folder.path.string()] = fs::is_directory(folder.path, ec);
            }
            return result;
        });
    }
    if (hostSaveJob_.ready()) {
        try {
            if (!*hostSaveJob_.take()) setStatus("Could not save the local library");
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (hostSavePending_ && !hostSaveJob_.busy()) {
        hostSavePending_ = false;
        hostSaveJob_.start([snapshot = host_] { return snapshot.save(); });
    }
    if (folderJob_.ready()) {
        try {
            auto picked = *folderJob_.take();
            if (!picked.empty() && !closeRequested_) {
                host_.addWatchFolder(picked);
                saveHost();
                rescanWatchFolders();
            }
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    // Keep a drop bound to the player on which it began, even if the user
    // switches sources while walking the directory tree.
    if (dropJob_.ready() && !deviceJob_.busy()) {
        try {
            auto drop = *dropJob_.take();
            if (!closeRequested_ && ejectRequestedMount_.empty()) {
                if (drop.files.empty()) setStatus("No supported audio files found");
                else if (watcher_.find(drop.mount)) queueFilesToDevice(drop.files, drop.mount);
                else setStatus("The player disconnected while finding audio files");
            }
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    if (monitorJob_.ready()) {
        try {
            auto result = *monitorJob_.take();
            if (result.mount == loadedMount_ && !deviceJob_.busy()) {
                musicSyncing_ = result.syncing;
                if (result.backups) {
                    backupRows_ = std::move(result.rows);
                    backupsLoaded_ = true;
                }
            }
        } catch (const std::exception& e) { setStatus(e.what()); }
    }
    const bool wantBackups = (restoreOpen_ || recovery_.open) && !backupsLoaded_;
    const double now = ImGui::GetTime();
    if (!closeRequested_ && ejectRequestedMount_.empty() && !deviceJob_.busy() && !monitorJob_.busy() &&
        connectedIpod() && (wantBackups || now - lastMonitor_ >= 2.0)) {
        lastMonitor_ = now;
        monitorJob_.start([snapshot = static_cast<const DeviceSession&>(*this), wantBackups] {
            MonitorResult result;
            result.mount = snapshot.loadedMount_;
            result.syncing = snapshot.appleMusicSyncing();
            result.backups = wantBackups;
            if (wantBackups) {
                for (const auto& path : snapshot.availableBackups()) {
                    const auto db = parseItunesDb(path);
                    if (!db.library) continue;
                    std::error_code ec;
                    const auto stamp = fs::last_write_time(path, ec);
                    char label[64] = "unknown time";
                    if (!ec) {
                        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            stamp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                        const std::time_t when = std::chrono::system_clock::to_time_t(sys);
                        std::tm tm{};
                        if (localtime_r(&when, &tm)) std::strftime(label, sizeof(label), "%d %b %H:%M", &tm);
                    }
                    result.rows.push_back({path, std::string(label) + "   " +
                        plural(db.library->tracks.size(), "song", "songs")});
                }
            }
            return result;
        });
    }
}
} // namespace podbox
