#include "util/finder.h"

#import <Cocoa/Cocoa.h>

#include <cstring>

namespace podbox {
namespace {

NSURL* fileUrl(const std::filesystem::path& p) {
    // The file-system representation, not UTF-8: paths on disk can be in
    // either Unicode normalisation, and this is the conversion that
    // round-trips them.
    NSString* s = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:p.c_str()
                                    length:std::strlen(p.c_str())];
    return s ? [NSURL fileURLWithPath:s] : nil;
}

}  // namespace

bool moveToTrash(const std::filesystem::path& path, std::string* error) {
    @autoreleasepool {
        NSURL* url = fileUrl(path);
        NSError* err = nil;
        if (url && [[NSFileManager defaultManager] trashItemAtURL:url
                                                 resultingItemURL:nil
                                                            error:&err])
            return true;
        if (error)
            *error = err ? std::string(err.localizedDescription.UTF8String)
                         : "could not move " + path.filename().string() +
                               " to the Trash";
        return false;
    }
}

bool openInFinder(const std::filesystem::path& folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return false;
    NSURL* url = fileUrl(folder);
    return url && [[NSWorkspace sharedWorkspace] openURL:url];
}

bool revealInFinder(const std::vector<std::filesystem::path>& files) {
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    std::error_code ec;
    for (const auto& f : files)
        if (std::filesystem::exists(f, ec))
            if (NSURL* url = fileUrl(f)) [urls addObject:url];
    if (urls.count == 0) return false;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
    return true;
}

}  // namespace podbox
