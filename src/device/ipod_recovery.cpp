#include "device/ipod_recovery.h"
#include "library/metadata.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace podbox {
namespace {

bool present(const fs::path& path) {
    std::error_code ec;
    const auto s = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        throw std::runtime_error("Could not inspect " + path.string() + ": " + ec.message());
    return fs::exists(s);
}

std::map<fs::path, RecoveryFile> snapshot(const fs::path& mount, const fs::path& ignore = {}) {
    std::map<fs::path, RecoveryFile> result;
    const fs::path control = mount / "iPod_Control";
    // Include directories, sidecars and backups so a concurrent sync or device
    // replacement invalidates the preview even when it changes no audio size.
    for (auto it = fs::recursive_directory_iterator(control); it != fs::recursive_directory_iterator(); ++it) {
        const auto& e = *it;
        if (!ignore.empty() && e.path() == ignore) {
            it.disable_recursion_pending();
            continue;
        }
        const auto status = e.symlink_status();
        if (fs::is_symlink(status) ||
            (!fs::is_directory(status) && !fs::is_regular_file(status)))
            throw std::runtime_error("Recovery cannot follow links or special files: " + e.path().string());
        result.emplace(e.path().lexically_relative(mount),
                       RecoveryFile{fs::is_regular_file(status) ? e.file_size() : 0,
                                    e.last_write_time()});
    }
    return result;
}

bool flushFile(const fs::path& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = "Could not open recovery data for flushing: " + std::string(std::strerror(errno));
        return false;
    }
    const int flushed = ::fsync(fd);
    const int saved = errno;
    const int closed = ::close(fd);
    if (flushed != 0 || closed != 0) {
        *error = "Could not flush recovery data: " + std::string(std::strerror(flushed ? saved : errno));
        return false;
    }
    return true;
}

struct stat volumeIdentity(const fs::path& mount) {
    struct stat result{};
    if (::stat(mount.c_str(), &result) != 0)
        throw std::runtime_error("The iPod is no longer available");
    return result;
}
}  // namespace

std::string ipodRecoveryBlockReason(const IpodInfo& device) {
    if (!isIpodVideo(device))
        return "Rebuilding from files currently supports iPod video (5th/5.5th generation). Other models need a verified database backup.";
    if (!device.writable) return "This iPod is mounted read-only";
    try {
        for (const fs::path& p : {device.mountPoint, device.mountPoint / "iPod_Control",
                                  device.mountPoint / "iPod_Control" / "iTunes",
                                  device.mountPoint / "iPod_Control" / "Music"})
            if (!fs::is_directory(fs::symlink_status(p)))
                return "The iPod's control and music folders must be real, accessible directories";
        const fs::path dir = device.mountPoint / "iPod_Control" / "iTunes";
        for (const char* name : {"iTunesCDB", "iTunesSD", "iTunes Library.itlp"})
            if (present(dir / name))
                return "This iPod has companion databases; restore a matching database backup instead";
        if (present(dir / "iTunesDB")) {
            if (!fs::is_regular_file(fs::symlink_status(dir / "iTunesDB")))
                return "The existing database is not a regular file";
            if (parseItunesDb(dir / "iTunesDB").library)
                return "This iPod already has a readable database. Use Restore Database to recover an earlier version.";
        }
    } catch (const std::exception& e) { return e.what(); }
    return {};
}

IpodRecovery scanIpodRecovery(const IpodInfo& device, const std::atomic<bool>* cancel) {
    IpodRecovery result;
    result.device = device;
    result.error = ipodRecoveryBlockReason(device);
    if (!result.error.empty()) return result;
    try {
        const auto identity = volumeIdentity(device.mountPoint);
        result.volumeDevice = identity.st_dev;
        result.volumeInode = identity.st_ino;
        result.files = snapshot(device.mountPoint);
        result.library.masterName = device.volumeName;
        result.library.version = 0x19;
        std::mt19937_64 rng{std::random_device{}()};
        std::uint32_t id = 100;
        for (const auto& [relative, info] : result.files) {
            if (cancel && cancel->load()) {
                result.error = "Recovery scan cancelled";
                return result;
            }
            if (relative.generic_string().rfind("iPod_Control/Music/", 0) != 0 ||
                !fs::is_regular_file(device.mountPoint / relative)) continue;
            if (relative.filename() == ".DS_Store") continue;
            if (!isSupportedAudioFile(relative)) {
                result.skipped.push_back(relative.string() + ": unsupported format");
                continue;
            }
            auto meta = readFileMetadata(device.mountPoint / relative);
            if (!meta.ok || meta.track.lengthMs == 0 || info.size > UINT32_MAX) {
                result.skipped.push_back(relative.string() + ": unreadable or invalid audio");
                continue;
            }
            auto& track = meta.track;
            track.id = id++;
            track.dbid = rng();
            if (!track.dbid) track.dbid = track.id;
            track.location = ":" + relative.generic_string();
            std::replace(track.location.begin(), track.location.end(), '/', ':');
            result.musicBytes += info.size;
            result.library.tracks.push_back(std::move(track));
        }
        if (snapshot(device.mountPoint) != result.files)
            result.error = "The iPod changed during the scan. Wait for other syncing to finish and scan again.";
        else if (result.library.tracks.empty())
            result.error = "No readable music files were found. The iPod was left unchanged.";
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

bool installIpodRecovery(const IpodRecovery& recovery, fs::path* archive, std::string* error) {
    std::string localError;
    if (!error) error = &localError;
    *error = recovery.error;
    if (!error->empty()) return false;
    *error = ipodRecoveryBlockReason(recovery.device);
    if (!error->empty()) return false;
    const fs::path mount = recovery.device.mountPoint;
    const fs::path dir = mount / "iPod_Control" / "iTunes";
    const fs::path db = dir / "iTunesDB";
    fs::path staging;
    bool movedCounts = false;
    try {
        const auto identity = volumeIdentity(mount);
        if (std::uint64_t(identity.st_dev) != recovery.volumeDevice ||
            std::uint64_t(identity.st_ino) != recovery.volumeInode ||
            snapshot(mount) != recovery.files)
            throw std::runtime_error("The iPod changed since the preview. Scan again before recovering.");
        if (recovery.library.tracks.empty() || recovery.library.hashingScheme != kChecksumNone ||
            recovery.library.compressed)
            throw std::runtime_error("The recovery preview is not a usable video library");
        // Creating a new directory claims a unique archive without replacing
        // any older recovery or database backup.
        std::mt19937_64 rng{std::random_device{}()};
        do { staging = dir / ("PodBox Recovery " + std::to_string(rng())); }
        while (!fs::create_directory(staging));
        if (present(db)) {
            fs::copy_file(db, staging / "iTunesDB.original");
            if (!flushFile(staging / "iTunesDB.original", error))
                throw std::runtime_error(*error);
        }
        const fs::path candidate = staging / "iTunesDB.rebuilt";
        if (!writeItunesDb(recovery.library, candidate, error) || !flushFile(candidate, error))
            throw std::runtime_error(*error);
        const auto parsed = parseItunesDb(candidate);
        if (!parsed.library || parsed.library->tracks.size() != recovery.library.tracks.size())
            throw std::runtime_error("The rebuilt database did not pass validation");
        for (std::size_t i = 0; i < parsed.library->tracks.size(); ++i)
            if (parsed.library->tracks[i].location != recovery.library.tracks[i].location ||
                parsed.library->tracks[i].title != recovery.library.tracks[i].title)
                throw std::runtime_error("The rebuilt song list did not pass validation");
        // Staging changed only the iTunes directory timestamp. Any other
        // difference means another writer or a disconnect invalidated the scan.
        auto current = snapshot(mount, staging);
        const fs::path itunesRelative = fs::path("iPod_Control") / "iTunes";
        current[itunesRelative] = recovery.files.at(itunesRelative);
        const auto latestIdentity = volumeIdentity(mount);
        if (current != recovery.files ||
            std::uint64_t(latestIdentity.st_dev) != recovery.volumeDevice ||
            std::uint64_t(latestIdentity.st_ino) != recovery.volumeInode)
            throw std::runtime_error("The iPod changed while preparing recovery. Scan again.");
        // This permanent snapshot is offered by the normal Restore Database
        // sheet as well as preserving the very first successful index.
        const fs::path recoveredBackup = db.string() + ".podbox-recovered." + staging.filename().string().substr(16);
        fs::copy_file(candidate, recoveredBackup);
        if (!flushFile(recoveredBackup, error)) throw std::runtime_error(*error);
        // Counts use old track positions, so loading them against a rebuilt
        // list could give one song another's history. Archive them untouched.
        if (present(dir / "Play Counts")) {
            fs::rename(dir / "Play Counts", staging / "Play Counts");
            movedCounts = true;
        }
        fs::rename(candidate, db);
        if (archive) *archive = staging;
        return true;
    } catch (const std::exception& e) {
        *error = e.what();
        if (movedCounts) {
            std::error_code rollback;
            fs::rename(staging / "Play Counts", dir / "Play Counts", rollback);
            if (rollback) *error += "; old play counts remain in " + staging.string();
        }
        // Preserve staged data for inspection/retry; the original database
        // has not been removed and audio files have never been touched.
        return false;
    }
}
}  // namespace podbox
