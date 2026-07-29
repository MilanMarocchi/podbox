#pragma once

#include "audio/player.h"
#include "device/device_watcher.h"
#include "itdb/itunesdb.h"
#include "sync/sync_engine.h"
#include "ui/theme.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace podbox {

class App {
public:
    explicit App(const Fonts& fonts)
        : fonts_(fonts), player_(AudioPlayer::create()) {}

    // Draws one frame of the UI. Call between ImGui NewFrame/Render.
    void frame();

    // Files/folders dropped onto the window (from the GLFW drop callback).
    void onFilesDropped(const std::vector<std::string>& paths);

    // True when the UI should redraw continuously (e.g. playback in progress),
    // so the main loop can pick a shorter event-wait timeout.
    bool animating() const;

private:
    enum class View { Device, Music, Playlist };

    void updateLibrary();
    void rebuildVisible();
    void applyCompletedAdds();
    bool writeDatabase();
    void performDelete(std::uint32_t trackId);
    void drawDeleteModal();
    void drawDeletePlaylistModal();
    void setStatus(const std::string& msg);
    void createPlaylist(std::uint32_t withTrackId);
    void addToPlaylist(int playlistIndex, std::uint32_t trackId);
    void trackContextMenu(const Track& t);
    void updateArtwork();
    void drawArtworkPane(float sidebarHeight);
    void updatePlayback();
    void playTrackId(std::uint32_t trackId);
    void playRelative(int delta);
    void drawTransport(float toolbarWidth);
    void drawNowPlaying(ImVec2 lcdMin, ImVec2 lcdMax);
    void drawToolbar();
    void drawSidebar(float height);
    void drawMainPanel(float height);
    void drawDeviceView(const IpodInfo& dev);
    void drawTrackTable();
    void drawCapacityBar(const IpodInfo& dev);
    void drawStatusBar();

    Fonts fonts_;
    DeviceWatcher watcher_;

    std::optional<Library> library_;
    std::string libraryError_;
    std::filesystem::path loadedMount_;
    std::unordered_map<std::uint32_t, int> trackIndexById_;

    View view_ = View::Device;
    int playlistIndex_ = -1;
    char search_[128] = {};
    int sortCol_ = 0;
    bool sortAsc_ = true;
    bool visibleDirty_ = true;
    // (position in unsorted list, index into library tracks)
    std::vector<std::pair<int, int>> visible_;
    std::uint32_t selectedTrackId_ = 0;

    SyncEngine sync_;
    ImportFormat importFormat_ = ImportFormat::Original;
    bool pendingDbWrite_ = false;
    int lastBatchAdded_ = 0;
    std::uint32_t nextTrackId_ = 100;
    std::uint32_t deleteRequestId_ = 0;
    std::string statusMsg_;
    double statusMsgUntil_ = 0.0;

    // Playlist editing.
    int renamePlaylistIndex_ = -1;   // -1 = not renaming
    int deletePlaylistIndex_ = -2;   // -2 = no request pending
    char renameBuf_[128] = {};
    bool renameJustOpened_ = false;

    // Artwork preview for the selected track. The GL texture is created
    // lazily and refreshed only when the selection changes.
    unsigned int artTexture_ = 0;
    std::uint32_t artTrackId_ = 0;
    bool artHasImage_ = false;
    bool ejectRequested_ = false;

    // Playback.
    std::unique_ptr<AudioPlayer> player_;
    std::uint32_t playingTrackId_ = 0;
    bool scrubbing_ = false;
};

}  // namespace podbox
