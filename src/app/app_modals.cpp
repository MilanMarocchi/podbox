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
                       " songs from your iPod?")
                          .c_str());
    else
        aqua::heading(fonts_, ("Are you sure you want to remove \u201c" +
                               library_->tracks[it->second].title +
                               "\u201d from your iPod?")
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
            const int removed = performDeleteMany(ids);
            setStatus("Removed " + plural(removed, "song", "songs"));
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

    aqua::heading(fonts_, "Where should PodBox look for music?");
    aqua::body(fonts_,
               "Your files stay where they are and are never modified or "
               "moved. Folders named “downloading” or "
               "“incomplete” are skipped, so part-finished "
               "downloads are never indexed.");
    aqua::divider();

    int removeIndex = -1;
    for (std::size_t i = 0; i < host_.watchFolders().size(); ++i) {
        const WatchFolder& w = host_.watchFolders()[i];
        ImGui::PushID(int(i));
        bool on = w.enabled;
        if (ImGui::Checkbox("##on", &on)) host_.setWatchFolderEnabled(i, on);
        ImGui::SameLine();

        int here = 0;
        for (const HostTrack& t : host_.tracks())
            if (t.file.string().rfind(w.path.string(), 0) == 0) ++here;

        std::error_code ec;
        const bool exists = std::filesystem::is_directory(w.path, ec);
        ImGui::TextColored(
            exists ? v4(pal::Text) : v4(pal::Warning), "%s",
            w.path.c_str());
        ImGui::PushFont(fonts_.label);
        ImGui::TextColored(v4(pal::TextDim), "        %s",
                           exists ? (std::to_string(here) + " songs").c_str()
                                  : "folder not found — is the drive "
                                    "connected?");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
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
            host_.save();
            rebuildHostView();
            setStatus("Removed " +
                      plural(gone, "missing song", "missing songs"));
        }
    }

    aqua::divider();
    if (aqua::button("Check for Missing Files", ImVec2(180, 0))) {
        const int n = host_.refreshMissing();
        host_.save();
        rebuildHostView();
        setStatus(n == 0 ? "Every song in your library is present"
                         : plural(n, "song is", "songs are") + " missing");
    }
    ImGui::SameLine();
    if (aqua::button("Add Folder…", ImVec2(120, 0))) {
        const std::filesystem::path picked = chooseFolderDialog();
        if (!picked.empty()) {
            host_.addWatchFolder(picked);
            host_.save();
            rescanWatchFolders();
        }
    }
    ImGui::SameLine();
    aqua::rightAlignButtons(1, 92.0f);
    if (aqua::button("Done", ImVec2(92, 0), true)) {
        host_.save();
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
        AppleMusicRead r = readAppleMusicLibrary();
        {
            std::lock_guard<std::mutex> lock(apple_.mutex);
            apple_.read = std::move(r);
        }
        apple_.finished.store(true);
    });
}

void App::startAppleMusicCopy() {
    if (apple_.busy || apple_.read.tracks.empty()) return;
    if (apple_.thread.joinable()) apple_.thread.join();
    apple_.busy = true;
    apple_.copying = true;
    apple_.finished.store(false);
    apple_.cancel.store(false);
    apple_.done.store(0);
    apple_.total.store(int(apple_.read.tracks.size()));

    apple_.thread = std::thread([this] {
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
    // Fold the copies into the library. Matching on the destination path
    // means re-running an import updates play counts instead of duplicating.
    int added = 0;
    for (const AppleMusicTrack& t : apple_.read.tracks) {
        if (t.file.empty()) continue;
        std::error_code ec;
        if (!fs::exists(t.file, ec)) continue;  // cancelled before this one
        if (host_.upsert(t.file, t.meta, "applemusic", fingerprintFile(t.file)))
            ++added;
    }
    host_.save();
    rebuildHostView();

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
                   "%d skipped — DRM protected, an iPod cannot play these",
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
        aqua::body(fonts_,
                   "Changes the iPod's database only — the audio files on "
                   "the device keep their own tags.");
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
        int changed = 0, tagFailures = 0;
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
                if (getInfo_.writeTags) {
                    std::string err;
                    if (!writeFileTags(h->file, h->meta, &err)) ++tagFailures;
                }
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
            }
        }

        if (viewingHost()) {
            host_.save();
            rebuildHostView();
        } else if (changed > 0) {
            writeDatabase();
        }
        visibleDirty_ = true;
        getInfo_.open = false;
        ImGui::CloseCurrentPopup();

        std::string msg = "Updated " + plural(changed, "song", "songs");
        if (tagFailures)
            msg += " (" + std::to_string(tagFailures) + " file tags failed)";
        setStatus(msg);
    }
    aqua::endSheet();
}

void App::refreshSyncPlan() {
    syncUi_.dirty = false;
    syncUi_.plan = {};
    if (!library_) return;
    syncUi_.plan = planSync(host_, *library_, fingerprints_, syncUi_.options);
}

void App::startSync() {
    if (!library_ || syncUi_.plan.toCopy.empty()) return;

    // Removals first, so space is freed before anything is copied in.
    if (!syncUi_.plan.toRemove.empty()) {
        const int removed = performDeleteMany(syncUi_.plan.toRemove);
        setStatus("Removed " + plural(removed, "song", "songs") +
                  " not in your library");
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

    DupeGuard guard;
    guard.enabled = true;
    for (const Track& t : library_->tracks) {
        const std::string key = duplicateKey(t, MatchMode::Exact);
        if (!key.empty()) guard.metaKeys.insert(key);
    }
    for (const auto& [dbid, fp] : fingerprints_.all())
        if (fp.ok()) guard.hashes.insert(fp.hash);

    sync_.queueAdds(files, loadedMount_, importFormat_, std::move(guard));
    setStatus("Syncing " + plural(int(files.size()), "song", "songs") +
              " to the iPod…");
}

void App::drawSyncModal() {
    if (!syncUi_.open) return;
    if (!ImGui::IsPopupOpen("Sync to iPod")) ImGui::OpenPopup("Sync to iPod");
    if (!aqua::beginSheet("Sync to iPod", 560.0f)) return;
    if (!library_) {
        syncUi_.open = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (syncUi_.dirty) refreshSyncPlan();

    aqua::heading(fonts_, "Sync your library to this iPod");
    aqua::body(fonts_,
               "Everything in your Mac library that isn't already on the iPod "
               "is copied over. Nothing is written until you press Sync.");
    aqua::divider();

    ImGui::Text("%s to copy · %s",
                plural(int(syncUi_.plan.toCopy.size()), "song", "songs").c_str(),
                formatBytes(syncUi_.plan.bytesToCopy).c_str());
    aqua::body(fonts_,
               "%d already on the iPod · %d duplicates skipped · %d "
               "missing from your Mac",
               syncUi_.plan.alreadyOnDevice, syncUi_.plan.skippedDuplicate,
               syncUi_.plan.skippedMissing);
    aqua::body(fonts_,
               "FLAC and other lossless files are converted to 16-bit Apple "
               "Lossless so the iPod can play them.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Also remove songs that aren't in my library",
                        &syncUi_.options.removeFromDevice)) {
        syncUi_.dirty = true;
        syncUi_.confirmRemove = false;
    }
    if (syncUi_.options.removeFromDevice) {
        ImGui::TextColored(v4(pal::Warning),
                           "This deletes %s from the iPod, freeing %s.",
                           plural(int(syncUi_.plan.toRemove.size()), "song",
                                  "songs")
                               .c_str(),
                           formatBytes(syncUi_.plan.bytesToFree).c_str());
        // Every song queued for removal is one the Mac has no copy of, so
        // this is the only step in a sync that destroys music outright.
        if (syncUi_.plan.deviceOnly > 0)
            ImGui::TextColored(v4(pal::Danger),
                               "%s exist only on the iPod — deleting them "
                               "loses them for good.",
                               plural(syncUi_.plan.deviceOnly, "song", "songs")
                                   .c_str());
        if (!syncUi_.plan.toRemove.empty())
            ImGui::Checkbox("Yes, delete those songs from the iPod",
                            &syncUi_.confirmRemove);
    }

    // Capacity guard: refuse a plan that cannot fit rather than filling the
    // device and failing part way.
    const auto& dev = watcher_.device();
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
    const bool blocked = syncUi_.plan.empty() || !fits || blockedByRemoval ||
                         sync_.busy();
    ImGui::BeginDisabled(blocked);
    if (aqua::button("Sync", ImVec2(92, 0), !blocked)) {
        startSync();
        syncUi_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (syncUi_.plan.empty())
        aqua::body(fonts_, "Nothing to do — the iPod already matches.");
    aqua::endSheet();
}

void App::refreshDuplicates() {
    dupes_.dirty = false;
    dupes_.groups.clear();
    dupes_.enabled.clear();
    if (!library_) return;

    dupes_.groups =
        findDuplicates(*library_, dupes_.mode, fingerprints_.all());
    if (dupes_.identicalOnly)
        std::erase_if(dupes_.groups, [](const DuplicateGroup& g) {
            return !g.allIdenticalFiles;
        });
    dupes_.enabled.assign(dupes_.groups.size(), 1);
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
        setStatus("Every song on this iPod is already verified");
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
        if (!dupes_.verify.running() && library_) fingerprints_.save(loadedMount_);
        dupes_.dirty = true;
    }

    if (!dupes_.open) return;
    if (!ImGui::IsPopupOpen("Duplicate Songs"))
        ImGui::OpenPopup("Duplicate Songs");
    if (!aqua::beginSheet("Duplicate Songs", 720.0f)) return;
    if (!library_) {
        dupes_.open = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (dupes_.dirty) refreshDuplicates();

    aqua::heading(fonts_, "Duplicate songs on this iPod");

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
    ImGui::SameLine();
    const int unverified =
        int(library_->tracks.size()) - int(fingerprints_.all().size());
    if (dupes_.verify.running()) {
        ImGui::Text("Verifying %d/%d…", dupes_.verify.done(), dupes_.verify.total());
        ImGui::SameLine();
        if (aqua::button("Stop", ImVec2(70, 0))) dupes_.verify.cancel();
    } else {
        ImGui::BeginDisabled(unverified <= 0);
        if (aqua::button("Verify Files on iPod", ImVec2(160, 0)))
            startVerifyPass();
        ImGui::EndDisabled();
        if (unverified > 0) {
            ImGui::SameLine();
            aqua::body(fonts_, "%d not yet hashed", unverified);
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
        const auto keeperIt = trackIndexById_.find(g.trackIds[0]);
        if (keeperIt == trackIndexById_.end()) continue;
        const Track& keeper = library_->tracks[keeperIt->second];

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
                const auto it = trackIndexById_.find(g.trackIds[k]);
                if (it == trackIndexById_.end()) continue;
                const Track& t = library_->tracks[it->second];
                ImGui::TextColored(
                    k == 0 ? v4(pal::Success) : v4(pal::TextDim),
                    "%s  %s  %u kbps  %s  %u plays", k == 0 ? "keep" : "  ✕ ",
                    formatDuration(t.lengthMs).c_str(), t.bitrate,
                    t.location.c_str(), t.playCount);
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
    ImGui::BeginDisabled(totalCopies == 0);
    if (aqua::button("Remove Duplicates", ImVec2(110, 0), totalCopies > 0)) {
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
        const int removed = performDeleteMany(doomed, &remap);
        setStatus("Removed " + plural(removed, "duplicate", "duplicates") +
                  " · freed " + formatBytes(totalBytes));
        dupes_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
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

    const std::vector<fs::path> backups = availableBackups();
    if (backups.empty()) ImGui::TextDisabled("No backups yet.");

    fs::path chosen;
    for (const fs::path& b : backups) {
        std::error_code ec;
        const auto stamp = fs::last_write_time(b, ec);
        const auto sys =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                stamp - fs::file_time_type::clock::now() +
                std::chrono::system_clock::now());
        const std::time_t when = std::chrono::system_clock::to_time_t(sys);
        char stampText[64] = "unknown time";
        if (const std::tm* tm = std::localtime(&when))
            std::strftime(stampText, sizeof(stampText), "%d %b %H:%M", tm);

        // Parsing each candidate is what makes this trustworthy: the song
        // count is the only thing that tells you which backup you want.
        const ParseResult res = parseItunesDb(b);
        ImGui::PushID(b.c_str());
        ImGui::BeginDisabled(!res.library);
        if (aqua::button("Restore", ImVec2(88, 0))) chosen = b;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%s   %s", stampText,
                    res.library
                        ? plural(int(res.library->tracks.size()), "song",
                                 "songs")
                              .c_str()
                        : "unreadable");
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
    if (!writesSupported()) {
        setStatus("This iPod needs a hashed database — writes not yet supported");
        return;
    }
    if (appleMusicSyncing()) {
        setStatus("Apple Music is syncing this iPod — try again when it "
                  "finishes");
        return;
    }
    const fs::path dbPath = dbFilePath();
    std::error_code ec;
    rotateBackups(dbPath);  // the current state becomes undoable too
    fs::copy_file(chosen, dbPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        restoreOpen_ = false;
        setStatus("Could not restore: " + ec.message());
        return;
    }
    // On a compressed-database device the placeholder iTunesDB must stay a
    // zero-byte placeholder, or the device reads the wrong file.
    if (library_ && library_->compressed) {
        std::ofstream plain(dbPath.parent_path() / "iTunesDB",
                            std::ios::binary | std::ios::trunc);
    }
    restoreOpen_ = false;
    ownWriteTime_ = fs::last_write_time(dbPath, ec);
    if (player_) player_->stop();
    playingTrackId_ = 0;
    loadedMount_.clear();  // forces updateLibrary() to re-read from disk
    setStatus("Restored the database from " + chosen.filename().string());
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
               "The songs stay on the iPod; only the playlist is removed.");

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
        library_->playlists.erase(library_->playlists.begin() + idx);
        if (view_ == View::Playlist && playlistIndex_ == idx)
            switchSource(View::Music);
        else if (playlistIndex_ > idx)
            --playlistIndex_;
        visibleDirty_ = true;
        ImGui::CloseCurrentPopup();
        if (writeDatabase()) setStatus("Deleted playlist");
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

    ImGui::BeginDisabled(!canEdit);
    if (!viewingHost() && ImGui::BeginMenu(
            n > 1 ? "Add These Songs to Playlist" : "Add to Playlist")) {
        if (ImGui::MenuItem("New Playlist…")) createPlaylist(t.id);
        if (library_ && !library_->playlists.empty()) ImGui::Separator();
        for (int i = 0; library_ && i < int(library_->playlists.size()); ++i) {
            if (ImGui::MenuItem(library_->playlists[i].name.c_str()))
                for (std::uint32_t id : selection_) addToPlaylist(i, id);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(n > 1 ? "Rate These Songs" : "Rating")) {
        static const char* kStars[] = {"No rating", "1 star", "2 stars",
                                       "3 stars", "4 stars", "5 stars"};
        for (int i = 0; i < 6; ++i)
            if (ImGui::MenuItem(kStars[i]))
                for (std::uint32_t id : selection_) setTrackRating(id, i * 20);
        ImGui::EndMenu();
    }

    if (view_ == View::Playlist && playlistIndex_ >= 0) {
        if (ImGui::MenuItem("Remove from Playlist")) {
            auto& ids = library_->playlists[playlistIndex_].trackIds;
            // Remove a single occurrence at the selected position if possible.
            if (auto it = std::find(ids.begin(), ids.end(), t.id);
                it != ids.end())
                ids.erase(it);
            visibleDirty_ = true;
            if (writeDatabase()) setStatus("Removed from playlist");
        }
    }

    ImGui::Separator();
    if (!viewingHost()) {
        const std::string label =
            n > 1 ? ("Remove " + std::to_string(n) + " Songs from iPod")
                  : "Remove from iPod";
        if (ImGui::MenuItem(label.c_str())) deleteRequestId_ = t.id;
    }
    ImGui::EndDisabled();

    ImGui::PopStyleColor();
    ImGui::EndPopup();
}
}  // namespace podbox
