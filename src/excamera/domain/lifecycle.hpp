#pragma once

#include "controller.hpp"

namespace excamera {

class LifecycleTracker {
public:
    bool changed(const CameraIdentity& identity) noexcept {
        if (!has_identity_ || current_ != identity) {
            current_ = identity;
            has_identity_ = true;
            return true;
        }
        return false;
    }

    void reset() noexcept {
        current_ = {};
        has_identity_ = false;
    }

private:
    CameraIdentity current_{};
    bool has_identity_ = false;
};

} // namespace excamera
