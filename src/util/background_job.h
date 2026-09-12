#pragma once

#include <chrono>
#include <future>
#include <optional>
#include <utility>

namespace podbox {

// A single owned job. Polling never waits for unfinished work; destruction
// joins as a lifetime backstop. Workers must capture values, not UI state.
template <typename T> class BackgroundJob {
public:
    bool busy() const { return future_.valid(); }
    bool ready() const {
        return busy() && future_.wait_for(std::chrono::seconds(0)) ==
                             std::future_status::ready;
    }
    template <typename F> bool start(F&& work) {
        if (busy()) return false;
        future_ = std::async(std::launch::async, std::forward<F>(work));
        return true;
    }
    std::optional<T> take() {
        if (!ready()) return std::nullopt;
        return future_.get();
    }
private:
    std::future<T> future_;
};

} // namespace podbox
