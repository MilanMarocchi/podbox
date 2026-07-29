#pragma once

#include "itdb/itunesdb.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace podbox {

// Allocates a destination for a new song on the device: a random F## music
// folder plus an unused iTunes-style 4-letter filename. Returns the absolute
// path and sets `location` to the iPod-style ':'-separated DB path.
std::filesystem::path allocateMusicPath(const std::filesystem::path& mount,
                                        const std::string& extension,
                                        std::string* location);

// Copies queued audio files onto the device on a worker thread and reads
// their metadata. The UI thread polls takeCompleted() and owns all library
// mutation and DB writing.
class SyncEngine {
public:
    ~SyncEngine();

    struct Completed {
        Track track;        // filled when error is empty
        std::string name;   // source filename, for progress/status
        std::string error;
    };

    void queueAdds(const std::vector<std::filesystem::path>& files,
                   const std::filesystem::path& mount);
    std::vector<Completed> takeCompleted();

    bool busy() const;
    int batchTotal() const { return total_.load(); }
    int batchDone() const { return done_.load(); }
    std::string currentName() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::filesystem::path> pending_;
    std::vector<Completed> completed_;
    std::filesystem::path mount_;
    std::string current_;
    std::atomic<int> total_{0};
    std::atomic<int> done_{0};
    std::atomic<bool> working_{false};
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace podbox
