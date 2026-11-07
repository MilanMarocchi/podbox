#include "library/host_library.h"

#include "library/dedupe.h"
#include "library/metadata.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr const char* kMagic = "podbox-library 1";

// Columns a "T" row must have to be worth reading. Anything beyond this is
// read when present and defaulted when not. Malformed rows reject the whole
// load rather than silently turning a partial parse into the next saved index.
constexpr std::size_t kMinTrackFields = 23;

// Values are tab-separated, so anything that could break a line gets escaped.
// Paths with tabs are vanishingly rare, but a library that silently corrupts
// itself on one is not worth shipping.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case '\\': out += '\\'; break;
            default: out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

std::uint64_t toU64(const std::string& s) {
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}
std::int64_t toI64(const std::string& s) {
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}
std::uint32_t toU32(const std::string& s) { return std::uint32_t(toU64(s)); }

std::int64_t fileMtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::int64_t(t.time_since_epoch().count());
}

std::string fileIdentity(const fs::path& p) {
    struct stat info{};
    if (::stat(p.c_str(), &info) != 0) return {};
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
}

std::uint64_t indexChecksum(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}

}  // namespace

bool isWithinFolder(const fs::path& file, const fs::path& root) {
    fs::path base = root.lexically_normal();
    // "/a/b/" normalises with an empty final element, which would make every
    // relative path start with "..".
    if (!base.has_filename() && base.has_parent_path() &&
        base != base.root_path())
        base = base.parent_path();
    const fs::path rel = file.lexically_normal().lexically_relative(base);
    if (rel.empty() || rel == ".") return false;
    return *rel.begin() != "..";
}

fs::path HostLibrary::configDir() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Application Support" /
           "PodBox";
}

fs::path HostLibrary::libraryPath() { return configDir() / "library.tsv"; }

bool HostLibrary::isPartialFolder(const std::string& name) {
    const std::string lower = toLower(name);
    return lower == "downloading" || lower == "incomplete" ||
           lower == "incomplete downloads" || lower == ".partial";
}

void HostLibrary::addWatchFolder(const fs::path& p) {
    for (const WatchFolder& w : watch_)
        if (w.path == p) return;
    watch_.push_back({p, true});
}

void HostLibrary::removeWatchFolder(std::size_t index) {
    if (index < watch_.size()) watch_.erase(watch_.begin() + index);
}

void HostLibrary::setWatchFolderEnabled(std::size_t index, bool enabled) {
    if (index < watch_.size()) watch_[index].enabled = enabled;
}

void HostLibrary::seedDefaultWatchFolders() {
    if (!watch_.empty()) return;
    const char* home = std::getenv("HOME");
    if (!home) return;

    // Where music tends to land. Only a folder that exists gets added, so this
    // quietly does nothing on a machine without one.
    static const char* kCandidates[] = {
        "Soulseek Downloads/complete",
        "Soulseek Downloads",
        "Nicotine Downloads",
    };
    std::error_code ec;
    for (const char* rel : kCandidates) {
        const fs::path p = fs::path(home) / rel;
        if (fs::is_directory(p, ec)) {
            addWatchFolder(p);
            return;  // the most specific match is enough
        }
    }
}

bool HostLibrary::load() { return load(libraryPath()); }

bool HostLibrary::load(const fs::path& path) {
    HostLibrary parsed;
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) throw std::runtime_error("Cannot inspect local library: " + ec.message());
    if (!exists) {
        parsed.persistence_->path = path;
        parsed.persistence_->observed = true;
        parsed.musicFolder_ = musicFolder_;
        *this = std::move(parsed);
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read local library: " + path.string());
    std::ostringstream raw;
    raw << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Could not finish reading local library");
    const std::string contents = raw.str();
    std::istringstream in(contents);
    auto invalid = [&] { throw std::runtime_error("Damaged local library; original index preserved at " + path.string()); };
    std::string line;
    if (!std::getline(in, line)) invalid();
    const bool checked = line == std::string(kMagic) + "\tchecked";
    if (line != kMagic && !checked) invalid();
    bool footer = false;
    std::unordered_set<std::uint64_t> ids;
    auto number = [&](const std::string& value) {
        std::uint64_t n = 0;
        if (value.empty()) return true; // legacy optional fields
        const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        return error == std::errc{} && last == value.data() + value.size();
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (footer) invalid();
        const std::vector<std::string> f = split(line);
        if (f[0] == "C") {
            const auto footerAt = contents.rfind("\nC\t");
            if (!checked || f.size() != 5 || !number(f[1]) || !number(f[2]) || !number(f[3]) ||
                !number(f[4]) || footerAt == std::string::npos ||
                indexChecksum(std::string_view(contents).substr(0, footerAt + 1)) != toU64(f[4]) ||
                toU64(f[1]) != parsed.tracks_.size() || toU64(f[2]) != parsed.imported_.size() ||
                toU64(f[3]) != parsed.watch_.size()) invalid();
            footer = true;
        } else if (f[0] == "W" && f.size() >= 3) {
            if ((f[1] != "0" && f[1] != "1") || f[2].empty()) invalid();
            WatchFolder w;
            w.enabled = f[1] == "1";
            w.path = unescape(f[2]);
            parsed.watch_.push_back(std::move(w));
        } else if (f[0] == "I" && f.size() >= 2) {
            // An import folder file already copied in. An older PodBox
            // skips these rows unread.
            parsed.imported_.insert(unescape(f[1]));
        } else if (f[0] == "T" && f.size() >= kMinTrackFields) {
            for (std::size_t col : {1, 3, 4, 5, 14, 15, 16, 17, 18, 19, 20, 21, 22})
                if (!number(f[col])) invalid();
            std::int64_t mtime;
            const auto [last, error] = std::from_chars(f[2].data(), f[2].data() + f[2].size(), mtime);
            if (error != std::errc{} || last != f[2].data() + f[2].size() ||
                toU64(f[1]) == 0 || toU64(f[1]) >= std::numeric_limits<std::uint32_t>::max() ||
                !ids.insert(toU64(f[1])).second || f[8].empty() ||
                (f[7] != "0" && f[7] != "1")) invalid();
            // Read by position, tolerating a longer row than we know about:
            // a file written by a newer PodBox keeps working here, minus
            // whatever the extra columns held. Anything shorter than the
            // original layout rejects the load.
            auto at = [&f](std::size_t i) -> std::string {
                return i < f.size() ? f[i] : std::string();
            };
            HostTrack t;
            t.id = toU64(f[1]);
            t.mtime = toI64(f[2]);
            t.size = toU64(f[3]);
            t.fp.bytes = toU64(f[4]);
            t.fp.hash = toU64(f[5]);
            t.origin = unescape(f[6]);
            t.missing = f[7] == "1";
            t.file = unescape(f[8]);
            t.meta.title = unescape(f[9]);
            t.meta.artist = unescape(f[10]);
            t.meta.album = unescape(f[11]);
            t.meta.genre = unescape(f[12]);
            t.meta.composer = unescape(f[13]);
            t.meta.lengthMs = toU32(f[14]);
            t.meta.sizeBytes = toU32(f[15]);
            t.meta.trackNumber = toU32(f[16]);
            t.meta.discNumber = toU32(f[17]);
            t.meta.year = toU32(f[18]);
            t.meta.bitrate = toU32(f[19]);
            t.meta.sampleRate = toU32(f[20]);
            t.meta.playCount = toU32(f[21]);
            t.meta.rating = std::uint8_t(toU32(f[22]));
            t.meta.mediaType = toU32(at(23));
            t.meta.dateAdded = toI64(at(24));
            // The dedupe key looks at the extension via meta.location.
            t.meta.location = t.file.string();
            parsed.nextId_ = std::max(parsed.nextId_, t.id + 1);
            parsed.tracks_.push_back(std::move(t));
        } else {
            invalid();
        }
    }
    if (checked && !footer) invalid();
    parsed.persistence_->path = path;
    parsed.persistence_->contents = contents;
    parsed.persistence_->observed = true;
    parsed.musicFolder_ = musicFolder_;
    *this = std::move(parsed);
    return true;
}

bool HostLibrary::save() const { return save(libraryPath()); }

bool HostLibrary::save(const fs::path& path) const {
    std::lock_guard<std::mutex> guard(persistence_->mutex);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    // A separate lock survives atomic replacement of the index itself.
    const int lockFd = ::open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lockFd < 0) return false;
    struct Close { int fd; ~Close() { ::close(fd); } } closeLock{lockFd};
    if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) return false;
    const bool exists = fs::exists(path, ec);
    if (ec) return false;
    std::optional<std::string> current;
    if (exists) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::ostringstream data; data << input.rdbuf();
        if (input.bad()) return false;
        current = data.str();
    }
    // A failed load, or another writer, can never turn into an empty save.
    if (persistence_->observed) {
        if (persistence_->path != path || persistence_->contents != current) return false;
    } else if (exists) return false;
    std::ostringstream out;
        out << kMagic << "\tchecked\n";
        for (const WatchFolder& w : watch_)
            out << "W\t" << (w.enabled ? 1 : 0) << '\t'
                << escape(w.path.string()) << '\n';
        for (const HostTrack& t : tracks_) {
            out << "T\t" << t.id << '\t' << t.mtime << '\t' << t.size << '\t'
                << t.fp.bytes << '\t' << t.fp.hash << '\t' << escape(t.origin)
                << '\t' << (t.missing ? 1 : 0) << '\t'
                << escape(t.file.string()) << '\t' << escape(t.meta.title)
                << '\t' << escape(t.meta.artist) << '\t'
                << escape(t.meta.album) << '\t' << escape(t.meta.genre) << '\t'
                << escape(t.meta.composer) << '\t' << t.meta.lengthMs << '\t'
                << t.meta.sizeBytes << '\t' << t.meta.trackNumber << '\t'
                << t.meta.discNumber << '\t' << t.meta.year << '\t'
                << t.meta.bitrate << '\t' << t.meta.sampleRate << '\t'
                << t.meta.playCount << '\t' << unsigned(t.meta.rating) << '\t'
                << t.meta.mediaType << '\t' << t.meta.dateAdded << '\n';
        }
        for (const std::string& path : imported_)
            out << "I\t" << escape(path) << '\n';
    const auto checksum = indexChecksum(out.str());
    out << "C\t" << tracks_.size() << '\t' << imported_.size() << '\t' << watch_.size() << '\t' << checksum << '\n';
    const std::string data = out.str();
    if (current && *current == data) return true;

    auto durableWrite = [](const fs::path& target, const std::string& bytes) {
        std::string name = target.string() + ".tmp.XXXXXX";
        std::vector<char> pattern(name.begin(), name.end()); pattern.push_back(0);
        const int fd = ::mkstemp(pattern.data());
        if (fd < 0) return false;
        bool ok = true;
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            const auto n = ::write(fd, bytes.data() + pos, bytes.size() - pos);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            pos += std::size_t(n);
        }
        if (ok) ok = ::fsync(fd) == 0;
        if (::close(fd) != 0) ok = false;
        std::error_code err;
        if (ok) { fs::rename(pattern.data(), target, err); ok = !err; }
        if (!ok) fs::remove(pattern.data(), err);
        return ok;
    };
    // Install each backup atomically; a failed rotation leaves the live
    // index untouched. At least the previous five successful revisions remain.
    if (current) {
        for (int i = 4; i >= 1; --i) {
            const fs::path prev = path.string() + ".bak." + std::to_string(i);
            if (!fs::exists(prev, ec)) { if (ec) return false; continue; }
            const fs::path next = path.string() + ".bak." + std::to_string(i + 1);
            fs::rename(prev, next, ec);
            if (ec) return false;
        }
        if (!durableWrite(path.string() + ".bak.1", *current)) return false;
    }
    if (!durableWrite(path, data)) return false;
    persistence_->path = path;
    persistence_->contents = data;
    persistence_->observed = true;
    const int dirFd = ::open(path.parent_path().c_str(), O_RDONLY);
    if (dirFd >= 0) { ::fsync(dirFd); ::close(dirFd); }
    return true;
}

int HostLibrary::refreshMissing() {
    std::error_code ec;
    int missing = 0;
    for (HostTrack& t : tracks_) {
        t.missing = !fs::exists(t.file, ec);
        if (t.missing) ++missing;
    }
    return missing;
}

int HostLibrary::removeMissing() {
    const std::size_t before = tracks_.size();
    std::erase_if(tracks_, [](const HostTrack& t) {
        std::error_code ec;
        return t.missing && !fs::exists(t.file, ec) && !ec;
    });
    return int(before - tracks_.size());
}

bool HostLibrary::upsert(const fs::path& file, const Track& meta,
                         const std::string& origin,
                         const AudioFingerprint& fp) {
    std::error_code ec;
    const std::int64_t mtime = fileMtime(file);
    const std::uint64_t size = std::uint64_t(fs::file_size(file, ec));
    const auto identity = fileIdentity(file);

    for (HostTrack& t : tracks_) {
        if (t.file != file && (identity.empty() || fileIdentity(t.file) != identity)) continue;
        t.meta = meta;
        t.meta.location = file.string();
        t.origin = origin;
        t.mtime = mtime;
        t.size = ec ? t.size : size;
        if (fp.ok()) t.fp = fp;
        t.missing = false;
        return false;
    }

    HostTrack t;
    t.id = nextId_++;
    t.file = file;
    t.meta = meta;
    t.meta.location = file.string();
    t.origin = origin;
    t.mtime = mtime;
    t.size = ec ? 0 : size;
    t.fp = fp;
    tracks_.push_back(std::move(t));
    return true;
}

namespace {

// Walks `dir` for audio files, skipping download-in-progress folders and,
// when given, the folder `skip` (the library, should an import folder
// contain it). Returns false when cancelled.
template <class OnFile>
bool forEachAudioFile(const fs::path& dir, const fs::path& skip,
                      const std::atomic<bool>* cancelled, ScanStats& stats,
                      OnFile onFile) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (cancelled && cancelled->load()) return false;
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            if (HostLibrary::isPartialFolder(p.filename().string())) {
                it.disable_recursion_pending();
                ++stats.skippedPartial;
            } else if (!skip.empty() && fs::equivalent(p, skip, ec)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || !isImportableAudioFile(p)) continue;
        onFile(p);
    }
    return true;
}

}  // namespace

ScanStats HostLibrary::rescan(bool fingerprintFiles, const std::atomic<bool>* cancelled,
                              std::string* currentFile) {
    ScanStats stats;
    std::error_code ec;
    const fs::path& root = musicFolder_;
    fs::create_directories(root, ec);

    std::unordered_map<std::string, std::size_t> byPath;
    std::unordered_map<std::string, std::size_t> byFile;
    byPath.reserve(tracks_.size());
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        byPath[tracks_[i].file.string()] = i;
        const auto identity = fileIdentity(tracks_[i].file);
        if (!identity.empty()) byFile.emplace(identity, i);
    }
    std::vector<bool> seen(tracks_.size(), false);

    auto addTrack = [&](HostTrack t) {
        t.id = nextId_++;
        byPath[t.file.string()] = tracks_.size();
        const auto identity = fileIdentity(t.file);
        if (!identity.empty()) byFile[identity] = tracks_.size();
        seen.push_back(true);
        tracks_.push_back(std::move(t));
    };

    // 1. The library itself: every audio file in the music folder.
    const bool finished = forEachAudioFile(root, {}, cancelled, stats, [&](const fs::path& p) {
        const std::string key = p.string();
        const std::int64_t mtime = fileMtime(p);
        std::error_code e;
        const std::uint64_t size = std::uint64_t(fs::file_size(p, e));
        if (e) {
            ++stats.failed;
            return;
        }
        // APFS may return a different case/Unicode spelling from the one
        // originally imported. Match the actual file before inventing an id.
        if (!byPath.count(key)) {
            const auto same = byFile.find(fileIdentity(p));
            if (same != byFile.end()) byPath[key] = same->second;
        }
        const auto found = byPath.find(key);
        if (found != byPath.end()) {
            HostTrack& existing = tracks_[found->second];
            existing.file = p;
            existing.meta.location = key;
            seen[found->second] = true;
            existing.missing = false;
            if (existing.mtime == mtime && existing.size == size) {
                const auto ext = toLower(p.extension().string());
                if (fingerprintFiles && (ext == ".wav" || ext == ".aif" || ext == ".aiff"))
                    existing.fp = fingerprintFile(p); // migrate old whole-container fingerprints
                ++stats.unchanged;
                return;  // untouched since last time
            }
        }

        if (currentFile) *currentFile = p.filename().string();
        FileMeta meta = readFileMetadata(p);
        if (!meta.ok) {
            ++stats.failed;
            return;
        }
        HostTrack t;
        t.file = p;
        t.meta = std::move(meta.track);
        t.meta.location = key;  // gives the dedupe key its extension
        t.mtime = mtime;
        t.size = size;
        t.origin = "watch";
        if (fingerprintFiles) t.fp = fingerprintFile(p);

        if (found != byPath.end()) {
            // Retagged. The file has its tags; the library keeps its history.
            const HostTrack& old = tracks_[found->second];
            t.id = old.id;
            t.origin = old.origin;
            t.meta.playCount = std::max(t.meta.playCount, old.meta.playCount);
            if (old.meta.rating) t.meta.rating = old.meta.rating;
            if (old.meta.dateAdded) t.meta.dateAdded = old.meta.dateAdded;
            tracks_[found->second] = std::move(t);
            ++stats.updated;
        } else {
            addTrack(std::move(t));
            ++stats.added;
        }
    });
    if (!finished) return stats;

    // 2. Tracks still played from outside the library. Libraries from before
    // the music folder indexed their folders in place; each song is copied in
    // and repointed, so it keeps its play count and rating. Its original
    // counts as imported, so step 3 does not bring it in a second time.
    std::unordered_set<std::uint64_t> merged;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (cancelled && cancelled->load()) return stats;
        HostTrack& t = tracks_[i];
        if (t.missing || isWithinFolder(t.file, root)) continue;
        if (!fs::is_regular_file(t.file, ec)) continue;  // refreshMissing's job

        if (currentFile) *currentFile = t.file.filename().string();
        fs::path dest;
        bool reused = false;
        std::string error;
        if (!placeInMusicFolder(t.file, t.meta, root, &dest, &reused, &error)) {
            ++stats.failed;
            continue;
        }
        imported_.insert(t.file.string());
        const auto other = byPath.find(dest.string());
        if (other != byPath.end() && other->second != i) {
            // That file is already in the library: this was the same song
            // listed twice. Fold its history into the entry already there.
            HostTrack& o = tracks_[other->second];
            o.meta.playCount = std::max(o.meta.playCount, t.meta.playCount);
            if (!o.meta.rating) o.meta.rating = t.meta.rating;
            merged.insert(t.id);
            continue;
        }
        byPath.erase(t.file.string());
        t.file = dest;
        t.meta.location = dest.string();
        t.mtime = fileMtime(dest);
        t.size = std::uint64_t(fs::file_size(dest, ec));
        byPath[dest.string()] = i;
        seen[i] = true;
        ++stats.imported;
    }

    // 3. The import folders: anything not imported before is copied in.
    std::unordered_map<std::string, std::vector<std::size_t>> missingCopies;
    auto fpKey = [](const AudioFingerprint& fp) {
        return std::to_string(fp.bytes) + ":" + std::to_string(fp.hash);
    };
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const auto& t = tracks_[i];
        std::error_code e;
        if (t.fp.ok() && isWithinFolder(t.file, root) && !fs::exists(t.file, e) && !e)
            missingCopies[fpKey(t.fp)].push_back(i);
    }
    for (const WatchFolder& w : watch_) {
        if (!w.enabled) continue;
        // A folder on an unmounted drive is simply unavailable this time.
        if (!fs::is_directory(w.path, ec)) continue;
        // Partial folders are skipped wherever they turn up, including as the
        // import folder itself. "/x/downloading/" has an empty filename().
        const fs::path named = w.path.has_filename() ? w.path : w.path.parent_path();
        if (isPartialFolder(named.filename().string())) {
            ++stats.skippedPartial;
            continue;
        }
        if (isWithinFolder(w.path, root) || fs::equivalent(w.path, root, ec)) continue;
        ec.clear();

        const bool done = forEachAudioFile(w.path, root, cancelled, stats, [&](const fs::path& p) {
            const std::string key = p.string();
            if (imported_.count(key)) {
                // Only restore an entry that still belongs to the library.
                // Deliberately removed duplicates have no entry and stay gone.
                if (!missingCopies.empty()) {
                    const auto fp = fingerprintFile(p);
                    const auto match = missingCopies.find(fpKey(fp));
                    if (fp.ok() && match != missingCopies.end()) {
                        for (const auto i : match->second) {
                            auto& t = tracks_[i];
                            if (seen[i]) continue;
                            fs::path dest;
                            bool reused = false;
                            std::string error;
                            if (!readFileMetadata(p).ok || !placeInMusicFolder(
                                    p, t.meta, root, &dest, &reused, &error)) {
                                ++stats.failed;
                                continue;
                            }
                            byPath.erase(t.file.string());
                            t.file = dest;
                            t.meta.location = dest.string();
                            t.mtime = fileMtime(dest);
                            t.size = fs::file_size(dest);
                            t.missing = false;
                            byPath[dest.string()] = i;
                            seen[i] = true;
                            ++stats.restored;
                        }
                    }
                }
                ++stats.unchanged;
                return;
            }
            if (currentFile) *currentFile = p.filename().string();
            FileMeta meta = readFileMetadata(p);
            if (!meta.ok) {
                // Not marked imported, so it is tried again next time — a
                // download may simply not have finished.
                ++stats.failed;
                return;
            }
            fs::path dest;
            bool reused = false;
            std::string error;
            if (!placeInMusicFolder(p, meta.track, root, &dest, &reused, &error)) {
                ++stats.failed;
                return;
            }
            imported_.insert(key);
            if (byPath.count(dest.string())) return;  // already in the library

            HostTrack t;
            t.file = dest;
            t.meta = std::move(meta.track);
            t.meta.location = dest.string();
            t.mtime = fileMtime(dest);
            std::error_code e;
            t.size = std::uint64_t(fs::file_size(dest, e));
            t.origin = "import";
            if (fingerprintFiles) t.fp = fingerprintFile(dest);
            addTrack(std::move(t));
            ++stats.imported;
        });
        if (!done) break;
    }

    // 4. Library songs whose file has gone from the music folder. Flagged,
    // not dropped: the user decides, and play counts survive a mistake.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (seen[i] || !isWithinFolder(tracks_[i].file, root)) continue;
        std::error_code e;
        const bool present = fs::is_regular_file(tracks_[i].file, e);
        if (e && e != std::errc::no_such_file_or_directory) continue;
        if (!present && !tracks_[i].missing) ++stats.missing;
        tracks_[i].missing = !present;
    }

    if (!merged.empty())
        std::erase_if(tracks_, [&merged](const HostTrack& t) { return merged.count(t.id) > 0; });
    return stats;
}

}  // namespace podbox
