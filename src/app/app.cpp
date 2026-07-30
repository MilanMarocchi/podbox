#include "app/app.h"

#include "device/ipod_device.h"
#include "itdb/playcounts.h"
#include "library/artwork.h"
#include "library/dedupe.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include "ui/aqua.h"
#include "ui/theme.h"

#include <imgui.h>

#define GL_SILENCE_DEPRECATION
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <unordered_set>

#include <strings.h>  // strcasecmp

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr float kSidebarWidth = 200.0f;
constexpr float kToolbarHeight = 56.0f;
constexpr float kStatusBarHeight = 24.0f;
constexpr float kLcdWidth = 360.0f;
constexpr float kLcdHeight = 40.0f;
constexpr float kSearchWidth = 170.0f;

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void addTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                     ImU32 color, const char* text) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), color,
                text);
}

// Five stars, iTunes-style. Returns the rating (0-100) the user clicked, or
// -1 when they did not. Drawn rather than using a widget so it matches the
// rest of the hand-drawn chrome.
int drawStars(ImDrawList* dl, ImVec2 p, std::uint8_t rating, bool hovered,
              ImVec2 mouse, bool clicked) {
    constexpr float kStep = 13.0f, kR = 5.0f;
    const int filled = rating / 20;
    int result = -1;
    for (int i = 0; i < 5; ++i) {
        const ImVec2 c(p.x + kStep * i + kR, p.y + kR);
        const bool on = i < filled;
        // A hovered row previews the rating the click would set.
        const bool preview =
            hovered && mouse.x >= p.x && mouse.x < p.x + kStep * 5 &&
            mouse.x >= c.x - kR;
        const ImU32 col = (on || preview) ? pal::rgb(70, 110, 190)
                                          : pal::rgb(200, 202, 206);
        dl->AddCircleFilled(c, on || preview ? kR * 0.62f : kR * 0.34f, col, 8);
        if (clicked && hovered && mouse.x >= c.x - kR && mouse.x < c.x + kR)
            result = (i + 1) * 20;
    }
    // Clicking left of the first star clears the rating.
    if (clicked && hovered && mouse.x >= p.x - kStep * 0.6f && mouse.x < p.x)
        result = 0;
    return result;
}

// Small iPod glyph drawn with primitives, iTunes-sidebar style.
void drawIpodIcon(ImDrawList* dl, ImVec2 p) {
    const float w = 11.0f, h = 16.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal::rgb(250, 250, 250),
                      2.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pal::rgb(110, 116, 124), 2.5f);
    dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + w - 2, p.y + 7),
                      pal::rgb(120, 150, 200), 1.0f);
    dl->AddCircle(ImVec2(p.x + w * 0.5f, p.y + 11.5f), 2.6f,
                  pal::rgb(150, 155, 160));
}

bool containsCi(const std::string& haystack, const std::string& lowerNeedle) {
    if (lowerNeedle.empty()) return true;
    return toLower(haystack).find(lowerNeedle) != std::string::npos;
}

int cmpCi(const std::string& a, const std::string& b) {
    return strcasecmp(a.c_str(), b.c_str());
}

// ':'-separated iTunesDB location -> absolute path on the mounted device.
fs::path locationToPath(const fs::path& mount, const std::string& location) {
    std::string rel = location;
    if (!rel.empty() && rel[0] == ':') rel.erase(0, 1);
    std::replace(rel.begin(), rel.end(), ':', '/');
    return mount / rel;
}

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + ' ' + (n == 1 ? one : many);
}

// What the status bar says once an import batch drains.
std::string importSummary(int added, int skipped) {
    if (added == 0 && skipped > 0)
        return skipped == 1
                   ? "That song is already on this iPod"
                   : "All " + plural(skipped, "song is", "songs are") +
                         " already on this iPod";
    std::string msg = "Added " + plural(added, "song", "songs");
    if (skipped > 0)
        msg += "  ·  skipped " + plural(skipped, "duplicate", "duplicates");
    return msg;
}

std::string formatTotalDuration(std::uint64_t ms) {
    const double minutes = double(ms) / 60000.0;
    char buf[32];
    if (minutes < 60.0)
        std::snprintf(buf, sizeof(buf), "%.0f minutes", minutes);
    else if (minutes < 60.0 * 24.0)
        std::snprintf(buf, sizeof(buf), "%.1f hours", minutes / 60.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1f days", minutes / (60.0 * 24.0));
    return buf;
}

}  // namespace

App::~App() {
    appleCancel_.store(true);
    if (scanThread_.joinable()) scanThread_.join();
    if (appleThread_.joinable()) appleThread_.join();
}

void App::frame() {
    if (!hostLoaded_) {
        hostLoaded_ = true;
        // A missing file just means this is a first run; offer the folder
        // most people's music is already in.
        if (!host_.load()) {
            host_.seedDefaultWatchFolders();
            host_.save();
        }
        rebuildHostView();
        if (hostView_.tracks.empty() && !host_.watchFolders().empty())
            view_ = View::Library;
    }
    applyFinishedScan();
    watcher_.update(ImGui::GetTime());
    updateLibrary();
    applyCompletedAdds();
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
    drawFoldersModal();
    drawAppleMusicModal();
    drawSyncModal();
    drawGetInfoModal();

    if (ejectRequested_) {
        ejectRequested_ = false;
        std::string err;
        const fs::path mount = loadedMount_;
        if (player_) player_->stop();
        playingTrackId_ = 0;
        if (ejectDevice(mount, &err)) {
            // Drop the library now; the watcher will confirm the unmount.
            library_.reset();
            loadedMount_.clear();
            setStatus("Ejected — safe to disconnect");
        } else {
            setStatus("Could not eject: " + err);
        }
    }

    ImGui::End();
}

void App::setStatus(const std::string& msg) {
    statusMsg_ = msg;
    statusMsgUntil_ = ImGui::GetTime() + 4.0;
}

void App::onFilesDropped(const std::vector<std::string>& paths) {
    if (!watcher_.device() || !library_) {
        setStatus("Connect an iPod before adding songs");
        return;
    }
    if (library_->hashingScheme != 0) {
        setStatus("This iPod needs a hashed database — writes not yet supported");
        return;
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (const std::string& p : paths) {
        const fs::path path = p;
        if (fs::is_directory(path, ec)) {
            for (const auto& e :
                 fs::recursive_directory_iterator(path, ec)) {
                if (e.is_regular_file(ec) &&
                    isImportableAudioFile(e.path()))
                    files.push_back(e.path());
            }
        } else if (isImportableAudioFile(path)) {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        setStatus("No supported audio files (MP3/AAC/ALAC/WAV/AIFF/FLAC)");
        return;
    }

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
        for (const auto& [dbid, fp] : fingerprints_.all())
            if (fp.ok()) guard.hashes.insert(fp.hash);
    }
    sync_.queueAdds(files, loadedMount_, importFormat_, std::move(guard));
}

void App::applyCompletedAdds() {
    if (!library_) return;
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
        done.track.dbid = rng();
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
    if (pendingDbWrite_ && !sync_.busy()) {
        pendingDbWrite_ = false;
        const bool wrote = lastBatchAdded_ > 0 ? writeDatabase() : true;
        if (wrote) setStatus(importSummary(lastBatchAdded_, lastBatchSkipped_));
        lastBatchAdded_ = 0;
        lastBatchSkipped_ = 0;
    }
}

bool App::appleMusicSyncing() const {
    if (loadedMount_.empty()) return false;
    // Apple Music rewrites both of these continuously while it syncs. A
    // timestamp newer than our own last write means the change was not ours.
    const fs::path itunes = loadedMount_ / "iPod_Control" / "iTunes";
    const auto now = fs::file_time_type::clock::now();
    for (const char* name : {"iTunesDB", "iTunesPrefs"}) {
        std::error_code ec;
        const auto stamp = fs::last_write_time(itunes / name, ec);
        if (ec || stamp <= ownWriteTime_) continue;
        const auto age =
            std::chrono::duration_cast<std::chrono::seconds>(now - stamp)
                .count();
        if (age >= 0 && age < 20) return true;
    }
    return false;
}

void App::rotateBackups(const fs::path& dbPath) {
    // Five generations of a sub-megabyte file: cheap, and it means any PodBox
    // write can be undone rather than only the very first one.
    constexpr int kKeep = 5;
    const std::string base = dbPath.string() + ".podbox-bak.";
    std::error_code ec;
    fs::remove(base + std::to_string(kKeep), ec);
    for (int i = kKeep - 1; i >= 1; --i)
        fs::rename(base + std::to_string(i), base + std::to_string(i + 1), ec);
    fs::copy_file(dbPath, base + "1", ec);
}

std::vector<fs::path> App::availableBackups() const {
    std::vector<fs::path> out;
    if (loadedMount_.empty()) return out;
    const fs::path dbPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesDB";
    std::error_code ec;
    // The one-shot backup from before rotation existed is still the oldest and
    // most valuable snapshot, so keep offering it.
    const fs::path legacy = dbPath.string() + ".podbox-backup";
    for (int i = 1; i <= 5; ++i) {
        const fs::path p = dbPath.string() + ".podbox-bak." + std::to_string(i);
        if (fs::exists(p, ec)) out.push_back(p);
    }
    if (fs::exists(legacy, ec)) out.push_back(legacy);
    return out;
}

bool App::writeDatabase() {
    if (!library_ || loadedMount_.empty()) return false;
    if (appleMusicSyncing()) {
        setStatus("Apple Music is syncing this iPod — try again when it finishes");
        return false;
    }
    const fs::path dbPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesDB";
    std::error_code ec;
    // Keep the original iTunes-written DB around, once.
    const fs::path backup = dbPath.string() + ".podbox-backup";
    if (fs::exists(dbPath, ec) && !fs::exists(backup, ec))
        fs::copy_file(dbPath, backup, ec);
    if (fs::exists(dbPath, ec)) rotateBackups(dbPath);

    const fs::path tmp = dbPath.string() + ".podbox-tmp";
    std::string err;
    if (!writeItunesDb(*library_, tmp, &err)) {
        setStatus(err);
        return false;
    }
    fs::rename(tmp, dbPath, ec);
    if (ec) {
        setStatus("Could not update database: " + ec.message());
        return false;
    }
    // The DB we just wrote already includes any merged play counts, so the
    // firmware's Play Counts file is now stale — remove it to avoid double
    // counting on the next connect. The iPod recreates it as it plays.
    //
    // Unless the merge never happened: deleting an unmatched file throws away
    // listening history that was never folded in anywhere.
    if (!playCountsUnmatched_)
        fs::remove(dbPath.parent_path() / "Play Counts", ec);

    // Keep the fingerprint sidecar in step with the DB it describes.
    fingerprints_.prune(*library_);
    fingerprints_.save(loadedMount_);

    // Remember this write so appleMusicSyncing() can tell ours from theirs.
    ownWriteTime_ = fs::last_write_time(dbPath, ec);
    return true;
}

void App::setTrackRating(std::uint32_t trackId, int rating) {
    if (rating < 0 || rating > 100) return;

    if (viewingHost()) {
        for (HostTrack& h : host_.tracks()) {
            if (std::uint32_t(h.id) != trackId) continue;
            h.meta.rating = std::uint8_t(rating);
            host_.save();
            rebuildHostView();
            return;
        }
        return;
    }

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
        setStatus("Removed “" + title + "”");
}

int App::performDeleteMany(
    const std::vector<std::uint32_t>& ids,
    const std::unordered_map<std::uint32_t, std::uint32_t>* remap) {
    if (!library_ || ids.empty()) return 0;

    std::unordered_set<std::uint32_t> doomed(ids.begin(), ids.end());
    // A track that something is being remapped *to* is by definition a keeper;
    // removing it as well would lose the song entirely.
    if (remap)
        for (const auto& [from, to] : *remap) doomed.erase(to);

    std::error_code ec;
    int removed = 0;
    for (std::uint32_t id : doomed) {
        const auto it = trackIndexById_.find(id);
        if (it == trackIndexById_.end()) continue;
        fs::remove(locationToPath(loadedMount_,
                                  library_->tracks[it->second].location),
                   ec);
        ++removed;
    }
    if (removed == 0) return 0;

    std::erase_if(library_->tracks,
                  [&](const Track& t) { return doomed.count(t.id) != 0; });

    for (auto& pl : library_->playlists)
        applyRemovalToPlaylist(doomed, remap, &pl.trackIds);

    trackIndexById_.clear();
    trackIndexById_.reserve(library_->tracks.size());
    for (int i = 0; i < int(library_->tracks.size()); ++i)
        trackIndexById_[library_->tracks[i].id] = i;

    std::erase_if(selection_, [&](std::uint32_t id) { return doomed.count(id) != 0; });
    if (doomed.count(selectedTrackId_))
        selectedTrackId_ = selection_.empty() ? 0 : selection_.back();
    if (doomed.count(selectionAnchor_)) selectionAnchor_ = selectedTrackId_;
    // Stop playback rather than leave it pointed at a file we just unlinked.
    if (doomed.count(playingTrackId_)) {
        if (player_) player_->stop();
        playingTrackId_ = 0;
    }
    visibleDirty_ = true;
    writeDatabase();
    return removed;
}

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
            exists ? v4(pal::rgb(30, 30, 30)) : v4(pal::rgb(150, 90, 20)), "%s",
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
        ImGui::TextColored(v4(pal::rgb(160, 60, 20)),
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
    if (appleBusy_) return;
    if (appleThread_.joinable()) appleThread_.join();
    appleBusy_ = true;
    appleCopying_ = false;
    appleFinished_.store(false);
    appleCancel_.store(false);
    appleThread_ = std::thread([this] {
        AppleMusicRead r = readAppleMusicLibrary();
        {
            std::lock_guard<std::mutex> lock(appleMutex_);
            appleRead_ = std::move(r);
        }
        appleFinished_.store(true);
    });
}

void App::startAppleMusicCopy() {
    if (appleBusy_ || appleRead_.tracks.empty()) return;
    if (appleThread_.joinable()) appleThread_.join();
    appleBusy_ = true;
    appleCopying_ = true;
    appleFinished_.store(false);
    appleCancel_.store(false);
    appleDone_.store(0);
    appleTotal_.store(int(appleRead_.tracks.size()));

    appleThread_ = std::thread([this] {
        CopyResult res = copyAppleMusicFiles(
            appleRead_.tracks, appleMusicCopyRoot(),
            [this](int done, int total, const std::string& name) {
                appleDone_.store(done);
                appleTotal_.store(total);
                {
                    std::lock_guard<std::mutex> lock(appleMutex_);
                    appleCurrent_ = name;
                }
                return !appleCancel_.load();
            });
        {
            std::lock_guard<std::mutex> lock(appleMutex_);
            appleCopy_ = res;
        }
        appleFinished_.store(true);
    });
}

void App::applyFinishedAppleMusic() {
    if (!appleFinished_.load()) return;
    appleFinished_.store(false);
    if (appleThread_.joinable()) appleThread_.join();
    appleBusy_ = false;
    if (!appleCopying_) return;  // a read just finished; the sheet shows it

    appleCopying_ = false;
    // Fold the copies into the library. Matching on the destination path
    // means re-running an import updates play counts instead of duplicating.
    int added = 0;
    for (const AppleMusicTrack& t : appleRead_.tracks) {
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
              (appleCopy_.cancelled ? " (stopped early)" : ""));
}

void App::drawAppleMusicModal() {
    applyFinishedAppleMusic();
    if (!appleMusicOpen_) return;
    if (!ImGui::IsPopupOpen("Import from Apple Music"))
        ImGui::OpenPopup("Import from Apple Music");
    if (!aqua::beginSheet("Import from Apple Music", 540.0f)) return;

    aqua::heading(fonts_, "Import your Apple Music library");
    aqua::body(fonts_,
               "Songs are copied into %s. Apple Music is only read from — "
               "nothing there is changed or moved.",
               appleMusicCopyRoot().c_str());
    aqua::divider();

    if (appleBusy_ && appleCopying_) {
        const int done = appleDone_.load(), total = appleTotal_.load();
        std::string current;
        {
            std::lock_guard<std::mutex> lock(appleMutex_);
            current = appleCurrent_;
        }
        ImGui::Text("Copying %d of %d", done, total);
        ImGui::ProgressBar(total > 0 ? float(done) / float(total) : 0.0f,
                           ImVec2(-1, 14));
        aqua::body(fonts_, "%.60s", current.c_str());
        ImGui::Spacing();
        aqua::rightAlignButtons(1, 92.0f);
        if (aqua::button("Stop", ImVec2(92, 0))) appleCancel_.store(true);
        aqua::body(fonts_, "Stopping keeps everything copied so far.");
        aqua::endSheet();
        return;
    }

    if (appleBusy_) {
        ImGui::TextUnformatted("Reading your Apple Music library…");
        aqua::endSheet();
        return;
    }

    if (!appleRead_.ok && appleRead_.error.empty()) {
        if (aqua::button("Read Apple Music Library", ImVec2(200, 0), true))
            startAppleMusicRead();
        ImGui::SameLine();
        if (aqua::button("Cancel", ImVec2(92, 0))) {
            appleMusicOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        aqua::endSheet();
        return;
    }

    if (!appleRead_.error.empty()) {
        ImGui::TextColored(v4(pal::rgb(160, 40, 40)), "%s",
                           appleRead_.error.c_str());
        ImGui::Spacing();
        aqua::rightAlignButtons(2, 92.0f);
        if (aqua::button("Close", ImVec2(92, 0))) {
            appleMusicOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aqua::button("Try Again", ImVec2(92, 0), true)) startAppleMusicRead();
        aqua::endSheet();
        return;
    }

    std::uint64_t bytes = 0;
    for (const AppleMusicTrack& t : appleRead_.tracks)
        bytes += t.meta.sizeBytes;
    ImGui::Text("%s to copy · %s",
                plural(int(appleRead_.tracks.size()), "song", "songs").c_str(),
                formatBytes(bytes).c_str());

    // Anything Apple Music lists but cannot hand over is reported rather than
    // quietly dropped.
    if (appleRead_.streamingOnly)
        aqua::body(fonts_, "%d skipped — streaming only, no file on this Mac",
                   appleRead_.streamingOnly);
    if (appleRead_.fileMissing)
        aqua::body(fonts_, "%d skipped — file listed but not on disk",
                   appleRead_.fileMissing);
    if (appleRead_.drmProtected)
        aqua::body(fonts_,
                   "%d skipped — DRM protected, an iPod cannot play these",
                   appleRead_.drmProtected);

    aqua::divider();
    if (aqua::button("Re-read", ImVec2(92, 0))) startAppleMusicRead();
    ImGui::SameLine();
    aqua::rightAlignButtons(2, 100.0f);
    if (aqua::button("Cancel", ImVec2(100, 0))) {
        appleMusicOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aqua::button("Copy to PodBox", ImVec2(100, 0), true))
        startAppleMusicCopy();
    aqua::endSheet();
}

void App::openGetInfo() {
    const Library* shown = shownLibrary();
    const auto* index = shownIndex();
    if (!shown || !index || selection_.empty()) return;

    // Fields shared by every selected track are pre-filled; fields that
    // differ start blank and are left alone unless the user types something.
    const Track* first = nullptr;
    bool sameTitle = true, sameArtist = true, sameAlbum = true;
    bool sameGenre = true, sameYear = true, sameTrack = true;
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
    }
    if (!first) return;

    auto put = [](char* buf, std::size_t n, const std::string& v, bool same) {
        std::snprintf(buf, n, "%s", same ? v.c_str() : "");
    };
    // A title is per-song by nature, so editing many at once never prefills it.
    put(giTitle_, sizeof(giTitle_), first->title,
        sameTitle && selection_.size() == 1);
    put(giArtist_, sizeof(giArtist_), first->artist, sameArtist);
    put(giAlbum_, sizeof(giAlbum_), first->album, sameAlbum);
    put(giGenre_, sizeof(giGenre_), first->genre, sameGenre);
    std::snprintf(giYear_, sizeof(giYear_), "%s",
                  sameYear && first->year ? std::to_string(first->year).c_str()
                                          : "");
    std::snprintf(giTrack_, sizeof(giTrack_), "%s",
                  sameTrack && first->trackNumber
                      ? std::to_string(first->trackNumber).c_str()
                      : "");
    giWriteTags_ = false;
    getInfoOpen_ = true;
}

void App::drawGetInfoModal() {
    if (!getInfoOpen_) return;
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
    if (n == 1) ImGui::InputText("Name", giTitle_, sizeof(giTitle_));
    ImGui::InputText("Artist", giArtist_, sizeof(giArtist_));
    ImGui::InputText("Album", giAlbum_, sizeof(giAlbum_));
    ImGui::InputText("Genre", giGenre_, sizeof(giGenre_));
    ImGui::InputText("Year", giYear_, sizeof(giYear_),
                     ImGuiInputTextFlags_CharsDecimal);
    if (n == 1)
        ImGui::InputText("Track number", giTrack_, sizeof(giTrack_),
                         ImGuiInputTextFlags_CharsDecimal);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (viewingHost()) {
        ImGui::Checkbox("Also write these tags into the files", &giWriteTags_);
        aqua::body(fonts_, giWriteTags_
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
        getInfoOpen_ = false;
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
                assign(h->meta.title, giTitle_);
                assign(h->meta.artist, giArtist_);
                assign(h->meta.album, giAlbum_);
                assign(h->meta.genre, giGenre_);
                if (giYear_[0]) h->meta.year = std::uint32_t(std::atoi(giYear_));
                if (giTrack_[0])
                    h->meta.trackNumber = std::uint32_t(std::atoi(giTrack_));
                ++changed;
                if (giWriteTags_) {
                    std::string err;
                    if (!writeFileTags(h->file, h->meta, &err)) ++tagFailures;
                }
            } else {
                if (!library_ || !index) break;
                const auto it = index->find(id);
                if (it == index->end()) continue;
                Track& t = library_->tracks[it->second];
                assign(t.title, giTitle_);
                assign(t.artist, giArtist_);
                assign(t.album, giAlbum_);
                assign(t.genre, giGenre_);
                if (giYear_[0]) t.year = std::uint32_t(std::atoi(giYear_));
                if (giTrack_[0])
                    t.trackNumber = std::uint32_t(std::atoi(giTrack_));
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
        getInfoOpen_ = false;
        ImGui::CloseCurrentPopup();

        std::string msg = "Updated " + plural(changed, "song", "songs");
        if (tagFailures)
            msg += " (" + std::to_string(tagFailures) + " file tags failed)";
        setStatus(msg);
    }
    aqua::endSheet();
}

void App::refreshSyncPlan() {
    syncDirty_ = false;
    syncPlan_ = {};
    if (!library_) return;
    syncPlan_ = planSync(host_, *library_, fingerprints_, syncOptions_);
}

void App::startSync() {
    if (!library_ || syncPlan_.toCopy.empty()) return;

    // Removals first, so space is freed before anything is copied in.
    if (!syncPlan_.toRemove.empty()) {
        const int removed = performDeleteMany(syncPlan_.toRemove);
        setStatus("Removed " + plural(removed, "song", "songs") +
                  " not in your library");
    }

    // Copying reuses the drag-and-drop pipeline: same worker, same transcode,
    // same duplicate guard. The guard also protects against a plan that has
    // gone stale since it was computed.
    std::vector<fs::path> files;
    files.reserve(syncPlan_.toCopy.size());
    for (std::uint64_t id : syncPlan_.toCopy) {
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
    if (!syncOpen_) return;
    if (!ImGui::IsPopupOpen("Sync to iPod")) ImGui::OpenPopup("Sync to iPod");
    if (!aqua::beginSheet("Sync to iPod", 560.0f)) return;
    if (!library_) {
        syncOpen_ = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (syncDirty_) refreshSyncPlan();

    aqua::heading(fonts_, "Sync your library to this iPod");
    aqua::body(fonts_,
               "Everything in your Mac library that isn't already on the iPod "
               "is copied over. Nothing is written until you press Sync.");
    aqua::divider();

    ImGui::Text("%s to copy · %s",
                plural(int(syncPlan_.toCopy.size()), "song", "songs").c_str(),
                formatBytes(syncPlan_.bytesToCopy).c_str());
    aqua::body(fonts_,
               "%d already on the iPod · %d duplicates skipped · %d "
               "missing from your Mac",
               syncPlan_.alreadyOnDevice, syncPlan_.skippedDuplicate,
               syncPlan_.skippedMissing);
    aqua::body(fonts_,
               "FLAC and other lossless files are converted to 16-bit Apple "
               "Lossless so the iPod can play them.");

    ImGui::Spacing();
    if (ImGui::Checkbox("Also remove songs that aren't in my library",
                        &syncOptions_.removeFromDevice)) {
        syncDirty_ = true;
        syncConfirmRemove_ = false;
    }
    if (syncOptions_.removeFromDevice) {
        ImGui::TextColored(v4(pal::rgb(160, 60, 20)),
                           "This deletes %s from the iPod, freeing %s.",
                           plural(int(syncPlan_.toRemove.size()), "song",
                                  "songs")
                               .c_str(),
                           formatBytes(syncPlan_.bytesToFree).c_str());
        // Every song queued for removal is one the Mac has no copy of, so
        // this is the only step in a sync that destroys music outright.
        if (syncPlan_.deviceOnly > 0)
            ImGui::TextColored(v4(pal::rgb(170, 30, 30)),
                               "%s exist only on the iPod — deleting them "
                               "loses them for good.",
                               plural(syncPlan_.deviceOnly, "song", "songs")
                                   .c_str());
        if (!syncPlan_.toRemove.empty())
            ImGui::Checkbox("Yes, delete those songs from the iPod",
                            &syncConfirmRemove_);
    }

    // Capacity guard: refuse a plan that cannot fit rather than filling the
    // device and failing part way.
    const auto& dev = watcher_.device();
    bool fits = true;
    if (dev && dev->freeBytes > 0) {
        const std::uint64_t after =
            syncPlan_.bytesToCopy > syncPlan_.bytesToFree
                ? syncPlan_.bytesToCopy - syncPlan_.bytesToFree
                : 0;
        fits = after <= dev->freeBytes;
        if (!fits)
            ImGui::TextColored(v4(pal::rgb(160, 40, 40)),
                               "Not enough room — needs %s more.",
                               formatBytes(after - dev->freeBytes).c_str());
    }

    const bool blockedByRemoval =
        syncOptions_.removeFromDevice && !syncPlan_.toRemove.empty() &&
        !syncConfirmRemove_;

    aqua::divider();
    aqua::rightAlignButtons(2, 92.0f);
    if (aqua::button("Cancel", ImVec2(92, 0))) {
        syncOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const bool blocked = syncPlan_.empty() || !fits || blockedByRemoval ||
                         sync_.busy();
    ImGui::BeginDisabled(blocked);
    if (aqua::button("Sync", ImVec2(92, 0), !blocked)) {
        startSync();
        syncOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (syncPlan_.empty())
        aqua::body(fonts_, "Nothing to do — the iPod already matches.");
    aqua::endSheet();
}

void App::refreshDuplicates() {
    duplicatesDirty_ = false;
    duplicateGroups_.clear();
    duplicateEnabled_.clear();
    if (!library_) return;

    duplicateGroups_ =
        findDuplicates(*library_, duplicateMode_, fingerprints_.all());
    if (duplicatesIdenticalOnly_)
        std::erase_if(duplicateGroups_, [](const DuplicateGroup& g) {
            return !g.allIdenticalFiles;
        });
    duplicateEnabled_.assign(duplicateGroups_.size(), 1);
}

void App::startVerifyPass() {
    if (!library_ || verify_.running()) return;
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
    verify_.start(std::move(items));
}

void App::drawDuplicatesModal() {
    // Fold in whatever the verify pass has produced, whether or not the
    // sheet is open, so a cancelled run still keeps its work.
    if (auto results = verify_.take(); !results.empty()) {
        for (const auto& [dbid, fp] : results)
            fingerprints_.put(dbid, fp, FingerprintStore::Origin::Device);
        if (!verify_.running() && library_) fingerprints_.save(loadedMount_);
        duplicatesDirty_ = true;
    }

    if (!duplicatesOpen_) return;
    if (!ImGui::IsPopupOpen("Duplicate Songs"))
        ImGui::OpenPopup("Duplicate Songs");
    if (!aqua::beginSheet("Duplicate Songs", 720.0f)) return;
    if (!library_) {
        duplicatesOpen_ = false;
        ImGui::CloseCurrentPopup();
        aqua::endSheet();
        return;
    }
    if (duplicatesDirty_) refreshDuplicates();

    aqua::heading(fonts_, "Duplicate songs on this iPod");

    int mode = int(duplicateMode_);
    if (ImGui::RadioButton("Exact", &mode, int(MatchMode::Exact)))
        duplicatesDirty_ = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Loose", &mode, int(MatchMode::Loose)))
        duplicatesDirty_ = true;
    duplicateMode_ = MatchMode(mode);
    ImGui::SameLine();
    aqua::body(fonts_, duplicateMode_ == MatchMode::Exact
                           ? "artist, title, album and length"
                           : "artist and title only — will group live and "
                             "studio versions of a song");

    if (ImGui::Checkbox("Only byte-identical copies", &duplicatesIdenticalOnly_))
        duplicatesDirty_ = true;
    ImGui::SameLine();
    const int unverified =
        int(library_->tracks.size()) - int(fingerprints_.all().size());
    if (verify_.running()) {
        ImGui::Text("Verifying %d/%d…", verify_.done(), verify_.total());
        ImGui::SameLine();
        if (aqua::button("Stop", ImVec2(70, 0))) verify_.cancel();
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
    for (std::size_t i = 0; i < duplicateGroups_.size(); ++i) {
        if (!duplicateEnabled_[i]) continue;
        ++activeGroups;
        totalCopies += int(duplicateGroups_[i].trackIds.size()) - 1;
        totalBytes += duplicateGroups_[i].reclaimBytes;
    }

    ImGui::BeginChild("dupe_list", ImVec2(0, 300));
    if (duplicateGroups_.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::TextDisabled(
            duplicatesIdenticalOnly_
                ? "No byte-identical duplicates. Transcoded copies never match "
                  "byte-for-byte — clear the checkbox to see them."
                : "No duplicates found.");
    }
    for (std::size_t i = 0; i < duplicateGroups_.size(); ++i) {
        const DuplicateGroup& g = duplicateGroups_[i];
        const auto keeperIt = trackIndexById_.find(g.trackIds[0]);
        if (keeperIt == trackIndexById_.end()) continue;
        const Track& keeper = library_->tracks[keeperIt->second];

        ImGui::PushID(int(i));
        bool on = duplicateEnabled_[i] != 0;
        if (ImGui::Checkbox("##on", &on)) duplicateEnabled_[i] = on ? 1 : 0;
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
                    k == 0 ? v4(pal::rgb(20, 110, 30)) : v4(pal::TextDim),
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
        duplicatesOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(totalCopies == 0);
    if (aqua::button("Remove Duplicates", ImVec2(110, 0), totalCopies > 0)) {
        std::vector<std::uint32_t> doomed;
        KeeperRemap remap;
        for (std::size_t i = 0; i < duplicateGroups_.size(); ++i) {
            if (!duplicateEnabled_[i]) continue;
            const auto& ids = duplicateGroups_[i].trackIds;
            for (std::size_t k = 1; k < ids.size(); ++k) {
                doomed.push_back(ids[k]);
                remap[ids[k]] = ids[0];  // playlists follow the keeper
            }
        }
        const int removed = performDeleteMany(doomed, &remap);
        setStatus("Removed " + plural(removed, "duplicate", "duplicates") +
                  " · freed " + formatBytes(totalBytes));
        duplicatesOpen_ = false;
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
    if (appleMusicSyncing()) {
        setStatus("Apple Music is syncing this iPod — try again when it "
                  "finishes");
        return;
    }
    const fs::path dbPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesDB";
    std::error_code ec;
    rotateBackups(dbPath);  // the current state becomes undoable too
    fs::copy_file(chosen, dbPath, fs::copy_options::overwrite_existing, ec);
    restoreOpen_ = false;
    if (ec) {
        setStatus("Could not restore: " + ec.message());
        return;
    }
    ownWriteTime_ = fs::last_write_time(dbPath, ec);
    if (player_) player_->stop();
    playingTrackId_ = 0;
    loadedMount_.clear();  // forces updateLibrary() to re-read from disk
    setStatus("Restored the database from " + chosen.filename().string());
}

void App::createPlaylist(std::uint32_t withTrackId) {
    if (!library_) return;
    Playlist pl;
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
    if (withTrackId) pl.trackIds.push_back(withTrackId);
    library_->playlists.push_back(std::move(pl));
    playlistIndex_ = int(library_->playlists.size()) - 1;
    view_ = View::Playlist;
    visibleDirty_ = true;
    // Open inline rename on the fresh playlist.
    renamePlaylistIndex_ = playlistIndex_;
    renameJustOpened_ = true;
    std::snprintf(renameBuf_, sizeof(renameBuf_), "%s",
                  library_->playlists[playlistIndex_].name.c_str());
    if (writeDatabase()) setStatus("Created playlist");
}

void App::addToPlaylist(int playlistIndex, std::uint32_t trackId) {
    if (!library_ || playlistIndex < 0 ||
        playlistIndex >= int(library_->playlists.size()))
        return;
    auto& ids = library_->playlists[playlistIndex].trackIds;
    ids.push_back(trackId);
    if (view_ == View::Playlist && playlistIndex_ == playlistIndex)
        visibleDirty_ = true;
    if (writeDatabase())
        setStatus("Added to “" + library_->playlists[playlistIndex].name + "”");
}

void App::drawDeletePlaylistModal() {
    if (deletePlaylistIndex_ >= 0 && !ImGui::IsPopupOpen("Delete Playlist"))
        ImGui::OpenPopup("Delete Playlist");
    if (!ImGui::BeginPopupModal("Delete Playlist", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (!library_ || deletePlaylistIndex_ < 0 ||
        deletePlaylistIndex_ >= int(library_->playlists.size())) {
        deletePlaylistIndex_ = -2;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Delete the playlist “%s”?",
                library_->playlists[deletePlaylistIndex_].name.c_str());
    ImGui::TextDisabled("The songs stay on the iPod; only the playlist is removed.");
    ImGui::Spacing();
    if (ImGui::Button("Delete", ImVec2(90, 0))) {
        const int idx = deletePlaylistIndex_;
        deletePlaylistIndex_ = -2;
        library_->playlists.erase(library_->playlists.begin() + idx);
        if (view_ == View::Playlist && playlistIndex_ == idx) {
            view_ = View::Music;
            playlistIndex_ = -1;
        } else if (playlistIndex_ > idx) {
            --playlistIndex_;
        }
        visibleDirty_ = true;
        ImGui::CloseCurrentPopup();
        if (writeDatabase()) setStatus("Deleted playlist");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        deletePlaylistIndex_ = -2;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
            const int lo = std::min(anchorRow, row), hi = std::max(anchorRow, row);
            selection_.clear();
            for (int i = lo; i <= hi && i < int(visible_.size()); ++i)
                selection_.push_back(shown->tracks[visible_[i].second].id);
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

void App::trackContextMenu(const Track& t) {
    if (!ImGui::BeginPopupContextItem()) return;
    // Right-clicking outside the selection moves to that track; inside it,
    // the existing selection is kept so the menu can act on all of it.
    if (!isSelected(t.id)) selectOnly(t.id);
    const int n = int(selection_.size());
    ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(30, 30, 30)));

    if (ImGui::MenuItem("Play")) playTrackId(t.id);
    if (ImGui::MenuItem(n > 1 ? "Get Info…" : "Get Info…", "Cmd+I"))
        openGetInfo();
    ImGui::Separator();

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

    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void App::updateArtwork() {
    const Library* shown = shownLibrary();
    const auto* index = shownIndex();
    if (!shown || !index || selectedTrackId_ == 0) {
        artHasImage_ = false;
        artTrackId_ = 0;
        return;
    }
    if (selectedTrackId_ == artTrackId_) return;  // already current
    artTrackId_ = selectedTrackId_;
    artHasImage_ = false;

    const auto it = index->find(selectedTrackId_);
    if (it == index->end()) return;
    const fs::path file = trackFilePath(shown->tracks[it->second]);
    const ArtImage img = loadEmbeddedArtwork(file);
    if (!img.ok()) return;

    if (artTexture_ == 0) glGenTextures(1, &artTexture_);
    glBindTexture(GL_TEXTURE_2D, artTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.rgba.data());
    artHasImage_ = true;
}

void App::drawArtworkPane(float sidebarHeight) {
    constexpr float kPane = 148.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float top = wp.y + sidebarHeight - kPane;
    dl->AddLine(ImVec2(wp.x, top), ImVec2(wp.x + kSidebarWidth, top),
                pal::SidebarBorder);

    const float box = 120.0f;
    const ImVec2 p(wp.x + (kSidebarWidth - box) * 0.5f, top + 14.0f);
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), pal::rgb(255, 255, 255),
                      2.0f);
    if (artHasImage_ && artTexture_) {
        dl->AddImage((ImTextureID)(intptr_t)artTexture_, p,
                     ImVec2(p.x + box, p.y + box));
    } else {
        // Empty "no artwork" well, like iTunes' placeholder note glyph.
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(p.x + box * 0.5f, p.y + box * 0.5f),
                        pal::rgb(170, 176, 184),
                        selectedTrackId_ ? "No artwork" : "");
    }
    dl->AddRect(p, ImVec2(p.x + box, p.y + box), pal::rgb(168, 174, 182), 2.0f);
}

bool App::animating() const {
    return player_ && player_->state() == PlaybackState::Playing;
}

void App::updatePlayback() {
    if (!player_ || playingTrackId_ == 0) return;
    // Auto-advance to the next visible track when one finishes.
    if (player_->reachedEnd()) playRelative(+1);
}

void App::playTrackId(std::uint32_t trackId) {
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

void App::playRelative(int delta) {
    if (!library_ || visible_.empty()) {
        if (player_) player_->stop();
        playingTrackId_ = 0;
        return;
    }
    // Find the currently playing row within the visible list, then step.
    int cur = -1;
    for (int i = 0; i < int(visible_.size()); ++i) {
        if (library_->tracks[visible_[i].second].id == playingTrackId_) {
            cur = i;
            break;
        }
    }
    const int next = cur + delta;
    if (next < 0 || next >= int(visible_.size())) {
        // Off either end: stop playback.
        player_->stop();
        playingTrackId_ = 0;
        return;
    }
    playTrackId(library_->tracks[visible_[next].second].id);
}

void App::updateLibrary() {
    const auto& dev = watcher_.device();
    if (!dev) {
        if (library_ || !loadedMount_.empty()) {
            if (player_) player_->stop();
            playingTrackId_ = 0;
            library_.reset();
            libraryError_.clear();
            loadedMount_.clear();
            trackIndexById_.clear();
            view_ = hostView_.tracks.empty() ? View::Device : View::Library;
            playlistIndex_ = -1;
            visibleDirty_ = true;
        }
        return;
    }
    if (dev->mountPoint == loadedMount_) return;
    loadedMount_ = dev->mountPoint;

    ParseResult res = parseItunesDb(loadedMount_ / "iPod_Control" / "iTunes" /
                                    "iTunesDB");
    library_ = std::move(res.library);
    libraryError_ = res.error;
    trackIndexById_.clear();
    fingerprints_.load(loadedMount_);
    nextTrackId_ = 100;
    if (library_) {
        trackIndexById_.reserve(library_->tracks.size());
        for (int i = 0; i < int(library_->tracks.size()); ++i) {
            trackIndexById_[library_->tracks[i].id] = i;
            nextTrackId_ = std::max(nextTrackId_, library_->tracks[i].id + 1);
        }
    }
    // Fold the firmware-written play counts/ratings into the in-memory
    // library so they show up and survive the next write. We re-merge from
    // scratch on every load; the on-disk Play Counts file is only cleared
    // once its contents have been written into the DB (see writeDatabase).
    playCountsUnmatched_ = false;
    if (library_) {
        const PlayCountsMerge merge = mergePlayCounts(
            loadedMount_ / "iPod_Control" / "iTunes" / "Play Counts",
            *library_);
        if (merge.applied > 0) {
            setStatus("Updated play counts for " +
                      std::to_string(merge.applied) +
                      (merge.applied == 1 ? " song" : " songs"));
        } else if (merge.mismatched) {
            // The iPod wrote counts against a different track list than the
            // one in the database now. Say so, and keep the file.
            playCountsUnmatched_ = true;
            setStatus("Play counts on this iPod (" +
                      std::to_string(merge.entries) +
                      " entries) don't match its " +
                      std::to_string(merge.trackCount) +
                      " songs — left untouched");
        }
    }

    pullPlayCountsToHost();

    view_ = library_ ? View::Music : View::Device;
    playlistIndex_ = -1;
    selectedTrackId_ = 0;
    selection_.clear();
    selectionAnchor_ = 0;
    artTrackId_ = 0;
    visibleDirty_ = true;
}

const Library* App::shownLibrary() const {
    if (view_ == View::Library) return &hostView_;
    return library_ ? &*library_ : nullptr;
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
}

void App::pullPlayCountsToHost() {
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
        host_.save();
        rebuildHostView();
        setStatus("Brought play counts back for " +
                  plural(updated, "song", "songs"));
    }
}

void App::rescanWatchFolders() {
    if (hostScanning_) return;
    if (scanThread_.joinable()) scanThread_.join();

    // The worker gets its own copy so the UI can keep reading the live
    // library while a cold scan (which reads and hashes every file) runs.
    hostScanning_ = true;
    scanFinished_.store(false);
    scanResult_ = std::make_unique<HostLibrary>(host_);
    scanThread_ = std::thread([this] {
        scanStats_ = scanResult_->rescan();
        scanResult_->save();
        scanFinished_.store(true);
    });
    setStatus("Scanning your music folders…");
}

void App::applyFinishedScan() {
    if (!scanFinished_.load()) return;
    scanFinished_.store(false);
    if (scanThread_.joinable()) scanThread_.join();
    hostScanning_ = false;
    if (!scanResult_) return;

    host_ = std::move(*scanResult_);
    scanResult_.reset();
    rebuildHostView();

    const ScanStats& s = scanStats_;
    if (s.added || s.updated || s.missing) {
        std::string msg = "Library: " + plural(s.added, "song", "songs") +
                          " added";
        if (s.updated) msg += ", " + std::to_string(s.updated) + " updated";
        if (s.missing) msg += ", " + std::to_string(s.missing) + " missing";
        setStatus(msg);
    } else {
        setStatus("Library up to date — " +
                  plural(int(host_.tracks().size()), "song", "songs"));
    }
}

void App::rebuildVisible() {
    visibleDirty_ = false;
    visible_.clear();
    const Library* lib = shownLibrary();
    const auto* index = shownIndex();
    if (!lib || !index) return;

    std::vector<int> base;
    if (view_ == View::Playlist && playlistIndex_ >= 0 &&
        playlistIndex_ < int(lib->playlists.size())) {
        for (const std::uint32_t id : lib->playlists[playlistIndex_].trackIds) {
            if (auto it = index->find(id); it != index->end())
                base.push_back(it->second);
        }
    } else {
        base.resize(lib->tracks.size());
        for (int i = 0; i < int(base.size()); ++i) base[i] = i;
    }

    const std::string needle = toLower(search_);
    visible_.reserve(base.size());
    for (const int ti : base) {
        const Track& t = lib->tracks[ti];
        if (needle.empty() || containsCi(t.title, needle) ||
            containsCi(t.artist, needle) || containsCi(t.album, needle)) {
            visible_.emplace_back(int(visible_.size()), ti);
        }
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

void App::drawTransport(float toolbarWidth) {
    (void)toolbarWidth;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = kToolbarHeight * 0.5f;
    const PlaybackState st = player_ ? player_->state() : PlaybackState::Stopped;
    const bool haveList = library_ && !visible_.empty();
    const ImU32 glyph = haveList ? pal::rgb(60, 66, 74) : pal::rgb(178, 184, 190);
    constexpr float kPrevW = 26.0f, kPlayW = 30.0f, kNextW = 26.0f;
    constexpr float kGap = 4.0f, kStartX = 16.0f;
    float x = kStartX;
    bool clicked = false;

    // One bezelled capsule behind all three, the way the iTunes transport was
    // drawn — separate flat glyphs read as a debug UI.
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const float w = kPrevW + kPlayW + kNextW + kGap * 2.0f;
        const ImVec2 a(wp.x + kStartX - 5.0f, wp.y + cy - 15.0f);
        const ImVec2 b(a.x + w + 10.0f, wp.y + cy + 15.0f);
        aqua::gradientRect(dl, a, b, pal::rgb(252, 252, 253),
                           pal::rgb(214, 217, 221), pal::rgb(150, 154, 160),
                           15.0f);
        // Hairlines separating the three, as on the real control.
        for (float sx : {kStartX + kPrevW + kGap * 0.5f,
                         kStartX + kPrevW + kGap + kPlayW + kGap * 0.5f})
            dl->AddLine(ImVec2(wp.x + sx, a.y + 5.0f),
                        ImVec2(wp.x + sx, b.y - 5.0f),
                        pal::rgb(190, 194, 199));
    }

    auto button = [&](const char* id, float w) -> ImVec2 {
        ImGui::SetCursorPos(ImVec2(x, cy - 13.0f));
        ImGui::InvisibleButton(id, ImVec2(w, 26.0f));
        clicked = ImGui::IsItemClicked();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        x += w + kGap;
        return ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    };

    // Previous.
    {
        const ImVec2 c = button("##prev", 26.0f);
        dl->AddTriangleFilled(ImVec2(c.x + 3, c.y - 5), ImVec2(c.x + 3, c.y + 5),
                              ImVec2(c.x - 3, c.y), glyph);
        dl->AddRectFilled(ImVec2(c.x - 6, c.y - 5), ImVec2(c.x - 4, c.y + 5),
                          glyph);
        if (clicked && haveList) playRelative(-1);
    }
    // Play / pause.
    {
        const ImVec2 c = button("##playpause", 30.0f);
        if (st == PlaybackState::Playing) {
            dl->AddRectFilled(ImVec2(c.x - 5, c.y - 6), ImVec2(c.x - 1, c.y + 6),
                              glyph);
            dl->AddRectFilled(ImVec2(c.x + 1, c.y - 6), ImVec2(c.x + 5, c.y + 6),
                              glyph);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 7),
                                  ImVec2(c.x - 5, c.y + 7),
                                  ImVec2(c.x + 6, c.y), glyph);
        }
        if (clicked) {
            if (st == PlaybackState::Playing)
                player_->pause();
            else if (st == PlaybackState::Paused)
                player_->play();
            else if (playingTrackId_)
                playTrackId(playingTrackId_);
            else if (selectedTrackId_)
                playTrackId(selectedTrackId_);
            else if (haveList)
                playTrackId(library_->tracks[visible_[0].second].id);
        }
    }
    // Next.
    {
        const ImVec2 c = button("##next", 26.0f);
        dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 5), ImVec2(c.x - 3, c.y + 5),
                              ImVec2(c.x + 3, c.y), glyph);
        dl->AddRectFilled(ImVec2(c.x + 4, c.y - 5), ImVec2(c.x + 6, c.y + 5),
                          glyph);
        if (clicked && haveList) playRelative(+1);
    }

    // Volume slider with a small speaker glyph.
    x += 10.0f;
    ImGui::SetCursorPos(ImVec2(x, cy - 8.0f));
    ImGui::InvisibleButton("##volume", ImVec2(88.0f, 16.0f));
    const ImVec2 vmn = ImGui::GetItemRectMin();
    const ImVec2 vmx = ImGui::GetItemRectMax();
    const float ty = (vmn.y + vmx.y) * 0.5f;
    const float tx0 = vmn.x + 16.0f, tx1 = vmx.x;
    // Speaker.
    dl->AddRectFilled(ImVec2(vmn.x, ty - 3), ImVec2(vmn.x + 4, ty + 3),
                      pal::rgb(120, 126, 134));
    dl->AddTriangleFilled(ImVec2(vmn.x + 4, ty - 5), ImVec2(vmn.x + 4, ty + 5),
                          ImVec2(vmn.x + 9, ty), pal::rgb(120, 126, 134));
    float vol = player_ ? player_->volume() : 0.0f;
    if (ImGui::IsItemActive()) {
        vol = std::clamp((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0), 0.0f,
                         1.0f);
        if (player_) player_->setVolume(vol);
    }
    dl->AddLine(ImVec2(tx0, ty), ImVec2(tx1, ty), pal::rgb(176, 182, 190), 2.0f);
    dl->AddLine(ImVec2(tx0, ty), ImVec2(tx0 + (tx1 - tx0) * vol, ty),
                pal::rgb(120, 140, 175), 2.0f);
    dl->AddCircleFilled(ImVec2(tx0 + (tx1 - tx0) * vol, ty), 5.0f,
                        pal::rgb(88, 94, 102));
}

void App::drawNowPlaying(ImVec2 a, ImVec2 b) {
    if (!library_) return;
    const auto it = trackIndexById_.find(playingTrackId_);
    if (it == trackIndexById_.end()) return;
    const Track& t = library_->tracks[it->second];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = (a.x + b.x) * 0.5f;

    addTextCentered(dl, fonts_.ui, fonts_.uiSize, ImVec2(cx, a.y + 11.0f),
                    pal::LcdText, t.title.c_str());
    std::string sub = t.artist;
    if (!t.album.empty()) sub += sub.empty() ? t.album : "  —  " + t.album;
    addTextCentered(dl, fonts_.label, fonts_.labelSize,
                    ImVec2(cx, a.y + 24.0f), pal::LcdTextDim, sub.c_str());

    const double pos = player_->position();
    const double dur = player_->duration();
    const float y = b.y - 9.0f;
    const float x0 = a.x + 46.0f, x1 = b.x - 46.0f;
    const float frac =
        dur > 0 ? float(std::clamp(pos / dur, 0.0, 1.0)) : 0.0f;
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), pal::rgb(185, 192, 200), 2.0f);
    dl->AddLine(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * frac, y),
                pal::rgb(84, 132, 214), 2.0f);
    dl->AddCircleFilled(ImVec2(x0 + (x1 - x0) * frac, y), 4.5f,
                        pal::rgb(70, 110, 180));

    dl->AddText(fonts_.label, fonts_.labelSize, ImVec2(a.x + 8.0f, y - 6.0f),
                pal::LcdTextDim,
                formatDuration(std::uint32_t(pos * 1000)).c_str());
    const std::string rem =
        "-" + formatDuration(std::uint32_t(std::max(0.0, dur - pos) * 1000));
    const float rw = fonts_.label->CalcTextSizeA(fonts_.labelSize, FLT_MAX, 0,
                                                 rem.c_str())
                         .x;
    dl->AddText(fonts_.label, fonts_.labelSize,
                ImVec2(b.x - 8.0f - rw, y - 6.0f), pal::LcdTextDim, rem.c_str());

    // Draggable scrub region over the progress line.
    ImGui::SetCursorScreenPos(ImVec2(x0, y - 8.0f));
    ImGui::InvisibleButton("##scrub", ImVec2(std::max(1.0f, x1 - x0), 16.0f));
    if (ImGui::IsItemActive() && dur > 0) {
        const float tt = std::clamp(
            (ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        player_->seek(tt * dur);
    }
}

void App::drawToolbar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const ImVec2 br(p.x + w, p.y + kToolbarHeight);
    dl->AddRectFilledMultiColor(p, br, pal::ToolbarTop, pal::ToolbarTop,
                                pal::ToolbarBottom, pal::ToolbarBottom);
    // The lit top edge and the shadowed base that give the titlebar its
    // depth; without them the gradient reads as a flat grey band.
    dl->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(br.x, p.y + 0.5f),
                IM_COL32(255, 255, 255, 200));
    dl->AddLine(ImVec2(p.x, br.y - 1.5f), ImVec2(br.x, br.y - 1.5f),
                IM_COL32(255, 255, 255, 90));
    dl->AddLine(ImVec2(p.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f),
                pal::ToolbarBorder);

    drawTransport(w);

    // Centered "LCD" status display, like the old iTunes readout.
    const float lcdW = std::min(kLcdWidth, w - 40.0f);
    if (lcdW > 80.0f) {
        const ImVec2 a(p.x + (w - lcdW) * 0.5f,
                       p.y + (kToolbarHeight - kLcdHeight) * 0.5f);
        const ImVec2 b(a.x + lcdW, a.y + kLcdHeight);
        // Sunken well: a soft vertical wash, a dark inner top edge for the
        // recess, and a highlight just below the frame.
        dl->AddRectFilled(a, b, pal::LcdBg, 5.0f);
        dl->AddRectFilledMultiColor(
            ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + kLcdHeight * 0.55f),
            IM_COL32(255, 255, 255, 150), IM_COL32(255, 255, 255, 150),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        dl->AddLine(ImVec2(a.x + 4, a.y + 1.0f), ImVec2(b.x - 4, a.y + 1.0f),
                    IM_COL32(120, 132, 145, 110));
        dl->AddRect(a, b, pal::LcdBorder, 5.0f);
        dl->AddLine(ImVec2(a.x + 4, b.y + 0.5f), ImVec2(b.x - 4, b.y + 0.5f),
                    IM_COL32(255, 255, 255, 170));

        const bool syncing = sync_.busy();
        if (!syncing && playingTrackId_ != 0) {
            drawNowPlaying(a, b);
        } else {
            const auto& dev = watcher_.device();
            std::string line1 = dev ? dev->volumeName : "PodBox";
            std::string line2;
            if (syncing) {
                line1 = "Copying “" + sync_.currentName() + "”";
                line2 = std::to_string(std::min(sync_.batchDone() + 1,
                                                sync_.batchTotal())) +
                        " of " + std::to_string(sync_.batchTotal());
            } else if (dev && library_) {
                line2 = std::to_string(library_->tracks.size()) + " songs — " +
                        formatBytes(dev->freeBytes) + " available";
            } else if (dev) {
                line2 = formatBytes(dev->freeBytes) + " available";
            } else {
                line2 = "No iPod connected";
            }
            const float cx = (a.x + b.x) * 0.5f;
            addTextCentered(dl, fonts_.ui, fonts_.uiSize,
                            ImVec2(cx, a.y + 12.0f), pal::LcdText,
                            line1.c_str());
            addTextCentered(dl, fonts_.label, fonts_.labelSize,
                            ImVec2(cx, a.y + 25.0f), pal::LcdTextDim,
                            line2.c_str());
            if (syncing && sync_.batchTotal() > 0) {
                // Thin progress bar along the bottom of the LCD.
                const float frac =
                    float(sync_.batchDone()) / float(sync_.batchTotal());
                const ImVec2 p0(a.x + 8, b.y - 7);
                const ImVec2 p1(b.x - 8, b.y - 3);
                dl->AddRectFilled(p0, p1, pal::rgb(196, 205, 212), 2.0f);
                dl->AddRectFilled(
                    p0,
                    ImVec2(p0.x + std::max(4.0f, (p1.x - p0.x) * frac), p1.y),
                    pal::CapacityFill, 2.0f);
            }
        }
    }

    // Search box, right-aligned like iTunes.
    if (library_ && w > kLcdWidth + 2.0f * (kSearchWidth + 60.0f)) {
        ImGui::SetCursorPos(
            ImVec2(w - kSearchWidth - 16.0f, (kToolbarHeight - 23.0f) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
        ImGui::SetNextItemWidth(kSearchWidth);
        if (ImGui::InputTextWithHint("##search", "Search", search_,
                                     sizeof(search_)))
            visibleDirty_ = true;
        ImGui::PopStyleVar(2);
    }
}

void App::drawSidebar(float height) {
    constexpr float kArtPane = 148.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::SidebarBg));
    ImGui::BeginChild("sidebar", ImVec2(kSidebarWidth, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + kSidebarWidth - 0.5f, wp.y),
                ImVec2(wp.x + kSidebarWidth - 0.5f, wp.y + height),
                pal::SidebarBorder);

    const bool haveLib = library_.has_value();
    const float listHeight =
        haveLib ? std::max(60.0f, height - kArtPane) : height;
    ImGui::BeginChild("sidebar_list", ImVec2(kSidebarWidth, listHeight));
    dl = ImGui::GetWindowDrawList();

    auto sectionHeader = [&](const char* text) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX(10);
        ImGui::PushFont(fonts_.labelBold);
        ImGui::TextColored(v4(pal::SidebarHeader), "%s", text);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
    };

    // Full-width row: transparent selectable with the text drawn manually so
    // we control the indent, like the iTunes source list.
    auto row = [&](const char* id, const std::string& text, float indent,
                   bool selected) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Header, v4(pal::Selection));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, v4(pal::Selection));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::Selection));
        const bool clicked = ImGui::Selectable(
            (std::string("##") + id).c_str(), selected,
            ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 18));
        ImGui::PopStyleColor(3);
        if (selected) {
            // Repaint the flat fill as the gradient the source list used.
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            dl->AddRectFilledMultiColor(mn, mx, pal::rgb(116, 162, 226),
                                        pal::rgb(116, 162, 226),
                                        pal::rgb(50, 108, 200),
                                        pal::rgb(50, 108, 200));
            dl->AddLine(mn, ImVec2(mx.x, mn.y), pal::rgb(150, 186, 236));
        }
        dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(pos.x + indent, pos.y + 2),
                    selected ? IM_COL32_WHITE : pal::rgb(30, 30, 30),
                    text.c_str());
        return clicked;
    };

    // The Mac's own collection, above the device — this is the library that
    // exists whether or not an iPod is plugged in.
    sectionHeader("LIBRARY");
    {
        const std::string label =
            "Music  (" + std::to_string(host_.tracks().size()) + ")";
        if (row("hostlib", label, 28.0f, view_ == View::Library)) {
            view_ = View::Library;
            playlistIndex_ = -1;
            selectedTrackId_ = 0;
            selection_.clear();
            selectionAnchor_ = 0;
            visibleDirty_ = true;
        }
    }
    ImGui::SetCursorPosX(10);
    ImGui::PushFont(fonts_.label);
    ImGui::BeginDisabled(hostScanning_);
    if (ImGui::SmallButton(hostScanning_ ? "Scanning…" : "Rescan"))
        rescanWatchFolders();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Folders…")) foldersOpen_ = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Apple Music…")) appleMusicOpen_ = true;
    ImGui::PopFont();

    sectionHeader("DEVICES");
    const auto& dev = watcher_.device();
    if (dev) {
        const ImVec2 iconPos = ImGui::GetCursorScreenPos();
        if (row("device", dev->volumeName, 28.0f, view_ == View::Device))
            view_ = View::Device;
        const ImVec2 afterRow = ImGui::GetCursorScreenPos();
        // Eject button overlapping the right side of the device row.
        const bool devSelected = view_ == View::Device;
        ImGui::SetCursorScreenPos(
            ImVec2(iconPos.x + kSidebarWidth - 32.0f, iconPos.y));
        if (ImGui::InvisibleButton("##eject", ImVec2(26.0f, 18.0f)))
            ejectRequested_ = true;
        const ImVec2 em = ImGui::GetItemRectMin();
        const float ejx = em.x + 8.0f, ejy = em.y + 4.0f;
        const ImU32 ejCol = ImGui::IsItemHovered()
                                ? pal::rgb(40, 90, 200)
                                : (devSelected ? IM_COL32_WHITE
                                               : pal::rgb(90, 98, 108));
        dl->AddTriangleFilled(ImVec2(ejx, ejy + 6), ImVec2(ejx + 10, ejy + 6),
                              ImVec2(ejx + 5, ejy), ejCol);
        dl->AddRectFilled(ImVec2(ejx, ejy + 8), ImVec2(ejx + 10, ejy + 11),
                          ejCol);
        ImGui::SetCursorScreenPos(afterRow);
        drawIpodIcon(dl, ImVec2(iconPos.x + 10, iconPos.y + 1));
        if (library_) {
            if (row("music", "Music", 28.0f, view_ == View::Music)) {
                view_ = View::Music;
                playlistIndex_ = -1;
                selectedTrackId_ = 0;
                selection_.clear();
                selectionAnchor_ = 0;
                visibleDirty_ = true;
            }
        }
    } else {
        ImGui::SetCursorPosX(10);
        ImGui::PushFont(fonts_.label);
        ImGui::TextColored(v4(pal::TextDim), "No iPod connected");
        ImGui::PopFont();
    }

    if (library_) {
        sectionHeader("PLAYLISTS");
        for (int i = 0; i < int(library_->playlists.size()); ++i) {
            const bool selected =
                view_ == View::Playlist && playlistIndex_ == i;
            if (renamePlaylistIndex_ == i) {
                // Inline rename field.
                ImGui::SetCursorPosX(18);
                ImGui::SetNextItemWidth(kSidebarWidth - 26.0f);
                if (renameJustOpened_) {
                    ImGui::SetKeyboardFocusHere();
                    renameJustOpened_ = false;
                }
                const bool done = ImGui::InputText(
                    "##rename", renameBuf_, sizeof(renameBuf_),
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    if (renameBuf_[0]) {
                        library_->playlists[i].name = renameBuf_;
                        writeDatabase();
                    }
                    renamePlaylistIndex_ = -1;
                }
                continue;
            }
            const std::string id = "pl" + std::to_string(i);
            if (row(id.c_str(), library_->playlists[i].name, 20.0f, selected)) {
                view_ = View::Playlist;
                playlistIndex_ = i;
                selectedTrackId_ = 0;
                selection_.clear();
                selectionAnchor_ = 0;
                visibleDirty_ = true;
            }
            if (ImGui::BeginPopupContextItem(
                    ("plctx" + std::to_string(i)).c_str())) {
                ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(30, 30, 30)));
                if (ImGui::MenuItem("Rename")) {
                    renamePlaylistIndex_ = i;
                    renameJustOpened_ = true;
                    std::snprintf(renameBuf_, sizeof(renameBuf_), "%s",
                                  library_->playlists[i].name.c_str());
                }
                if (ImGui::MenuItem("Delete")) deletePlaylistIndex_ = i;
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(18);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(70, 80, 92)));
        if (ImGui::SmallButton("+ New Playlist")) createPlaylist(0);
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    if (haveLib) drawArtworkPane(height);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawMainPanel(float height) {
    const float width = ImGui::GetWindowWidth() - kSidebarWidth;
    const auto& dev = watcher_.device();
    const bool trackView =
        view_ == View::Library
            ? true
            : (dev && library_ &&
               (view_ == View::Music || view_ == View::Playlist));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        trackView ? ImVec2(0, 0) : ImVec2(18, 14));
    ImGui::BeginChild("main", ImVec2(width, height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    if (view_ == View::Library && hostView_.tracks.empty()) {
        const char* msg =
            host_.watchFolders().empty()
                ? "No music folders yet — add one in the device pane"
                : "Nothing indexed yet — press Rescan in the sidebar";
        const ImVec2 avail = ImGui::GetWindowSize();
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(
            ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
    } else if (!dev && view_ != View::Library) {
        const char* msg = "Connect an iPod, or pick Music under Library";
        const ImVec2 avail = ImGui::GetWindowSize();
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(
            ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
    } else if (trackView) {
        drawTrackTable();
    } else {
        drawDeviceView(*dev);
    }

    ImGui::EndChild();
}

void App::drawDeviceView(const IpodInfo& dev) {
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted(dev.volumeName.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::BeginTable("device_info", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.empty() ? "—" : value.c_str());
        };
        row("Model", dev.modelName);
        row("Serial number", dev.serialNumber);
        row("Firmware", dev.firmwareVersion);
        row("Format", dev.filesystem);
        row("Location", dev.mountPoint.string());
        std::string dbInfo;
        if (library_) {
            dbInfo = std::to_string(library_->tracks.size()) + " songs";
            switch (library_->hashingScheme) {
                case 0: dbInfo += " — writable, no hash required"; break;
                case 1: dbInfo += " — writes require hash58"; break;
                case 2: dbInfo += " — writes require hash72"; break;
                default:
                    dbInfo += " — unknown hash scheme";
                    break;
            }
        } else {
            dbInfo = libraryError_;
        }
        row("Database", dbInfo);
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Capacity");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    drawCapacityBar(dev);

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Import format");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    int fmt = int(importFormat_);
    ImGui::RadioButton("Keep original format", &fmt,
                       int(ImportFormat::Original));
    ImGui::RadioButton("Convert everything to Apple Lossless (ALAC)", &fmt,
                       int(ImportFormat::Alac));
    ImGui::RadioButton(mp3EncoderAvailable()
                           ? "Convert everything to MP3 (320 kbps)"
                           : "Convert everything to AAC (256 kbps)",
                       &fmt, int(ImportFormat::Mp3));
    importFormat_ = ImportFormat(fmt);

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "Drag songs onto the window to add them. FLAC is always "
                       "converted so it plays on the iPod.");
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Sync");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::BeginDisabled(host_.tracks().empty() || sync_.busy());
    if (ImGui::Button("Sync Library to iPod…")) {
        syncOpen_ = true;
        syncDirty_ = true;
        syncConfirmRemove_ = false;
    }
    ImGui::EndDisabled();
    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "%zu songs in your Mac library. You'll see exactly what "
                       "would change before anything is written.",
                       host_.tracks().size());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Duplicates");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Checkbox("Skip songs already on this iPod", &skipDuplicates_);

    const bool canScan = library_ && !library_->tracks.empty();
    ImGui::BeginDisabled(!canScan);
    if (ImGui::Button("Find Duplicates…")) {
        duplicatesOpen_ = true;
        duplicatesDirty_ = true;
    }
    ImGui::EndDisabled();

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "Matches on artist, title, album and length. The better "
                       "copy is kept — lossless first, then play count.");
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Safety");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    if (ImGui::Button("Restore Database…")) restoreOpen_ = true;
    ImGui::PushFont(fonts_.label);
    if (appleMusicSyncing())
        ImGui::TextColored(v4(pal::rgb(150, 90, 20)),
                           "Apple Music is syncing this iPod right now — "
                           "PodBox will not write until it finishes.");
    else
        ImGui::TextColored(v4(pal::TextDim),
                           "PodBox saves the database before every change and "
                           "keeps the last five.");
    ImGui::PopFont();
}

void App::drawTrackTable() {
    const Library* shown = shownLibrary();
    if (!shown) return;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("tracks", 8, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(
        "#",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
            ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_NoReorder,
        34.0f, 0);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 48.0f, 2);
    ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 2.0f,
                            3);
    ImGui::TableSetupColumn("Album", ImGuiTableColumnFlags_WidthStretch, 2.0f,
                            4);
    ImGui::TableSetupColumn("Genre", ImGuiTableColumnFlags_WidthStretch, 1.2f,
                            5);
    ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthFixed, 44.0f,
                            6);
    ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed, 72.0f,
                            7);
    // ImGui fills the header row flat; iTunes' were a soft gradient with a
    // hard rule beneath, so paint over it before the labels are drawn.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetFrameHeight();
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h),
                                    pal::rgb(250, 250, 251),
                                    pal::rgb(250, 250, 251),
                                    pal::rgb(226, 228, 232),
                                    pal::rgb(226, 228, 232));
        dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f),
                    pal::rgb(168, 173, 180));
    }
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty) {
            if (specs->SpecsCount > 0) {
                sortCol_ = int(specs->Specs[0].ColumnUserID);
                sortAsc_ = specs->Specs[0].SortDirection !=
                           ImGuiSortDirection_Descending;
            } else {
                sortCol_ = 0;
                sortAsc_ = true;
            }
            specs->SpecsDirty = false;
            visibleDirty_ = true;
        }
    }
    if (visibleDirty_) rebuildVisible();

    // Reordering is only meaningful for a playlist shown in manual order
    // (no search filter, no column sort).
    const bool reorderable = view_ == View::Playlist && playlistIndex_ >= 0 &&
                             sortCol_ == 0 && search_[0] == '\0';
    int reorderFrom = -1, reorderTo = -1;

    // iTunes highlights rows on selection only, not hover.
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::Selection));

    ImGuiListClipper clipper;
    clipper.Begin(int(visible_.size()));
    std::uint32_t newRatingId = 0;
    int newRating = -1;

    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto [origPos, ti] = visible_[r];
            const Track& t = shown->tracks[ti];
            const bool selected = isSelected(t.id);

            ImGui::TableNextRow();
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));

            ImGui::TableNextColumn();
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%d##%u", r + 1, t.id);
            if (ImGui::Selectable(lbl, selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                const ImGuiIO& io = ImGui::GetIO();
                selectRow(r, t.id, io.KeyShift, io.KeySuper || io.KeyCtrl);
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                playTrackId(t.id);

            // Drag to reorder, only within a playlist in manual order.
            if (reorderable) {
                if (ImGui::BeginDragDropSource(
                        ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    ImGui::SetDragDropPayload("PODBOX_ROW", &origPos,
                                              sizeof(origPos));
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("PODBOX_ROW")) {
                        const int from = *static_cast<const int*>(pl->Data);
                        reorderFrom = from;
                        reorderTo = origPos;
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            trackContextMenu(t);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatDuration(t.lengthMs).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.artist.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.album.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.genre.c_str());
            ImGui::TableNextColumn();
            if (t.playCount) ImGui::Text("%u", t.playCount);

            ImGui::TableNextColumn();
            {
                const ImVec2 sp = ImGui::GetCursorScreenPos();
                // A real item, not a hand-rolled rectangle test: this is what
                // makes the stars respect an open sheet, window ordering and
                // hover ownership. Testing io.MousePos directly would let a
                // click on a dialog above also set a rating down here.
                ImGui::PushID(int(t.id));
                ImGui::InvisibleButton("##rate", ImVec2(70, 16));
                const bool hot = ImGui::IsItemHovered();
                const int picked =
                    drawStars(ImGui::GetWindowDrawList(), ImVec2(sp.x, sp.y + 3),
                              t.rating, hot, ImGui::GetIO().MousePos,
                              ImGui::IsItemClicked());
                ImGui::PopID();
                if (picked >= 0) newRatingId = t.id, newRating = picked;
            }

            if (selected) ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndTable();

    if (newRatingId && newRating >= 0) setTrackRating(newRatingId, newRating);

    // Apply a completed drag-reorder to the underlying playlist order.
    if (reorderable && reorderFrom >= 0 && reorderTo >= 0 &&
        reorderFrom != reorderTo) {
        auto& ids = library_->playlists[playlistIndex_].trackIds;
        if (reorderFrom < int(ids.size()) && reorderTo < int(ids.size())) {
            const std::uint32_t moved = ids[reorderFrom];
            ids.erase(ids.begin() + reorderFrom);
            ids.insert(ids.begin() + reorderTo, moved);
            visibleDirty_ = true;
            writeDatabase();
        }
    }

    if (!selection_.empty() && !ImGui::GetIO().WantTextInput &&
        (ImGui::GetIO().KeySuper || ImGui::GetIO().KeyCtrl) &&
        ImGui::IsKeyPressed(ImGuiKey_I))
        openGetInfo();

    if (selectedTrackId_ && !deleteRequestId_ &&
        !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        // In a playlist, plain Delete removes from the playlist; the
        // "Remove from iPod" path stays in the context menu to avoid
        // accidental file deletion.
        if (view_ == View::Playlist && playlistIndex_ >= 0) {
            auto& ids = library_->playlists[playlistIndex_].trackIds;
            int removed = 0;
            for (std::uint32_t id : selection_) {
                if (auto it = std::find(ids.begin(), ids.end(), id);
                    it != ids.end()) {
                    ids.erase(it);
                    ++removed;
                }
            }
            if (removed > 0) {
                visibleDirty_ = true;
                if (writeDatabase())
                    setStatus("Removed " + plural(removed, "song", "songs") +
                              " from the playlist");
            }
        } else if (viewingHost()) {
            // Deleting from the Mac library would mean deleting the user's
            // own files out of a folder PodBox only indexes. Not this key's
            // job, and not something to do by accident.
            setStatus("Delete removes songs from the iPod, not from your Mac");
        } else {
            deleteRequestId_ = selectedTrackId_;
        }
    }
}

void App::drawCapacityBar(const IpodInfo& dev) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 14.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const std::uint64_t used = dev.capacityBytes - dev.freeBytes;
    const float frac =
        dev.capacityBytes ? float(double(used) / double(dev.capacityBytes))
                          : 0.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal::CapacityBg, 3.0f);
    const float fillW = std::clamp(w * frac, 6.0f, w);
    dl->AddRectFilled(p, ImVec2(p.x + fillW, p.y + h), pal::CapacityFill,
                      3.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pal::CapacityBorder, 3.0f);
    ImGui::Dummy(ImVec2(w, h + 4));

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim), "%s used", formatBytes(used).c_str());
    const std::string freeTxt = formatBytes(dev.freeBytes) + " free";
    const float tw = ImGui::CalcTextSize(freeTxt.c_str()).x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - tw);
    ImGui::TextColored(v4(pal::TextDim), "%s", freeTxt.c_str());
    ImGui::PopFont();
}

void App::drawStatusBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const float y0 = wp.y + ImGui::GetWindowHeight() - kStatusBarHeight;
    dl->AddRectFilledMultiColor(
        ImVec2(wp.x, y0), ImVec2(wp.x + w, y0 + kStatusBarHeight),
        pal::StatusTop, pal::StatusTop, pal::StatusBottom, pal::StatusBottom);
    dl->AddLine(ImVec2(wp.x, y0 + 0.5f), ImVec2(wp.x + w, y0 + 0.5f),
                pal::StatusBorder);

    const auto& dev = watcher_.device();
    std::string text;
    if (!statusMsg_.empty() && ImGui::GetTime() < statusMsgUntil_) {
        text = statusMsg_;
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(wp.x + w * 0.5f, y0 + kStatusBarHeight * 0.5f),
                        pal::StatusText, text.c_str());
        return;
    }
    const Library* shown = shownLibrary();
    const bool onTracks = shown && (view_ == View::Library ||
                                    ((view_ == View::Music ||
                                      view_ == View::Playlist) && dev));
    if (onTracks) {
        std::uint64_t totalMs = 0, totalBytes = 0;
        for (const auto& [pos, ti] : visible_) {
            totalMs += shown->tracks[ti].lengthMs;
            totalBytes += shown->tracks[ti].sizeBytes;
        }
        text = std::to_string(visible_.size()) + " songs, " +
               formatTotalDuration(totalMs) + ", " + formatBytes(totalBytes);
        // A multi-selection is worth stating: it is what the context menu and
        // Delete will act on.
        if (selection_.size() > 1)
            text += "   ·   " + std::to_string(selection_.size()) + " selected";
    } else if (dev) {
        text = formatBytes(dev->capacityBytes) + " capacity, " +
               formatBytes(dev->freeBytes) + " available";
    } else {
        text = "No iPod connected";
    }
    addTextCentered(dl, fonts_.label, fonts_.labelSize,
                    ImVec2(wp.x + w * 0.5f, y0 + kStatusBarHeight * 0.5f),
                    pal::StatusText, text.c_str());
}

}  // namespace podbox
