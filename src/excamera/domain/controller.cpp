#include "controller.hpp"

#include <algorithm>
#include <cmath>

namespace excamera {
namespace {

float bounded(float value, float fallback, float minimum, float maximum) {
    return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
}

float frame_seconds(float value) {
    return std::clamp(std::isfinite(value) ? value : 0.f, 0.f, 0.25f);
}

float ease(float value) {
    return value * value * (3.f - 2.f * value);
}

float mix(float from, float to, float amount) {
    return from + (to - from) * amount;
}

} // namespace

CameraSettings sanitize_camera_settings(CameraSettings settings) noexcept {
    settings.first_fov = bounded(settings.first_fov, 75.f, 30.f, 130.f);
    settings.third_fov = settings.third_fov == 0.f ? 0.f : bounded(settings.third_fov, 82.f, 30.f, 130.f);
    settings.look_sensitivity = bounded(settings.look_sensitivity, 0.003f, 0.0001f, 0.02f);
    settings.near_plane = bounded(settings.near_plane, 3.f, 0.1f, 100.f);
    settings.eye_setback = bounded(settings.eye_setback, 8.f, 0.f, 16.f);
    settings.third_distance = bounded(settings.third_distance, 240.f, 0.f, 1200.f);
    settings.third_shoulder = bounded(settings.third_shoulder, 32.f, -160.f, 160.f);
    settings.third_height = bounded(settings.third_height, 28.f, -100.f, 300.f);
    settings.transition_seconds = bounded(settings.transition_seconds, 0.25f, 0.f, 2.f);
    settings.release_seconds = bounded(settings.release_seconds, 0.35f, 0.01f, 2.f);
    settings.invalid_hold_seconds = bounded(settings.invalid_hold_seconds, 0.10f, 0.f, 1.f);
    return settings;
}

CameraController::CameraController(CameraSettings settings) : settings_(sanitize_camera_settings(settings)) {
    reset();
}

void CameraController::set_settings(CameraSettings settings) noexcept {
    settings_ = sanitize_camera_settings(settings);
    if (third_distance_ > settings_.third_distance) third_distance_ = settings_.third_distance;
}

void CameraController::set_mode(CameraMode mode) noexcept {
    if (mode == mode_) return;
    mode_ = mode;
    invalid_seconds_ = 0.f;
    transition_active_ = has_last_;
    transition_from_ = last_pose_;
    transition_from_fov_ = last_fov_;
    transition_from_near_ = last_near_plane_;
    transition_elapsed_ = 0.f;
}

void CameraController::reset() noexcept {
    identity_ = {};
    has_identity_ = false;
    last_pose_ = {};
    last_pivot_ = {};
    last_fov_ = 75.f;
    last_near_plane_ = 100.f;
    has_last_ = false;
    last_custom_ = false;
    transition_active_ = false;
    transition_from_ = {};
    transition_from_fov_ = 75.f;
    transition_from_near_ = 100.f;
    transition_elapsed_ = 0.f;
    third_distance_ = settings_.third_distance;
    invalid_seconds_ = 0.f;
}

CameraOutput CameraController::native(const CameraFrame& frame) const noexcept {
    CameraOutput output;
    output.mode = mode_;
    output.pose = frame.native_camera_pose;
    output.pivot = last_pivot_;
    output.fov = std::isfinite(frame.native_fov) && frame.native_fov > 0.f ? frame.native_fov : 75.f;
    output.near_plane = std::isfinite(frame.native_near_plane) && frame.native_near_plane >= 0.f
        ? frame.native_near_plane : 100.f;
    output.third_distance = third_distance_;
    return output;
}

bool CameraController::custom(const CameraFrame& frame, CameraOutput& output) noexcept {
    if (mode_ == CameraMode::overhead) {
        output = native(frame);
        return true;
    }
    Pose eye;
    if (!eye_pose(frame.head_pose, settings_.eye_setback, eye)) return false;
    eye.origin = eye.origin + frame.sector_origin;
    if (!finite(eye.origin)) return false;

    output = CameraOutput{};
    output.mode = mode_;
    output.fov = mode_ == CameraMode::first_person
        ? settings_.first_fov
        : settings_.third_fov > 0.f ? settings_.third_fov : settings_.first_fov;
    output.near_plane = settings_.near_plane;
    output.write_camera = true;

    if (mode_ == CameraMode::first_person) {
        output.pose = eye;
        if (frame.look_valid) {
            const Pose orientation = look_pose(frame.look);
            output.pose.right = orientation.right;
            output.pose.up = orientation.up;
            output.pose.back = orientation.back;
        }
        output.pivot = eye.origin;
        output.third_distance = 0.f;
        return finite(output.pose);
    }

    const Look look = frame.look_valid ? sanitize_look(frame.look) : look_from_pose(eye);
    const Pose orientation = look_pose(look);
    output.pivot = third_person_pivot(eye, settings_.third_height, settings_.third_shoulder, look,
                                      frame.pivot_clearance);

    float allowed = settings_.third_distance;
    if (std::isnan(frame.clearance)) allowed = 0.f;
    else if (std::isfinite(frame.clearance)) allowed = std::min(allowed, std::max(0.f, frame.clearance));
    allowed = ground_clearance(output.pivot, orientation.back, allowed, frame.ground);
    if (third_distance_ > allowed) third_distance_ = allowed;
    else {
        const float seconds = frame_seconds(frame.delta_seconds);
        const float amount = 1.f - std::exp(-seconds / settings_.release_seconds);
        third_distance_ += (allowed - third_distance_) * amount;
    }
    output.third_distance = third_distance_;
    output.pose = orientation;
    output.pose.origin = output.pivot + orientation.back * third_distance_;
    return finite(output.pose) && finite(output.pivot);
}

CameraOutput CameraController::finish(CameraOutput output) noexcept {
    remember(output);
    return output;
}

void CameraController::remember(const CameraOutput& output) noexcept {
    if (!finite(output.pose)) {
        has_last_ = false;
        last_custom_ = false;
        return;
    }
    has_last_ = true;
    last_custom_ = output.write_camera;
    last_pose_ = output.pose;
    last_pivot_ = output.pivot;
    last_fov_ = output.fov;
    last_near_plane_ = output.near_plane;
}

CameraOutput CameraController::evaluate(const CameraFrame& frame) noexcept {
    const float seconds = frame_seconds(frame.delta_seconds);
    if (!has_identity_ || identity_ != frame.identity) {
        reset();
        identity_ = frame.identity;
        has_identity_ = true;
    }
    if (!frame.control_allowed) {
        transition_active_ = false;
        invalid_seconds_ = 0.f;
        third_distance_ = settings_.third_distance;
        return finish(native(frame));
    }
    if (mode_ == CameraMode::overhead && !transition_active_) return finish(native(frame));

    CameraOutput target;
    if (!custom(frame, target)) {
        transition_active_ = false;
        invalid_seconds_ += seconds;
        if (has_last_ && last_custom_ && invalid_seconds_ <= settings_.invalid_hold_seconds) {
            CameraOutput held;
            held.mode = mode_;
            held.pose = last_pose_;
            held.pivot = last_pivot_;
            held.fov = last_fov_;
            held.near_plane = last_near_plane_;
            held.third_distance = third_distance_;
            held.write_camera = true;
            held.held = true;
            return finish(held);
        }
        return finish(native(frame));
    }
    invalid_seconds_ = 0.f;

    if (transition_active_) {
        transition_elapsed_ += seconds;
        const float amount = settings_.transition_seconds > 0.f
            ? std::min(1.f, transition_elapsed_ / settings_.transition_seconds) : 1.f;
        const float eased = ease(amount);
        CameraOutput output = target;
        output.pose = blend_pose(transition_from_, target.pose, amount);
        // Blend orientation and lens settings only. The target position has
        // current collision validation; a linear path between modes may not.
        output.pose.origin = target.pose.origin;
        output.fov = mix(transition_from_fov_, target.fov, eased);
        output.near_plane = mix(transition_from_near_, target.near_plane, eased);
        output.write_camera = amount < 1.f || target.write_camera;
        output.transitioning = amount < 1.f;
        if (amount >= 1.f) transition_active_ = false;
        return finish(output);
    }
    return finish(target);
}

} // namespace excamera
