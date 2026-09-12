#include "app/app.h"
#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
template <typename F> bool until(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
void watcherTest() {
    std::promise<void> release;
    auto gate = release.get_future().share();
    podbox::DeviceInfo device;
    device.mountPoint = "/fake/ipod";
    podbox::DeviceWatcher watcher([gate, device] { gate.wait(); return std::vector{device}; });
    const auto start = std::chrono::steady_clock::now();
    watcher.update(0);
    for (int i = 0; i < 100; ++i) watcher.update(1);
    check(std::chrono::steady_clock::now() - start < 250ms, "discovery polling never waits for the filesystem scan");
    check(watcher.devices().empty(), "unfinished scans are not published");
    watcher.forget(device.mountPoint);
    release.set_value();
    check(until([&] { watcher.update(1); return !watcher.busy(); }), "discovery worker completes");
    check(watcher.devices().empty(), "scan started before eject cannot resurrect the iPod");
    watcher.update(3);
    check(until([&] { watcher.update(3); return !watcher.busy(); }), "subsequent discovery completes");
    check(watcher.find(device.mountPoint), "a subsequent scan can discover a reconnected iPod");
}
}

namespace podbox {
struct AppTestAccess {
    static void ejectTest(bool success) {
        App app(Fonts{});
        app.loadedMount_ = "/fake/ipod";
        app.library_.emplace();
        std::promise<void> release;
        auto gate = release.get_future().share();
        const auto uiThread = std::this_thread::get_id();
        std::atomic<bool> offUi{false};
        const auto start = std::chrono::steady_clock::now();
        app.startEject(app.loadedMount_, [gate, uiThread, &offUi, success](const fs::path&, std::string* error) {
            offUi = std::this_thread::get_id() != uiThread;
            gate.wait();
            if (!success) *error = "device is busy";
            return success;
        });
        for (int i = 0; i < 100; ++i) app.applyDeviceJob();
        check(std::chrono::steady_clock::now() - start < 250ms, "eject and frame polling return while the eject command is blocked");
        check(app.library_.has_value(), "keep library until eject succeeds");
        check(app.deviceJob_.busy(), "eject stays marked in progress");
        check(!app.prepareToClose(), "close returns immediately while eject is running");
        release.set_value();
        check(until([&] { return app.deviceJob_.ready(); }), "eject worker finishes");
        app.applyDeviceJob();
        check(offUi, "eject command ran on a worker");
        check(success ? !app.library_ : app.library_.has_value(), "only successful eject clears library");
        check(success ? app.statusMsg_.find("safe to disconnect") != std::string::npos
                      : app.statusMsg_.find("device is busy") != std::string::npos,
              "eject success/failure reaches the UI");
    }
    static void ejectWaitsForReads() {
        App app(Fonts{});
        app.loadedMount_ = "/fake/ipod";
        std::promise<void> release;
        auto gate = release.get_future().share();
        app.monitorJob_.start([gate] { gate.wait(); return App::MonitorResult{}; });
        bool called = false;
        auto eject = [&called](const fs::path&, std::string*) { called = true; return true; };
        app.startEject(app.loadedMount_, eject);
        check(!app.deviceJob_.busy() && !called && !app.ejectRequestedMount_.empty(),
              "eject waits asynchronously for PodBox's outstanding reads");
        release.set_value();
        check(until([&] { return app.monitorJob_.ready(); }), "read completes");
        app.monitorJob_.take();
        app.startEject(app.loadedMount_, eject);
        check(until([&] { return app.deviceJob_.ready(); }), "eject starts after reads finish");
        app.applyDeviceJob();
        check(called, "queued eject executes");
    }
    static void snapshotSaveTest() {
        const fs::path mount = fs::temp_directory_path() /
            ("podbox-background-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const bool made = fs::create_directory(mount);
        check(made, "temporary device fixture created");
        if (!made) return;
        fs::create_directories(mount / "iPod_Control/iTunes");
        {
            App app(Fonts{});
            app.loadedMount_ = mount;
            app.loadedDeviceInfo_.emplace();
            app.loadedDeviceInfo_->mountPoint = mount;
            app.library_.emplace();
            app.library_->masterName = "snapshot";
            app.ownWriteTime_ = fs::file_time_type::max();
            check(app.writeDatabase(), "save is accepted");
            check(app.pendingDbWrite_, "save stays pending before completion is applied");
            // A worker must use its own snapshot, never the live library.
            app.library_->masterName = "changed on UI";
            check(until([&] { return app.deviceJob_.ready(); }), "database writer finishes");
            app.applyDeviceJob();
            const auto parsed = parseItunesDb(mount / "iPod_Control/iTunes/iTunesDB");
            check(parsed.library && parsed.library->masterName == "snapshot", "worker writes captured library state");
            check(!app.pendingDbWrite_, "actual successful write clears pending state");
            fs::create_directory(mount / "iPod_Control/iTunes/iTunesDB.podbox-tmp");
            app.ownWriteTime_ = fs::file_time_type::max();
            check(app.writeDatabase(), "failing save is queued");
            check(until([&] { return app.deviceJob_.ready(); }), "failing writer finishes");
            app.applyDeviceJob();
            check(app.pendingDbWrite_ && app.batchWriteFailed_, "actual disk failure leaves changes retryable");
        }
        fs::remove_all(mount);
    }
    static void saveTest() {
        App app(Fonts{});
        app.loadedMount_ = "/fake/ipod";
        app.library_.emplace();
        app.pendingDbWrite_ = true;
        app.lastBatchAdded_ = 2;
        app.deviceJobKind_ = App::DeviceJobKind::Save;
        std::promise<void> release;
        auto gate = release.get_future().share();
        app.deviceJob_.start([gate] {
            gate.wait();
            App::DeviceResult result;
            result.ok = false;
            result.session.status = "test save failure";
            return result;
        });
        app.applyCompletedAdds();
        check(app.pendingDbWrite_ && app.lastBatchAdded_ == 2, "queuing a save must not mark copied songs persisted");
        release.set_value();
        check(until([&] { return app.deviceJob_.ready(); }), "failed save completes");
        app.applyDeviceJob();
        check(app.pendingDbWrite_ && app.batchWriteFailed_, "failed save remains pending for retry");
        check(!app.prepareToClose(), "unsaved copies prevent normal close");
        check(app.closeSaveFailedOpen_, "failed close offers keep-open or discard");
        bool called = false;
        app.startEject(app.loadedMount_, [&called](const fs::path&, std::string*) { called = true; return true; });
        check(!app.deviceJob_.busy() && !called, "eject refuses unsaved changes");
        app.deviceJobKind_ = App::DeviceJobKind::Save;
        app.deviceJob_.start([] { return App::DeviceResult{}; });
        check(until([&] { return app.deviceJob_.ready(); }), "retry completes");
        app.applyDeviceJob();
        check(!app.pendingDbWrite_ && !app.batchWriteFailed_ && app.lastBatchAdded_ == 0,
              "only successful completion clears pending copies");
    }
};
}
int main() {
    ImGui::CreateContext();
    watcherTest();
    podbox::AppTestAccess::ejectTest(true);
    podbox::AppTestAccess::ejectTest(false);
    podbox::AppTestAccess::saveTest();
    podbox::AppTestAccess::ejectWaitsForReads();
    podbox::AppTestAccess::snapshotSaveTest();
    ImGui::DestroyContext();
    std::printf("Background tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
