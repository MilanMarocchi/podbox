#include "device/filesystem_player.h"

#include "library/dedupe.h"
#include "library/metadata.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr const char* kManifestMagic = "podbox-filesystem-player 1";

fs::path manifestPath(const fs::path& mount) {
    return mount / ".podbox" / "filesystem-player.tsv";
}

// Purely lexical: every path here is built from `mount`. Resolving through
// the filesystem would return whichever capitalisation FAT's name cache holds
// ("MUSIC" vs "Music"), so a location would change with lookup history.
std::string relativeLocation(const fs::path& mount, const fs::path& path) {
    const fs::path relative =
        path.lexically_normal().lexically_relative(mount.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") return {};
    return relative.generic_string();
}

std::string pathKey(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    return toLower(fs::path(path).lexically_normal().generic_string());
}

std::string pathKey(const fs::path& path) {
    return pathKey(path.lexically_normal().generic_string());
}

bool playlistFile(const fs::path& path) {
    const std::string ext = toLower(path.extension().string());
    return ext == ".m3u" || ext == ".m3u8";
}

std::string safeComponent(std::string value, const char* fallback) {
    if (value.empty()) value = fallback;
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.'))
        value.pop_back();
    return value.empty() ? fallback : value;
}

void loadState(const fs::path& mount, FilesystemPlayerState* state) {
    state->managedTracks.clear();
    state->playlistLocations.clear();
    state->playlistContents.clear();
    state->playlistNames.clear();

    std::ifstream in(manifestPath(mount));
    std::string line;
    if (!in || !std::getline(in, line) || line != kManifestMagic) return;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        char kind = 0;
        row >> kind;
        if (kind == 'T') {
            std::string location;
            if (row >> std::quoted(location))
                state->managedTracks.insert(location);
        } else if (kind == 'P') {
            std::uint64_t dbid = 0;
            std::string location;
            if (row >> dbid >> std::quoted(location); dbid && !location.empty())
                state->playlistLocations[dbid] = location;
        }
    }
}

bool saveState(const fs::path& mount, const FilesystemPlayerState& state,
               std::string* error) {
    const fs::path path = manifestPath(mount);
    const fs::path tmp = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Could not create PodBox's player state folder";
        return false;
    }
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            if (error) *error = "Could not write PodBox's player state";
            return false;
        }
        out << kManifestMagic << '\n';
        std::vector<std::string> tracks(state.managedTracks.begin(),
                                        state.managedTracks.end());
        std::sort(tracks.begin(), tracks.end());
        for (const std::string& location : tracks)
            out << "T " << std::quoted(location) << '\n';

        std::vector<std::pair<std::uint64_t, std::string>> playlists(
            state.playlistLocations.begin(), state.playlistLocations.end());
        std::sort(playlists.begin(), playlists.end());
        for (const auto& [dbid, location] : playlists)
            out << "P " << dbid << ' ' << std::quoted(location) << '\n';
        if (!out) {
            if (error) *error = "Could not finish PodBox's player state";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "Could not install PodBox's player state";
        return false;
    }
    return true;
}

std::uint64_t stableId(const std::string& kind, const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::string* part : {&kind, &value}) {
        for (unsigned char c : *part) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xff;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

// `name` in Unicode NFC, or empty when it is already NFC or cannot be
// converted. macOS lists exFAT names decomposed (NFD), yet unlink() of a
// "._" companion only succeeds with the composed form stored on disk.
std::string composedName(const std::string& name) {
#ifdef __APPLE__
    CFStringRef source = CFStringCreateWithCString(
        kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
    if (!source) return {};
    CFMutableStringRef composed =
        CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
    CFRelease(source);
    if (!composed) return {};
    CFStringNormalize(composed, kCFStringNormalizationFormC);
    std::string out(std::size_t(CFStringGetMaximumSizeForEncoding(
                        CFStringGetLength(composed),
                        kCFStringEncodingUTF8)) + 1,
                    '\0');
    const bool ok = CFStringGetCString(composed, out.data(),
                                       CFIndex(out.size()),
                                       kCFStringEncodingUTF8);
    CFRelease(composed);
    if (!ok) return {};
    out.resize(std::strlen(out.c_str()));
    return out == name ? std::string() : out;
#else
    (void)name;
    return {};
#endif
}

// Removes a "._" companion, retrying under its composed name when the
// listed (decomposed) spelling is refused.
void removeCompanion(const fs::path& companion) {
    std::error_code ec;
    if (fs::remove(companion, ec) && !ec) return;
    const std::string composed = composedName(companion.filename().string());
    if (!composed.empty())
        fs::remove(companion.parent_path() / composed, ec);
}

// macOS tags every file it creates with extended attributes (at least
// com.apple.provenance). FAT and exFAT cannot hold them, so the kernel writes
// a "._name" AppleDouble companion beside each file and folder, and player
// firmware lists those as broken tracks. Remove a companion only when its
// real file is beside it, as `dot_clean -m` would.
void removeAppleDoubleFiles(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> companions;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() <= 2 || name.rfind("._", 0) != 0) continue;
        std::error_code existsError;
        if (fs::exists(it->path().parent_path() / name.substr(2), existsError))
            companions.push_back(it->path());
    }
    for (const fs::path& companion : companions) removeCompanion(companion);
}

// The folders PodBox creates get companions beside them too, e.g. "._Music"
// at the volume root.
void removeAppleDoubleFolderCompanions(const fs::path& mount,
                                       const fs::path& relative) {
    fs::path current = mount;
    for (const fs::path& component : relative) {
        std::error_code ec;
        const fs::path companion = current / ("._" + component.string());
        if (fs::exists(companion, ec)) removeCompanion(companion);
        current /= component;
    }
}

}  // namespace

std::uint64_t filesystemTrackDbid(const std::string& location) {
    return stableId("track", pathKey(location));
}

FilesystemPlayerLoad loadFilesystemPlayer(
    const fs::path& mount, const fs::path& musicDirectory,
    FilesystemPlayerState* state) {
    FilesystemPlayerLoad result;
    if (!state) {
        result.error = "Filesystem player state was not provided";
        return result;
    }
    loadState(mount, state);

    const fs::path musicRoot = mount / musicDirectory;
    std::error_code ec;
    if (!fs::is_directory(mount, ec)) {
        result.error = "The player is no longer mounted";
        return result;
    }
    // A recognised player may not have a music folder until the first sync
    // creates one; that is an empty library, not a failure.
    if (!fs::is_directory(musicRoot, ec)) {
        state->managedTracks.clear();
        state->playlistLocations.clear();
        result.library = Library{};
        return result;
    }

    std::vector<fs::path> audioFiles;
    std::vector<fs::path> playlistFiles;
    for (const auto& entry : fs::recursive_directory_iterator(musicRoot, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        // macOS leaves AppleDouble "._name" companions on FAT volumes; they
        // share the audio extension but hold no audio.
        if (entry.path().filename().string().rfind("._", 0) == 0) continue;
        if (isImportableAudioFile(entry.path()))
            audioFiles.push_back(entry.path());
        else if (playlistFile(entry.path()))
            playlistFiles.push_back(entry.path());
    }
    if (ec) {
        result.error = "Could not scan the player's music folder: " +
                       ec.message();
        return result;
    }
    std::sort(audioFiles.begin(), audioFiles.end());
    std::sort(playlistFiles.begin(), playlistFiles.end());

    // FAT is case-insensitive, so a manifest written as "Music/..." still
    // owns "MUSIC/...". Match by key and adopt the scanned spelling so later
    // exact comparisons (deletion, re-import) agree with it.
    std::unordered_map<std::string, std::string> managedByKey;
    for (const std::string& location : state->managedTracks)
        managedByKey.emplace(pathKey(location), location);
    state->managedTracks.clear();

    Library library;
    std::unordered_map<std::string, std::uint32_t> idByPath;
    std::uint32_t nextId = 100;
    int unreadable = 0;
    for (const fs::path& path : audioFiles) {
        FileMeta metadata = readFileMetadata(path);
        if (!metadata.ok) {
            ++unreadable;
            continue;
        }
        Track track = std::move(metadata.track);
        track.id = nextId++;
        track.location = relativeLocation(mount, path);
        if (track.location.empty()) {
            ++unreadable;
            continue;
        }
        track.dbid = filesystemTrackDbid(track.location);
        idByPath[pathKey(path)] = track.id;
        if (managedByKey.erase(pathKey(track.location))) {
            state->managedTracks.insert(track.location);
            result.managedTrackIds.insert(track.id);
        }
        library.tracks.push_back(std::move(track));
    }

    // A managed path whose file vanished no longer grants deletion authority
    // over anything. The host library will naturally queue a replacement
    // because the missing file was not scanned into `library`.
    for (const auto& [key, location] : managedByKey) {
        std::error_code existsError;
        if (fs::is_regular_file(mount / fs::path(location), existsError))
            state->managedTracks.insert(location);
    }

    std::unordered_map<std::string, std::uint64_t> playlistIdByLocation;
    for (const auto& [dbid, location] : state->playlistLocations)
        playlistIdByLocation[pathKey(location)] = dbid;
    state->playlistLocations.clear();

    for (const fs::path& path : playlistFiles) {
        const std::string location = relativeLocation(mount, path);
        if (location.empty()) continue;
        Playlist playlist;
        playlist.name = path.stem().string();
        const auto known = playlistIdByLocation.find(pathKey(location));
        playlist.dbid = known == playlistIdByLocation.end()
                            ? stableId("playlist", pathKey(location))
                            : known->second;

        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xef &&
                static_cast<unsigned char>(line[1]) == 0xbb &&
                static_cast<unsigned char>(line[2]) == 0xbf)
                line.erase(0, 3);
            if (line.empty() || line.front() == '#') continue;
            std::replace(line.begin(), line.end(), '\\', '/');
            fs::path referenced = line;
            // Some players write `/MUSIC/...` to mean the root of their own
            // volume, not the Mac's filesystem root. Ordinary relative M3U
            // paths remain relative to the playlist file.
            if (referenced.is_absolute())
                referenced = mount / referenced.relative_path();
            else
                referenced = path.parent_path() / referenced;
            const auto found = idByPath.find(pathKey(referenced));
            if (found != idByPath.end()) playlist.trackIds.push_back(found->second);
        }
        state->playlistLocations[playlist.dbid] = location;
        state->playlistContents[playlist.dbid] = playlist.trackIds;
        state->playlistNames[playlist.dbid] = playlist.name;
        library.playlists.push_back(std::move(playlist));
    }

    if (unreadable > 0)
        result.error = std::to_string(unreadable) +
                       (unreadable == 1 ? " audio file could not be read"
                                        : " audio files could not be read");
    result.library = std::move(library);
    return result;
}

fs::path allocateFilesystemMusicPath(const fs::path& mount,
                                     const fs::path& musicDirectory,
                                     const Track& metadata,
                                     const std::string& extension,
                                     std::string* location) {
    const std::string artist =
        safeComponent(metadata.artist, "Unknown Artist");
    const std::string album = safeComponent(metadata.album, "Unknown Album");
    std::string title = safeComponent(metadata.title, "Untitled");
    if (metadata.trackNumber > 0) {
        char number[8];
        std::snprintf(number, sizeof(number), "%02u - ", metadata.trackNumber);
        title.insert(0, number);
    }

    const fs::path directory = mount / musicDirectory / artist / album;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return {};

    for (int suffix = 1; suffix <= 1000; ++suffix) {
        const std::string tail =
            suffix == 1 ? std::string() : " (" + std::to_string(suffix) + ")";
        const fs::path destination = directory / (title + tail + extension);
        if (fs::exists(destination, ec)) continue;
        if (location) *location = relativeLocation(mount, destination);
        return destination;
    }
    return {};
}

bool saveFilesystemPlayer(const fs::path& mount,
                          const fs::path& musicDirectory,
                          const Library& library,
                          FilesystemPlayerState* state, std::string* error) {
    if (!state) {
        if (error) *error = "Filesystem player state was not provided";
        return false;
    }
    const fs::path musicRoot = mount / musicDirectory;
    std::error_code ec;
    fs::create_directories(musicRoot, ec);
    if (ec) {
        if (error) *error = "Could not create the player's music folder";
        return false;
    }

    std::unordered_map<std::uint32_t, const Track*> trackById;
    for (const Track& track : library.tracks) trackById[track.id] = &track;

    const auto oldLocations = state->playlistLocations;
    const auto oldContents = state->playlistContents;
    const auto oldNames = state->playlistNames;
    std::unordered_map<std::uint64_t, std::string> newLocations;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> newContents;
    std::unordered_map<std::uint64_t, std::string> newNames;
    std::unordered_set<std::string> claimed;
    for (const Playlist& playlist : library.playlists) {
        const std::string stem = safeComponent(playlist.name, "Playlist");
        fs::path path;
        if (const auto old = oldLocations.find(playlist.dbid);
            old != oldLocations.end() &&
            fs::path(old->second).stem().string() == playlist.name) {
            // Merely adding a song must not relocate an existing nested M3U.
            path = mount / old->second;
        } else {
            path = musicRoot / (stem + ".m3u8");
        }
        for (int suffix = 2; claimed.count(pathKey(path)); ++suffix)
            path = musicRoot /
                   (stem + " (" + std::to_string(suffix) + ").m3u8");
        claimed.insert(pathKey(path));

        const std::uint64_t dbid =
            playlist.dbid ? playlist.dbid
                          : stableId("playlist", pathKey(path));
        newLocations[dbid] = relativeLocation(mount, path);
        newContents[dbid] = playlist.trackIds;
        newNames[dbid] = playlist.name;

        const auto oldContent = oldContents.find(playlist.dbid);
        const auto oldName = oldNames.find(playlist.dbid);
        const auto oldLocation = oldLocations.find(playlist.dbid);
        const bool unchanged =
            oldContent != oldContents.end() &&
            oldContent->second == playlist.trackIds &&
            oldName != oldNames.end() && oldName->second == playlist.name &&
            oldLocation != oldLocations.end() &&
            pathKey(mount / oldLocation->second) == pathKey(path);
        if (unchanged) continue;

        const fs::path tmp = path.string() + ".podbox-tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) {
                if (error) *error = "Could not write playlist “" +
                                    playlist.name + "”";
                return false;
            }
            out << "#EXTM3U\n";
            for (std::uint32_t id : playlist.trackIds) {
                const auto found = trackById.find(id);
                if (found == trackById.end()) continue;
                const fs::path trackPath = mount / found->second->location;
                const fs::path relative = fs::relative(trackPath, path.parent_path(), ec);
                if (!ec) out << relative.generic_string() << '\n';
            }
            if (!out) {
                if (error) *error = "Could not finish playlist “" +
                                    playlist.name + "”";
                return false;
            }
        }
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            if (error) *error = "Could not install playlist “" +
                                playlist.name + "”";
            return false;
        }
    }

    // Only paths previously recorded by PodBox are candidates here. An M3U
    // that was never loaded into the library is never swept up accidentally.
    for (const auto& [dbid, oldLocation] : oldLocations) {
        const auto current = newLocations.find(dbid);
        if (current != newLocations.end() && current->second == oldLocation)
            continue;
        fs::remove(mount / oldLocation, ec);
    }
    state->playlistLocations = std::move(newLocations);
    state->playlistContents = std::move(newContents);
    state->playlistNames = std::move(newNames);

    std::erase_if(state->managedTracks, [&](const std::string& location) {
        std::error_code existsError;
        return !fs::is_regular_file(mount / fs::path(location), existsError);
    });
    if (!saveState(mount, *state, error)) return false;
    // Also clears companions of files copied since the last save.
    removeAppleDoubleFiles(musicRoot);
    removeAppleDoubleFiles(manifestPath(mount).parent_path());
    removeAppleDoubleFolderCompanions(mount, musicDirectory);
    removeAppleDoubleFolderCompanions(mount, ".podbox");
    return true;
}

}  // namespace podbox
