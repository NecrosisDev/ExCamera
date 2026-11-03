#include "input.hpp"

#include "../test_support.hpp"

namespace {

excamera::InputSnapshot snapshot() {
    excamera::InputSnapshot result;
    result.mode = excamera::CameraMode::first_person;
    return result;
}

bool initial_capture_suppresses_native_and_resets_delta() {
    excamera::InputController controller;
    controller.reset();
    const excamera::InputOutput output = controller.evaluate(snapshot());
    CHECK(output.capture_cursor && output.suppress_native && output.reset_deltas);
    CHECK(!output.restore_cursor && !output.change_mode);
    const excamera::InputOutput next = controller.evaluate(snapshot());
    CHECK(next.capture_cursor && !next.reset_deltas);
    return true;
}

bool f6_cycles_once_per_press() {
    excamera::InputController controller;
    controller.reset();
    (void)controller.evaluate(snapshot());
    excamera::InputSnapshot input = snapshot();
    input.f6_down = true;
    excamera::InputOutput output = controller.evaluate(input);
    CHECK(output.change_mode && output.next_mode == excamera::CameraMode::third_person);
    output = controller.evaluate(input);
    CHECK(!output.change_mode);
    input.f6_down = false;
    (void)controller.evaluate(input);
    input.mode = excamera::CameraMode::third_person;
    input.f6_down = true;
    output = controller.evaluate(input);
    CHECK(output.change_mode && output.next_mode == excamera::CameraMode::overhead);
    CHECK(!output.capture_cursor && output.restore_cursor);
    input.mode = excamera::CameraMode::overhead;
    input.f6_down = false;
    (void)controller.evaluate(input);
    input.f6_down = true;
    output = controller.evaluate(input);
    CHECK(output.change_mode && output.next_mode == excamera::CameraMode::first_person);
    return true;
}

bool native_ui_releases_and_recaptures_cleanly() {
    excamera::InputController controller;
    controller.reset();
    (void)controller.evaluate(snapshot());
    excamera::InputSnapshot input = snapshot();
    input.native_ui_active = true;
    excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.capture_cursor && output.restore_cursor && !output.suppress_native && output.reset_deltas);
    input.native_ui_active = false;
    output = controller.evaluate(input);
    CHECK(output.capture_cursor && output.suppress_native && output.reset_deltas);
    return true;
}

bool focus_loss_suppresses_once_and_recaptures() {
    excamera::InputController controller;
    controller.reset();
    (void)controller.evaluate(snapshot());
    excamera::InputSnapshot input = snapshot();
    input.focused = false;
    excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.capture_cursor && output.restore_cursor && output.suppress_native && output.reset_deltas);
    input.focused = true;
    output = controller.evaluate(input);
    CHECK(output.capture_cursor && output.suppress_native && output.reset_deltas);
    return true;
}

bool invalid_client_cannot_capture() {
    excamera::InputController controller;
    controller.reset();
    (void)controller.evaluate(snapshot());
    excamera::InputSnapshot input = snapshot();
    input.client_valid = false;
    const excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.capture_cursor && output.restore_cursor && output.reset_deltas);
    return true;
}

bool held_f6_does_not_retrigger_after_focus_change() {
    excamera::InputController controller;
    controller.reset();
    (void)controller.evaluate(snapshot());
    excamera::InputSnapshot input = snapshot();
    input.focused = false;
    input.f6_down = true;
    (void)controller.evaluate(input);
    input.focused = true;
    const excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.change_mode && output.capture_cursor);
    return true;
}

bool reset_uses_current_f6_state() {
    excamera::InputController controller;
    controller.reset(true);
    excamera::InputSnapshot input = snapshot();
    input.f6_down = true;
    const excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.change_mode && output.capture_cursor);
    return true;
}

bool overhead_never_captures_cursor() {
    excamera::InputController controller;
    controller.reset();
    excamera::InputSnapshot input = snapshot();
    input.mode = excamera::CameraMode::overhead;
    const excamera::InputOutput output = controller.evaluate(input);
    CHECK(!output.capture_cursor && !output.suppress_native && !output.restore_cursor);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"initial capture suppresses native and resets delta", initial_capture_suppresses_native_and_resets_delta},
        {"f6 cycles once per press", f6_cycles_once_per_press},
        {"native ui releases and recaptures cleanly", native_ui_releases_and_recaptures_cleanly},
        {"focus loss suppresses once and recaptures", focus_loss_suppresses_once_and_recaptures},
        {"invalid client cannot capture", invalid_client_cannot_capture},
        {"held f6 does not retrigger after focus change", held_f6_does_not_retrigger_after_focus_change},
        {"reset uses current f6 state", reset_uses_current_f6_state},
        {"overhead never captures cursor", overhead_never_captures_cursor},
    });
}
