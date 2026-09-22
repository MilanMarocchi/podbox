#pragma once

#include "audio/player.h"
#include "device/device_session.h"
#include "util/background_job.h"
#include "library/artwork.h"
#include "device/device_watcher.h"
#include "device/ipod_recovery.h"
#include "device/filesystem_player.h"
#include "itdb/itunesdb.h"
#include "itdb/itunessd.h"
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
#include <unordered_set>
#include <utility>
#include <vector>

struct GLFWwindow;

namespace podbox {

class App : private DeviceSession {
public:
    explicit App(const Fonts& fonts)
        : fonts_(fonts), player_(AudioPlayer::create()) {}

    // Joins owned workers as a lifetime backstop after close has drained them.
    ~App();

    // Draws one frame of the UI. Call between ImGui NewFrame/Render.
    void frame();
    // Called before UI teardown; returns false if copied songs remain unsaved.
    bool prepareToClose();
    bool wantsToClose() const { return closeWithoutSaving_ || closeRequested_; }

    // The window, so dragging the toolbar can move it. The toolbar occupies
    // the title bar, so there is nothing else left to grab.
    void setWindow(GLFWwindow* w) { window_ = w; }

    // Files/folders dropped onto the window (from the GLFW drop callback).
    void onFilesDropped(const std::vector<std::string>& paths);

    // True when the UI should redraw continuously (e.g. playback in progress),
    // so the main loop can pick a shorter event-wait timeout.
    bool animating() const;

    // System media commands use the same playback decisions as the toolbar.
    // These are public because macOS delivers them at the application level,
    // outside the immediate-mode UI frame.
    void play();
    void pause();
    void togglePlayback();

private:
    friend struct AppTestAccess;
    // Which source the main panel is showing. Library is the Mac-side
    // collection; the rest are the connected iPod's. Music, Podcasts and
    // Audiobooks are the same track list partitioned by media type, the way
    // iTunes split its source list.
    enum class View { Device, Music, Playlist, Library, Podcasts, Audiobooks };

    void updateLibrary();
    void applyDeviceJob();
    void startEject(const std::filesystem::path& mount,
        std::function<bool(const std::filesystem::path&, std::string*)> eject = ejectDevice);
    void applyBackgroundWork();
    void saveHost();
    bool appleMusicSyncing() const { return musicSyncing_; }
    bool musicSyncing_ = false;
    struct BackupRow { std::filesystem::path path; std::string label; };
    struct MonitorResult { std::filesystem::path mount; bool syncing = false; bool backups = false; std::vector<BackupRow> rows; };
    BackgroundJob<MonitorResult> monitorJob_;
    double lastMonitor_ = -2;
    bool backupsLoaded_ = false;
    std::vector<BackupRow> backupRows_;
    BackgroundJob<bool> hostSaveJob_;
    bool hostSavePending_ = false;
    BackgroundJob<std::filesystem::path> folderJob_;
    struct DropResult { std::filesystem::path mount; std::vector<std::filesystem::path> files; };
    BackgroundJob<DropResult> dropJob_;
    BackgroundJob<ArtImage> artJob_;
    BackgroundJob<int> tagJob_;
    BackgroundJob<bool> encoderJob_;
    bool encoderChecked_ = false;
    bool mp3Available_ = false;
    BackgroundJob<bool> fingerprintSaveJob_;
    bool fingerprintSavePending_ = false;
    BackgroundJob<std::unordered_map<std::string, bool>> folderCheckJob_;
    std::unordered_map<std::string, bool> folderExists_;
    double lastFolderCheck_ = -2;

    BackgroundJob<SyncPlan> syncPlanJob_;
    BackgroundJob<std::vector<DuplicateGroup>> duplicateJob_;
    std::vector<std::pair<std::filesystem::path, Track>> pendingHostTags_;
    std::filesystem::path artworkPath_, artworkJobPath_;
    std::filesystem::path ejectingMount_;

    enum class DeviceJobKind { Load, Save, Restore, Eject, Recovery, Delete };
    struct DeviceResult {
        DeviceSession session;
        bool ok = true;
        std::string error;
        int removed = 0;
    };
    BackgroundJob<DeviceResult> deviceJob_;
    DeviceJobKind deviceJobKind_ = DeviceJobKind::Load;
    bool closeRequested_ = false;

    // Point the main panel at a different source. Selection is per-source, so
    // switching always clears it — four copies of this used to drift apart.
    // `playlistIndex` is only meaningful for View::Playlist.
    void switchSource(View view, int playlistIndex = -1);
    // The media type a view shows, or 0 for views that do not partition by it.
    std::uint32_t viewMediaType() const;
    // True when the device library holds anything of that media type, which is
    // what decides whether the sidebar offers the row at all.
    // True when the main panel is showing a track list rather than the
    // device pane or an empty state.
    bool showingTracks() const;
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
    // Queues a snapshot save; true means accepted, not persisted.
    bool writeDatabase();
    const DeviceInfo* activeDevice() const;
    void activateDevice(const std::filesystem::path& mount);
    void finishPendingDeviceCopy();
    bool connectedIpod() const;
    ImportTarget currentImportTarget() const;
    // importFormat_, or Original when the active device cannot play what the
    // chosen conversion produces.
    ImportFormat currentImportFormat() const;
    // Radio buttons for the conversions `dev` can play. Returns true when the
    // choice changed, so callers can re-plan a sync.
    bool drawImportFormatChoices(const DeviceInfo& dev);
    bool restoreSupported() const;
    void drawRestoreModal();
    void openRecovery();
    void drawRecoveryModal();
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
    // Returns how many removals were queued; completion reports actual results.
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
    void addToPlaylist(int playlistIndex,
                       const std::vector<std::uint32_t>& trackIds);
    void queueFilesToDevice(const std::vector<std::filesystem::path>& files);
    void queueFilesToDevice(const std::vector<std::filesystem::path>& files,
                            const std::filesystem::path& targetMount);
    void addSelectedHostTracksToDevice(
        const std::filesystem::path& targetMount);
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
    // The track currently playing, whichever library it came from. Playback
    // outlives a source switch, so this looks in the shown library first and
    // then the other one rather than assuming the iPod's.
    const Track* playingTrack() const;
    void playRelative(int delta);
    void drawTransport();
    void drawNowPlaying(ImVec2 lcdMin, ImVec2 lcdMax);
    void drawToolbar();
    void drawSidebar(float height);
    void drawMainPanel(float height);
    void drawDeviceView(const DeviceInfo& dev);
    void drawTrackTable();
    void handleTrackTableKeys();
    void drawCapacityBar(const DeviceInfo& dev);
    void drawStatusBar();

    Fonts fonts_;
    GLFWwindow* window_ = nullptr;
    DeviceWatcher watcher_;

    // The Mac-side library, plus a Library-shaped view of it so the track
    // table, search, sorting and dedupe all work on it unchanged.
    HostLibrary host_;
    Library hostView_;
    std::unordered_map<std::uint32_t, int> hostIndexById_;
    bool hostLoaded_ = false;

    // A rescan runs on a worker over its own copy of the library and is
    // swapped in when it finishes, so a cold scan of a large folder never
    // blocks the frame loop.
    struct HostScan {
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::unique_ptr<HostLibrary> result;
        ScanStats stats;
        std::string error;
        bool running = false;
    };
    HostScan scan_;

    std::filesystem::path requestedDeviceMount_;
    std::filesystem::path pendingCopyMount_;
    std::vector<std::filesystem::path> pendingCopyFiles_;

    View view_ = View::Device;
    int playlistIndex_ = -1;
    char search_[128] = {};
    int sortCol_ = 0;
    bool sortAsc_ = true;
    bool visibleDirty_ = true;
    // (position in unsorted list, index into library tracks)
    std::vector<std::pair<int, int>> visible_;
    // Derived from visible_ and the device library by rebuildVisible(). Both
    // are read by the chrome every frame, so neither is recomputed there.
    std::uint64_t visibleTotalMs_ = 0;
    std::uint64_t visibleTotalBytes_ = 0;
    bool devHasPodcasts_ = false;
    bool devHasAudiobooks_ = false;
    // The primary selection (artwork, keyboard target). selection_ holds the
    // whole set, which is usually just this one.
    std::uint32_t selectedTrackId_ = 0;
    std::vector<std::uint32_t> selection_;
    std::uint32_t selectionAnchor_ = 0;  // for shift-click ranges
    int dragSelectAnchorRow_ = -1;
    bool dragSelectMoved_ = false;

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
    bool skipDuplicates_ = true;
    bool pendingDbWrite_ = false;
    bool batchWriteFailed_ = false;
    bool closeSaveFailedOpen_ = false;
    bool closeWithoutSaving_ = false;
    int lastBatchAdded_ = 0;
    int lastBatchSkipped_ = 0;
    std::uint32_t deleteRequestId_ = 0;
    std::string statusMsg_;
    double statusMsgUntil_ = 0.0;

    bool restoreOpen_ = false;
    struct RecoveryUi {
        bool open = false;
        bool running = false;
        std::atomic<bool> finished{false};
        std::atomic<bool> cancel{false};
        std::thread thread;
        IpodRecovery result;
        std::filesystem::path mount;
    } recovery_;
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
        HostLibrary hostResult;
        int added = 0;
        std::string error;
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

    std::filesystem::path ejectRequestedMount_;

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
