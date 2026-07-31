#pragma once

#include "audio/player.h"
#include "device/device_watcher.h"
#include "itdb/itunesdb.h"
#include "library/fingerprint_store.h"
#include "library/dedupe.h"
#include "library/applemusic.h"
#include "library/host_library.h"
#include "sync/sync_engine.h"
#include "sync/sync_plan.h"
#include "sync/verify_job.h"
#include "ui/theme.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
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

    // Joins the scan worker so a rescan in flight cannot outlive the app.
    ~App();

    // Draws one frame of the UI. Call between ImGui NewFrame/Render.
    void frame();

    // Files/folders dropped onto the window (from the GLFW drop callback).
    void onFilesDropped(const std::vector<std::string>& paths);

    // True when the UI should redraw continuously (e.g. playback in progress),
    // so the main loop can pick a shorter event-wait timeout.
    bool animating() const;

private:
    // Which source the main panel is showing. Library is the Mac-side
    // collection; the rest are the connected iPod's. Music, Podcasts and
    // Audiobooks are the same track list partitioned by media type, the way
    // iTunes split its source list.
    enum class View { Device, Music, Playlist, Library, Podcasts, Audiobooks };

    void updateLibrary();
    // Point the main panel at a different source. Selection is per-source, so
    // switching always clears it — four copies of this used to drift apart.
    // `playlistIndex` is only meaningful for View::Playlist.
    void switchSource(View view, int playlistIndex = -1);
    // The media type a view shows, or 0 for views that do not partition by it.
    std::uint32_t viewMediaType() const;
    // True when the device library holds anything of that media type, which is
    // what decides whether the sidebar offers the row at all.
    bool deviceHasMedia(std::uint32_t mediaType) const;
    // The tracks currently on screen, and their id index — the Mac library's
    // or the iPod's. Everything that merely displays tracks goes through
    // these so one table serves both. Null when nothing is loaded.
    const Library* shownLibrary() const;
    const std::unordered_map<std::uint32_t, int>* shownIndex() const;
    bool viewingHost() const { return view_ == View::Library; }
    // Where a track's audio actually lives, whichever source it came from.
    std::filesystem::path trackFilePath(const Track& t) const;
    void rebuildHostView();
    void rescanWatchFolders();
    void pullPlayCountsToHost();
    void applyFinishedScan();
    void rebuildVisible();
    // The column browser only applies where faceting a whole collection makes
    // sense. Playlists are small and manually ordered, and a hidden filter is
    // exactly what would corrupt a drag-reorder.
    bool browserApplies() const;
    void drawColumnBrowser(float width);
    void applyCompletedAdds();
    bool writeDatabase();
    // True when this iPod's database can be rewritten at all. iPod classic and
    // nano 3G onwards carry a checksum over the database that PodBox cannot
    // produce yet; writing without it leaves the device unable to read its own
    // library. Every mutation funnels through writeDatabase(), which refuses
    // when this is false — the UI calls it too, so the affected controls are
    // disabled rather than failing after the user commits to something.
    bool writesSupported() const;
    // True when Apple Music appears to be mid-sync on this device. Two
    // writers on one iTunesDB is the one thing that can genuinely corrupt it.
    bool appleMusicSyncing() const;
    void rotateBackups(const std::filesystem::path& dbPath);
    std::vector<std::filesystem::path> availableBackups() const;
    void drawRestoreModal();
    void drawFoldersModal();
    void drawGetInfoModal();
    void openGetInfo();
    void drawSyncModal();
    void refreshSyncPlan();
    void startSync();
    void drawAppleMusicModal();
    void startAppleMusicRead();
    void startAppleMusicCopy();
    void applyFinishedAppleMusic();
    // Sets a track's rating on whichever library is on screen, persisting it.
    void setTrackRating(std::uint32_t trackId, int rating);
    void performDelete(std::uint32_t trackId);
    // Removes many tracks with a single index rebuild and a single DB write.
    // `remap` optionally redirects playlist references from a removed track to
    // one that is being kept, so deduplicating never shortens a playlist.
    // Returns how many tracks were actually removed.
    int performDeleteMany(
        const std::vector<std::uint32_t>& ids,
        const std::unordered_map<std::uint32_t, std::uint32_t>* remap = nullptr);
    void drawDeleteModal();
    void drawDuplicatesModal();
    void refreshDuplicates();
    void startVerifyPass();
    void drawDeletePlaylistModal();
    void setStatus(const std::string& msg);
    void createPlaylist(std::uint32_t withTrackId);
    void addToPlaylist(int playlistIndex, std::uint32_t trackId);
    void trackContextMenu(const Track& t);
    bool isSelected(std::uint32_t trackId) const;
    // Applies a click's modifiers to the selection. `row` is the index into
    // visible_, so shift-ranges follow what is actually on screen.
    void selectRow(int row, std::uint32_t trackId, bool shift, bool cmd);
    void selectOnly(std::uint32_t trackId);
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
    // The Mac-side library, plus a Library-shaped view of it so the track
    // table, search, sorting and dedupe all work on it unchanged.
    HostLibrary host_;
    Library hostView_;
    std::unordered_map<std::uint32_t, int> hostIndexById_;
    // Set when the device's Play Counts file could not be matched to the
    // track list. While true the file is preserved rather than deleted: it
    // still holds real listening history that a later load may be able to
    // merge.
    bool playCountsUnmatched_ = false;
    bool hostLoaded_ = false;

    // A rescan runs on a worker over its own copy of the library and is
    // swapped in when it finishes, so a cold scan of a large folder never
    // blocks the frame loop.
    struct HostScan {
        std::thread thread;
        std::atomic<bool> finished{false};
        std::unique_ptr<HostLibrary> result;
        ScanStats stats;
        bool running = false;
    };
    HostScan scan_;

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
    // The primary selection (artwork, keyboard target). selection_ holds the
    // whole set, which is usually just this one.
    std::uint32_t selectedTrackId_ = 0;
    std::vector<std::uint32_t> selection_;
    std::uint32_t selectionAnchor_ = 0;  // for shift-click ranges

    // The Genres | Artists | Albums browser above the track list.
    //
    // Selections are strings rather than indices because the facet lists are
    // rebuilt on every keystroke, which would leave an index pointing at
    // something else. nullopt means "All"; an engaged empty string is the real
    // blank-genre facet, shown as "Unknown" — a lot of ripped music has no
    // genre, and conflating the two would make that row unselectable.
    struct ColumnBrowser {
        std::optional<std::string> genre, artist, album;
        std::vector<std::string> genres, artists, albums;
        bool visible = true;
        float height = 170.0f;
        bool engaged() const { return genre || artist || album; }
        void clearSelection() { genre.reset(); artist.reset(); album.reset(); }
    };
    ColumnBrowser browser_;

    SyncEngine sync_;
    ImportFormat importFormat_ = ImportFormat::Original;
    // Source fingerprints for the tracks on this device, so a re-drop of a
    // file is recognised even when importing transcoded it.
    FingerprintStore fingerprints_;
    bool skipDuplicates_ = true;
    bool pendingDbWrite_ = false;
    int lastBatchAdded_ = 0;
    int lastBatchSkipped_ = 0;
    std::uint32_t nextTrackId_ = 100;
    std::uint32_t deleteRequestId_ = 0;
    std::string statusMsg_;
    double statusMsgUntil_ = 0.0;

    // Set after each of our own writes, so a fresher mtime on the device
    // means somebody else touched it.
    std::filesystem::file_time_type ownWriteTime_{};
    bool restoreOpen_ = false;
    bool foldersOpen_ = false;

    // Get Info. Editing many tracks at once leaves any field the user does
    // not touch alone, which is why a blank field means "leave alone" rather
    // than "clear".
    struct GetInfoEdit {
        bool open = false;
        char title[256] = {};
        char artist[256] = {};
        char album[256] = {};
        char genre[128] = {};
        char year[8] = {};
        char track[8] = {};
        bool writeTags = false;
        // Index into the kMediaChoices table, or -1 when a multi-selection
        // spans several types — which means "leave each track's own alone".
        int mediaChoice = -1;
    };
    GetInfoEdit getInfo_;

    // Sync review. The plan is recomputed only when something that affects it
    // changes, never per frame.
    struct SyncReview {
        bool open = false;
        bool dirty = false;
        SyncOptions options;
        SyncPlan plan;
        bool confirmRemove = false;
    };
    SyncReview syncUi_;

    // Apple Music import. The read and the copy both run on a worker: the
    // read takes seconds, the copy can move gigabytes.
    struct AppleMusicImport {
        bool open = false;
        std::thread thread;
        std::atomic<bool> finished{false};
        std::atomic<bool> cancel{false};
        std::atomic<int> done{0};
        std::atomic<int> total{0};
        bool busy = false;
        bool copying = false;
        AppleMusicRead read;
        CopyResult copy;
        std::string current;
        std::mutex mutex;
    };
    AppleMusicImport apple_;

    // Duplicate review. Groups are recomputed only when something that
    // affects them changes, not every frame.
    struct DuplicateReview {
        bool open = false;
        bool dirty = false;
        MatchMode mode = MatchMode::Exact;
        bool identicalOnly = false;
        std::vector<DuplicateGroup> groups;
        std::vector<char> enabled;  // per group; char to stay indexable
        VerifyJob verify;
    };
    DuplicateReview dupes_;

    // Playlist editing.
    struct PlaylistEdit {
        int renameIndex = -1;  // -1 = not renaming
        int deleteIndex = -2;  // -2 = no request pending
        char buf[128] = {};
        bool justOpened = false;
    };
    PlaylistEdit plEdit_;

    // Artwork preview for the selected track. The GL texture is created
    // lazily and refreshed only when the selection changes.
    struct ArtworkPreview {
        unsigned int texture = 0;
        std::uint32_t trackId = 0;
        bool hasImage = false;
    };
    ArtworkPreview art_;

    bool ejectRequested_ = false;

    // Playback.
    enum class Repeat { Off, All, One };
    std::unique_ptr<AudioPlayer> player_;
    std::uint32_t playingTrackId_ = 0;
    bool scrubbing_ = false;
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::Off;
    // The sidebar's Now Playing well, toggled from the status bar.
    bool artworkPaneOpen_ = true;
};

}  // namespace podbox
