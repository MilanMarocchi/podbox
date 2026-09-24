// App lifecycle, the frame loop, library loading, every mutation path,
// selection and playback. The drawing lives in app_chrome.cpp (the window
// chrome) and app_modals.cpp (the dialogs).

#include "app/app.h"
#include "app/app_util.h"

#include "device/ipod_device.h"
#include "itdb/hash58.h"
#include "itdb/hash72.h"
#include "itdb/hashab.h"
#include "itdb/itunessqlite.h"
#include "itdb/playcounts.h"
#include "itdb/itunessd.h"
#include "library/artwork.h"
#include "library/dedupe.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include "ui/aqua.h"
#include "ui/theme.h"
#include "util/finder.h"

#include <imgui.h>

#define GL_SILENCE_DEPRECATION
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>

#include <strings.h>  // strcasecmp

namespace fs = std::filesystem;

namespace podbox {

App::~App() {
    apple_.cancel.store(true);
    recovery_.cancel.store(true);
    scan_.cancel.store(true);
    if (recovery_.thread.joinable()) recovery_.thread.join();
    if (scan_.thread.joinable()) scan_.thread.join();
    if (apple_.thread.joinable()) apple_.thread.join();
}

bool App::prepareToClose() {
    closeRequested_ = true;
    sync_.requestStop();
    apple_.cancel.store(true);
    recovery_.cancel.store(true);
    scan_.cancel.store(true);
    dupes_.verify.cancel();
    // Keep pumping frames while the current copy/save finishes. In
    // particular, never join a transcode or diskutil process here.
    if (deviceJob_.busy() || sync_.busy() || scan_.running || apple_.busy ||
        recovery_.running || dupes_.verify.running() || dropJob_.busy() ||
        artJob_.busy() || monitorJob_.busy() || hostSaveJob_.busy() ||
        folderJob_.busy() || tagJob_.busy() || syncPlanJob_.busy() ||
        duplicateJob_.busy() || hostRemovalJob_.busy() || hostSavePending_ || !pendingHostTags_.empty() ||
        fingerprintSaveJob_.busy() || fingerprintSavePending_ || folderCheckJob_.busy() || encoderJob_.busy() || watcher_.busy()) return false;
    if (closeWithoutSaving_) return true;
    applyCompletedAdds();
    if (deviceJob_.busy()) return false;
    closeSaveFailedOpen_ = pendingDbWrite_;
    if (pendingDbWrite_) closeRequested_ = false;
    return !pendingDbWrite_;
}

bool App::writeDatabase() {
    if (!library_ || deviceJob_.busy()) return false;
    if (fingerprintSaveJob_.busy()) { pendingDbWrite_ = true; return true; }
    if (!writesSupported()) { setStatus(writeBlockReason()); return false; }
    pendingDbWrite_ = true;
    deviceJobKind_ = DeviceJobKind::Save;
    deviceJob_.start([snapshot = static_cast<const DeviceSession&>(*this)]() mutable {
        DeviceResult result;
        snapshot.status.clear();
        result.ok = snapshot.writeDatabase();
        result.session = std::move(snapshot);
        return result;
    });
    setStatus("Saving player database…");
    return true; // accepted; success/failure is reported by applyDeviceJob()
}

void App::applyDeviceJob() {
    if (!deviceJob_.ready()) return;
    DeviceResult result;
    try { result = std::move(*deviceJob_.take()); }
    catch (const std::exception& e) { result.ok = false; result.error = e.what(); }
    if (deviceJobKind_ == DeviceJobKind::Eject) {
        if (result.ok) {
            watcher_.forget(ejectingMount_);
            if (loadedMount_ == ejectingMount_) {
                static_cast<DeviceSession&>(*this) = {};
                requestedDeviceMount_.clear();
                visibleDirty_ = true;
                art_.trackId = 0;
                art_.hasImage = false;
            }
            setStatus("Ejected — safe to disconnect");
        } else setStatus("Could not eject: " + result.error);
        ejectingMount_.clear();
        return;
    }
    if (deviceJobKind_ == DeviceJobKind::Delete && !result.session.loadedMount_.empty()) {
        static_cast<DeviceSession&>(*this) = std::move(result.session);
        result.session.status = status;
        result.session.ownWriteTime_ = ownWriteTime_;
        result.session.filesystemState_ = filesystemState_;
        std::erase_if(selection_, [this](auto id) { return !trackIndexById_.count(id); });
        if (!trackIndexById_.count(selectedTrackId_)) selectedTrackId_ = selection_.empty() ? 0 : selection_.back();
        selectionAnchor_ = selectedTrackId_;
        visibleDirty_ = true;
    }
    if (deviceJobKind_ == DeviceJobKind::Save || deviceJobKind_ == DeviceJobKind::Delete) {
        if (result.ok) {
            filesystemState_ = std::move(result.session.filesystemState_);
            ownWriteTime_ = result.session.ownWriteTime_;
            pendingFileTags_.clear();
            refreshPlaylistVoiceOver_.clear();
            removedPlaylistVoiceOver_.clear();
            pendingDbWrite_ = false;
            batchWriteFailed_ = false;
            setStatus(lastBatchAdded_ || lastBatchSkipped_
                          ? importSummary(lastBatchAdded_, lastBatchSkipped_)
                          : deviceJobKind_ == DeviceJobKind::Delete
                                ? "Removed " + plural(result.removed, "song", "songs")
                                : "Player database saved");
            lastBatchAdded_ = lastBatchSkipped_ = 0;
        } else {
            batchWriteFailed_ = true;
            pendingDbWrite_ = true;
            setStatus(result.error.empty() ? result.session.status : result.error);
            switchSource(View::Device);
        }
        backupsLoaded_ = false;
        return;
    }
    if (!result.ok) {
        libraryError_ = result.error.empty() ? result.session.status : result.error;
        setStatus(libraryError_);
        return;
    }
    if (deviceJobKind_ == DeviceJobKind::Restore ||
        deviceJobKind_ == DeviceJobKind::Recovery) {
        loadedMount_.clear();
        backupsLoaded_ = false;
        setStatus(result.session.status);
        return;
    }
    // Discovery may have observed a disconnect while the read was running.
    if (!watcher_.find(result.session.loadedMount_)) {
        loadedMount_.clear();
        return;
    }
    static_cast<DeviceSession&>(*this) = std::move(result.session);
    if (!status.empty()) setStatus(status);
    if (connectedIpod()) pullPlayCountsToHost();
    switchSource(library_ ? View::Music : View::Device);
    art_.trackId = 0;
    artworkJobPath_.clear();
    backupRows_.clear();
    backupsLoaded_ = false;
}

void App::frame() {
    applyDeviceJob();
    applyBackgroundWork();
    if (!hostLoaded_) {
        hostLoaded_ = true;
        scan_.running = true;
        scan_.result = std::make_unique<HostLibrary>();
        scan_.thread = std::thread([this] {
            try {
                if (!scan_.result->load()) scan_.result->seedDefaultWatchFolders();
            } catch (const std::exception& e) { scan_.error = e.what(); }
            scan_.finished.store(true);
        });
    }
    applyFinishedScan();
    // Drain a device copy before discovery can switch away from its target.
    // The worker may become idle between frames, but its completed tracks
    // still belong to the library that launched it.
    applyCompletedAdds();
    watcher_.update(ImGui::GetTime(), !deviceJob_.busy() && !closeRequested_ && ejectRequestedMount_.empty());
    updateLibrary();
    finishPendingDeviceCopy();
    updateArtwork();
    updatePlayback();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##podbox", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const float middleHeight =
        std::max(0.0f, vp->WorkSize.y - kToolbarHeight - kStatusBarHeight);

    ImGui::BeginDisabled(deviceJob_.busy() || !ejectRequestedMount_.empty() || closeRequested_ || hostBusy());
    drawToolbar();
    ImGui::SetCursorPos(ImVec2(0, kToolbarHeight));
    drawSidebar(middleHeight);
    ImGui::SetCursorPos(ImVec2(kSidebarWidth, kToolbarHeight));
    drawMainPanel(middleHeight);
    drawStatusBar();
    drawDeleteModal();
    drawDeletePlaylistModal();
    drawDuplicatesModal();
    drawRestoreModal();
    drawRecoveryModal();
    if (closeSaveFailedOpen_) {
        if (!ImGui::IsPopupOpen("Songs Have Not Been Saved"))
            ImGui::OpenPopup("Songs Have Not Been Saved");
        if (aqua::beginSheet("Songs Have Not Been Saved", 550.0f)) {
            aqua::heading(fonts_, "The copied songs still need a database save");
            aqua::body(fonts_, "PodBox could not save the song list. Keep the player connected and use Retry Saving Database. Quitting now leaves the copied files on the player, but they may not appear in its music library.");
            if (aqua::button("Keep PodBox Open", ImVec2(170, 0))) {
                closeSaveFailedOpen_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (aqua::button("Quit Without Saving", ImVec2(170, 0))) {
                closeWithoutSaving_ = true;
                closeSaveFailedOpen_ = false;
                ImGui::CloseCurrentPopup();
            }
            aqua::endSheet();
        }
    }
    drawFoldersModal();
    ImGui::EndDisabled();
    drawAppleMusicModal();
    ImGui::BeginDisabled(deviceJob_.busy() || !ejectRequestedMount_.empty() || closeRequested_ || hostBusy());
    drawSyncModal();
    drawGetInfoModal();

    ImGui::EndDisabled();

    if (!ejectRequestedMount_.empty()) {
        const fs::path mount = std::move(ejectRequestedMount_);
        ejectRequestedMount_.clear();
        startEject(mount);
    }

    ImGui::End();
}

void App::startEject(const fs::path& mount,
    std::function<bool(const fs::path&, std::string*)> eject) {
    if (deviceJob_.busy() || sync_.busy() || pendingDbWrite_ ||
        recovery_.running || dupes_.verify.running() || fingerprintSaveJob_.busy() ||
        fingerprintSavePending_) {
        setStatus("Wait for player operations to finish before ejecting");
    } else if (monitorJob_.busy() || artJob_.busy() || dropJob_.busy() ||
               syncPlanJob_.busy() || watcher_.busy()) {
        ejectRequestedMount_ = mount;
        setStatus("Finishing player reads before ejecting…");
    } else {
        if (mount == loadedMount_) {
            if (player_) player_->stop();
            playingTrackId_ = 0;
        }
        ejectingMount_ = mount;
        deviceJobKind_ = DeviceJobKind::Eject;
        deviceJob_.start([mount, eject = std::move(eject)] {
            DeviceResult result;
            result.ok = eject(mount, &result.error);
            return result;
        });
        setStatus("Ejecting…");
    }
}

void App::setStatus(const std::string& msg) {
    statusMsg_ = msg;
    statusMsgUntil_ = ImGui::GetTime() + 4.0;
}

const DeviceInfo* App::activeDevice() const {
    if (loadedMount_.empty()) return nullptr;
    if (const DeviceInfo* live = watcher_.find(loadedMount_)) return live;
    return loadedDeviceInfo_ && loadedDeviceInfo_->mountPoint == loadedMount_
               ? &*loadedDeviceInfo_
               : nullptr;
}

void App::activateDevice(const fs::path& mount) {
    if (mount == loadedMount_) {
        switchSource(View::Device);
        return;
    }
    if (deviceJob_.busy() || sync_.busy() || pendingDbWrite_ || dupes_.verify.running() || fingerprintSaveJob_.busy() || fingerprintSavePending_) {
        setStatus("Wait for the copy and database save to finish");
        return;
    }
    plEdit_ = {};
    syncUi_.open = false;
    dupes_.open = false;
    restoreOpen_ = false;
    recovery_.open = false;
    recovery_.cancel.store(true);
    getInfo_.open = false;
    requestedDeviceMount_ = mount;
    switchSource(View::Device);
}

bool App::connectedIpod() const {
    const DeviceInfo* device = activeDevice();
    return device && device->isIpod();
}

ImportTarget App::currentImportTarget() const {
    ImportTarget target;
    const DeviceInfo* device = activeDevice();
    if (!device) return target;
    target.mount = device->mountPoint;
    target.musicDirectory = device->musicDirectory;
    target.layout = device->isIpod()
                        ? DeviceMusicLayout::IpodFolders
                        : DeviceMusicLayout::ArtistAlbumFolders;
    target.originalExtensions = device->originalExtensions;
    target.maxSampleRate = device->maxSampleRate;
    return target;
}

ImportFormat App::currentImportFormat() const {
    const DeviceInfo* device = activeDevice();
    return device && importFormatPlayable(importFormat_,
                                          device->originalExtensions)
               ? importFormat_
               : ImportFormat::Original;
}

void App::onFilesDropped(const std::vector<std::string>& paths) {
    if (deviceJob_.busy() || !ejectRequestedMount_.empty() || closeRequested_ || dropJob_.busy()) {
        setStatus("Wait for the current player operation to finish");
        return;
    }
    if (!library_ || !activeDevice()) {
        setStatus("Connect a music player before adding songs");
        return;
    }
    const fs::path mount = loadedMount_;
    dropJob_.start([paths, mount] {
        DropResult result;
        result.mount = mount;
        std::error_code ec;
        for (const auto& p : paths) {
            if (fs::is_directory(p, ec)) {
                fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
                for (; !ec && it != end; it.increment(ec))
                    if (it->is_regular_file(ec) && isImportableAudioFile(it->path()))
                        result.files.push_back(it->path());
            } else if (isImportableAudioFile(p)) result.files.emplace_back(p);
            ec.clear();
        }
        std::sort(result.files.begin(), result.files.end());
        return result;
    });
    setStatus("Finding audio files…");
}

void App::queueFilesToDevice(const std::vector<fs::path>& files) {
    if (deviceJob_.busy() || !ejectRequestedMount_.empty() || closeRequested_) return;
    if (!activeDevice() || !library_) {
        setStatus("Connect a music player before adding songs");
        return;
    }
    // writeDatabase() refuses too, but catching it here means source files are
    // not transcoded and copied before we admit that this device is read-only.
    if (!writesSupported()) {
        setStatus(writeBlockReason());
        return;
    }
    if (batchWriteFailed_) {
        setStatus("Retry saving the database before copying more songs");
        return;
    }
    if (files.empty()) return;

    // Snapshot what is already here for the worker to check against. Building
    // it from memory keeps the drop handler off the disk.
    DupeGuard guard;
    guard.enabled = skipDuplicates_;
    if (guard.enabled) {
        guard.metaKeys.reserve(library_->tracks.size());
        for (const Track& t : library_->tracks) {
            const std::string key = duplicateKey(t, MatchMode::Exact);
            if (!key.empty()) guard.metaKeys.insert(key);
        }
        for (const Track& t : library_->tracks)
            if (const AudioFingerprint* fp = fingerprints_.get(t.dbid);
                fp && fp->ok())
                guard.hashes.insert(fp->hash);
    }
    sync_.queueAdds(files, currentImportTarget(), currentImportFormat(),
                    std::move(guard));
}

void App::queueFilesToDevice(const std::vector<fs::path>& files,
                             const fs::path& targetMount) {
    if (files.empty()) return;
    if (targetMount == loadedMount_) {
        queueFilesToDevice(files);
        return;
    }
    if (deviceJob_.busy() || sync_.busy() || pendingDbWrite_ || dupes_.verify.running() || fingerprintSaveJob_.busy() || fingerprintSavePending_) {
        setStatus("Wait for the copy and database save to finish");
        return;
    }
    pendingCopyMount_ = targetMount;
    pendingCopyFiles_ = files;
    activateDevice(targetMount);
    setStatus("Opening the selected player…");
}

void App::finishPendingDeviceCopy() {
    if (deviceJob_.busy() || pendingDbWrite_ || pendingCopyFiles_.empty() || loadedMount_ != pendingCopyMount_) return;
    std::vector<fs::path> files = std::move(pendingCopyFiles_);
    pendingCopyFiles_.clear();
    pendingCopyMount_.clear();
    queueFilesToDevice(files);
}

void App::addSelectedHostTracksToDevice(const fs::path& targetMount) {
    if (!viewingHost() || selection_.empty()) return;

    std::vector<fs::path> files;
    int unavailable = 0;
    std::error_code ec;
    files.reserve(selection_.size());
    for (const std::uint32_t id : selection_) {
        const auto found = hostIndexById_.find(id);
        if (found == hostIndexById_.end()) {
            ++unavailable;
            continue;
        }
        const fs::path path = hostView_.tracks[found->second].location;
        ec.clear();
        if (!isImportableAudioFile(path)) {
            ++unavailable;
            continue;
        }
        files.push_back(path);
    }
    if (files.empty()) {
        setStatus("The selected songs are not available on this Mac");
        return;
    }

    if (unavailable > 0)
        setStatus("Adding " + plural(files.size(), "song", "songs") +
                  "; skipped " +
                  std::to_string(unavailable) + " unavailable");
    queueFilesToDevice(files, targetMount);
}

void App::applyCompletedAdds() {
    if (!library_ || deviceJob_.busy()) return;
    static std::mt19937_64 rng{std::random_device{}()};
    for (auto& done : sync_.takeCompleted()) {
        if (!done.error.empty()) {
            setStatus(done.error);
            continue;
        }
        if (done.duplicate) {
            ++lastBatchSkipped_;
            pendingDbWrite_ = true;  // so the batch still reports itself
            continue;
        }
        done.track.id = nextTrackId_++;
        if (connectedIpod()) {
            done.track.dbid = rng();
        } else {
            done.track.dbid = filesystemTrackDbid(done.track.location);
            filesystemState_.managedTracks.insert(done.track.location);
            managedFilesystemTrackIds_.insert(done.track.id);
        }
        // Record what this track was made from, before any transcode is
        // forgotten: this is what lets a later drop of the same source file
        // be recognised even though the device holds a different encoding.
        if (done.fp.ok())
            fingerprints_.put(done.track.dbid, done.fp,
                              FingerprintStore::Origin::Source);
        trackIndexById_[done.track.id] = int(library_->tracks.size());
        library_->tracks.push_back(std::move(done.track));
        ++lastBatchAdded_;
        pendingDbWrite_ = true;
        visibleDirty_ = true;
    }
    if (pendingDbWrite_ && !batchWriteFailed_ && !sync_.busy()) {
        // Save completion owns these flags; queuing a write is not a save.
        if (!writeDatabase()) batchWriteFailed_ = true;
    }
}

bool App::restoreSupported() const {
    if (deviceJob_.busy() || sync_.busy() || pendingDbWrite_) return false;
    if (writesSupported()) return true;
    const auto* device = activeDevice();
    return !library_ && device && recoveryBlockReason_.empty();
}

void App::setTrackRating(std::uint32_t trackId, int rating) {
    if (deviceJob_.busy() || hostBusy()) return;
    if (rating < 0 || rating > 100) return;

    if (viewingHost()) {
        for (HostTrack& h : host_.tracks()) {
            if (std::uint32_t(h.id) != trackId) continue;
            h.meta.rating = std::uint8_t(rating);
            saveHost();
            rebuildHostView();
            return;
        }
        return;
    }

    const DeviceInfo* device = activeDevice();
    if (!device || !device->capabilities.ratings) return;
    if (!library_) return;
    const auto it = trackIndexById_.find(trackId);
    if (it == trackIndexById_.end()) return;
    library_->tracks[it->second].rating = std::uint8_t(rating);
    if (sortCol_ == 7) visibleDirty_ = true;
    writeDatabase();
}

void App::performDelete(std::uint32_t trackId) {
    if (!library_) return;
    const auto it = trackIndexById_.find(trackId);
    if (it == trackIndexById_.end()) return;
    const std::string title = library_->tracks[it->second].title;
    if (performDeleteMany({trackId}) > 0)
        setStatus("Removing “" + title + "”…");
}

int App::performDeleteMany(
    const std::vector<std::uint32_t>& ids,
    const std::unordered_map<std::uint32_t, std::uint32_t>* remap) {
    if (!library_ || ids.empty() || deviceJob_.busy() || sync_.busy()) return 0;
    if (!writesSupported()) { setStatus(writeBlockReason()); return 0; }
    if (std::find(ids.begin(), ids.end(), playingTrackId_) != ids.end()) {
        if (player_) player_->stop();
        playingTrackId_ = 0;
    }
    deviceJobKind_ = DeviceJobKind::Delete;
    pendingDbWrite_ = true;
    deviceJob_.start([snapshot = static_cast<const DeviceSession&>(*this), ids,
                     remap = remap ? *remap : std::unordered_map<std::uint32_t, std::uint32_t>{}]() mutable {
        DeviceResult result;
        if (snapshot.appleMusicSyncing()) {
            result.ok = false;
            snapshot.status = "Apple Music is syncing this iPod — try again when it finishes";
        } else {
            result.removed = snapshot.deleteTracks(ids, &remap);
            result.ok = snapshot.writeDatabase();
        }
        result.session = std::move(snapshot);
        return result;
    });
    setStatus("Removing songs…");
    return int(ids.size()); // queued; the worker reports the actual count
}

void App::createPlaylist(std::uint32_t withTrackId) {
    if (!library_) return;
    static std::mt19937_64 rng{std::random_device{}()};
    Playlist pl;
    pl.dbid = rng();
    if (!pl.dbid) pl.dbid = 1;
    pl.name = "New Playlist";
    // Disambiguate against existing names.
    int suffix = 1;
    bool clash = true;
    while (clash) {
        clash = false;
        for (const auto& p : library_->playlists)
            if (p.name == pl.name) {
                clash = true;
                pl.name = "New Playlist " + std::to_string(++suffix);
                break;
            }
    }
    if (withTrackId) {
        if (isSelected(withTrackId))
            pl.trackIds = selection_;
        else
            pl.trackIds.push_back(withTrackId);
    }
    library_->playlists.push_back(std::move(pl));
    playlistIndex_ = int(library_->playlists.size()) - 1;
    view_ = View::Playlist;
    visibleDirty_ = true;
    // Open inline rename on the fresh playlist.
    plEdit_.renameIndex = playlistIndex_;
    plEdit_.justOpened = true;
    std::snprintf(plEdit_.buf, sizeof(plEdit_.buf), "%s",
                  library_->playlists[playlistIndex_].name.c_str());
    writeDatabase();
}

void App::addToPlaylist(int playlistIndex,
                        const std::vector<std::uint32_t>& trackIds) {
    if (!library_ || playlistIndex < 0 ||
        playlistIndex >= int(library_->playlists.size()) || trackIds.empty())
        return;
    auto& ids = library_->playlists[playlistIndex].trackIds;
    ids.insert(ids.end(), trackIds.begin(), trackIds.end());
    if (view_ == View::Playlist && playlistIndex_ == playlistIndex)
        visibleDirty_ = true;
    writeDatabase();
}

bool App::isSelected(std::uint32_t trackId) const {
    return std::find(selection_.begin(), selection_.end(), trackId) !=
           selection_.end();
}

void App::selectOnly(std::uint32_t trackId) {
    selection_.assign(1, trackId);
    selectedTrackId_ = trackId;
    selectionAnchor_ = trackId;
}

void App::selectRow(int row, std::uint32_t trackId, bool shift, bool cmd) {
    const Library* shown = shownLibrary();
    if (!shown) return;

    if (shift && selectionAnchor_) {
        // Range over the rows as displayed, so a sorted or filtered view
        // selects what the user can actually see between the two clicks.
        int anchorRow = -1;
        for (int i = 0; i < int(visible_.size()); ++i)
            if (shown->tracks[visible_[i].second].id == selectionAnchor_) {
                anchorRow = i;
                break;
            }
        if (anchorRow >= 0) {
            selection_ =
                displayedTrackRange(*shown, visible_, anchorRow, row);
            selectedTrackId_ = trackId;
            return;
        }
    }
    if (cmd) {
        if (const auto it = std::find(selection_.begin(), selection_.end(),
                                      trackId);
            it != selection_.end()) {
            selection_.erase(it);
            if (selectedTrackId_ == trackId)
                selectedTrackId_ = selection_.empty() ? 0 : selection_.back();
            return;
        }
        selection_.push_back(trackId);
        selectedTrackId_ = trackId;
        selectionAnchor_ = trackId;
        return;
    }
    selectOnly(trackId);
}

bool App::animating() const {
    return player_ && player_->state() == PlaybackState::Playing;
}

void App::play() {
    if (deviceJob_.busy() && deviceJobKind_ == DeviceJobKind::Eject) return;
    if (!player_) return;
    const PlaybackState state = player_->state();
    if (state == PlaybackState::Playing) return;
    if (state == PlaybackState::Paused) {
        player_->play();
        return;
    }
    if (playingTrackId_) {
        playTrackId(playingTrackId_);
        return;
    }
    if (selectedTrackId_) {
        playTrackId(selectedTrackId_);
        return;
    }
    const Library* shown = shownLibrary();
    if (shown && !visible_.empty())
        playTrackId(shown->tracks[visible_[0].second].id);
}

void App::pause() {
    if (player_) player_->pause();
}

void App::togglePlayback() {
    if (player_ && player_->state() == PlaybackState::Playing)
        pause();
    else
        play();
}

void App::updatePlayback() {
    if (!player_ || playingTrackId_ == 0) return;
    if (!player_->reachedEnd()) return;
    // Repeat One re-opens the same track; everything else advances.
    if (repeat_ == Repeat::One)
        playTrackId(playingTrackId_);
    else
        playRelative(+1);
}

void App::playTrackId(std::uint32_t trackId) {
    if (deviceJob_.busy() && deviceJobKind_ == DeviceJobKind::Eject) return;
    const Library* shown = shownLibrary();
    const auto* index = shownIndex();
    if (!shown || !index) return;
    const auto it = index->find(trackId);
    if (it == index->end()) return;
    const Track& t = shown->tracks[it->second];
    if (player_->open(trackFilePath(t))) {
        playingTrackId_ = trackId;
        selectedTrackId_ = trackId;
    } else {
        setStatus("Could not play “" + t.title + "”");
    }
}

const Track* App::playingTrack() const {
    if (!playingTrackId_) return nullptr;
    auto find = [&](const Library* lib,
                    const std::unordered_map<std::uint32_t, int>* idx)
        -> const Track* {
        if (!lib || !idx) return nullptr;
        const auto it = idx->find(playingTrackId_);
        return it == idx->end() ? nullptr : &lib->tracks[it->second];
    };
    if (const Track* t = find(shownLibrary(), shownIndex())) return t;
    if (const Track* t = find(library_ ? &*library_ : nullptr, &trackIndexById_))
        return t;
    return find(&hostView_, &hostIndexById_);
}

void App::playRelative(int delta) {
    const Library* shown = shownLibrary();
    if (!shown || visible_.empty()) {
        if (player_) player_->stop();
        playingTrackId_ = 0;
        return;
    }
    // Find the currently playing row within the visible list, then step.
    int cur = -1;
    for (int i = 0; i < int(visible_.size()); ++i) {
        if (shown->tracks[visible_[i].second].id == playingTrackId_) {
            cur = i;
            break;
        }
    }
    if (shuffle_ && visible_.size() > 1) {
        // Anywhere but here, so a two-track list still alternates.
        static std::mt19937 rng{std::random_device{}()};
        int pick = cur;
        while (pick == cur)
            pick = int(rng() % visible_.size());
        playTrackId(shown->tracks[visible_[pick].second].id);
        return;
    }

    int next = cur + delta;
    if (next < 0 || next >= int(visible_.size())) {
        if (repeat_ != Repeat::All) {
            // Off either end: stop playback.
            player_->stop();
            playingTrackId_ = 0;
            return;
        }
        next = next < 0 ? int(visible_.size()) - 1 : 0;
    }
    playTrackId(shown->tracks[visible_[next].second].id);
}

void App::updateLibrary() {
    if (deviceJob_.busy() || !ejectRequestedMount_.empty() || pendingDbWrite_ || closeRequested_ ||
        dupes_.verify.running() || fingerprintSaveJob_.busy() || fingerprintSavePending_) return;
    const auto& devices = watcher_.devices();
    if (!loadedMount_.empty() && !watcher_.find(loadedMount_) && sync_.busy()) {
        setStatus("The active player disconnected while files were copying");
        return;
    }
    if (devices.empty()) {
        if (library_ || !loadedMount_.empty()) {
            if (player_) player_->stop();
            playingTrackId_ = 0;
            library_.reset();
            libraryError_.clear();
            loadedMount_.clear();
            loadedDeviceInfo_.reset();
            requestedDeviceMount_.clear();
            filesystemState_ = {};
            managedFilesystemTrackIds_.clear();
            trackIndexById_.clear();
            itunesSdKind_ = ItunesSdKind::None;
            view_ = hostView_.tracks.empty() ? View::Device : View::Library;
            playlistIndex_ = -1;
            visibleDirty_ = true;
        }
        pendingCopyFiles_.clear();
        pendingCopyMount_.clear();
        return;
    }

    const DeviceInfo* dev = nullptr;
    if (!requestedDeviceMount_.empty())
        dev = watcher_.find(requestedDeviceMount_);
    if (!requestedDeviceMount_.empty() && !dev &&
        pendingCopyMount_ == requestedDeviceMount_) {
        pendingCopyFiles_.clear();
        pendingCopyMount_.clear();
        setStatus("That player is no longer connected");
    }
    if (!dev && !loadedMount_.empty()) dev = watcher_.find(loadedMount_);
    if (!dev) dev = &devices.front();
    requestedDeviceMount_ = dev->mountPoint;
    if (dev->mountPoint == loadedMount_) return;

    if (player_) player_->stop();
    playingTrackId_ = 0;
    plEdit_ = {};
    refreshPlaylistVoiceOver_.clear();
    removedPlaylistVoiceOver_.clear();
    ownWriteTime_ = {};
    pendingDbWrite_ = false;
    batchWriteFailed_ = false;
    lastBatchAdded_ = lastBatchSkipped_ = 0;
    syncUi_.dirty = dupes_.dirty = true;
    musicSyncing_ = false;
    backupRows_.clear();
    backupsLoaded_ = false;
    loadedMount_ = dev->mountPoint;
    loadedDeviceInfo_ = *dev;
    filesystemState_ = {};
    managedFilesystemTrackIds_.clear();
    library_.reset();
    libraryError_ = "Loading player library…";
    const DeviceInfo device = *dev;
    deviceJob_.start([device] {
        DeviceResult result;
        result.session.load(device);
        return result;
    });
    deviceJobKind_ = DeviceJobKind::Load;
    switchSource(View::Device);
    art_.trackId = 0;
}

void App::switchSource(View view, int playlistIndex) {
    view_ = view;
    playlistIndex_ = playlistIndex;
    selectedTrackId_ = 0;
    selection_.clear();
    selectionAnchor_ = 0;
    visibleDirty_ = true;
}

const Library* App::shownLibrary() const {
    if (view_ == View::Library) return &hostView_;
    return library_ ? &*library_ : nullptr;
}

std::uint32_t App::viewMediaType() const {
    switch (view_) {
        case View::Music: return kMediaAudio;
        case View::Podcasts: return kMediaPodcast;
        case View::Audiobooks: return kMediaAudiobook;
        default: return 0;
    }
}

bool App::showingTracks() const {
    if (view_ == View::Library) return true;
    return activeDevice() && library_ && view_ != View::Device;
}

const std::unordered_map<std::uint32_t, int>* App::shownIndex() const {
    if (view_ == View::Library) return &hostIndexById_;
    return library_ ? &trackIndexById_ : nullptr;
}

fs::path App::trackFilePath(const Track& t) const {
    // Host tracks carry a real filesystem path; iPod tracks carry the
    // ':'-separated on-device location.
    if (!t.location.empty() && t.location[0] == '/') return t.location;
    return locationToPath(loadedMount_, t.location);
}

void App::rebuildHostView() {
    hostView_.tracks.clear();
    hostView_.playlists.clear();
    hostView_.tracks.reserve(host_.tracks().size());
    hostIndexById_.clear();
    hostIndexById_.reserve(host_.tracks().size());
    for (const HostTrack& h : host_.tracks()) {
        Track t = h.meta;
        t.id = std::uint32_t(h.id);
        t.dbid = h.id;
        t.location = h.file.string();
        hostIndexById_[t.id] = int(hostView_.tracks.size());
        hostView_.tracks.push_back(std::move(t));
    }
    visibleDirty_ = true;
    // A review of the Mac library names tracks by id; this may have changed
    // which ids exist.
    if (dupes_.host) dupes_.dirty = true;
}

void App::revealSelectedHostTracks() {
    if (!viewingHost() || selection_.empty()) return;
    std::vector<fs::path> files;
    std::unordered_set<std::string> folders;
    for (const std::uint32_t id : selection_) {
        const auto it = hostIndexById_.find(id);
        if (it == hostIndexById_.end()) continue;
        fs::path file = hostView_.tracks[it->second].location;
        folders.insert(file.parent_path().string());
        files.push_back(std::move(file));
    }
    // Finder opens a window for each folder involved, so a selection spread
    // across the whole library would bury the screen in them.
    constexpr std::size_t kMaxFolders = 10;
    if (folders.size() > kMaxFolders) {
        setStatus("Those songs are in " +
                  plural(int(folders.size()), "folder", "folders") +
                  " — select fewer to show them in Finder");
        return;
    }
    if (!revealInFinder(files))
        setStatus(files.size() == 1 ? "That song's file is no longer on this Mac"
                                    : "Those songs' files are no longer on this Mac");
}

void App::applyHostRemoval(const HostRemovalResult& r) {
    // Applied even while closing: the files are already in the Trash, so the
    // library has to stop listing them whatever happens next.
    const std::unordered_set<std::uint64_t> gone(r.removed.begin(),
                                                 r.removed.end());
    if (const Track* t = playingTrack()) {
        const fs::path playing = trackFilePath(*t);
        if (std::find(r.removedFiles.begin(), r.removedFiles.end(), playing) !=
            r.removedFiles.end()) {
            player_->stop();
            playingTrackId_ = 0;
        }
    }
    if (!gone.empty()) {
        std::erase_if(host_.tracks(), [&gone](const HostTrack& h) {
            return gone.count(h.id) > 0;
        });
        saveHost();
        rebuildHostView();
        if (viewingHost()) {
            std::erase_if(selection_, [this](std::uint32_t id) {
                return !hostIndexById_.count(id);
            });
            if (!hostIndexById_.count(selectedTrackId_))
                selectedTrackId_ = selection_.empty() ? 0 : selection_.back();
            selectionAnchor_ = selectedTrackId_;
        }
    }

    std::string msg;
    if (r.trashed)
        msg = "Moved " + plural(r.trashed, "duplicate", "duplicates") +
              " to the Trash";
    if (r.unlisted)
        msg += (msg.empty() ? "Removed " : ", removed ") +
               plural(r.unlisted, "duplicate", "duplicates") +
               " from the library";
    if (r.kept) {
        msg += (msg.empty() ? "" : " · ") +
               plural(r.kept, "duplicate was", "duplicates were") + " left in place";
        if (!r.error.empty()) msg += ": " + r.error;
    }
    setStatus(msg.empty() ? "No duplicates were removed" : msg);
}

void App::pullPlayCountsToHost() {
    if (hostBusy()) return;
    if (!library_ || host_.tracks().empty()) return;

    // Index the Mac library by both measures, so a song that went over as a
    // transcode still matches the device copy it became.
    std::unordered_map<std::string, HostTrack*> byKey;
    std::unordered_map<std::uint64_t, HostTrack*> byHash;
    for (HostTrack& h : host_.tracks()) {
        const std::string key = duplicateKey(h.meta, MatchMode::Exact);
        if (!key.empty()) byKey.emplace(key, &h);
        if (h.fp.ok()) byHash.emplace(h.fp.hash, &h);
    }

    int updated = 0;
    for (const Track& t : library_->tracks) {
        HostTrack* h = nullptr;
        if (const AudioFingerprint* fp = fingerprints_.get(t.dbid))
            if (fp->ok())
                if (const auto it = byHash.find(fp->hash); it != byHash.end())
                    h = it->second;
        if (!h) {
            const std::string key = duplicateKey(t, MatchMode::Exact);
            if (const auto it = byKey.find(key);
                !key.empty() && it != byKey.end())
                h = it->second;
        }
        if (!h) continue;

        // Counts only ever go up: the iPod is the authority on plays that
        // happened on it, but it knows nothing about plays on the Mac.
        bool changed = false;
        if (t.playCount > h->meta.playCount) {
            h->meta.playCount = t.playCount;
            changed = true;
        }
        if (t.rating > 0 && t.rating != h->meta.rating) {
            h->meta.rating = t.rating;
            changed = true;
        }
        if (changed) ++updated;
    }

    if (updated > 0) {
        saveHost();
        rebuildHostView();
        setStatus("Brought play counts back for " +
                  plural(updated, "song", "songs"));
    }
}

void App::rescanWatchFolders() {
    if (scan_.running || apple_.busy || hostRemovalJob_.busy()) return;
    if (scan_.thread.joinable()) scan_.thread.join();

    // The worker gets its own copy so the UI can keep reading the live
    // library while a cold scan (which reads and hashes every file) runs.
    scan_.running = true;
    scan_.finished.store(false);
    scan_.cancel.store(false);
    scan_.result = std::make_unique<HostLibrary>(host_);
    scan_.error.clear();
    scan_.thread = std::thread([this] {
        try { scan_.stats = scan_.result->rescan(true, &scan_.cancel); }
        catch (const std::exception& e) { scan_.error = e.what(); }

        scan_.finished.store(true);
    });
    setStatus("Scanning your library and importing new songs…");
}

void App::applyFinishedScan() {
    if (!scan_.finished.load()) return;
    scan_.finished.store(false);
    if (scan_.thread.joinable()) scan_.thread.join();
    scan_.running = false;
    if (!scan_.result) return;

    if (!scan_.error.empty()) {
        setStatus("Library scan failed: " + scan_.error);
        scan_.result.reset();
        return;
    }
    host_ = std::move(*scan_.result);
    saveHost();
    scan_.result.reset();
    rebuildHostView();
    pullPlayCountsToHost();

    const ScanStats& s = scan_.stats;
    if (s.imported || s.added || s.updated || s.missing) {
        std::string msg = "Library: " + plural(s.imported + s.added, "song", "songs") +
                          " added";
        if (s.updated) msg += ", " + std::to_string(s.updated) + " updated";
        if (s.missing) msg += ", " + std::to_string(s.missing) + " missing";
        setStatus(msg);
    } else {
        setStatus("Library up to date — " +
                  plural(int(host_.tracks().size()), "song", "songs"));
    }
}

bool App::browserApplies() const {
    return browser_.visible &&
           (view_ == View::Music || view_ == View::Library ||
            view_ == View::Podcasts || view_ == View::Audiobooks);
}

namespace {

// Distinct values of one Track string field over `rows`, case-insensitively
// sorted, keeping only rows the caller accepts. The string_view keys are safe
// because nothing mutates the library while a frame is being built.
template <class Keep>
std::vector<std::string> distinctValues(const Library& lib,
                                        const std::vector<int>& rows,
                                        std::string Track::*field, Keep keep) {
    std::unordered_set<std::string_view> seen;
    std::vector<std::string> out;
    for (const int ti : rows) {
        const Track& t = lib.tracks[ti];
        if (!keep(t)) continue;
        if (seen.insert(t.*field).second) out.push_back(t.*field);
    }
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) {
                  return cmpCi(a, b) < 0;
              });
    return out;
}

}  // namespace

void App::rebuildVisible() {
    visibleDirty_ = false;
    visible_.clear();
    visibleTotalMs_ = 0;
    visibleTotalBytes_ = 0;

    // Which media types the device holds decides whether the sidebar offers
    // the Podcasts and Audiobooks rows. Answering it here rather than in the
    // sidebar keeps it off the frame path: it was two full scans of the
    // library every frame, and a library with no podcasts scanned all of it
    // both times.
    devHasPodcasts_ = devHasAudiobooks_ = false;
    if (library_) {
        for (const Track& t : library_->tracks) {
            if (t.mediaType == kMediaPodcast) devHasPodcasts_ = true;
            else if (t.mediaType == kMediaAudiobook) devHasAudiobooks_ = true;
            if (devHasPodcasts_ && devHasAudiobooks_) break;
        }
    }

    const Library* lib = shownLibrary();
    const auto* index = shownIndex();
    if (!lib || !index) return;

    // A browser selection that is not on screen would still filter, which is
    // the worst bug available here — so leaving its views drops it entirely
    // rather than merely ignoring it.
    if (!browserApplies()) browser_.clearSelection();

    // (A) everything this source could show, in its natural order.
    std::vector<int> base;
    if (view_ == View::Playlist && playlistIndex_ >= 0 &&
        playlistIndex_ < int(lib->playlists.size())) {
        for (const std::uint32_t id : lib->playlists[playlistIndex_].trackIds) {
            if (auto it = index->find(id); it != index->end())
                base.push_back(it->second);
        }
    } else if (const std::uint32_t want = viewMediaType()) {
        // Music, Podcasts and Audiobooks are one list partitioned by media
        // type. A track with no type set is music: that is what everything
        // imported before PodBox started recording it will look like.
        for (int i = 0; i < int(lib->tracks.size()); ++i) {
            const std::uint32_t mt = lib->tracks[i].mediaType;
            if (mt == want || (want == kMediaAudio && mt == 0))
                base.push_back(i);
        }
    } else {
        base.resize(lib->tracks.size());
        for (int i = 0; i < int(base.size()); ++i) base[i] = i;
    }

    // (B) narrowed by the search box alone. The browser's lists are built from
    // this, and the table from a further narrowing of it — which is what stops
    // the two from being circular: the browser reads the set upstream of the
    // one it constrains, never the one it produces.
    const std::string needle = toLower(search_);
    std::vector<int> searched;
    searched.reserve(base.size());
    for (const int ti : base) {
        const Track& t = lib->tracks[ti];
        if (needle.empty() || containsCi(t.title, needle) ||
            containsCi(t.artist, needle) || containsCi(t.album, needle))
            searched.push_back(ti);
    }

    // (C) the three facet lists, built strictly left to right. Each list is
    // narrowed by the selections to its left, and a selection that is no
    // longer in its own list falls back to All. Because list k depends only on
    // facets 0..k-1, one forward pass reaches a fixpoint.
    auto matches = [](const std::optional<std::string>& sel,
                      const std::string& value) {
        return !sel || *sel == value;
    };
    if (browserApplies()) {
        auto& b = browser_;
        b.genres = distinctValues(*lib, searched, &Track::genre,
                                  [](const Track&) { return true; });
        if (b.genre && std::find(b.genres.begin(), b.genres.end(), *b.genre) ==
                           b.genres.end())
            b.genre.reset();

        b.artists =
            distinctValues(*lib, searched, &Track::artist, [&](const Track& t) {
                return matches(b.genre, t.genre);
            });
        if (b.artist && std::find(b.artists.begin(), b.artists.end(),
                                  *b.artist) == b.artists.end())
            b.artist.reset();

        b.albums =
            distinctValues(*lib, searched, &Track::album, [&](const Track& t) {
                return matches(b.genre, t.genre) &&
                       matches(b.artist, t.artist);
            });
        if (b.album && std::find(b.albums.begin(), b.albums.end(), *b.album) ==
                           b.albums.end())
            b.album.reset();
    } else {
        browser_.genres.clear();
        browser_.artists.clear();
        browser_.albums.clear();
    }

    // (D) what the table shows. The numbering is load-bearing: shift-click
    // ranges and playlist drag-reorder both index through it.
    visible_.reserve(searched.size());
    for (const int ti : searched) {
        const Track& t = lib->tracks[ti];
        if (matches(browser_.genre, t.genre) &&
            matches(browser_.artist, t.artist) &&
            matches(browser_.album, t.album))
            visible_.emplace_back(int(visible_.size()), ti);
    }

    // The status bar shows these every frame; summing them there meant
    // walking the whole visible list sixty times a second.
    for (const auto& [pos, ti] : visible_) {
        visibleTotalMs_ += lib->tracks[ti].lengthMs;
        visibleTotalBytes_ += lib->tracks[ti].sizeBytes;
    }

    const auto& tracks = lib->tracks;
    auto byField = [&](const std::pair<int, int>& x,
                       const std::pair<int, int>& y) {
        const Track& a = tracks[x.second];
        const Track& b = tracks[y.second];
        int c = 0;
        switch (sortCol_) {
            case 1:
                c = cmpCi(a.title, b.title);
                if (c == 0) c = cmpCi(a.artist, b.artist);
                break;
            case 2:
                c = a.lengthMs < b.lengthMs ? -1 : a.lengthMs > b.lengthMs;
                break;
            case 3:
                c = cmpCi(a.artist, b.artist);
                if (c == 0) c = cmpCi(a.album, b.album);
                if (c == 0)
                    c = a.discNumber < b.discNumber ? -1
                                                    : a.discNumber > b.discNumber;
                if (c == 0)
                    c = a.trackNumber < b.trackNumber
                            ? -1
                            : a.trackNumber > b.trackNumber;
                break;
            case 4:
                c = cmpCi(a.album, b.album);
                if (c == 0)
                    c = a.discNumber < b.discNumber ? -1
                                                    : a.discNumber > b.discNumber;
                if (c == 0)
                    c = a.trackNumber < b.trackNumber
                            ? -1
                            : a.trackNumber > b.trackNumber;
                break;
            case 5:
                c = cmpCi(a.genre, b.genre);
                if (c == 0) c = cmpCi(a.artist, b.artist);
                break;
            case 6:
                c = a.playCount < b.playCount ? -1 : a.playCount > b.playCount;
                break;
            case 7:
                c = a.rating < b.rating ? -1 : a.rating > b.rating;
                if (c == 0) c = cmpCi(a.artist, b.artist);
                break;
            default:
                return false;  // column 0: keep list order
        }
        return c < 0;
    };
    if (sortCol_ != 0)
        std::stable_sort(visible_.begin(), visible_.end(), byField);
    if (!sortAsc_) std::reverse(visible_.begin(), visible_.end());
}

}  // namespace podbox
