#pragma once

#include "library/fingerprint.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace podbox {

// Fingerprints files already on the device, in the background.
//
// Opt-in and never automatic: this reads every file it is given, and a full
// iPod over USB is tens of gigabytes. Its only job is to tell duplicate
// detection which copies are byte-identical rather than merely the same song,
// so it is worth running once per device and no more — results are meant to be
// folded into the FingerprintStore and persisted.
//
// Same threading contract as SyncEngine: the UI thread polls take() and owns
// everything the results touch.
class VerifyJob {
public:
    ~VerifyJob();

    struct Item {
        std::uint64_t dbid = 0;
        std::filesystem::path path;
    };
    using Result = std::pair<std::uint64_t, AudioFingerprint>;

    // Begins hashing `items`. Ignored while another run is in flight.
    void start(std::vector<Item> items);

    // Asks the worker to stop after the file it is on. Results already
    // produced remain valid and are still worth keeping.
    void cancel();

    bool running() const { return running_.load(); }
    int done() const { return done_.load(); }
    int total() const { return total_.load(); }

    // Hands over everything hashed since the last call.
    std::vector<Result> take();

private:
    void run();

    mutable std::mutex mutex_;
    std::vector<Item> pending_;
    std::vector<Result> results_;
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

}  // namespace podbox
