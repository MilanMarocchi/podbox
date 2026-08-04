// App lifecycle, the frame loop, library loading, every mutation path,
// selection and playback. The drawing lives in app_chrome.cpp (the window
// chrome) and app_modals.cpp (the dialogs).

#include "app/app.h"
#include "app/app_util.h"

#include "device/ipod_device.h"
#include "itdb/hash58.h"
#include "itdb/hash72.h"
#include "itdb/playcounts.h"
#include "itdb/itunessd.h"
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
    if (scan_.thread.joinable()) scan_.thread.join();
    if (apple_.thread.joinable()) apple_.thread.join();
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
    // writeDatabase() refuses too, but catching it here means a dropped folder
    // is not transcoded and copied onto the device before we admit we cannot
    // record it in the database.
    if (!writesSupported()) {
        setStatus(writeBlockReason());
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
    for (const char* name : {"iTunesDB", "iTunesSD", "iTunesPrefs"}) {
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
    auto usable = [&](const fs::path& candidate) {
        if (itunesSdKind_ != ItunesSdKind::Modern) return true;
        const std::string db = dbPath.string();
        const std::string selected = candidate.string();
        if (selected.rfind(db, 0) != 0) return false;
        const std::string suffix = selected.substr(db.size());
        const fs::path sd =
            loadedMount_ / "iPod_Control" / "iTunes" /
            ("iTunesSD" + suffix);
        const fs::path stats =
            loadedMount_ / "iPod_Control" / "iTunes" /
            ("iTunesStats" + suffix);
        const ParseResult oldDb = parseItunesDb(candidate);
        const auto oldSd = parseItunesSd(sd);
        if (!oldDb.library || !oldSd ||
            oldDb.library->tracks.size() != oldSd->tracks.size())
            return false;
        std::ifstream in(stats, std::ios::binary);
        std::array<unsigned char, 4> count{};
        if (!in.read(reinterpret_cast<char*>(count.data()), count.size()))
            return false;
        const std::uint32_t statsCount =
            count[0] | (std::uint32_t(count[1]) << 8) |
            (std::uint32_t(count[2]) << 16) |
            (std::uint32_t(count[3]) << 24);
        return statsCount == oldSd->tracks.size();
    };
    // The one-shot backup from before rotation existed is still the oldest and
    // most valuable snapshot, so keep offering it.
    const fs::path legacy = dbPath.string() + ".podbox-backup";
    for (int i = 1; i <= 5; ++i) {
        const fs::path p = dbPath.string() + ".podbox-bak." + std::to_string(i);
        if (fs::exists(p, ec) && usable(p)) out.push_back(p);
    }
    if (fs::exists(legacy, ec) && usable(legacy)) out.push_back(legacy);
    return out;
}

bool App::writesSupported() const {
    if (!library_) return false;
    if (itunesSdKind_ == ItunesSdKind::Legacy) return false;
    if (library_->hashingScheme == kChecksumNone) return true;
    // Hash devices become writable only once we have shown we can reproduce
    // the checksum they already carry. hashAB stays read-only.
    if (library_->hashingScheme == kChecksumHash58) return hash58Verified_;
    if (library_->hashingScheme == kChecksumHash72) return hash72Verified_;
    return false;
}

std::string App::writeBlockReason() const {
    if (itunesSdKind_ == ItunesSdKind::Legacy)
        return "This older iPod shuffle uses an unsupported database format";
    return "This iPod needs a hashed database — writes not yet supported";
}

std::filesystem::path App::dbFilePath() const {
    // Nano 5G and later keep a zero-byte iTunesDB and the real library as a
    // compressed iTunesCDB. Which one has content is the reliable tell: Apple
    // never leaves both populated.
    const fs::path dir = loadedMount_ / "iPod_Control" / "iTunes";
    std::error_code ec;
    const fs::path cdb = dir / "iTunesCDB";
    if (fs::exists(cdb, ec) && fs::file_size(cdb, ec) > 0) return cdb;
    return dir / "iTunesDB";
}

void App::verifyChecksum() {
    hash58Verified_ = false;
    hash58Guid_.clear();
    hash72Verified_ = false;
    hash72Iv_.clear();
    hash72Rndpart_.clear();
    if (!library_ || loadedMount_.empty()) return;

    const auto& dev = watcher_.device();
    if (!dev) return;
    const std::vector<std::uint8_t> guid =
        parseFirewireGuid(dev->firewireGuid);
    if (guid.empty()) {
        setStatus("This iPod needs a checksum, but its FireWire GUID is missing");
        return;
    }

    if (library_->hashingScheme == kChecksumHash58) {
        std::ifstream in(dbFilePath(), std::ios::binary);
        if (!in) return;
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const std::vector<std::uint8_t> stored = storedHash58(image);
        const std::vector<std::uint8_t> computed =
            hash58OfDatabase(image, guid);
        if (computed.empty() || stored != computed) {
            setStatus("Could not reproduce this iPod's checksum — staying read-only");
            return;
        }
        hash58Verified_ = true;
        hash58Guid_ = guid;
        return;
    }

    if (library_->hashingScheme == kChecksumHash72) {
        std::ifstream in(dbFilePath(), std::ios::binary);
        if (!in) return;
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const std::vector<std::uint8_t> digest = hash72Sha1(image);
        const std::vector<std::uint8_t> stored = storedHash72(image);
        if (digest.empty() || stored.empty()) return;

        const fs::path hashInfo =
            loadedMount_ / "iPod_Control" / "Device" / "HashInfo";
        std::vector<std::uint8_t> iv, rndpart;
        if (const auto info = readHashInfo(hashInfo, guid)) {
            // The database on the device was signed with the HashInfo's
            // values; regenerating its signature is the real proof that
            // writing with them will be accepted.
            if (hash72Signature(digest, info->iv, info->rndpart) != stored) {
                setStatus("This iPod's checksum does not match its HashInfo — "
                          "staying read-only");
                return;
            }
            iv = info->iv;
            rndpart = info->rndpart;
        } else {
            // No HashInfo yet: recover (IV, random) from the signature of the
            // database the device already accepts, then record it where the
            // firmware expects it.
            const auto params = hash72Extract(stored, digest);
            if (!params) {
                setStatus("Could not reproduce this iPod's checksum — staying "
                          "read-only");
                return;
            }
            iv = params->iv;
            rndpart = params->rndpart;
            writeHashInfo(hashInfo,
                          {hash72Uuid(guid), rndpart, iv});
        }
        hash72Verified_ = true;
        hash72Iv_ = std::move(iv);
        hash72Rndpart_ = std::move(rndpart);
    }
}

bool App::writeDatabase() {
    if (!library_ || loadedMount_.empty()) return false;
    // The one gate every mutation passes through. It used to sit only on the
    // drag-and-drop path, which left deletes, ratings, playlist edits, sync and
    // restore free to write an unhashed database to a device that needs one.
    if (!writesSupported()) {
        setStatus(writeBlockReason());
        return false;
    }
    if (appleMusicSyncing()) {
        setStatus("Apple Music is syncing this iPod — try again when it finishes");
        return false;
    }
    const fs::path dbPath = dbFilePath();
    const fs::path sdPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD";
    const fs::path statsPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesStats";
    std::error_code ec;
    const fs::path tmp = dbPath.string() + ".podbox-tmp";
    std::string err;
    WriteOptions opts;
    if (library_->hashingScheme == kChecksumHash58)
        opts.hash58Guid = hash58Guid_;
    if (library_->hashingScheme == kChecksumHash72) {
        opts.hash72Iv = hash72Iv_;
        opts.hash72Rndpart = hash72Rndpart_;
        opts.compressed = library_->compressed;
    }
    if (!writeItunesDb(*library_, tmp, &err, opts)) {
        setStatus(err);
        return false;
    }
    const fs::path sdTmp = sdPath.string() + ".podbox-tmp";
    const fs::path statsTmp = statsPath.string() + ".podbox-tmp";
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        if (!writeItunesSd(*library_, sdPath, sdTmp, loadedMount_, &err) ||
            !writeShuffleStats(*library_, sdPath, statsPath, statsTmp, &err)) {
            fs::remove(tmp, ec);
            fs::remove(sdTmp, ec);
            fs::remove(statsTmp, ec);
            setStatus(err);
            return false;
        }
    }
    // Every replacement file is ready before backups rotate or any live
    // database changes. A failed speech synthesis therefore remains a true
    // no-op from the user's point of view.
    const fs::path backup = dbPath.string() + ".podbox-backup";
    if (fs::exists(dbPath, ec) && !fs::exists(backup, ec))
        fs::copy_file(dbPath, backup, ec);
    if (fs::exists(dbPath, ec)) rotateBackups(dbPath);
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        const fs::path sdBackup = sdPath.string() + ".podbox-backup";
        if (fs::exists(sdPath, ec) && !fs::exists(sdBackup, ec))
            fs::copy_file(sdPath, sdBackup, ec);
        if (fs::exists(sdPath, ec)) rotateBackups(sdPath);
        const fs::path statsBackup = statsPath.string() + ".podbox-backup";
        if (fs::exists(statsPath, ec) && !fs::exists(statsBackup, ec))
            fs::copy_file(statsPath, statsBackup, ec);
        if (fs::exists(statsPath, ec)) rotateBackups(statsPath);
    }
    fs::rename(tmp, dbPath, ec);
    if (ec) {
        setStatus("Could not update database: " + ec.message());
        return false;
    }
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        fs::rename(sdTmp, sdPath, ec);
        if (ec) {
            // Keep the two databases paired. The rotating backup was made
            // immediately before this write and is the old iTunesDB.
            const fs::path previous = dbPath.string() + ".podbox-bak.1";
            std::error_code rollback;
            fs::copy_file(previous, dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            setStatus("Could not update the Shuffle database: " + ec.message());
            return false;
        }
        fs::rename(statsTmp, statsPath, ec);
        if (ec) {
            std::error_code rollback;
            fs::copy_file(dbPath.string() + ".podbox-bak.1", dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            fs::copy_file(sdPath.string() + ".podbox-bak.1", sdPath,
                          fs::copy_options::overwrite_existing, rollback);
            if (fs::exists(statsPath.string() + ".podbox-bak.1", rollback))
                fs::copy_file(statsPath.string() + ".podbox-bak.1", statsPath,
                              fs::copy_options::overwrite_existing, rollback);
            setStatus("Could not update Shuffle statistics: " + ec.message());
            return false;
        }
    }
    if (library_->compressed) {
        // A compressed-database device keeps its iTunesDB as a zero-byte
        // placeholder, exactly as Apple's own software leaves it.
        std::ofstream plain(dbPath.parent_path() / "iTunesDB",
                            std::ios::binary | std::ios::trunc);
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
    if (withTrackId) pl.trackIds.push_back(withTrackId);
    library_->playlists.push_back(std::move(pl));
    playlistIndex_ = int(library_->playlists.size()) - 1;
    view_ = View::Playlist;
    visibleDirty_ = true;
    // Open inline rename on the fresh playlist.
    plEdit_.renameIndex = playlistIndex_;
    plEdit_.justOpened = true;
    std::snprintf(plEdit_.buf, sizeof(plEdit_.buf), "%s",
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

bool App::animating() const {
    return player_ && player_->state() == PlaybackState::Playing;
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
    const auto& dev = watcher_.device();
    if (!dev) {
        if (library_ || !loadedMount_.empty()) {
            if (player_) player_->stop();
            playingTrackId_ = 0;
            library_.reset();
            libraryError_.clear();
            loadedMount_.clear();
            trackIndexById_.clear();
            itunesSdKind_ = ItunesSdKind::None;
            view_ = hostView_.tracks.empty() ? View::Device : View::Library;
            playlistIndex_ = -1;
            visibleDirty_ = true;
        }
        return;
    }
    if (dev->mountPoint == loadedMount_) return;
    loadedMount_ = dev->mountPoint;
    itunesSdKind_ = detectItunesSd(
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD");

    ParseResult res = parseItunesDb(dbFilePath());
    library_ = std::move(res.library);
    libraryError_ = res.error;
    if (library_ && itunesSdKind_ == ItunesSdKind::Modern)
        reconcileShufflePlaylistIds(
            *library_,
            loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD");
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

    verifyChecksum();
    pullPlayCountsToHost();

    switchSource(library_ ? View::Music : View::Device);
    // A new device means the cached artwork texture describes a track that no
    // longer exists; only this path needs to say so.
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
    return watcher_.device() && library_ && view_ != View::Device;
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
    if (scan_.running) return;
    if (scan_.thread.joinable()) scan_.thread.join();

    // The worker gets its own copy so the UI can keep reading the live
    // library while a cold scan (which reads and hashes every file) runs.
    scan_.running = true;
    scan_.finished.store(false);
    scan_.result = std::make_unique<HostLibrary>(host_);
    scan_.thread = std::thread([this] {
        scan_.stats = scan_.result->rescan();
        scan_.result->save();
        scan_.finished.store(true);
    });
    setStatus("Scanning your music folders…");
}

void App::applyFinishedScan() {
    if (!scan_.finished.load()) return;
    scan_.finished.store(false);
    if (scan_.thread.joinable()) scan_.thread.join();
    scan_.running = false;
    if (!scan_.result) return;

    host_ = std::move(*scan_.result);
    scan_.result.reset();
    rebuildHostView();

    const ScanStats& s = scan_.stats;
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
