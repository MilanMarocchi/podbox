#pragma once

#include "itdb/itunesdb.h"

#include <filesystem>
#include <string>

namespace podbox {

// ~/Music/PodBox: the Mac library's own folder. Every song in the library
// lives here, sorted into Artist/Album folders. Songs arrive by being copied
// in, from the folders PodBox imports from or from Apple Music, and the
// originals are never touched.
std::filesystem::path defaultMusicFolder();

// Where a song belongs in the library folder: Artist/Album/NN Title.ext.
std::string sanitizeComponent(const std::string& s);

// Copies `src` into `root` as Artist/Album/NN Title.ext, setting `dest` to
// where it landed.
//
// Never overwrites. A file already at the name is taken to be this song when
// it is the same size (an earlier run's copy, so `reused` is set and nothing
// is copied); anything else moves this one along to "NN Title (2).ext".
// Copies go through a temporary name, so an interrupted run never leaves a
// truncated file under a real one. Within one APFS volume the copy is a
// clone, sharing storage with the original.
bool placeInMusicFolder(const std::filesystem::path& src, const Track& meta,
                        const std::filesystem::path& root,
                        std::filesystem::path* dest, bool* reused,
                        std::string* error);

// Removes the folders under `root` that removing songs has left empty (or
// holding nothing but Finder's .DS_Store). `root` itself stays.
void pruneEmptyFolders(const std::filesystem::path& root);

}  // namespace podbox
