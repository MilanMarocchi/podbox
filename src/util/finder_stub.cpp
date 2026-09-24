#include "util/finder.h"

namespace podbox {

// There is no Finder to hand anything to. Failing, rather than deleting
// outright, keeps "remove a duplicate" recoverable on every platform.
bool moveToTrash(const std::filesystem::path&, std::string* error) {
    if (error) *error = "moving files to the Trash needs macOS";
    return false;
}
bool openInFinder(const std::filesystem::path&) { return false; }
bool revealInFinder(const std::vector<std::filesystem::path>&) { return false; }

}  // namespace podbox
