#include "device/device_session.h"

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
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
namespace fs = std::filesystem;
namespace podbox {
bool DeviceSession::appleMusicSyncing() const {
    if (!connectedIpod()) return false;
    if (loadedMount_.empty()) return false;
    // Apple Music rewrites both of these continuously while it syncs. A
    // timestamp newer than our own last write means the change was not ours.
    const fs::path itunes = loadedMount_ / "iPod_Control" / "iTunes";
    const auto now = fs::file_time_type::clock::now();
    for (const fs::path& name :
         {fs::path("iTunesDB"), fs::path("iTunesCDB"), fs::path("iTunesSD"),
          fs::path("iTunesPrefs"),
          fs::path("iTunes Library.itlp") / "Library.itdb",
          fs::path("iTunes Library.itlp") / "Locations.itdb"}) {
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

void DeviceSession::rotateBackups(const fs::path& dbPath) {
    // Five generations mean any PodBox write can be undone. This also handles
    // the nano 6G/7G SQLite bundle directory as one paired backup.
    constexpr int kKeep = 5;
    const std::string base = dbPath.string() + ".podbox-bak.";
    std::error_code ec;
    fs::remove_all(base + std::to_string(kKeep), ec);
    for (int i = kKeep - 1; i >= 1; --i)
        fs::rename(base + std::to_string(i), base + std::to_string(i + 1), ec);
    if (fs::is_directory(dbPath, ec))
        fs::copy(dbPath, base + "1", fs::copy_options::recursive |
                                      fs::copy_options::copy_symlinks, ec);
    else
        fs::copy_file(dbPath, base + "1", ec);
}

std::vector<fs::path> DeviceSession::availableBackups() const {
    std::vector<fs::path> out;
    if (loadedMount_.empty() || !connectedIpod()) return out;
    const fs::path dbPath = dbFilePath();
    std::error_code ec;
    auto usable = [&](const fs::path& candidate) {
        const std::string db = dbPath.string();
        const std::string selected = candidate.string();
        if (selected.rfind(db, 0) != 0) return false;
        const std::string suffix = selected.substr(db.size());
        if (library_ && library_->hashingScheme == kChecksumHashAB) {
            const fs::path sqlite = dbPath.parent_path() /
                ("iTunes Library.itlp" + suffix);
            const ParseResult oldDb = parseItunesDb(candidate);
            std::string error;
            return oldDb.library &&
                   validateItunesSqliteBundle(*oldDb.library, sqlite,
                                              hashAbUuid_, hashAbNonce_,
                                              &error);
        }
        if (!library_) {
            const auto* device = activeDevice();
            const auto backup = parseItunesDb(candidate);
            return device && ipodRecoveryBlockReason(*device).empty() &&
                   backup.library && backup.library->hashingScheme == kChecksumNone &&
                   !backup.library->compressed;
        }
        if (itunesSdKind_ != ItunesSdKind::Modern) return true;
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
    fs::directory_iterator entry(dbPath.parent_path(), ec), end;
    for (; !ec && entry != end; entry.increment(ec)) {
        const auto p = entry->path();
        if (p.filename().string().rfind("iTunesDB.podbox-recovered.", 0) == 0 &&
            fs::is_regular_file(entry->symlink_status(ec)) && usable(p))
            out.push_back(p);
    }
    return out;
}

bool DeviceSession::writesSupported() const {
    if (!library_) return false;
    if (const DeviceInfo* device = activeDevice(); !device || !device->writable)
        return false;
    if (!connectedIpod()) {
        const DeviceInfo* device = activeDevice();
        return device && device->writable;
    }
    if (itunesSdKind_ == ItunesSdKind::Legacy) return false;
    if (library_->hashingScheme == kChecksumNone) return true;
    // Hash devices become writable only once we have shown we can reproduce
    // the checksum and companion databases they already carry.
    if (library_->hashingScheme == kChecksumHash58) return hash58Verified_;
    if (library_->hashingScheme == kChecksumHash72) return hash72Verified_;
    if (library_->hashingScheme == kChecksumHashAB) return hashAbVerified_;
    return false;
}

std::string DeviceSession::writeBlockReason() const {
    if (!connectedIpod()) return "This player's music folder is read-only";
    if (const DeviceInfo* device = activeDevice(); device && !device->writable)
        return "This iPod is mounted read-only";
    if (!library_) return libraryError_.empty() ? "The iPod music database could not be loaded" : libraryError_;
    if (itunesSdKind_ == ItunesSdKind::Legacy)
        return "This older iPod shuffle uses an unsupported database format";
    if (library_ && library_->hashingScheme == kChecksumHashAB)
        return "Could not verify this nano's hashAB/SQLite database set — "
               "staying read-only";
    return "This iPod needs a hashed database that could not be verified";
}

std::filesystem::path DeviceSession::dbFilePath() const {
    // Nano 5G and later keep a zero-byte iTunesDB and the real library as a
    // compressed iTunesCDB. Which one has content is the reliable tell: Apple
    // never leaves both populated.
    return ipodDatabasePath(loadedMount_);
}

void DeviceSession::verifyChecksum() {
    hash58Verified_ = false;
    hash58Guid_.clear();
    hash72Verified_ = false;
    hash72Iv_.clear();
    hash72Rndpart_.clear();
    hashAbVerified_ = false;
    hashAbUuid_.clear();
    hashAbNonce_.clear();
    if (!library_ || loadedMount_.empty() || !connectedIpod()) return;
    if (library_->hashingScheme == kChecksumNone) return;

    const DeviceInfo* dev = activeDevice();
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
        return;
    }

    if (library_->hashingScheme == kChecksumHashAB) {
        std::ifstream in(dbFilePath(), std::ios::binary);
        if (!in) return;
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const std::vector<std::uint8_t> digest = hashAbSha1(image);
        const std::vector<std::uint8_t> stored = storedHashAb(image);
        const auto nonce = hashAbExtractNonce(stored, digest, guid);
        if (!nonce) {
            setStatus("Could not reproduce this nano's hashAB — staying read-only");
            return;
        }
        const fs::path sqlite = dbFilePath().parent_path() / "iTunes Library.itlp";
        std::string error;
        if (!validateItunesSqliteBundle(*library_, sqlite, guid, *nonce, &error)) {
            setStatus(error + " — staying read-only");
            return;
        }
        hashAbVerified_ = true;
        hashAbUuid_ = guid;
        hashAbNonce_ = *nonce;
    }
}

bool DeviceSession::writeDatabase() {
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
    for (const auto& [path, track] : pendingFileTags_) {
        std::string error;
        if (!writeFileTags(path, track, &error)) {
            setStatus("Could not write file tags: " + error);
            return false;
        }
    }
    pendingFileTags_.clear();
    if (!connectedIpod()) {
        std::string error;
        const DeviceInfo* device = activeDevice();
        if (!device ||
            !saveFilesystemPlayer(loadedMount_, device->musicDirectory,
                                  *library_, &filesystemState_, &error)) {
            setStatus(error.empty() ? "Could not update this player" : error);
            return false;
        }
        fingerprints_.prune(*library_);
        if (!fingerprints_.save(loadedMount_)) {
            setStatus("Could not save PodBox's fingerprints on this player");
            return false;
        }
        return true;
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
    if (library_->hashingScheme == kChecksumHashAB) {
        opts.hashAbUuid = hashAbUuid_;
        opts.hashAbNonce = hashAbNonce_;
        opts.compressed = true;
    }
    if (!writeItunesDb(*library_, tmp, &err, opts)) {
        setStatus(err);
        return false;
    }
    const bool writeSqlite = library_->hashingScheme == kChecksumHashAB;
    const fs::path sqlitePath =
        dbPath.parent_path() / "iTunes Library.itlp";
    const fs::path sqliteTmp = sqlitePath.string() + ".podbox-tmp";
    if (writeSqlite &&
        !writeItunesSqliteBundle(*library_, sqlitePath, sqliteTmp,
                                 hashAbUuid_, hashAbNonce_, &err)) {
        fs::remove(tmp, ec);
        fs::remove_all(sqliteTmp, ec);
        setStatus(err);
        return false;
    }
    const fs::path sdTmp = sdPath.string() + ".podbox-tmp";
    const fs::path statsTmp = statsPath.string() + ".podbox-tmp";
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        ItunesSdWriteOptions sdOptions;
        sdOptions.refreshPlaylistVoiceOver.assign(
            refreshPlaylistVoiceOver_.begin(),
            refreshPlaylistVoiceOver_.end());
        if (!writeItunesSd(*library_, sdPath, sdTmp, loadedMount_, &err,
                           sdOptions) ||
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
    if (writeSqlite) {
        const fs::path sqliteBackup = sqlitePath.string() + ".podbox-backup";
        if (fs::exists(sqlitePath, ec) && !fs::exists(sqliteBackup, ec))
            fs::copy(sqlitePath, sqliteBackup,
                     fs::copy_options::recursive |
                         fs::copy_options::copy_symlinks, ec);
        if (fs::exists(sqlitePath, ec)) rotateBackups(sqlitePath);
    }
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
        const std::error_code writeError = ec;
        std::error_code cleanup;
        fs::remove_all(sqliteTmp, cleanup);
        setStatus("Could not update database: " + writeError.message());
        return false;
    }
    if (writeSqlite) {
        const fs::path sqliteOld = sqlitePath.string() + ".podbox-old";
        fs::remove_all(sqliteOld, ec);
        fs::rename(sqlitePath, sqliteOld, ec);
        if (!ec) fs::rename(sqliteTmp, sqlitePath, ec);
        if (ec) {
            std::error_code rollback;
            fs::copy_file(dbPath.string() + ".podbox-bak.1", dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            if (!fs::exists(sqlitePath, rollback) &&
                fs::exists(sqliteOld, rollback))
                fs::rename(sqliteOld, sqlitePath, rollback);
            fs::remove_all(sqliteTmp, rollback);
            setStatus("Could not update the nano SQLite databases: " +
                      ec.message());
            return false;
        }
        fs::remove_all(sqliteOld, ec);
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

    // Announcements for deleted playlists are not referenced by the new
    // iTunesSD. Remove them only after all of the paired database files have
    // been installed successfully, so a failed write remains retryable.
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        const fs::path announcements =
            loadedMount_ / "iPod_Control" / "Speakable" / "Playlists";
        for (const std::uint64_t dbid : removedPlaylistVoiceOver_) {
            char stem[17];
            std::snprintf(stem, sizeof(stem), "%016llX",
                          static_cast<unsigned long long>(dbid));
            fs::remove(announcements / (std::string(stem) + ".wav"), ec);
            fs::remove(announcements / (std::string(stem) + ".aiff"), ec);
        }
    }
    refreshPlaylistVoiceOver_.clear();
    removedPlaylistVoiceOver_.clear();

    // Remember this write so appleMusicSyncing() can tell ours from theirs.
    ownWriteTime_ = fs::last_write_time(dbPath, ec);
    return true;
}

void DeviceSession::load(const DeviceInfo& device) {
    const DeviceInfo* dev = &device;
    loadedMount_ = device.mountPoint;
    loadedDeviceInfo_ = device;
    recoveryBlockReason_ = device.isIpod() ? ipodRecoveryBlockReason(device) : "Not an iPod";
    if (dev->isIpod()) {
        itunesSdKind_ = detectItunesSd(
            loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD");
        ParseResult res = loadIpodLibrary(*dev);
        library_ = std::move(res.library);
        libraryError_ = res.error;
        if (library_ && itunesSdKind_ == ItunesSdKind::Modern)
            reconcileShufflePlaylistIds(
                *library_,
                loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD");
    } else {
        itunesSdKind_ = ItunesSdKind::None;
        FilesystemPlayerLoad loaded = loadFilesystemPlayer(
            loadedMount_, dev->musicDirectory, &filesystemState_);
        library_ = std::move(loaded.library);
        managedFilesystemTrackIds_ = std::move(loaded.managedTrackIds);
        libraryError_ = std::move(loaded.error);
    }
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
    if (library_ && dev->isIpod()) {
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
}

bool DeviceSession::restoreDatabase(const fs::path& chosen) {
    if (appleMusicSyncing()) {
        setStatus("Apple Music is syncing this iPod — try again when it "
                  "finishes");
        return false;
    }
    const fs::path dbPath = dbFilePath();
    const fs::path sdPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesSD";
    const fs::path statsPath =
        loadedMount_ / "iPod_Control" / "iTunes" / "iTunesStats";
    const bool restoreSqlite =
        library_ && library_->hashingScheme == kChecksumHashAB;
    const fs::path sqlitePath =
        dbPath.parent_path() / "iTunes Library.itlp";
    std::error_code ec;
    fs::path chosenSd, chosenStats, chosenSqlite;
    const std::string db = dbPath.string();
    const std::string selected = chosen.string();
    if (selected.rfind(db, 0) != 0) {
        setStatus("Could not match this backup to its companion databases");
        return false;
    }
    const std::string suffix = selected.substr(db.size());
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        chosenSd = sdPath.string() + suffix;
        chosenStats = statsPath.string() + suffix;
        if (!fs::exists(chosenSd, ec) || !fs::exists(chosenStats, ec)) {
                setStatus("This backup predates Shuffle support and has no matching "
                      "iTunesSD/iTunesStats backup");
            return false;
        }
    }
    if (restoreSqlite) {
        chosenSqlite = sqlitePath.string() + suffix;
        if (!fs::is_directory(chosenSqlite, ec)) {
                setStatus("This backup has no matching nano SQLite bundle");
            return false;
        }
    }
    if (!library_) {
        const auto candidate = parseItunesDb(chosen);
        if (!candidate.library || candidate.library->hashingScheme != kChecksumNone ||
            candidate.library->compressed) {
            setStatus("This backup is not a usable unsigned iPod video database");
            return false;
        }
    }
    // A selected .bak.1 becomes .bak.2 when rotation starts, so stage the
    // chosen pair before rotating the live state into .bak.1.
    const fs::path restoreTmp = dbPath.string() + ".podbox-restore-tmp";
    fs::copy_file(chosen, restoreTmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        setStatus("Could not stage restore: " + ec.message());
        return false;
    }
    const fs::path restoreSqliteTmp =
        sqlitePath.string() + ".podbox-restore-tmp";
    if (restoreSqlite) {
        fs::remove_all(restoreSqliteTmp, ec);
        ec.clear();
        fs::copy(chosenSqlite, restoreSqliteTmp,
                 fs::copy_options::recursive |
                     fs::copy_options::copy_symlinks, ec);
        if (ec) {
            const std::error_code stageError = ec;
            std::error_code cleanup;
            fs::remove(restoreTmp, cleanup);
            fs::remove_all(restoreSqliteTmp, cleanup);
                setStatus("Could not stage nano SQLite restore: " +
                      stageError.message());
            return false;
        }
    }
    const fs::path restoreSdTmp = sdPath.string() + ".podbox-restore-tmp";
    const fs::path restoreStatsTmp =
        statsPath.string() + ".podbox-restore-tmp";
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        fs::copy_file(chosenSd, restoreSdTmp,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            const std::error_code stageError = ec;
            std::error_code cleanup;
            fs::remove(restoreTmp, cleanup);
                setStatus("Could not stage Shuffle restore: " +
                      stageError.message());
            return false;
        }
        fs::copy_file(chosenStats, restoreStatsTmp,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            const std::error_code stageError = ec;
            std::error_code cleanup;
            fs::remove(restoreTmp, cleanup);
            fs::remove(restoreSdTmp, cleanup);
                setStatus("Could not stage Shuffle statistics restore: " +
                      stageError.message());
            return false;
        }
    }
    rotateBackups(dbPath);  // the current state becomes undoable too
    if (restoreSqlite) rotateBackups(sqlitePath);
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        rotateBackups(sdPath);
        rotateBackups(statsPath);
    }
    fs::rename(restoreTmp, dbPath, ec);
    const std::error_code dbRestoreError = ec;
    std::error_code cleanup;
    fs::remove(restoreTmp, cleanup);
    if (dbRestoreError) {
        fs::remove_all(restoreSqliteTmp, cleanup);
        fs::remove(restoreSdTmp, cleanup);
        fs::remove(restoreStatsTmp, cleanup);
        setStatus("Could not restore: " + dbRestoreError.message());
        return false;
    }
    if (restoreSqlite) {
        const fs::path sqliteOld = sqlitePath.string() + ".podbox-old";
        fs::remove_all(sqliteOld, cleanup);
        fs::rename(sqlitePath, sqliteOld, ec);
        if (!ec) fs::rename(restoreSqliteTmp, sqlitePath, ec);
        if (ec) {
            const std::error_code sqliteRestoreError = ec;
            std::error_code rollback;
            fs::copy_file(dbPath.string() + ".podbox-bak.1", dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            if (!fs::exists(sqlitePath, rollback) &&
                fs::exists(sqliteOld, rollback))
                fs::rename(sqliteOld, sqlitePath, rollback);
            fs::remove_all(restoreSqliteTmp, rollback);
                setStatus("Could not restore nano SQLite databases: " +
                      sqliteRestoreError.message());
            return false;
        }
        fs::remove_all(sqliteOld, cleanup);
    }
    if (itunesSdKind_ == ItunesSdKind::Modern) {
        fs::copy_file(restoreSdTmp, sdPath,
                      fs::copy_options::overwrite_existing, ec);
        const std::error_code sdRestoreError = ec;
        fs::remove(restoreSdTmp, cleanup);
        if (sdRestoreError) {
            fs::remove(restoreStatsTmp, cleanup);
            std::error_code rollback;
            fs::copy_file(dbPath.string() + ".podbox-bak.1", dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            fs::copy_file(sdPath.string() + ".podbox-bak.1", sdPath,
                          fs::copy_options::overwrite_existing, rollback);
                setStatus("Could not restore Shuffle database: " +
                      sdRestoreError.message());
            return false;
        }
        fs::copy_file(restoreStatsTmp, statsPath,
                      fs::copy_options::overwrite_existing, ec);
        const std::error_code statsRestoreError = ec;
        fs::remove(restoreStatsTmp, cleanup);
        if (statsRestoreError) {
            std::error_code rollback;
            fs::copy_file(dbPath.string() + ".podbox-bak.1", dbPath,
                          fs::copy_options::overwrite_existing, rollback);
            fs::copy_file(sdPath.string() + ".podbox-bak.1", sdPath,
                          fs::copy_options::overwrite_existing, rollback);
            fs::copy_file(statsPath.string() + ".podbox-bak.1", statsPath,
                          fs::copy_options::overwrite_existing, rollback);
                setStatus("Could not restore Shuffle statistics: " +
                      statsRestoreError.message());
            return false;
        }
    }
    // On a compressed-database device the placeholder iTunesDB must stay a
    // zero-byte placeholder, or the device reads the wrong file.
    if (library_ && library_->compressed) {
        std::ofstream plain(dbPath.parent_path() / "iTunesDB",
                            std::ios::binary | std::ios::trunc);
    }
    ownWriteTime_ = fs::last_write_time(dbPath, ec);
    setStatus("Restored the database from " + chosen.filename().string());
    return true;
}

int DeviceSession::deleteTracks(const std::vector<std::uint32_t>& ids,
    const std::unordered_map<std::uint32_t, std::uint32_t>* remap) {
    if (!library_ || ids.empty()) return 0;

    std::unordered_set<std::uint32_t> doomed(ids.begin(), ids.end());
    // A track that something is being remapped *to* is by definition a keeper;
    // removing it as well would lose the song entirely.
    if (remap)
        for (const auto& [from, to] : *remap) doomed.erase(to);

    std::error_code ec;
    std::unordered_set<std::uint32_t> failed;
    int removed = 0;
    for (std::uint32_t id : doomed) {
        const auto it = trackIndexById_.find(id);
        if (it == trackIndexById_.end()) continue;
        const std::string location = library_->tracks[it->second].location;
        ec.clear();
        std::string relative = location;
        if (!relative.empty() && relative.front() == ':') relative.erase(0, 1);
        std::replace(relative.begin(), relative.end(), ':', '/');
        fs::remove(loadedMount_ / relative, ec);
        if (ec) {
            failed.insert(id);
            continue;
        }
        if (!connectedIpod()) {
            filesystemState_.managedTracks.erase(location);
            managedFilesystemTrackIds_.erase(id);
        }
        ++removed;
    }
    for (std::uint32_t id : failed) doomed.erase(id);
    if (removed == 0) return 0;

    std::erase_if(library_->tracks,
                  [&](const Track& t) { return doomed.count(t.id) != 0; });

    for (auto& pl : library_->playlists)
        applyRemovalToPlaylist(doomed, remap, &pl.trackIds);

    trackIndexById_.clear();
    trackIndexById_.reserve(library_->tracks.size());
    for (int i = 0; i < int(library_->tracks.size()); ++i)
        trackIndexById_[library_->tracks[i].id] = i;

    return removed;
}

} // namespace podbox
