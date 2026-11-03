#include "input.hpp"

namespace excamera {
namespace {

CameraMode next_mode(CameraMode mode) {
    switch (mode) {
        case CameraMode::first_person: return CameraMode::third_person;
        case CameraMode::third_person: return CameraMode::overhead;
        case CameraMode::overhead: return CameraMode::first_person;
        default: return CameraMode::first_person;
    }
}

} // namespace

void InputController::reset(bool f6_down) noexcept {
    captured_ = false;
    previous_f6_ = f6_down;
}

InputOutput InputController::evaluate(const InputSnapshot& snapshot) noexcept {
    InputOutput output;
    if (snapshot.f6_down && !previous_f6_) {
        output.change_mode = true;
        output.next_mode = next_mode(snapshot.mode);
        output.reset_deltas = true;
    }
    previous_f6_ = snapshot.f6_down;

    const CameraMode effective_mode = output.change_mode ? output.next_mode : snapshot.mode;
    const bool capture = snapshot.focused && !snapshot.native_ui_active && snapshot.client_valid &&
        effective_mode != CameraMode::overhead;
    output.capture_cursor = capture;
    output.suppress_native = capture || !snapshot.focused;
    output.restore_cursor = captured_ && !capture;
    if (!captured_ || !capture) output.reset_deltas = true;
    captured_ = capture;
    return output;
}

} // namespace excamera
