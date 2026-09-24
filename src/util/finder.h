#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace podbox {

// Finder, as far as PodBox needs it. Off macOS each of these fails and does
// nothing.

// Moves `path` to the Trash (the drive's own, on an external volume), as
// Finder's Move to Trash does, so it can be dragged back out. Safe to call
// from a worker thread. On failure the file stays where it was and `error`
// says why.
bool moveToTrash(const std::filesystem::path& path, std::string* error);

// Opens `folder` in a Finder window. False when it is not there to open, e.g.
// on a drive that is not connected.
bool openInFinder(const std::filesystem::path& folder);

// Opens Finder with `files` selected, a window per folder. UI thread only.
bool revealInFinder(const std::vector<std::filesystem::path>& files);

}  // namespace podbox
