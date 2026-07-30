#pragma once

#include "itdb/itunesdb.h"
#include "library/fingerprint.h"
#include "library/transcode.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace podbox {

// Allocates a destination for a new song on the device: a random F## music
// folder plus an unused iTunes-style 4-letter filename. Returns the absolute
// path and sets `location` to the iPod-style ':'-separated DB path.
std::filesystem::path allocateMusicPath(const std::filesystem::path& mount,
                                        const std::string& extension,
                                        std::string* location);

// What the worker treats as "already on the device".
//
// A snapshot, taken on the UI thread from the library and the fingerprint
// store, so the worker never touches live library state. The worker adds each
// file it imports, which is what stops a folder holding two copies of one song
// from importing both.
struct DupeGuard {
    bool enabled = true;
    std::unordered_set<std::string> metaKeys;   // duplicateKey(Exact)
    std::unordered_set<std::uint64_t> hashes;   // AudioFingerprint::hash
};

// Copies queued audio files onto the device on a worker thread and reads
// their metadata. The UI thread polls takeCompleted() and owns all library
// mutation and DB writing.
class SyncEngine {
public:
    ~SyncEngine();

    struct Completed {
        Track track;        // filled when error is empty and !duplicate
        std::string name;   // source filename, for progress/status
        std::string error;
        bool duplicate = false;  // already on the device; nothing was copied
        AudioFingerprint fp;     // of the source file, for the caller to store
    };

    // Copies (or transcodes, per `fmt`) each file onto the device, skipping
    // any that `guard` already knows about.
    void queueAdds(const std::vector<std::filesystem::path>& files,
                   const std::filesystem::path& mount, ImportFormat fmt,
                   DupeGuard guard = {});
    std::vector<Completed> takeCompleted();

    bool busy() const;
    int batchTotal() const { return total_.load(); }
    int batchDone() const { return done_.load(); }
    std::string currentName() const;

private:
    void run();
    // True when `meta`/`src` is already accounted for by the guard. Fills
    // `fp` with the source fingerprint either way, so the caller can record
    // it even for files it skips.
    bool isDuplicate(const Track& meta, const std::filesystem::path& src,
                     AudioFingerprint* fp);
    void noteImported(const Track& track, const AudioFingerprint& fp);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::filesystem::path> pending_;
    std::vector<Completed> completed_;
    std::filesystem::path mount_;
    ImportFormat importFmt_ = ImportFormat::Original;
    DupeGuard guard_;
    std::string current_;
    std::atomic<int> total_{0};
    std::atomic<int> done_{0};
    std::atomic<bool> working_{false};
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace podbox
