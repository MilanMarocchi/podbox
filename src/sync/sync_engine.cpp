#include "sync/sync_engine.h"

#include "library/metadata.h"

#include <random>

namespace fs = std::filesystem;

namespace podbox {

fs::path allocateMusicPath(const fs::path& mount, const std::string& extension,
                           std::string* location) {
    static std::mt19937 rng{std::random_device{}()};

    const fs::path musicDir = mount / "iPod_Control" / "Music";
    std::error_code ec;
    std::vector<std::string> subdirs;
    for (const auto& entry : fs::directory_iterator(musicDir, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec) && name.size() == 3 && name[0] == 'F')
            subdirs.push_back(name);
    }
    if (subdirs.empty()) {
        // Fresh/restored device: create the folder set iTunes uses.
        for (int i = 0; i < 20; ++i) {
            char name[4];
            std::snprintf(name, sizeof(name), "F%02d", i);
            fs::create_directories(musicDir / name, ec);
            subdirs.push_back(name);
        }
    }

    std::uniform_int_distribution<size_t> pickDir(0, subdirs.size() - 1);
    std::uniform_int_distribution<int> pickLetter(0, 25);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::string& dir = subdirs[pickDir(rng)];
        std::string name;
        for (int i = 0; i < 4; ++i) name += char('A' + pickLetter(rng));
        name += extension;
        const fs::path dest = musicDir / dir / name;
        if (!fs::exists(dest, ec)) {
            if (location)
                *location = ":iPod_Control:Music:" + dir + ":" + name;
            return dest;
        }
    }
    return {};
}

SyncEngine::~SyncEngine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SyncEngine::queueAdds(const std::vector<fs::path>& files,
                           const fs::path& mount) {
    if (files.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mount_ = mount;
        for (const auto& f : files) pending_.push_back(f);
        if (!working_.load() && pending_.size() == files.size()) {
            // Fresh batch: reset progress counters.
            total_.store(int(files.size()));
            done_.store(0);
        } else {
            total_.fetch_add(int(files.size()));
        }
        if (!worker_.joinable())
            worker_ = std::thread(&SyncEngine::run, this);
    }
    cv_.notify_all();
}

std::vector<SyncEngine::Completed> SyncEngine::takeCompleted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Completed> out;
    out.swap(completed_);
    return out;
}

bool SyncEngine::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return working_.load() || !pending_.empty();
}

std::string SyncEngine::currentName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void SyncEngine::run() {
    for (;;) {
        fs::path src, mount;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            src = pending_.front();
            pending_.pop_front();
            mount = mount_;
            current_ = src.filename().string();
            working_.store(true);
        }

        Completed result;
        result.name = src.filename().string();
        FileMeta meta = readFileMetadata(src);
        if (!meta.ok) {
            result.error = meta.error;
        } else {
            std::string location;
            const fs::path dest =
                allocateMusicPath(mount, src.extension().string(), &location);
            std::error_code ec;
            if (dest.empty()) {
                result.error = result.name + ": could not allocate a filename";
            } else if (!fs::copy_file(src, dest, ec) || ec) {
                result.error = result.name + ": copy failed (" + ec.message() +
                               ")";
            } else {
                result.track = std::move(meta.track);
                result.track.location = location;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_.push_back(std::move(result));
            done_.fetch_add(1);
            if (pending_.empty()) {
                working_.store(false);
                current_.clear();
            }
        }
    }
}

}  // namespace podbox
