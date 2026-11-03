#pragma once

#include "controller.hpp"

namespace excamera {

struct InputSnapshot {
    CameraMode mode = CameraMode::first_person;
    bool focused = true;
    bool native_ui_active = false;
    bool client_valid = true;
    bool f6_down = false;
};

struct InputOutput {
    CameraMode next_mode = CameraMode::overhead;
    bool change_mode = false;
    bool capture_cursor = false;
    bool restore_cursor = false;
    bool suppress_native = false;
    bool reset_deltas = false;
};

class InputController {
public:
    void reset(bool f6_down = false) noexcept;
    InputOutput evaluate(const InputSnapshot& snapshot) noexcept;

private:
    bool captured_ = false;
    bool previous_f6_ = false;
};

} // namespace excamera
