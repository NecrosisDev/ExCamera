#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "controller.hpp"

namespace excamera::settings {

class Worker {
public:
    ~Worker();
    bool start(std::filesystem::path file);
    void request(const CameraSettings& settings) noexcept;
    void stop() noexcept;
    uint32_t error_count() const noexcept { return errors_.load(std::memory_order_relaxed); }

private:
    struct Pending {
        CameraSettings settings;
        uint64_t revision = 0;
    };

    void run() noexcept;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    std::filesystem::path file_;
    std::optional<Pending> pending_;
    uint64_t revision_ = 0;
    bool stopping_ = false;
    std::atomic<uint32_t> errors_{0};
};

} // namespace excamera::settings
