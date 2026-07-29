#include "app/app.h"

#include "device/ipod_device.h"
#include "itdb/playcounts.h"
#include "library/artwork.h"
#include "library/metadata.h"
#include "ui/theme.h"

#include <imgui.h>

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

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

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
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

void App::frame() {
    watcher_.update(ImGui::GetTime());
    updateLibrary();
    applyCompletedAdds();
    updateArtwork();

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

    if (ejectRequested_) {
        ejectRequested_ = false;
        std::string err;
        const fs::path mount = loadedMount_;
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
                    isSupportedAudioFile(e.path()))
                    files.push_back(e.path());
            }
        } else if (isSupportedAudioFile(path)) {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        setStatus("No supported audio files (MP3/AAC/ALAC/WAV/AIFF)");
        return;
    }
    sync_.queueAdds(files, loadedMount_);
}

void App::applyCompletedAdds() {
    if (!library_) return;
    static std::mt19937_64 rng{std::random_device{}()};
    for (auto& done : sync_.takeCompleted()) {
        if (!done.error.empty()) {
            setStatus(done.error);
            continue;
        }
        done.track.id = nextTrackId_++;
        done.track.dbid = rng();
        trackIndexById_[done.track.id] = int(library_->tracks.size());
        library_->tracks.push_back(std::move(done.track));
        ++lastBatchAdded_;
        pendingDbWrite_ = true;
        visibleDirty_ = true;
    }
    if (pendingDbWrite_ && !sync_.busy()) {
        pendingDbWrite_ = false;
        if (writeDatabase())
            setStatus("Added " + std::to_string(lastBatchAdded_) +
                      (lastBatchAdded_ == 1 ? " song" : " songs"));
        lastBatchAdded_ = 0;
    }
}

bool App::writeDatabase() {
    if (!library_ || loadedMount_.empty()) return false;
    const fs::path dbPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesDB";
    std::error_code ec;
    // Keep the original iTunes-written DB around, once.
    const fs::path backup = dbPath.string() + ".podbox-backup";
    if (fs::exists(dbPath, ec) && !fs::exists(backup, ec))
        fs::copy_file(dbPath, backup, ec);

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
    fs::remove(dbPath.parent_path() / "Play Counts", ec);
    return true;
}

void App::performDelete(std::uint32_t trackId) {
    if (!library_) return;
    const auto it = trackIndexById_.find(trackId);
    if (it == trackIndexById_.end()) return;
    const int index = it->second;

    std::error_code ec;
    const std::string title = library_->tracks[index].title;
    fs::remove(locationToPath(loadedMount_, library_->tracks[index].location),
               ec);
    library_->tracks.erase(library_->tracks.begin() + index);
    for (auto& pl : library_->playlists)
        std::erase(pl.trackIds, trackId);
    trackIndexById_.clear();
    for (int i = 0; i < int(library_->tracks.size()); ++i)
        trackIndexById_[library_->tracks[i].id] = i;

    if (selectedTrackId_ == trackId) selectedTrackId_ = 0;
    visibleDirty_ = true;
    if (writeDatabase()) setStatus("Removed “" + title + "”");
}

void App::drawDeleteModal() {
    if (deleteRequestId_ && !ImGui::IsPopupOpen("Remove Song"))
        ImGui::OpenPopup("Remove Song");
    if (!ImGui::BeginPopupModal("Remove Song", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const auto it = trackIndexById_.find(deleteRequestId_);
    if (!library_ || it == trackIndexById_.end()) {
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Remove “%s” from the iPod?",
                library_->tracks[it->second].title.c_str());
    ImGui::TextDisabled("The audio file will be deleted from the device.");
    ImGui::Spacing();
    if (ImGui::Button("Remove", ImVec2(90, 0))) {
        const std::uint32_t id = deleteRequestId_;
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
        performDelete(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        deleteRequestId_ = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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

void App::trackContextMenu(const Track& t) {
    if (!ImGui::BeginPopupContextItem()) return;
    selectedTrackId_ = t.id;
    ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(30, 30, 30)));

    if (ImGui::BeginMenu("Add to Playlist")) {
        if (ImGui::MenuItem("New Playlist…")) createPlaylist(t.id);
        if (library_ && !library_->playlists.empty()) ImGui::Separator();
        for (int i = 0; library_ && i < int(library_->playlists.size()); ++i) {
            if (ImGui::MenuItem(library_->playlists[i].name.c_str()))
                addToPlaylist(i, t.id);
        }
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
    if (ImGui::MenuItem("Remove from iPod")) deleteRequestId_ = t.id;

    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void App::updateArtwork() {
    if (!library_ || selectedTrackId_ == 0) {
        artHasImage_ = false;
        artTrackId_ = 0;
        return;
    }
    if (selectedTrackId_ == artTrackId_) return;  // already current
    artTrackId_ = selectedTrackId_;
    artHasImage_ = false;

    const auto it = trackIndexById_.find(selectedTrackId_);
    if (it == trackIndexById_.end()) return;
    const fs::path file =
        locationToPath(loadedMount_, library_->tracks[it->second].location);
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

void App::updateLibrary() {
    const auto& dev = watcher_.device();
    if (!dev) {
        if (library_ || !loadedMount_.empty()) {
            library_.reset();
            libraryError_.clear();
            loadedMount_.clear();
            trackIndexById_.clear();
            view_ = View::Device;
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
    if (library_) {
        const PlayCountsMerge merge = mergePlayCounts(
            loadedMount_ / "iPod_Control" / "iTunes" / "Play Counts",
            *library_);
        if (merge.applied > 0)
            setStatus("Updated play counts for " +
                      std::to_string(merge.applied) +
                      (merge.applied == 1 ? " song" : " songs"));
    }

    view_ = library_ ? View::Music : View::Device;
    playlistIndex_ = -1;
    selectedTrackId_ = 0;
    artTrackId_ = 0;
    visibleDirty_ = true;
}

void App::rebuildVisible() {
    visibleDirty_ = false;
    visible_.clear();
    if (!library_) return;

    std::vector<int> base;
    if (view_ == View::Playlist && playlistIndex_ >= 0 &&
        playlistIndex_ < int(library_->playlists.size())) {
        for (const std::uint32_t id :
             library_->playlists[playlistIndex_].trackIds) {
            if (auto it = trackIndexById_.find(id); it != trackIndexById_.end())
                base.push_back(it->second);
        }
    } else {
        base.resize(library_->tracks.size());
        for (int i = 0; i < int(base.size()); ++i) base[i] = i;
    }

    const std::string needle = toLower(search_);
    visible_.reserve(base.size());
    for (const int ti : base) {
        const Track& t = library_->tracks[ti];
        if (needle.empty() || containsCi(t.title, needle) ||
            containsCi(t.artist, needle) || containsCi(t.album, needle)) {
            visible_.emplace_back(int(visible_.size()), ti);
        }
    }

    const auto& tracks = library_->tracks;
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
            default:
                return false;  // column 0: keep list order
        }
        return c < 0;
    };
    if (sortCol_ != 0)
        std::stable_sort(visible_.begin(), visible_.end(), byField);
    if (!sortAsc_) std::reverse(visible_.begin(), visible_.end());
}

void App::drawToolbar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const ImVec2 br(p.x + w, p.y + kToolbarHeight);
    dl->AddRectFilledMultiColor(p, br, pal::ToolbarTop, pal::ToolbarTop,
                                pal::ToolbarBottom, pal::ToolbarBottom);
    dl->AddLine(ImVec2(p.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f),
                pal::ToolbarBorder);

    // Centered "LCD" status display, like the old iTunes readout.
    const float lcdW = std::min(kLcdWidth, w - 40.0f);
    if (lcdW > 80.0f) {
        const ImVec2 a(p.x + (w - lcdW) * 0.5f,
                       p.y + (kToolbarHeight - kLcdHeight) * 0.5f);
        const ImVec2 b(a.x + lcdW, a.y + kLcdHeight);
        dl->AddRectFilled(a, b, pal::LcdBg, 5.0f);
        dl->AddRect(a, b, pal::LcdBorder, 5.0f);

        const auto& dev = watcher_.device();
        std::string line1 = dev ? dev->volumeName : "PodBox";
        std::string line2;
        const bool syncing = sync_.busy();
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
        addTextCentered(dl, fonts_.ui, fonts_.uiSize, ImVec2(cx, a.y + 12.0f),
                        pal::LcdText, line1.c_str());
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(cx, a.y + 25.0f), pal::LcdTextDim,
                        line2.c_str());
        if (syncing && sync_.batchTotal() > 0) {
            // Thin progress bar along the bottom of the LCD, iTunes-style.
            const float frac =
                float(sync_.batchDone()) / float(sync_.batchTotal());
            const ImVec2 p0(a.x + 8, b.y - 7);
            const ImVec2 p1(b.x - 8, b.y - 3);
            dl->AddRectFilled(p0, p1, pal::rgb(196, 205, 212), 2.0f);
            dl->AddRectFilled(
                p0, ImVec2(p0.x + std::max(4.0f, (p1.x - p0.x) * frac), p1.y),
                pal::CapacityFill, 2.0f);
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
            (std::string("##") + id).c_str(), selected, 0, ImVec2(0, 18));
        ImGui::PopStyleColor(3);
        dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(pos.x + indent, pos.y + 2),
                    selected ? IM_COL32_WHITE : pal::rgb(30, 30, 30),
                    text.c_str());
        return clicked;
    };

    sectionHeader("DEVICES");
    const auto& dev = watcher_.device();
    if (dev) {
        const ImVec2 iconPos = ImGui::GetCursorScreenPos();
        if (row("device", dev->volumeName, 28.0f, view_ == View::Device))
            view_ = View::Device;
        // Eject button, right-aligned on the device row.
        const bool devSelected = view_ == View::Device;
        const ImVec2 ejp(iconPos.x + kSidebarWidth - 26.0f, iconPos.y + 3.0f);
        const ImU32 ejCol = devSelected ? IM_COL32_WHITE : pal::rgb(90, 98, 108);
        dl->AddTriangleFilled(ImVec2(ejp.x, ejp.y + 6), ImVec2(ejp.x + 10, ejp.y + 6),
                              ImVec2(ejp.x + 5, ejp.y), ejCol);
        dl->AddRectFilled(ImVec2(ejp.x, ejp.y + 8), ImVec2(ejp.x + 10, ejp.y + 11),
                          ejCol);
        if (ImGui::IsMouseHoveringRect(ejp, ImVec2(ejp.x + 12, ejp.y + 12)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ejectRequested_ = true;
        drawIpodIcon(dl, ImVec2(iconPos.x + 10, iconPos.y + 1));
        if (library_) {
            if (row("music", "Music", 28.0f, view_ == View::Music)) {
                view_ = View::Music;
                playlistIndex_ = -1;
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
        dev && library_ && (view_ == View::Music || view_ == View::Playlist);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        trackView ? ImVec2(0, 0) : ImVec2(18, 14));
    ImGui::BeginChild("main", ImVec2(width, height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    if (!dev) {
        const char* msg = "Connect an iPod to get started";
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
}

void App::drawTrackTable() {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("tracks", 7, flags)) return;

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
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto [origPos, ti] = visible_[r];
            const Track& t = library_->tracks[ti];
            const bool selected = t.id == selectedTrackId_;

            ImGui::TableNextRow();
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));

            ImGui::TableNextColumn();
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%d##%u", r + 1, t.id);
            if (ImGui::Selectable(lbl, selected,
                                  ImGuiSelectableFlags_SpanAllColumns))
                selectedTrackId_ = t.id;

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

            if (selected) ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndTable();

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

    if (selectedTrackId_ && !deleteRequestId_ &&
        !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        // In a playlist, plain Delete removes from the playlist; the
        // "Remove from iPod" path stays in the context menu to avoid
        // accidental file deletion.
        if (view_ == View::Playlist && playlistIndex_ >= 0) {
            auto& ids = library_->playlists[playlistIndex_].trackIds;
            if (auto it = std::find(ids.begin(), ids.end(), selectedTrackId_);
                it != ids.end()) {
                ids.erase(it);
                visibleDirty_ = true;
                if (writeDatabase()) setStatus("Removed from playlist");
            }
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
    if (dev && library_ &&
        (view_ == View::Music || view_ == View::Playlist)) {
        std::uint64_t totalMs = 0, totalBytes = 0;
        for (const auto& [pos, ti] : visible_) {
            totalMs += library_->tracks[ti].lengthMs;
            totalBytes += library_->tracks[ti].sizeBytes;
        }
        text = std::to_string(visible_.size()) + " songs, " +
               formatTotalDuration(totalMs) + ", " + formatBytes(totalBytes);
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
