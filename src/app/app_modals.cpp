// The modal sheets, and the work each one drives. Every dialog in the app
// lives here; the state they read and write is owned by App (app.cpp).

#include "app/app.h"
#include "app/app_util.h"

#include "device/ipod_device.h"
#include "itdb/itunesdb.h"
#include "library/dedupe.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include "ui/aqua.h"
#include "ui/theme.h"
#include "util/finder.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {

void App::drawDeleteModal() {
    if (deleteRequestId_ && !ImGui::IsPopupOpen("Remove Song"))
        ImGui::OpenPopup("Remove Song");
    if (!aqua::beginSheet("Remove Song", 460.0f)) return;

    const auto it = trackIndexById_.find(deleteRequestId_);
    if (!library_ || it == trackIndexById_.end()) {
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }

    const bool many = selection_.size() > 1 && isSelected(deleteRequestId_);
    if (many)
        aqua::heading(fonts_,
                      ("Are you sure you want to remove these " +
                       std::to_string(selection_.size()) +
                       " songs from your player?")
                          .c_str());
    else
        aqua::heading(fonts_, ("Are you sure you want to remove \u201c" +
                               library_->tracks[it->second].title +
                               "\u201d from your player?")
                                  .c_str());
    aqua::body(fonts_,
               many ? "Their audio files will be deleted from the device. "
                      "This cannot be undone."
                    : "The audio file will be deleted from the device. This "
                      "cannot be undone.");

    aqua::divider();
    aqua::rightAlignButtons(2, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aqua::button("Remove", ImVec2(92, 0), true)) {
        const std::uint32_t id = deleteRequestId_;
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
        if (many) {
            const std::vector<std::uint32_t> ids = selection_;
            performDeleteMany(ids);
        } else {
            performDelete(id);
        }
    }
    aqua::endSheet();
}

// Asks macOS for a folder. Shelling out to osascript avoids dragging an
// Objective-C panel into this file for a button pressed once in a while.
std::filesystem::path chooseFolderDialog() {
    FILE* p = popen(
        "osascript -e 'try' -e 'POSIX path of (choose folder with prompt "
        "\"Choose a music folder for PodBox to index\")' -e 'end try' 2>/dev/null",
        "r");
    if (!p) return {};
    char buf[4096] = {};
    const char* got = std::fgets(buf, sizeof(buf), p);
    pclose(p);
    if (!got) return {};
    std::string path(buf);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
    return path.empty() ? std::filesystem::path{} : std::filesystem::path(path);
}

void App::drawFoldersModal() {
    if (!foldersOpen_) return;
    if (!ImGui::IsPopupOpen("Music Folders")) ImGui::OpenPopup("Music Folders");
    if (!aqua::beginSheet("Music Folders", 620.0f)) return;

    aqua::heading(fonts_, "Where does your music come from?");
    aqua::body(fonts_,
               "When PodBox scans, new songs in these folders are copied into "
               "your library at %s. Nothing in these folders is ever changed, "
               "moved or deleted. Folders named “downloading” or "
               "“incomplete” are skipped, so part-finished "
               "downloads are never imported.",
               displayPath(host_.musicFolder()).c_str());
    aqua::divider();

    int removeIndex = -1;
    for (std::size_t i = 0; i < host_.watchFolders().size(); ++i) {
        const WatchFolder& w = host_.watchFolders()[i];
        ImGui::PushID(int(i));
        bool on = w.enabled;
        if (ImGui::Checkbox("##on", &on)) host_.setWatchFolderEnabled(i, on);
        ImGui::SameLine();

        std::string prefix = w.path.string();
        if (prefix.empty() || prefix.back() != '/') prefix += '/';
        int here = 0;
        for (const std::string& file : host_.importedFiles())
            if (file.compare(0, prefix.size(), prefix) == 0) ++here;

        const auto found = folderExists_.find(w.path.string());
        const bool exists = found == folderExists_.end() || found->second;
        ImGui::TextColored(
            exists ? v4(pal::Text) : v4(pal::Warning), "%s",
            w.path.c_str());
        ImGui::PushFont(fonts_.label);
        ImGui::TextColored(v4(pal::TextDim), "        %s",
                           exists ? (std::to_string(here) + " songs imported").c_str()
                                  : "folder not found — is the drive "
                                    "connected?");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth() - 226);
        ImGui::BeginDisabled(!exists);
        if (aqua::button("Show in Finder", ImVec2(118, 0)) &&
            !openInFinder(w.path))
            setStatus("Could not open " + displayPath(w.path));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (aqua::button("Remove", ImVec2(78, 0))) removeIndex = int(i);
        ImGui::PopID();
    }
    if (host_.watchFolders().empty())
        ImGui::TextDisabled("No folders yet.");
    if (removeIndex >= 0) host_.removeWatchFolder(std::size_t(removeIndex));

    int missing = 0;
    for (const HostTrack& t : host_.tracks())
        if (t.missing) ++missing;
    if (missing > 0) {
        ImGui::Spacing();
        ImGui::TextColored(v4(pal::Warning),
                           "%s in the library no longer exist on disk.",
                           plural(missing, "song", "songs").c_str());
        ImGui::SameLine();
        if (aqua::button("Remove Them", ImVec2(110, 0))) {
            const int gone = host_.removeMissing();
            saveHost();
            rebuildHostView();
            setStatus("Removed " +
                      plural(gone, "missing song", "missing songs"));
        }
    }

    aqua::divider();
    if (aqua::button("Check for Missing Files", ImVec2(180, 0))) {
        if (!hostBusy()) {
            if (scan_.thread.joinable()) scan_.thread.join();
            scan_.running = true;
            scan_.finished.store(false);
            scan_.error.clear();
            scan_.result = std::make_unique<HostLibrary>(host_);
            scan_.thread = std::thread([this] {
                try { scan_.stats = {}; scan_.stats.missing = scan_.result->refreshMissing(); }
                catch (const std::exception& e) { scan_.error = e.what(); }
                scan_.finished.store(true);
            });
        }
    }
    ImGui::SameLine();
    if (aqua::button("Add Folder…", ImVec2(120, 0))) {
        if (!folderJob_.busy()) folderJob_.start(chooseFolderDialog);
    }
    ImGui::SameLine();
    aqua::rightAlignButtons(1, 92.0f);
    if (aqua::button("Done", ImVec2(92, 0), true)) {
        saveHost();
        foldersOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    aqua::endSheet();
}

void App::startAppleMusicRead() {
    if (apple_.busy) return;
    if (apple_.thread.joinable()) apple_.thread.join();
    apple_.busy = true;
    apple_.copying = false;
    apple_.finished.store(false);
    apple_.cancel.store(false);
    apple_.thread = std::thread([this] {
        try {
            AppleMusicRead r = readAppleMusicLibrary();
            {
                std::lock_guard<std::mutex> lock(apple_.mutex);
                apple_.read = std::move(r);
            }
        } catch (const std::exception& e) { apple_.read.error = e.what(); }
        apple_.finished.store(true);
    });
}

void App::startAppleMusicCopy() {
    if (apple_.busy || scan_.running || hostRemovalJob_.busy() ||
        apple_.read.tracks.empty())
        return;
    if (apple_.thread.joinable()) apple_.thread.join();
    apple_.busy = true;
    apple_.copying = true;
    apple_.finished.store(false);
    apple_.cancel.store(false);
    apple_.done.store(0);
    apple_.total.store(int(apple_.read.tracks.size()));

    apple_.error.clear();
    apple_.hostResult = host_;
    apple_.thread = std::thread([this] {
        try {
            CopyResult res = copyAppleMusicFiles(
                apple_.read.tracks, appleMusicCopyRoot(),
                [this](int done, int total, const std::string& name) {
                    apple_.done.store(done);
                    apple_.total.store(total);
                    {
                        std::lock_guard<std::mutex> lock(apple_.mutex);
                        apple_.current = name;
                    }
                    return !apple_.cancel.load();
                });
            {
                std::lock_guard<std::mutex> lock(apple_.mutex);
                apple_.copy = res;
            }
            apple_.added = 0;
            for (const auto& track : apple_.read.tracks) {
                std::error_code ec;
                if (track.file.empty() || !fs::exists(track.file, ec)) continue;
                if (apple_.hostResult.upsert(track.file, track.meta, "applemusic", fingerprintFile(track.file)))
                    ++apple_.added;
            }
        } catch (const std::exception& e) { apple_.error = e.what(); }
        apple_.finished.store(true);
    });
}

void App::applyFinishedAppleMusic() {
    if (!apple_.finished.load()) return;
    apple_.finished.store(false);
    if (apple_.thread.joinable()) apple_.thread.join();
    apple_.busy = false;
    if (!apple_.copying) return;  // a read just finished; the sheet shows it

    apple_.copying = false;
    if (!apple_.error.empty()) { setStatus(apple_.error); return; }
    const int added = apple_.added;
    host_ = std::move(apple_.hostResult);
    saveHost();
    rebuildHostView();
    pullPlayCountsToHost();

    setStatus("Imported " + plural(added, "song", "songs") +
              " from Apple Music" +
              (apple_.copy.cancelled ? " (stopped early)" : ""));
}

void App::drawAppleMusicModal() {
    applyFinishedAppleMusic();
    if (!apple_.open) return;
    if (!ImGui::IsPopupOpen("Import from Apple Music"))
        ImGui::OpenPopup("Import from Apple Music");
    if (!aqua::beginSheet("Import from Apple Music", 540.0f)) return;

    aqua::heading(fonts_, "Import your Apple Music library");
    aqua::body(fonts_,
               "Songs are copied into %s. Apple Music is only read from — "
               "nothing there is changed or moved.",
               appleMusicCopyRoot().c_str());
    aqua::divider();

    if (apple_.busy && apple_.copying) {
        const int done = apple_.done.load(), total = apple_.total.load();
        std::string current;
        {
            std::lock_guard<std::mutex> lock(apple_.mutex);
            current = apple_.current;
        }
        ImGui::Text("Copying %d of %d", done, total);
        ImGui::ProgressBar(total > 0 ? float(done) / float(total) : 0.0f,
                           ImVec2(-1, 14));
        aqua::body(fonts_, "%.60s", current.c_str());
        ImGui::Spacing();
        aqua::rightAlignButtons(1, 92.0f);
        if (aqua::button("Stop", ImVec2(92, 0))) apple_.cancel.store(true);
        aqua::body(fonts_, "Stopping keeps everything copied so far.");
        aqua::endSheet();
        return;
    }

    if (apple_.busy) {
        ImGui::TextUnformatted("Reading your Apple Music library…");
        aqua::endSheet();
        return;
    }

    if (!apple_.read.ok && apple_.read.error.empty()) {
        if (aqua::button("Read Apple Music Library", ImVec2(200, 0), true))
            startAppleMusicRead();
        ImGui::SameLine();
        if (aqua::button("Cancel", ImVec2(92, 0))) {
            apple_.open = false;
            ImGui::CloseCurrentPopup();
        }
        aqua::endSheet();
        return;
    }

    if (!apple_.read.error.empty()) {
        ImGui::TextColored(v4(pal::Danger), "%s",
                           apple_.read.error.c_str());
        ImGui::Spacing();
        aqua::rightAlignButtons(2, 92.0f);
        if (aqua::button("Close", ImVec2(92, 0))) {
            apple_.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aqua::button("Try Again", ImVec2(92, 0), true)) startAppleMusicRead();
        aqua::endSheet();
        return;
    }

    std::uint64_t bytes = 0;
    for (const AppleMusicTrack& t : apple_.read.tracks)
        bytes += t.meta.sizeBytes;
    ImGui::Text("%s to copy · %s",
                plural(int(apple_.read.tracks.size()), "song", "songs").c_str(),
                formatBytes(bytes).c_str());

    // Anything Apple Music lists but cannot hand over is reported rather than
    // quietly dropped.
    if (apple_.read.streamingOnly)
        aqua::body(fonts_, "%d skipped — streaming only, no file on this Mac",
                   apple_.read.streamingOnly);
    if (apple_.read.fileMissing)
        aqua::body(fonts_, "%d skipped — file listed but not on disk",
                   apple_.read.fileMissing);
    if (apple_.read.drmProtected)
        aqua::body(fonts_,
                   "%d skipped — DRM protected, a portable player cannot "
                   "play these",
                   apple_.read.drmProtected);

    aqua::divider();
    if (aqua::button("Re-read", ImVec2(92, 0))) startAppleMusicRead();
    ImGui::SameLine();
    aqua::rightAlignButtons(2, 100.0f);
    if (aqua::button("Cancel", ImVec2(100, 0))) {
        apple_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aqua::button("Copy to PodBox", ImVec2(100, 0), true))
        startAppleMusicCopy();
    aqua::endSheet();
}

namespace {

// The media types a user can pick between. Video is deliberately absent: a
// classic iPod plays it, but PodBox neither imports nor lists it, so offering
// it here would only let someone hide a track from every view.
struct MediaChoice {
    const char* label;
    std::uint32_t type;
};
constexpr MediaChoice kMediaChoices[] = {
    {"Music", kMediaAudio},
    {"Podcast", kMediaPodcast},
    {"Audiobook", kMediaAudiobook},
};

int mediaIndexOf(std::uint32_t type) {
    for (int i = 0; i < int(std::size(kMediaChoices)); ++i)
        if (kMediaChoices[i].type == type) return i;
    return 0;  // an unset or unrecognised type is music
}

}  // namespace

void App::openGetInfo() {
    const Library* shown = shownLibrary();
    const auto* index = shownIndex();
    if (!shown || !index || selection_.empty()) return;

    // Fields shared by every selected track are pre-filled; fields that
    // differ start blank and are left alone unless the user types something.
    const Track* first = nullptr;
    bool sameTitle = true, sameArtist = true, sameAlbum = true;
    bool sameGenre = true, sameYear = true, sameTrack = true, sameMedia = true;
    for (std::uint32_t id : selection_) {
        const auto it = index->find(id);
        if (it == index->end()) continue;
        const Track& t = shown->tracks[it->second];
        if (!first) {
            first = &t;
            continue;
        }
        sameTitle &= t.title == first->title;
        sameArtist &= t.artist == first->artist;
        sameAlbum &= t.album == first->album;
        sameGenre &= t.genre == first->genre;
        sameYear &= t.year == first->year;
        sameTrack &= t.trackNumber == first->trackNumber;
        sameMedia &= t.mediaType == first->mediaType;
    }
    if (!first) return;

    auto put = [](char* buf, std::size_t n, const std::string& v, bool same) {
        std::snprintf(buf, n, "%s", same ? v.c_str() : "");
    };
    // A title is per-song by nature, so editing many at once never prefills it.
    put(getInfo_.title, sizeof(getInfo_.title), first->title,
        sameTitle && selection_.size() == 1);
    put(getInfo_.artist, sizeof(getInfo_.artist), first->artist, sameArtist);
    put(getInfo_.album, sizeof(getInfo_.album), first->album, sameAlbum);
    put(getInfo_.genre, sizeof(getInfo_.genre), first->genre, sameGenre);
    std::snprintf(getInfo_.year, sizeof(getInfo_.year), "%s",
                  sameYear && first->year ? std::to_string(first->year).c_str()
                                          : "");
    std::snprintf(getInfo_.track, sizeof(getInfo_.track), "%s",
                  sameTrack && first->trackNumber
                      ? std::to_string(first->trackNumber).c_str()
                      : "");
    getInfo_.mediaChoice = sameMedia ? mediaIndexOf(first->mediaType) : -1;
    getInfo_.writeTags = false;
    getInfo_.open = true;
}

void App::drawGetInfoModal() {
    if (!getInfo_.open) return;
    if (!ImGui::IsPopupOpen("Get Info")) ImGui::OpenPopup("Get Info");
    if (!aqua::beginSheet("Get Info", 520.0f)) return;

    const int n = int(selection_.size());
    if (n > 1) {
        aqua::heading(fonts_, ("Editing " + std::to_string(n) + " songs").c_str());
        aqua::body(fonts_, "Fields left blank keep whatever each song already "
                           "has.");
    } else {
        aqua::heading(fonts_, "Song information");
    }
    aqua::divider();

    ImGui::PushItemWidth(-130);
    if (n == 1) ImGui::InputText("Name", getInfo_.title, sizeof(getInfo_.title));
    ImGui::InputText("Artist", getInfo_.artist, sizeof(getInfo_.artist));
    ImGui::InputText("Album", getInfo_.album, sizeof(getInfo_.album));
    ImGui::InputText("Genre", getInfo_.genre, sizeof(getInfo_.genre));
    ImGui::InputText("Year", getInfo_.year, sizeof(getInfo_.year),
                     ImGuiInputTextFlags_CharsDecimal);
    if (n == 1)
        ImGui::InputText("Track number", getInfo_.track, sizeof(getInfo_.track),
                         ImGuiInputTextFlags_CharsDecimal);
    // Media type is classified on import from the extension and the genre,
    // which is a guess for anything that is not .m4b. This is where a wrong
    // guess gets corrected.
    {
        const char* preview =
            getInfo_.mediaChoice < 0
                ? "Multiple"
                : kMediaChoices[getInfo_.mediaChoice].label;
        if (ImGui::BeginCombo("Media kind", preview)) {
            for (int i = 0; i < int(std::size(kMediaChoices)); ++i) {
                const bool sel = getInfo_.mediaChoice == i;
                if (ImGui::Selectable(kMediaChoices[i].label, sel))
                    getInfo_.mediaChoice = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (viewingHost()) {
        ImGui::Checkbox("Also write these tags into the files", &getInfo_.writeTags);
        aqua::body(fonts_, getInfo_.writeTags
                               ? "This rewrites your own files on disk."
                               : "Off: only PodBox's library is changed.");
    } else {
        aqua::body(
            fonts_,
            connectedIpod()
                ? "Changes the iPod's database only — the audio files on "
                  "the device keep their own tags."
                : "Changes the tags in the copied audio files so this "
                  "player sees the updated information.");
    }

    aqua::divider();
    aqua::rightAlignButtons(2, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        getInfo_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aqua::button("Save", ImVec2(92, 0), true)) {
        const auto* index = shownIndex();
        auto assign = [](std::string& field, const char* buf) {
            if (buf[0] != '\0') field = buf;
        };
        int changed = 0;
        for (std::uint32_t id : selection_) {
            if (viewingHost()) {
                HostTrack* h = nullptr;
                for (HostTrack& cand : host_.tracks())
                    if (std::uint32_t(cand.id) == id) h = &cand;
                if (!h) continue;
                assign(h->meta.title, getInfo_.title);
                assign(h->meta.artist, getInfo_.artist);
                assign(h->meta.album, getInfo_.album);
                assign(h->meta.genre, getInfo_.genre);
                if (getInfo_.year[0]) h->meta.year = std::uint32_t(std::atoi(getInfo_.year));
                if (getInfo_.track[0])
                    h->meta.trackNumber = std::uint32_t(std::atoi(getInfo_.track));
                if (getInfo_.mediaChoice >= 0)
                    h->meta.mediaType = kMediaChoices[getInfo_.mediaChoice].type;
                ++changed;
                if (getInfo_.writeTags) pendingHostTags_.emplace_back(h->file, h->meta);
            } else {
                if (!library_ || !index) break;
                const auto it = index->find(id);
                if (it == index->end()) continue;
                Track& t = library_->tracks[it->second];
                assign(t.title, getInfo_.title);
                assign(t.artist, getInfo_.artist);
                assign(t.album, getInfo_.album);
                assign(t.genre, getInfo_.genre);
                if (getInfo_.year[0]) t.year = std::uint32_t(std::atoi(getInfo_.year));
                if (getInfo_.track[0])
                    t.trackNumber = std::uint32_t(std::atoi(getInfo_.track));
                if (getInfo_.mediaChoice >= 0)
                    t.mediaType = kMediaChoices[getInfo_.mediaChoice].type;
                ++changed;
                // Folder-based players build their own library from file
                // tags, so database-only editing would disappear on the next
                // reconnect. iPods keep the established database-only path.
                if (!connectedIpod())
                    pendingFileTags_.emplace_back(locationToPath(loadedMount_, t.location), t);
            }
        }

        if (viewingHost()) {
            saveHost();
            rebuildHostView();
        } else if (changed > 0) {
            writeDatabase();
        }
        visibleDirty_ = true;
        getInfo_.open = false;
        ImGui::CloseCurrentPopup();

        std::string msg = "Updated " + plural(changed, "song", "songs");
        if (viewingHost()) setStatus(msg);
    }
    aqua::endSheet();
}

void App::refreshSyncPlan() {
    if (syncPlanJob_.busy() || closeRequested_ || !ejectRequestedMount_.empty()) return;
    syncUi_.dirty = false;
    syncUi_.plan = {};
    if (!library_) return;
    // Size the plan by what will actually be written, not the originals.
    syncUi_.options.format = currentImportFormat();
    syncUi_.options.playableExtensions.clear();
    syncUi_.options.maxSampleRate = 0;
    if (const DeviceInfo* dev = activeDevice()) {
        syncUi_.options.playableExtensions = dev->originalExtensions;
        syncUi_.options.maxSampleRate = dev->maxSampleRate;
    }
    syncPlanJob_.start([host = host_, device = static_cast<const DeviceSession&>(*this), options = syncUi_.options] {
        return planSync(host, *device.library_, device.fingerprints_, device.loadedMount_, options,
                        device.connectedIpod() ? nullptr : &device.managedFilesystemTrackIds_);
    });
}

void App::startSync() {
    if (!library_ || syncUi_.plan.empty() || syncPlanJob_.busy() || syncUi_.dirty) return;

    // Remove dead database entries first. Besides cleaning up songs that can
    // no longer play, this keeps the import duplicate guard below from seeing
    // a missing file as an existing copy. User-requested removals join the
    // same transaction so the database is only rewritten once.
    std::vector<std::uint32_t> removeIds = syncUi_.plan.missingDeviceFiles;
    removeIds.insert(removeIds.end(), syncUi_.plan.toRemove.begin(),
                     syncUi_.plan.toRemove.end());
    if (!removeIds.empty()) {
        if (!performDeleteMany(removeIds)) return;
    }

    // Copying reuses the drag-and-drop pipeline: same worker, same transcode,
    // same duplicate guard. The guard also protects against a plan that has
    // gone stale since it was computed.
    std::vector<fs::path> files;
    files.reserve(syncUi_.plan.toCopy.size());
    for (std::uint64_t id : syncUi_.plan.toCopy) {
        for (const HostTrack& h : host_.tracks()) {
            if (h.id != id) continue;
            files.push_back(h.file);
            break;
        }
    }
    if (files.empty()) return;
    if (deviceJob_.busy()) {
        pendingCopyMount_ = loadedMount_;
        pendingCopyFiles_ = std::move(files);
        return;
    }

    DupeGuard guard;
    guard.enabled = true;
    for (const Track& t : library_->tracks) {
        const std::string key = duplicateKey(t, MatchMode::Exact);
        if (!key.empty()) guard.metaKeys.insert(key);
    }
    for (const Track& t : library_->tracks)
        if (const AudioFingerprint* fp = fingerprints_.get(t.dbid);
            fp && fp->ok())
            guard.hashes.insert(fp->hash);

    sync_.queueAdds(files, currentImportTarget(), currentImportFormat(),
                    std::move(guard));
    setStatus("Syncing " + plural(int(files.size()), "song", "songs") +
              " to the player…");
}

void App::drawSyncModal() {
    if (!syncUi_.open) return;
    if (!ImGui::IsPopupOpen("Sync to Player"))
        ImGui::OpenPopup("Sync to Player");
    if (!aqua::beginSheet("Sync to Player", 560.0f)) return;
    if (!library_) {
        syncUi_.open = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (syncUi_.dirty) refreshSyncPlan();

    aqua::heading(fonts_, connectedIpod()
                              ? "Sync your library to this iPod"
                              : "Sync your library to this player");
    aqua::body(fonts_,
               "Everything in your Mac library that isn't already on the "
               "player "
               "is copied over. Nothing is written until you press Sync.");
    aqua::divider();

    ImGui::Text("%s to copy · %s",
                plural(int(syncUi_.plan.toCopy.size()), "song", "songs").c_str(),
                formatBytes(syncUi_.plan.bytesToCopy).c_str());
    aqua::body(fonts_,
               "%d already on the player · %d duplicates skipped · %d "
               "missing from your Mac",
               syncUi_.plan.alreadyOnDevice, syncUi_.plan.skippedDuplicate,
               syncUi_.plan.skippedMissing);
    if (!syncUi_.plan.missingDeviceFiles.empty())
        aqua::body(fonts_,
                   "%s missing from the player. PodBox will restore any copy "
                   "available in your Mac library.",
                   plural(int(syncUi_.plan.missingDeviceFiles.size()),
                          "song file is", "song files are")
                       .c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("Import format");
    if (const DeviceInfo* formatDevice = activeDevice();
        formatDevice && drawImportFormatChoices(*formatDevice))
        syncUi_.dirty = true;
    aqua::body(
        fonts_,
        connectedIpod()
            ? "FLAC and other lossless files are converted to 16-bit Apple "
              "Lossless so the iPod can play them. Sizes are estimated "
              "after conversion."
            : "Files this player cannot play are converted. Sizes are "
              "estimated after conversion.");

    ImGui::Spacing();
    if (ImGui::Checkbox(connectedIpod()
                            ? "Also remove songs that aren't in my library"
                            : "Also remove PodBox-managed songs that aren't "
                              "in my library",
                        &syncUi_.options.removeFromDevice)) {
        syncUi_.dirty = true;
        syncUi_.confirmRemove = false;
    }
    if (syncUi_.options.removeFromDevice) {
        ImGui::TextColored(v4(pal::Warning),
                           "This deletes %s from the player, freeing %s.",
                           plural(int(syncUi_.plan.toRemove.size()), "song",
                                  "songs")
                               .c_str(),
                           formatBytes(syncUi_.plan.bytesToFree).c_str());
        // Every song queued for removal is one the Mac has no copy of, so
        // this is the only step in a sync that destroys music outright.
        if (syncUi_.plan.deviceOnly > 0)
            ImGui::TextColored(v4(pal::Danger),
                               "%s exist only on the player — deleting them "
                               "loses them for good.",
                               plural(syncUi_.plan.deviceOnly, "song", "songs")
                                   .c_str());
        if (!syncUi_.plan.toRemove.empty())
            ImGui::Checkbox("Yes, delete those songs from the player",
                            &syncUi_.confirmRemove);
    }

    // Capacity guard: refuse a plan that cannot fit rather than filling the
    // device and failing part way.
    const DeviceInfo* dev = activeDevice();
    bool fits = true;
    if (dev && dev->freeBytes > 0) {
        const std::uint64_t after =
            syncUi_.plan.bytesToCopy > syncUi_.plan.bytesToFree
                ? syncUi_.plan.bytesToCopy - syncUi_.plan.bytesToFree
                : 0;
        fits = after <= dev->freeBytes;
        if (!fits)
            ImGui::TextColored(v4(pal::Danger),
                               "Not enough room — needs %s more.",
                               formatBytes(after - dev->freeBytes).c_str());
    }

    const bool blockedByRemoval =
        syncUi_.options.removeFromDevice && !syncUi_.plan.toRemove.empty() &&
        !syncUi_.confirmRemove;

    aqua::divider();
    aqua::rightAlignButtons(2, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        syncUi_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const bool blocked = syncPlanJob_.busy() || syncUi_.dirty || syncUi_.plan.empty() || !fits || blockedByRemoval ||
                         sync_.busy();
    ImGui::BeginDisabled(blocked);
    if (aqua::button("Sync", ImVec2(92, 0), !blocked)) {
        startSync();
        syncUi_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (syncPlanJob_.busy() || syncUi_.dirty)
        aqua::body(fonts_, "Checking songs and available files…");
    else if (syncUi_.plan.empty())
        aqua::body(fonts_, "Nothing to do — the player already matches.");
    aqua::endSheet();
}

void App::refreshDuplicates() {
    if (duplicateJob_.busy()) return;
    dupes_.dirty = false;
    dupes_.groups.clear();
    dupes_.enabled.clear();
    if (dupes_.host) {
        duplicateJob_.start([host = host_, mode = dupes_.mode,
                             identical = dupes_.identicalOnly] {
            auto groups = findHostDuplicates(host, mode);
            if (identical) std::erase_if(groups, [](const auto& g) { return !g.allIdenticalFiles; });
            return groups;
        });
        return;
    }
    if (!library_) return;
    duplicateJob_.start([library = *library_, fingerprints = fingerprints_.all(),
                         mode = dupes_.mode, identical = dupes_.identicalOnly] {
        auto groups = findDuplicates(library, mode, fingerprints);
        if (identical) std::erase_if(groups, [](const auto& g) { return !g.allIdenticalFiles; });
        return groups;
    });
}

void App::startHostRemoval() {
    if (hostBusy()) return;
    std::vector<HostRemoval> items;
    for (std::size_t i = 0; i < dupes_.groups.size(); ++i) {
        if (!dupes_.enabled[i]) continue;
        const auto& ids = dupes_.groups[i].trackIds;
        const auto keeper = hostIndexById_.find(ids[0]);
        if (keeper == hostIndexById_.end()) continue;
        const Track& kept = hostView_.tracks[keeper->second];
        for (std::size_t k = 1; k < ids.size(); ++k) {
            const auto it = hostIndexById_.find(ids[k]);
            if (it == hostIndexById_.end()) continue;
            const Track& t = hostView_.tracks[it->second];
            items.push_back({t.dbid, t.location, kept.location});
        }
    }
    if (items.empty()) return;

    // Only the library folder is PodBox's to tidy. The folders it imports
    // from are never touched; anything outside the library is only unlisted.
    const fs::path library = host_.musicFolder();

    setStatus("Moving " + plural(int(items.size()), "duplicate", "duplicates") +
              " to the Trash…");
    hostRemovalJob_.start([items = std::move(items), library] {
        HostRemovalResult result = removeHostDuplicates(items, {library}, moveToTrash);
        pruneEmptyFolders(library);
        return result;
    });
}

void App::startVerifyPass() {
    if (!library_ || dupes_.verify.running()) return;
    std::vector<VerifyJob::Item> items;
    for (const Track& t : library_->tracks) {
        // Skip anything already fingerprinted; the whole point of persisting
        // the sidecar is that this is a one-time cost per device.
        if (t.dbid == 0 || fingerprints_.get(t.dbid)) continue;
        items.push_back({t.dbid, locationToPath(loadedMount_, t.location)});
    }
    if (items.empty()) {
        setStatus("Every song on this player is already verified");
        return;
    }
    dupes_.verify.start(std::move(items));
}

void App::drawDuplicatesModal() {
    // Fold in whatever the verify pass has produced, whether or not the
    // sheet is open, so a cancelled run still keeps its work.
    if (auto results = dupes_.verify.take(); !results.empty()) {
        for (const auto& [dbid, fp] : results)
            fingerprints_.put(dbid, fp, FingerprintStore::Origin::Device);
        if (library_) fingerprintSavePending_ = true;
        dupes_.dirty = true;
    }

    if (!dupes_.open) return;
    if (!ImGui::IsPopupOpen("Duplicate Songs"))
        ImGui::OpenPopup("Duplicate Songs");
    if (!aqua::beginSheet("Duplicate Songs", 720.0f)) return;
    // A player's review ends when the player goes; the Mac library stays.
    if (!dupes_.host && !library_) {
        dupes_.open = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (dupes_.dirty) refreshDuplicates();
    const Library& lib = dupes_.host ? hostView_ : *library_;
    const auto& index = dupes_.host ? hostIndexById_ : trackIndexById_;

    if (dupes_.host) {
        aqua::heading(fonts_, "Duplicate songs in your Mac library");
        aqua::body(fonts_,
                   "Only songs in %s are checked. The best copy of each is "
                   "kept — lossless first, then play count — and the others "
                   "go to the Trash. Your import folders are never touched.",
                   displayPath(host_.musicFolder()).c_str());
    } else {
        aqua::heading(fonts_, "Duplicate songs on this player");
    }

    int mode = int(dupes_.mode);
    if (ImGui::RadioButton("Exact", &mode, int(MatchMode::Exact)))
        dupes_.dirty = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Loose", &mode, int(MatchMode::Loose)))
        dupes_.dirty = true;
    dupes_.mode = MatchMode(mode);
    ImGui::SameLine();
    aqua::body(fonts_, dupes_.mode == MatchMode::Exact
                           ? "artist, title, album and length"
                           : "artist and title only — will group live and "
                             "studio versions of a song");

    if (ImGui::Checkbox("Only byte-identical copies", &dupes_.identicalOnly))
        dupes_.dirty = true;
    // The Mac library is fingerprinted as it is scanned, so only a player's
    // files ever need a separate pass.
    if (!dupes_.host) {
        ImGui::SameLine();
        const int unverified =
            int(library_->tracks.size()) - int(fingerprints_.all().size());
        if (dupes_.verify.running()) {
            ImGui::Text("Verifying %d/%d…", dupes_.verify.done(), dupes_.verify.total());
            ImGui::SameLine();
            if (aqua::button("Stop", ImVec2(70, 0))) dupes_.verify.cancel();
        } else {
            ImGui::BeginDisabled(unverified <= 0);
            if (aqua::button("Verify Player Files", ImVec2(160, 0)))
                startVerifyPass();
            ImGui::EndDisabled();
            if (unverified > 0) {
                ImGui::SameLine();
                aqua::body(fonts_, "%d not yet hashed", unverified);
            }
        }
    }

    aqua::divider();

    std::uint64_t totalBytes = 0;
    int totalCopies = 0, activeGroups = 0;
    for (std::size_t i = 0; i < dupes_.groups.size(); ++i) {
        if (!dupes_.enabled[i]) continue;
        ++activeGroups;
        totalCopies += int(dupes_.groups[i].trackIds.size()) - 1;
        totalBytes += dupes_.groups[i].reclaimBytes;
    }

    ImGui::BeginChild("dupe_list", ImVec2(0, 300));
    if (dupes_.groups.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::TextDisabled(
            dupes_.identicalOnly
                ? "No byte-identical duplicates. Transcoded copies never match "
                  "byte-for-byte — clear the checkbox to see them."
                : "No duplicates found.");
    }
    for (std::size_t i = 0; i < dupes_.groups.size(); ++i) {
        const DuplicateGroup& g = dupes_.groups[i];
        const auto keeperIt = index.find(g.trackIds[0]);
        if (keeperIt == index.end()) continue;
        const Track& keeper = lib.tracks[keeperIt->second];

        ImGui::PushID(int(i));
        bool on = dupes_.enabled[i] != 0;
        if (ImGui::Checkbox("##on", &on)) dupes_.enabled[i] = on ? 1 : 0;
        ImGui::SameLine();

        char header[320];
        std::snprintf(header, sizeof(header), "%s — %s   (%d copies, %s)",
                      keeper.artist.empty() ? "Unknown Artist"
                                            : keeper.artist.c_str(),
                      keeper.title.c_str(), int(g.trackIds.size()),
                      formatBytes(g.reclaimBytes).c_str());
        if (ImGui::TreeNode(header)) {
            for (std::size_t k = 0; k < g.trackIds.size(); ++k) {
                const auto it = index.find(g.trackIds[k]);
                if (it == index.end()) continue;
                const Track& t = lib.tracks[it->second];
                const std::string where =
                    dupes_.host ? displayPath(t.location) : t.location;
                ImGui::TextColored(
                    k == 0 ? v4(pal::Success) : v4(pal::TextDim),
                    "%s  %s  %u kbps  %s  %u plays", k == 0 ? "keep" : "  ✕ ",
                    formatDuration(t.lengthMs).c_str(), t.bitrate,
                    where.c_str(), t.playCount);
            }
            if (g.allIdenticalFiles)
                aqua::body(fonts_, "        these files are byte-identical");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    aqua::divider();
    ImGui::Text("%d groups · %d duplicates · %s", activeGroups,
                totalCopies, formatBytes(totalBytes).c_str());
    ImGui::SameLine();
    aqua::rightAlignButtons(2, 110.0f);
    if (aqua::button("Done", ImVec2(110, 0))) {
        dupes_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const bool canRemove = totalCopies > 0 && !(dupes_.host && hostBusy());
    ImGui::BeginDisabled(!canRemove);
    if (dupes_.host) {
        if (aqua::button("Move to Trash", ImVec2(110, 0), canRemove)) {
            startHostRemoval();
            dupes_.open = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (aqua::button("Remove Duplicates", ImVec2(110, 0), canRemove)) {
        std::vector<std::uint32_t> doomed;
        KeeperRemap remap;
        for (std::size_t i = 0; i < dupes_.groups.size(); ++i) {
            if (!dupes_.enabled[i]) continue;
            const auto& ids = dupes_.groups[i].trackIds;
            for (std::size_t k = 1; k < ids.size(); ++k) {
                doomed.push_back(ids[k]);
                remap[ids[k]] = ids[0];  // playlists follow the keeper
            }
        }
        performDeleteMany(doomed, &remap);
        dupes_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    aqua::endSheet();
}

void App::openRecovery() {
    if (recovery_.running || sync_.busy()) return;
    const auto* device = activeDevice();
    if (!device || !device->isIpod()) return;
    if (recovery_.thread.joinable()) recovery_.thread.join();
    recovery_.result = {};
    recovery_.mount = device->mountPoint;
    recovery_.cancel.store(false);
    recovery_.finished.store(false);
    recovery_.open = true;
    recovery_.running = true;
    recovery_.thread = std::thread([this, device = *device] {
        try { recovery_.result = scanIpodRecovery(device, &recovery_.cancel); }
        catch (const std::exception& e) { recovery_.result.error = e.what(); }
        recovery_.finished.store(true);
    });
}

void App::drawRecoveryModal() {
    if (recovery_.running && recovery_.finished.load()) {
        recovery_.thread.join();
        recovery_.running = false;
    }
    if (!recovery_.open) return;
    if (!ImGui::IsPopupOpen("Recover Music")) ImGui::OpenPopup("Recover Music");
    if (!aqua::beginSheet("Recover Music", 610.0f)) return;
    aqua::heading(fonts_, "Recover the music already on your iPod");
    aqua::body(fonts_, "Rebuild the song list from the files on this iPod. Audio files stay where they are and are never deleted or retagged.");
    aqua::divider();
    const bool connected = watcher_.find(recovery_.mount) && loadedMount_ == recovery_.mount;
    if (!connected) {
        ImGui::TextWrapped("This iPod disconnected or the selected player changed. Reconnect and scan again.");
        recovery_.cancel.store(true);
    } else if (recovery_.running) {
        ImGui::TextUnformatted("Scanning music files…");
    } else if (!recovery_.result.error.empty()) {
        ImGui::TextWrapped("%s", recovery_.result.error.c_str());
    } else {
        const auto& result = recovery_.result;
        ImGui::Text("%zu songs can be recovered (%s)", result.library.tracks.size(),
                    formatBytes(result.musicBytes).c_str());
        if (!backupRows_.empty())
            ImGui::TextWrapped("A database backup is available. Restore Backup can also recover playlists and listening history; try it first.");
        aqua::body(fonts_, "Titles, artists and albums come from the files' tags. Playlists, ratings and play counts cannot be reconstructed from audio. Any damaged database and old play counts are kept in a recovery archive on the iPod.");
        if (!result.skipped.empty()) {
            ImGui::TextWrapped("%zu files could not be indexed. They will stay on the iPod unchanged.", result.skipped.size());
            if (ImGui::TreeNode("Files that will be left unlisted")) {
                ImGui::BeginChild("skipped_recovery", ImVec2(0, 90), true);
                for (const auto& message : result.skipped) ImGui::TextWrapped("%s", message.c_str());
                ImGui::EndChild();
                ImGui::TreePop();
            }
        }
        if (ImGui::TreeNode("Preview recovered songs")) {
            ImGui::BeginChild("recovered_songs", ImVec2(0, 120), true);
            for (const Track& track : result.library.tracks)
                ImGui::TextWrapped("%s — %s", track.artist.c_str(), track.title.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
    }
    aqua::divider();
    const bool ready = connected && !recovery_.running && recovery_.result.error.empty() &&
                       !recovery_.result.library.tracks.empty() && !sync_.busy() && !appleMusicSyncing();
    ImGui::BeginDisabled(!ready);
    const bool rebuild = aqua::button("Rebuild Song Database", ImVec2(190, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        recovery_.cancel.store(true);
        recovery_.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (rebuild) {
        recovery_.open = false;
        ImGui::CloseCurrentPopup();
        deviceJobKind_ = DeviceJobKind::Recovery;
        deviceJob_.start([recovery = recovery_.result] {
            DeviceResult result;
            fs::path archive;
            result.ok = installIpodRecovery(recovery, &archive, &result.error);
            result.session.status = "Recovered " + std::to_string(recovery.library.tracks.size()) + " songs";
            return result;
        });
        setStatus("Rebuilding song database…");
    }
    aqua::endSheet();
}

void App::drawRestoreModal() {
    if (!restoreOpen_) return;
    if (!ImGui::IsPopupOpen("Restore Database"))
        ImGui::OpenPopup("Restore Database");
    if (!aqua::beginSheet("Restore Database", 580.0f)) return;

    aqua::heading(fonts_, "Put back an earlier version of this iPod's library");
    aqua::body(fonts_,
               "PodBox saves the database before every change it makes. Songs "
               "are not deleted — only the database is replaced, so a file "
               "added since the backup stays on the disk, just unlisted.");
    aqua::divider();

    if (!backupsLoaded_) ImGui::TextDisabled("Checking backups…");
    else if (backupRows_.empty()) ImGui::TextDisabled("No backups yet.");
    fs::path chosen;
    for (const auto& row : backupRows_) {
        ImGui::PushID(row.path.c_str());
        if (aqua::button("Restore", ImVec2(88, 0))) chosen = row.path;
        ImGui::SameLine();
        ImGui::TextUnformatted(row.label.c_str());
        ImGui::PopID();
    }

    aqua::divider();
    aqua::rightAlignButtons(1, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0), true)) {
        restoreOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    aqua::endSheet();

    if (chosen.empty()) return;
    // A restore is a database write like any other: on a device that needs a
    // checksum, replacing the current DB with an older PodBox-written one is
    // just as unreadable. (A backup taken by iTunes would be fine, but we
    // cannot tell the two apart, and guessing wrong bricks the library.)
    if (!restoreSupported()) {
        setStatus(writeBlockReason());
        return;
    }
    if (deviceJob_.busy()) return;
    restoreOpen_ = false;
    if (player_) player_->stop();
    playingTrackId_ = 0;
    deviceJobKind_ = DeviceJobKind::Restore;
    deviceJob_.start([snapshot = static_cast<const DeviceSession&>(*this), chosen]() mutable {
        DeviceResult result;
        result.ok = snapshot.restoreDatabase(chosen);
        result.session = std::move(snapshot);
        return result;
    });
    setStatus("Restoring database…");
}

void App::drawDeletePlaylistModal() {
    if (plEdit_.deleteIndex >= 0 && !ImGui::IsPopupOpen("Delete Playlist"))
        ImGui::OpenPopup("Delete Playlist");
    if (!aqua::beginSheet("Delete Playlist", 460.0f)) return;

    if (!library_ || plEdit_.deleteIndex < 0 ||
        plEdit_.deleteIndex >= int(library_->playlists.size())) {
        plEdit_.deleteIndex = -2;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }

    aqua::heading(fonts_,
                  ("Are you sure you want to delete the playlist \u201c" +
                   library_->playlists[plEdit_.deleteIndex].name +
                   "\u201d?")
                      .c_str());
    aqua::body(fonts_,
               "The songs stay on the player; only the playlist is removed.");

    aqua::divider();
    aqua::rightAlignButtons(2, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        plEdit_.deleteIndex = -2;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aqua::button("Delete", ImVec2(92, 0), true)) {
        const int idx = plEdit_.deleteIndex;
        plEdit_.deleteIndex = -2;
        if (library_->playlists[idx].dbid)
            removedPlaylistVoiceOver_.insert(library_->playlists[idx].dbid);
        library_->playlists.erase(library_->playlists.begin() + idx);
        if (view_ == View::Playlist && playlistIndex_ == idx)
            switchSource(View::Music);
        else if (playlistIndex_ > idx)
            --playlistIndex_;
        visibleDirty_ = true;
        ImGui::CloseCurrentPopup();
        writeDatabase();
    }
    aqua::endSheet();
}

void App::trackContextMenu(const Track& t) {
    if (!ImGui::BeginPopupContextItem()) return;
    // Right-clicking outside the selection moves to that track; inside it,
    // the existing selection is kept so the menu can act on all of it.
    if (!isSelected(t.id)) selectOnly(t.id);
    const int n = int(selection_.size());
    // In the Mac library these all edit library.tsv and touch no device; on the
    // iPod every one of them ends in a database write, so they are unavailable
    // when that write would be refused.
    const bool canEdit = viewingHost() || writesSupported();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::Text));

    if (ImGui::MenuItem("Play")) playTrackId(t.id);
    ImGui::BeginDisabled(!canEdit);
    if (ImGui::MenuItem(n > 1 ? "Get Info…" : "Get Info…", "Cmd+I"))
        openGetInfo();
    ImGui::EndDisabled();
    ImGui::Separator();

    if (viewingHost()) {
        const std::string label =
            n > 1 ? ("Copy " + std::to_string(n) + " Songs to…")
                  : "Copy to…";
        ImGui::BeginDisabled(watcher_.devices().empty() || sync_.busy());
        if (ImGui::BeginMenu(label.c_str())) {
            for (int i = 0; i < int(watcher_.devices().size()); ++i) {
                const DeviceInfo& target = watcher_.devices()[i];
                ImGui::PushID(i);
                if (ImGui::MenuItem(target.volumeName.c_str()))
                    addSelectedHostTracksToDevice(target.mountPoint);
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Show in Finder")) revealSelectedHostTracks();
        ImGui::Separator();
    }

    ImGui::BeginDisabled(!canEdit);
    if (!viewingHost() && ImGui::BeginMenu(
            n > 1 ? "Add These Songs to Playlist" : "Add to Playlist")) {
        if (ImGui::MenuItem("New Playlist…")) createPlaylist(t.id);
        if (library_ && !library_->playlists.empty()) ImGui::Separator();
        for (int i = 0; library_ && i < int(library_->playlists.size()); ++i) {
            if (ImGui::MenuItem(library_->playlists[i].name.c_str()))
                addToPlaylist(i, selection_);
        }
        ImGui::EndMenu();
    }

    const DeviceInfo* device = activeDevice();
    const bool canRate =
        viewingHost() ||
        (device && device->capabilities.ratings && writesSupported());
    ImGui::BeginDisabled(!canRate);
    if (ImGui::BeginMenu(n > 1 ? "Rate These Songs" : "Rating")) {
        static const char* kStars[] = {"No rating", "1 star", "2 stars",
                                       "3 stars", "4 stars", "5 stars"};
        for (int i = 0; i < 6; ++i)
            if (ImGui::MenuItem(kStars[i]))
                for (std::uint32_t id : selection_) setTrackRating(id, i * 20);
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();

    if (view_ == View::Playlist && playlistIndex_ >= 0) {
        if (ImGui::MenuItem("Remove from Playlist")) {
            auto& ids = library_->playlists[playlistIndex_].trackIds;
            // Remove a single occurrence at the selected position if possible.
            if (auto it = std::find(ids.begin(), ids.end(), t.id);
                it != ids.end())
                ids.erase(it);
            visibleDirty_ = true;
            writeDatabase();
        }
    }

    ImGui::Separator();
    if (!viewingHost()) {
        const std::string label =
            n > 1 ? ("Remove " + std::to_string(n) + " Songs from Player")
                  : "Remove from Player";
        if (ImGui::MenuItem(label.c_str())) deleteRequestId_ = t.id;
    }
    ImGui::EndDisabled();

    ImGui::PopStyleColor();
    ImGui::EndPopup();
}
}  // namespace podbox
