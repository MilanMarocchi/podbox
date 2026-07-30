#include "library/applemusic.h"

#include "library/dedupe.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {
namespace {

// Control characters, so a song called "Tabs, Commas & Newlines" cannot
// corrupt the parse. 0x1e separates records, 0x1d separates field blocks.
constexpr char kRecSep = '\x1e';
constexpr char kBlockSep = '\x1d';

// Runs an AppleScript and returns its stdout. The script goes via a temp
// file so a multi-line program never has to survive shell quoting.
std::string runOsascript(const std::string& script, std::string* error) {
    const fs::path tmp =
        fs::temp_directory_path() / "podbox_applemusic.scpt";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) {
            if (error) *error = "Could not write a temporary script";
            return {};
        }
        std::fwrite(script.data(), 1, script.size(), f);
        std::fclose(f);
    }

    const std::string cmd = "osascript " + std::string("'") + tmp.string() +
                            "' 2>/dev/null";
    std::FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        if (error) *error = "Could not run osascript";
        return {};
    }
    std::string out;
    std::array<char, 65536> buf{};
    while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
        out.append(buf.data(), n);
    const int rc = pclose(pipe);

    std::error_code ec;
    fs::remove(tmp, ec);

    if (rc != 0 && out.empty() && error)
        *error =
            "Music.app did not respond. If macOS asked for permission to "
            "control Music, allow it and try again.";
    return out;
}

std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::uint32_t toU32(const std::string& s) {
    return std::uint32_t(std::strtoul(s.c_str(), nullptr, 10));
}

double toDouble(const std::string& s) {
    return std::strtod(s.c_str(), nullptr);
}

std::string stripEol(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string sanitizeComponent(const std::string& s) {
    std::string out;
    for (char c : s) {
        // '/' and ':' both act as path separators on macOS depending on the
        // API, and control characters have no business in a filename.
        if (c == '/' || c == ':' || static_cast<unsigned char>(c) < 0x20)
            out += '_';
        else
            out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out.empty() ? "Unknown" : out;
}

}  // namespace

bool appleMusicAvailable() {
    std::error_code ec;
    return fs::exists("/System/Applications/Music.app", ec) ||
           fs::exists("/Applications/Music.app", ec);
}

fs::path appleMusicCopyRoot() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Music" / "PodBox";
}

AppleMusicRead readAppleMusicLibrary(int limit) {
    AppleMusicRead out;
    if (!appleMusicAvailable()) {
        out.error = "Music.app is not installed on this Mac";
        return out;
    }

    // Every property is fetched as one whole list and joined in one go.
    // Looping per track in AppleScript is orders of magnitude slower — it
    // times out outright on a library this size.
    std::ostringstream s;
    s << "on j(l)\n"
      << "  set d to AppleScript's text item delimiters\n"
      << "  set AppleScript's text item delimiters to (ASCII character 30)\n"
      << "  set t to l as text\n"
      << "  set AppleScript's text item delimiters to d\n"
      << "  return t\n"
      << "end j\n"
      << "tell application \"Music\"\n"
      << "  set b to (ASCII character 29)\n"
      // Each property is asked for as "<prop> of every file track". Binding
      // the tracks to a variable first would make it a plain list, and a list
      // has no properties to read.
      << "  set o to my j(name of every file track) & b\n"
      << "  set o to o & my j(artist of every file track) & b\n"
      << "  set o to o & my j(album of every file track) & b\n"
      << "  set o to o & my j(genre of every file track) & b\n"
      << "  set o to o & my j(year of every file track) & b\n"
      << "  set o to o & my j(track number of every file track) & b\n"
      << "  set o to o & my j(disc number of every file track) & b\n"
      << "  set o to o & my j(duration of every file track) & b\n"
      << "  set o to o & my j(played count of every file track) & b\n"
      << "  set o to o & my j(rating of every file track) & b\n"
      << "  set o to o & my j(bit rate of every file track) & b\n"
      << "  set o to o & my j(sample rate of every file track) & b\n"
      << "  set o to o & my j(size of every file track)\n"
      << "  set locs to location of every file track\n"
      << "end tell\n"
      << "set p to \"\"\n"
      << "set r to (ASCII character 30)\n"
      << "repeat with l in locs\n"
      << "  try\n"
      << "    set p to p & (POSIX path of l) & r\n"
      << "  on error\n"
      << "    set p to p & r\n"
      << "  end try\n"
      << "end repeat\n"
      << "return o & (ASCII character 29) & p\n";

    const std::string raw = runOsascript(s.str(), &out.error);
    if (raw.empty()) {
        if (out.error.empty()) out.error = "Apple Music returned nothing";
        return out;
    }

    const std::vector<std::string> blocks = splitOn(raw, kBlockSep);
    constexpr int kExpectedBlocks = 14;  // 13 properties plus the paths
    if (int(blocks.size()) < kExpectedBlocks) {
        out.error = "Unexpected reply from Music.app";
        return out;
    }

    std::vector<std::vector<std::string>> f;
    f.reserve(kExpectedBlocks);
    // AppleScript joins N values with N-1 separators, so a trailing separator
    // means the last value is genuinely empty (a song with no artist tag) and
    // must be kept. Trimming it silently loses a track.
    for (int i = 0; i < kExpectedBlocks - 1; ++i)
        f.push_back(splitOn(stripEol(blocks[i]), kRecSep));
    {
        // The path block is built by appending a separator after every entry,
        // so it really does carry one extra.
        std::string paths = stripEol(blocks[kExpectedBlocks - 1]);
        if (!paths.empty() && paths.back() == kRecSep) paths.pop_back();
        f.push_back(splitOn(paths, kRecSep));
    }

    const std::size_t n = f[0].size();
    for (const auto& col : f)
        if (col.size() != n) {
            out.error = "Music.app returned mismatched track lists";
            return out;
        }

    std::error_code ec;
    out.tracks.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (limit > 0 && int(out.tracks.size()) >= limit) break;

        AppleMusicTrack t;
        t.meta.title = f[0][i];
        t.meta.artist = f[1][i];
        t.meta.album = f[2][i];
        t.meta.genre = f[3][i];
        t.meta.year = toU32(f[4][i]);
        t.meta.trackNumber = toU32(f[5][i]);
        t.meta.discNumber = toU32(f[6][i]);
        t.meta.lengthMs = std::uint32_t(toDouble(f[7][i]) * 1000.0);
        t.meta.playCount = toU32(f[8][i]);
        t.meta.rating = std::uint8_t(toU32(f[9][i]));
        t.meta.bitrate = toU32(f[10][i]);
        t.meta.sampleRate = toU32(f[11][i]);
        t.meta.sizeBytes = toU32(f[12][i]);

        const std::string& path = f[13][i];
        if (path.empty()) {
            ++out.streamingOnly;
            continue;
        }
        t.file = path;
        if (toLower(t.file.extension().string()) == ".m4p") {
            ++out.drmProtected;  // FairPlay: unplayable on a classic iPod
            continue;
        }
        if (!fs::exists(t.file, ec)) {
            ++out.fileMissing;
            continue;
        }
        t.meta.location = t.file.string();
        out.tracks.push_back(std::move(t));
    }

    out.ok = true;
    return out;
}

CopyResult copyAppleMusicFiles(std::vector<AppleMusicTrack>& tracks,
                               const fs::path& root,
                               const CopyProgress& progress) {
    CopyResult res;
    std::error_code ec;
    fs::create_directories(root, ec);

    // Destinations handed out in this run, so two tracks can never be given
    // the same path.
    std::unordered_set<std::string> claimed;
    int done = 0;
    for (AppleMusicTrack& t : tracks) {
        if (progress && !progress(done, int(tracks.size()), t.meta.title)) {
            res.cancelled = true;
            break;
        }
        ++done;
        if (t.file.empty()) continue;

        const std::string artist = sanitizeComponent(
            t.meta.artist.empty() ? "Unknown Artist" : t.meta.artist);
        const std::string album = sanitizeComponent(
            t.meta.album.empty() ? "Unknown Album" : t.meta.album);

        // The disc number has to be in the name. Without it every track 1 of
        // a multi-disc album lands on the same path and the later one wins,
        // silently destroying the earlier.
        char prefix[16] = "";
        if (t.meta.discNumber > 1)
            std::snprintf(prefix, sizeof(prefix), "%u-%02u ", t.meta.discNumber,
                          t.meta.trackNumber);
        else if (t.meta.trackNumber > 0)
            std::snprintf(prefix, sizeof(prefix), "%02u ", t.meta.trackNumber);

        const std::string stem = prefix + sanitizeComponent(t.meta.title);
        const std::string ext = toLower(t.file.extension().string());

        const fs::path dir = root / artist / album;
        fs::create_directories(dir, ec);

        // Two genuinely different songs can still want the same name. Give
        // the later one a suffix rather than letting it overwrite.
        //
        // Compared case-insensitively, because macOS volumes normally are:
        // "Bring Me The Horizon" and "Bring Me the Horizon" are two strings
        // but one directory, and treating them as distinct silently
        // overwrites one song with the other.
        fs::path dest = dir / (stem + ext);
        for (int n = 2; claimed.count(toLower(dest.string())) && n < 1000; ++n)
            dest = dir / (stem + " (" + std::to_string(n) + ")" + ext);
        claimed.insert(toLower(dest.string()));

        // Resumable: a file already there at the right size is left alone, so
        // re-running after a cancel does not re-copy 13 GB.
        const std::uintmax_t srcSize = fs::file_size(t.file, ec);
        if (!ec && fs::exists(dest, ec) &&
            fs::file_size(dest, ec) == srcSize) {
            t.file = dest;
            t.meta.location = dest.string();
            ++res.skipped;
            continue;
        }

        fs::copy_file(t.file, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            ++res.failed;
            ec.clear();
            continue;
        }
        t.file = dest;
        t.meta.location = dest.string();
        ++res.copied;
        res.bytes += srcSize;
    }
    return res;
}

}  // namespace podbox
