#include "controller.hpp"

#include <cmath>
#include <limits>

#include "../test_support.hpp"

namespace {

bool near(float left, float right, float tolerance = 1e-3f) {
    return std::fabs(left - right) <= tolerance;
}

excamera::Pose head_pose() {
    return {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {10.f, 100.f, 20.f}};
}

excamera::CameraFrame frame() {
    excamera::CameraFrame result;
    result.identity = {1, 2, 3, 4, 5};
    result.head_pose = head_pose();
    result.native_camera_pose = excamera::look_pose({0.4f, 0.1f});
    result.native_camera_pose.origin = {900.f, 900.f, 900.f};
    result.sector_origin = {1000.f, 0.f, -500.f};
    result.look = {0.2f, -0.1f};
    result.look_valid = true;
    result.native_fov = 66.f;
    result.native_near_plane = 99.f;
    return result;
}

bool first_person_mounts_eye_and_applies_settings() {
    excamera::CameraSettings settings;
    settings.first_fov = 88.f;
    settings.near_plane = 2.5f;
    excamera::CameraController controller(settings);
    const excamera::CameraOutput output = controller.evaluate(frame());
    CHECK(output.mode == excamera::CameraMode::first_person);
    CHECK(output.write_camera);
    CHECK(!output.held);
    CHECK(near(output.fov, 88.f));
    CHECK(near(output.near_plane, 2.5f));
    const excamera::Vec3 expected = excamera::eye_position(head_pose(), settings.eye_setback) + frame().sector_origin;
    CHECK(excamera::length(output.pose.origin - expected) < 1e-3f);
    const excamera::Pose orientation = excamera::look_pose(frame().look);
    CHECK(near(output.pose.back.x, orientation.back.x));
    CHECK(near(output.pose.back.y, orientation.back.y));
    CHECK(near(output.pose.back.z, orientation.back.z));
    return true;
}

bool third_person_uses_pivot_and_desired_distance() {
    excamera::CameraController controller;
    controller.set_mode(excamera::CameraMode::third_person);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    const excamera::CameraOutput output = controller.evaluate(input);
    CHECK(output.write_camera);
    CHECK(near(output.third_distance, 240.f));
    const excamera::Pose orientation = excamera::look_pose(input.look);
    excamera::Pose eye;
    CHECK(excamera::eye_pose(head_pose(), 8.f, eye));
    eye.origin = eye.origin + input.sector_origin;
    const excamera::Vec3 pivot = excamera::third_person_pivot(eye, 28.f, 32.f, input.look);
    CHECK(excamera::length(output.pivot - pivot) < 1e-3f);
    CHECK(excamera::length(output.pose.origin - (pivot + orientation.back * 240.f)) < 1e-3f);
    return true;
}

bool obstruction_pulls_in_immediately_and_eases_out() {
    excamera::CameraController controller;
    controller.set_mode(excamera::CameraMode::third_person);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    CHECK(near(controller.evaluate(input).third_distance, 240.f));
    input.clearance = 40.f;
    CHECK(near(controller.evaluate(input).third_distance, 40.f));
    input.clearance = std::numeric_limits<float>::infinity();
    input.delta_seconds = 0.1f;
    const float released = controller.evaluate(input).third_distance;
    CHECK(released > 40.f && released < 240.f);
    CHECK(near(released, 40.f + 200.f * (1.f - std::exp(-0.1f / 0.35f)), 0.01f));
    input.clearance = std::numeric_limits<float>::quiet_NaN();
    CHECK(near(controller.evaluate(input).third_distance, 0.f));
    return true;
}

bool invalid_head_has_bounded_hold_then_native() {
    excamera::CameraController controller;
    const excamera::CameraOutput initial = controller.evaluate(frame());
    excamera::CameraFrame invalid = frame();
    invalid.head_pose.up = invalid.head_pose.back;
    invalid.delta_seconds = 0.05f;
    excamera::CameraOutput output = controller.evaluate(invalid);
    CHECK(output.held && output.write_camera);
    CHECK(excamera::length(output.pose.origin - initial.pose.origin) < 1e-4f);
    output = controller.evaluate(invalid);
    CHECK(output.held && output.write_camera);
    output = controller.evaluate(invalid);
    CHECK(!output.held && !output.write_camera);
    CHECK(near(output.fov, 66.f));
    CHECK(near(output.near_plane, 99.f));
    CHECK(excamera::length(output.pose.origin - invalid.native_camera_pose.origin) < 1e-4f);
    return true;
}

bool identity_change_never_reuses_held_pose() {
    excamera::CameraController controller;
    (void)controller.evaluate(frame());
    excamera::CameraFrame invalid = frame();
    invalid.identity.scene = 99;
    invalid.head_pose.up = invalid.head_pose.back;
    const excamera::CameraOutput output = controller.evaluate(invalid);
    CHECK(!output.held && !output.write_camera);
    CHECK(excamera::length(output.pose.origin - invalid.native_camera_pose.origin) < 1e-4f);
    return true;
}

bool control_release_and_overhead_use_native_camera() {
    excamera::CameraController controller;
    (void)controller.evaluate(frame());
    excamera::CameraFrame input = frame();
    input.control_allowed = false;
    excamera::CameraOutput output = controller.evaluate(input);
    CHECK(!output.write_camera && !output.held);
    CHECK(near(output.fov, 66.f));
    controller.set_mode(excamera::CameraMode::overhead);
    input.control_allowed = true;
    input.delta_seconds = 0.f;
    output = controller.evaluate(input);
    CHECK(output.write_camera && output.transitioning);
    input.delta_seconds = 0.25f;
    output = controller.evaluate(input);
    CHECK(!output.write_camera && !output.transitioning);
    CHECK(excamera::length(output.pose.origin - input.native_camera_pose.origin) < 1e-4f);
    output = controller.evaluate(input);
    CHECK(!output.write_camera);
    return true;
}

bool leaving_overhead_blends_into_custom_view() {
    excamera::CameraController controller;
    controller.set_mode(excamera::CameraMode::overhead);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    CHECK(!controller.evaluate(input).write_camera);
    controller.set_mode(excamera::CameraMode::first_person);
    excamera::CameraOutput output = controller.evaluate(input);
    CHECK(output.write_camera && output.transitioning);
    input.delta_seconds = std::numeric_limits<float>::quiet_NaN();
    output = controller.evaluate(input);
    CHECK(output.transitioning);
    input.delta_seconds = 0.25f;
    output = controller.evaluate(input);
    CHECK(output.write_camera && !output.transitioning);
    CHECK(near(output.fov, 75.f));
    return true;
}

bool invalid_settings_and_input_are_bounded() {
    excamera::CameraSettings settings;
    settings.first_fov = 1000.f;
    settings.third_fov = std::numeric_limits<float>::quiet_NaN();
    settings.near_plane = 0.f;
    settings.third_distance = std::numeric_limits<float>::quiet_NaN();
    excamera::CameraController controller(settings);
    excamera::CameraFrame input = frame();
    input.delta_seconds = std::numeric_limits<float>::quiet_NaN();
    excamera::CameraOutput output = controller.evaluate(input);
    CHECK(near(output.fov, 130.f));
    CHECK(near(output.near_plane, 0.1f));
    controller.set_mode(excamera::CameraMode::third_person);
    input.delta_seconds = 0.25f;
    output = controller.evaluate(input);
    CHECK(near(output.fov, 82.f));
    CHECK(near(output.third_distance, 240.f));
    CHECK(excamera::finite(output.pose));
    return true;
}

bool live_settings_updates_and_fov_inherit_work() {
    excamera::CameraSettings settings;
    settings.first_fov = 70.f;
    settings.third_fov = 0.f;
    excamera::CameraController controller(settings);
    controller.set_mode(excamera::CameraMode::third_person);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    excamera::CameraOutput output = controller.evaluate(input);
    CHECK(near(output.fov, 70.f));
    settings.first_fov = 95.f;
    controller.set_settings(settings);
    output = controller.evaluate(input);
    CHECK(near(output.fov, 95.f));
    settings.third_fov = 88.f;
    controller.set_settings(settings);
    output = controller.evaluate(input);
    CHECK(near(output.fov, 88.f));
    return true;
}

bool support_height_limits_only_downward_booms() {
    excamera::CameraSettings settings;
    settings.release_seconds = 0.01f;
    excamera::CameraController controller(settings);
    controller.set_mode(excamera::CameraMode::third_person);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    input.look = {0.f, 1.4f};
    input.ground = {12.f, true};
    const excamera::CameraOutput supported = controller.evaluate(input);
    const excamera::Vec3 pivot = supported.pivot;
    const float expected = (pivot.y - 12.f - excamera::ground_margin_cm) / -supported.pose.back.y;
    CHECK(near(supported.third_distance, expected));
    input.ground = {};
    const excamera::CameraOutput unsupported = controller.evaluate(input);
    CHECK(near(unsupported.third_distance, 0.f));
    input.look = {0.f, -1.4f};
    input.delta_seconds = 0.25f;
    const excamera::CameraOutput upward = controller.evaluate(input);
    CHECK(near(upward.third_distance, 240.f));
    return true;
}

bool pivot_clearance_is_applied_before_boom_clearance() {
    excamera::CameraController controller;
    controller.set_mode(excamera::CameraMode::third_person);
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    input.pivot_clearance = 8.f;
    input.clearance = 40.f;
    excamera::CameraOutput output = controller.evaluate(input);
    excamera::Pose eye;
    CHECK(excamera::eye_pose(head_pose(), 8.f, eye));
    eye.origin = eye.origin + input.sector_origin;
    const excamera::Vec3 desired = excamera::third_person_pivot(eye, 28.f, 32.f, input.look);
    const excamera::Vec3 offset = desired - eye.origin;
    const excamera::Vec3 expected = eye.origin + offset * (8.f / excamera::length(offset));
    CHECK(excamera::length(output.pivot - expected) < 1e-3f);
    input.pivot_clearance = std::numeric_limits<float>::quiet_NaN();
    output = controller.evaluate(input);
    CHECK(excamera::length(output.pivot - eye.origin) < 1e-3f);
    return true;
}

bool transition_origins_use_the_validated_target() {
    excamera::CameraController controller;
    excamera::CameraFrame input = frame();
    input.delta_seconds = 0.f;
    controller.set_mode(excamera::CameraMode::overhead);
    (void)controller.evaluate(input);
    controller.set_mode(excamera::CameraMode::third_person);
    input.clearance = 40.f;
    const excamera::CameraOutput third = controller.evaluate(input);
    CHECK(third.transitioning);
    excamera::Pose eye;
    CHECK(excamera::eye_pose(head_pose(), 8.f, eye));
    eye.origin = eye.origin + input.sector_origin;
    const excamera::Vec3 pivot = excamera::third_person_pivot(eye, 28.f, 32.f, input.look);
    const excamera::Vec3 expected_third = pivot + excamera::look_pose(input.look).back * 40.f;
    CHECK(excamera::length(third.pose.origin - expected_third) < 1e-3f);
    controller.set_mode(excamera::CameraMode::first_person);
    const excamera::CameraOutput first = controller.evaluate(input);
    CHECK(first.transitioning);
    const excamera::Vec3 expected_first = excamera::eye_position(head_pose(), 8.f) + input.sector_origin;
    CHECK(excamera::length(first.pose.origin - expected_first) < 1e-3f);
    return true;
}

bool repeated_mode_cycles_remain_finite() {
    excamera::CameraController controller;
    excamera::CameraFrame input = frame();
    input.delta_seconds = 1.f / 60.f;
    for (int index = 0; index < 30; ++index) {
        controller.set_mode(index % 3 == 0 ? excamera::CameraMode::first_person :
            index % 3 == 1 ? excamera::CameraMode::third_person : excamera::CameraMode::overhead);
        const excamera::CameraOutput output = controller.evaluate(input);
        CHECK(excamera::finite(output.pose));
        CHECK(std::isfinite(output.fov) && output.fov > 0.f);
        CHECK(std::isfinite(output.near_plane) && output.near_plane >= 0.f);
    }
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"first person mounts eye and applies settings", first_person_mounts_eye_and_applies_settings},
        {"third person uses pivot and desired distance", third_person_uses_pivot_and_desired_distance},
        {"obstruction pulls in immediately and eases out", obstruction_pulls_in_immediately_and_eases_out},
        {"invalid head has bounded hold then native", invalid_head_has_bounded_hold_then_native},
        {"identity change never reuses held pose", identity_change_never_reuses_held_pose},
        {"control release and overhead use native camera", control_release_and_overhead_use_native_camera},
        {"leaving overhead blends into custom view", leaving_overhead_blends_into_custom_view},
        {"invalid settings and input are bounded", invalid_settings_and_input_are_bounded},
        {"live settings updates and fov inherit work", live_settings_updates_and_fov_inherit_work},
        {"support height limits only downward booms", support_height_limits_only_downward_booms},
        {"pivot clearance is applied before boom clearance", pivot_clearance_is_applied_before_boom_clearance},
        {"transition origins use the validated target", transition_origins_use_the_validated_target},
        {"repeated mode cycles remain finite", repeated_mode_cycles_remain_finite},
    });
}
