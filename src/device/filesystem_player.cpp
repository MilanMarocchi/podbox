#include "device/filesystem_player.h"

#include "library/dedupe.h"
#include "library/metadata.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace podbox {
namespace {

constexpr const char* kManifestMagic = "podbox-filesystem-player 1";

fs::path manifestPath(const fs::path& mount) {
    return mount / ".podbox" / "filesystem-player.tsv";
}

std::string relativeLocation(const fs::path& mount, const fs::path& path) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, mount, ec);
    return ec ? std::string() : relative.generic_string();
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
    if (!fs::is_directory(musicRoot, ec)) {
        result.error = "The player's music folder is missing";
        return result;
    }

    std::vector<fs::path> audioFiles;
    std::vector<fs::path> playlistFiles;
    for (const auto& entry : fs::recursive_directory_iterator(musicRoot, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
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
        if (state->managedTracks.count(track.location))
            result.managedTrackIds.insert(track.id);
        library.tracks.push_back(std::move(track));
    }

    // A managed path whose file vanished no longer grants deletion authority
    // over anything. The host library will naturally queue a replacement
    // because the missing file was not scanned into `library`.
    std::erase_if(state->managedTracks, [&](const std::string& location) {
        std::error_code existsError;
        return !fs::is_regular_file(mount / fs::path(location), existsError);
    });

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
    return saveState(mount, *state, error);
}

}  // namespace podbox
