#include "library/music_folder.h"

#include "library/dedupe.h"  // toLower

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <copyfile.h>
#endif

namespace fs = std::filesystem;

namespace podbox {
namespace {

// A clone where the filesystem can (APFS, same volume), a plain copy where
// it cannot. `dst` must not exist.
bool cloneOrCopy(const fs::path& src, const fs::path& dst, std::string* error) {
#if defined(__APPLE__)
    if (copyfile(src.c_str(), dst.c_str(), nullptr, COPYFILE_CLONE) == 0)
        return true;
    *error = src.filename().string() + ": " + std::strerror(errno);
    return false;
#else
    std::error_code ec;
    if (fs::copy_file(src, dst, ec)) return true;
    *error = src.filename().string() + ": " + ec.message();
    return false;
#endif
}

}  // namespace

fs::path defaultMusicFolder() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Music" / "PodBox";
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

bool placeInMusicFolder(const fs::path& src, const Track& meta,
                        const fs::path& root, fs::path* dest, bool* reused,
                        std::string* error) {
    std::error_code ec;
    // COPYFILE_CLONE would copy a symlink as a symlink; the library wants
    // the audio it points at.
    const fs::path real = fs::canonical(src, ec);
    if (ec) {
        *error = src.filename().string() + ": " + ec.message();
        return false;
    }
    const std::uintmax_t size = fs::file_size(real, ec);
    if (ec) {
        *error = src.filename().string() + ": " + ec.message();
        return false;
    }

    const fs::path dir =
        root / sanitizeComponent(meta.artist.empty() ? "Unknown Artist" : meta.artist) /
        sanitizeComponent(meta.album.empty() ? "Unknown Album" : meta.album);
    fs::create_directories(dir, ec);
    if (ec) {
        *error = dir.string() + ": " + ec.message();
        return false;
    }

    // The disc number has to be in the name. Without it every track 1 of a
    // multi-disc album wants the same path.
    char prefix[16] = "";
    if (meta.discNumber > 1)
        std::snprintf(prefix, sizeof(prefix), "%u-%02u ", meta.discNumber,
                      meta.trackNumber);
    else if (meta.trackNumber > 0)
        std::snprintf(prefix, sizeof(prefix), "%02u ", meta.trackNumber);
    const std::string title =
        meta.title.empty() ? src.stem().string() : meta.title;
    const std::string stem = prefix + sanitizeComponent(title);
    const std::string ext = toLower(src.extension().string());

    for (int n = 1; n < 1000; ++n) {
        const fs::path candidate =
            dir / (n == 1 ? stem + ext
                          : stem + " (" + std::to_string(n) + ")" + ext);
        std::error_code e;
        if (fs::exists(candidate, e)) {
            if (fs::file_size(candidate, e) == size && !e) {
                *dest = candidate;
                *reused = true;
                return true;
            }
            continue;
        }
        if (e) break;

        const fs::path tmp = candidate.string() + ".podbox-partial";
        fs::remove(tmp, e);
        if (!cloneOrCopy(real, tmp, error)) {
            fs::remove(tmp, e);
            return false;
        }
        fs::rename(tmp, candidate, e);
        if (e) {
            *error = candidate.filename().string() + ": " + e.message();
            fs::remove(tmp, e);
            return false;
        }
        *dest = candidate;
        *reused = false;
        return true;
    }
    *error = "no free name for " + stem + ext;
    return false;
}

void pruneEmptyFolders(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) dirs.push_back(it->path());
    }
    // Deepest first, so an album emptied out can take its artist with it.
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    for (const fs::path& d : dirs) {
        bool onlyJunk = true;
        for (const auto& entry : fs::directory_iterator(d, ec)) {
            if (entry.path().filename() != ".DS_Store") {
                onlyJunk = false;
                break;
            }
        }
        if (ec || !onlyJunk) {
            ec.clear();
            continue;
        }
        fs::remove(d / ".DS_Store", ec);
        fs::remove(d, ec);  // only ever removes an empty folder
        ec.clear();
    }
}

}  // namespace podbox
