#include "settings_worker.hpp"

#include <chrono>

#include "settings_store.hpp"

namespace excamera::settings {

Worker::~Worker() {
    stop();
}

bool Worker::start(std::filesystem::path file) {
    if (file.empty()) return false;
    const std::lock_guard lock(mutex_);
    if (thread_.joinable()) return false;
    file_ = std::move(file);
    stopping_ = false;
    errors_.store(0, std::memory_order_relaxed);
    try {
        thread_ = std::thread(&Worker::run, this);
        return true;
    } catch (...) {
        return false;
    }
}

void Worker::request(const CameraSettings& settings) noexcept {
    try {
        {
            const std::lock_guard lock(mutex_);
            pending_ = Pending{settings, ++revision_};
        }
        condition_.notify_one();
    } catch (...) {
    }
}

void Worker::stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void Worker::run() noexcept {
    for (;;) {
        Pending job;
        {
            std::unique_lock lock(mutex_);
            if (!condition_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping_ || pending_.has_value(); }))
                continue;
            if (stopping_ && !pending_) break;
            job = *pending_;
            pending_.reset();
        }

        const bool saved = save(file_, job.settings);
        if (!saved) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock lock(mutex_);
            if (stopping_) break;
            if (!pending_) pending_ = job;
            condition_.wait_for(lock, std::chrono::seconds(2), [&] { return stopping_ || pending_.has_value(); });
        }
    }
}

} // namespace excamera::settings
