#include "sync/verify_job.h"

namespace podbox {

VerifyJob::~VerifyJob() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

void VerifyJob::start(std::vector<Item> items) {
    if (running_.load() || items.empty()) return;
    // A previous run's thread has finished but not been reaped yet.
    if (worker_.joinable()) worker_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(items);
        results_.clear();
        total_.store(int(pending_.size()));
        done_.store(0);
    }
    cancel_.store(false);
    running_.store(true);
    worker_ = std::thread(&VerifyJob::run, this);
}

void VerifyJob::cancel() { cancel_.store(true); }

std::vector<VerifyJob::Result> VerifyJob::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Result> out;
    out.swap(results_);
    return out;
}

void VerifyJob::run() {
    for (;;) {
        Item item;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancel_.load() || pending_.empty()) break;
            item = std::move(pending_.back());
            pending_.pop_back();
        }

        const AudioFingerprint fp = fingerprintFile(item.path);

        std::lock_guard<std::mutex> lock(mutex_);
        // A file that has gone missing is not an error here; it simply has no
        // fingerprint, and the library health scan is what reports it.
        if (fp.ok()) results_.emplace_back(item.dbid, fp);
        done_.fetch_add(1);
    }
    running_.store(false);
}

}  // namespace podbox
