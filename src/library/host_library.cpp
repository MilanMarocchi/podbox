#include "library/host_library.h"

#include "library/dedupe.h"
#include "library/metadata.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr const char* kMagic = "podbox-library 1";

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

}  // namespace

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

bool HostLibrary::load() {
    tracks_.clear();
    watch_.clear();
    nextId_ = 1;

    std::ifstream in(libraryPath());
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line.rfind(kMagic, 0) != 0) return false;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = split(line);
        if (f[0] == "W" && f.size() >= 3) {
            WatchFolder w;
            w.enabled = f[1] == "1";
            w.path = unescape(f[2]);
            watch_.push_back(std::move(w));
        } else if (f[0] == "T" && f.size() >= 25) {
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
            t.meta.mediaType = toU32(f[23]);
            t.meta.dateAdded = toI64(f[24]);
            // The dedupe key looks at the extension via meta.location.
            t.meta.location = t.file.string();
            nextId_ = std::max(nextId_, t.id + 1);
            tracks_.push_back(std::move(t));
        }
    }
    return true;
}

bool HostLibrary::save() const {
    std::error_code ec;
    fs::create_directories(configDir(), ec);

    const fs::path path = libraryPath();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << kMagic << '\n';
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
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
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
    std::erase_if(tracks_, [](const HostTrack& t) { return t.missing; });
    return int(before - tracks_.size());
}

bool HostLibrary::upsert(const fs::path& file, const Track& meta,
                         const std::string& origin,
                         const AudioFingerprint& fp) {
    std::error_code ec;
    const std::int64_t mtime = fileMtime(file);
    const std::uint64_t size = std::uint64_t(fs::file_size(file, ec));

    for (HostTrack& t : tracks_) {
        if (t.file != file) continue;
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

ScanStats HostLibrary::rescan(bool fingerprintFiles, const bool* cancelled,
                              std::string* currentFile) {
    ScanStats stats;
    std::error_code ec;

    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(tracks_.size());
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        byPath[tracks_[i].file.string()] = i;

    std::vector<bool> seen(tracks_.size(), false);

    for (const WatchFolder& w : watch_) {
        if (!w.enabled) continue;
        // A watch folder on an unmounted drive must not mark its whole
        // contents missing; treat it as simply unavailable this time.
        if (!fs::is_directory(w.path, ec)) continue;

        for (auto it = fs::recursive_directory_iterator(
                 w.path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (cancelled && *cancelled) return stats;

            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                if (isPartialFolder(p.filename().string())) {
                    it.disable_recursion_pending();
                    ++stats.skippedPartial;
                }
                continue;
            }
            if (!it->is_regular_file(ec) || !isImportableAudioFile(p)) continue;

            const std::string key = p.string();
            const std::int64_t mtime = fileMtime(p);
            const std::uint64_t size = std::uint64_t(fs::file_size(p, ec));
            if (ec) {
                ++stats.failed;
                continue;
            }

            const auto found = byPath.find(key);
            if (found != byPath.end()) {
                HostTrack& existing = tracks_[found->second];
                seen[found->second] = true;
                existing.missing = false;
                if (existing.mtime == mtime && existing.size == size) {
                    ++stats.unchanged;
                    continue;  // untouched since last time
                }
            }

            if (currentFile) *currentFile = p.filename().string();
            FileMeta meta = readFileMetadata(p);
            if (!meta.ok) {
                ++stats.failed;
                continue;
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
                t.id = tracks_[found->second].id;
                tracks_[found->second] = std::move(t);
                ++stats.updated;
            } else {
                t.id = nextId_++;
                byPath[key] = tracks_.size();
                seen.push_back(true);
                tracks_.push_back(std::move(t));
                ++stats.added;
            }
        }
    }

    // Anything indexed from a watch folder we scanned but did not see again
    // has gone. Flagged rather than deleted — the file may just be on a drive
    // that was not mounted, and losing play counts to a stale mount is worse.
    for (std::size_t i = 0; i < tracks_.size() && i < seen.size(); ++i) {
        if (seen[i] || tracks_[i].origin != "watch") continue;
        bool underScanned = false;
        for (const WatchFolder& w : watch_) {
            if (!w.enabled || !fs::is_directory(w.path, ec)) continue;
            const std::string root = w.path.string();
            if (tracks_[i].file.string().rfind(root, 0) == 0) {
                underScanned = true;
                break;
            }
        }
        if (!underScanned) continue;
        if (!tracks_[i].missing) ++stats.missing;
        tracks_[i].missing = true;
    }

    return stats;
}

}  // namespace podbox
