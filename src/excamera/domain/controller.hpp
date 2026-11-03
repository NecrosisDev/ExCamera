#pragma once

#include <cstdint>
#include <limits>

#include "ground.hpp"
#include "math.hpp"

namespace excamera {

enum class CameraMode { first_person, third_person, overhead };

struct CameraSettings {
    float first_fov = 75.f;
    float third_fov = 82.f; // 0 inherits first_fov.
    float look_sensitivity = 0.003f;
    float near_plane = 3.f;
    float eye_setback = 8.f;
    float third_distance = 240.f;
    float third_shoulder = 32.f;
    float third_height = 28.f;
    float transition_seconds = 0.25f;
    float release_seconds = 0.35f;
    float invalid_hold_seconds = 0.10f;
};

CameraSettings sanitize_camera_settings(CameraSettings settings) noexcept;

struct CameraIdentity {
    uint64_t player = 0;
    uint64_t scene = 0;
    uint64_t head = 0;
    uint64_t camera = 0;
    uint64_t controller = 0;
};

constexpr bool operator==(const CameraIdentity& left, const CameraIdentity& right) {
    return left.player == right.player && left.scene == right.scene && left.head == right.head &&
        left.camera == right.camera && left.controller == right.controller;
}

struct CameraFrame {
    CameraIdentity identity;
    Pose head_pose;
    Pose native_camera_pose;
    Vec3 sector_origin;
    Look look;
    bool look_valid = false;
    float native_fov = 75.f;
    float native_near_plane = 100.f;
    float delta_seconds = 1.f / 60.f;
    float clearance = std::numeric_limits<float>::infinity();
    float pivot_clearance = std::numeric_limits<float>::infinity();
    GroundConstraint ground{};
    bool control_allowed = true;
};

struct CameraOutput {
    CameraMode mode = CameraMode::overhead;
    Pose pose;
    Vec3 pivot;
    float fov = 75.f;
    float near_plane = 100.f;
    float third_distance = 0.f;
    bool write_camera = false;
    bool held = false;
    bool transitioning = false;
};

class CameraController {
public:
    explicit CameraController(CameraSettings settings = {});

    void set_mode(CameraMode mode) noexcept;
    CameraMode mode() const noexcept { return mode_; }
    void set_settings(CameraSettings settings) noexcept;
    const CameraSettings& settings() const noexcept { return settings_; }
    void reset() noexcept;
    CameraOutput evaluate(const CameraFrame& frame) noexcept;

private:
    CameraOutput native(const CameraFrame& frame) const noexcept;
    bool custom(const CameraFrame& frame, CameraOutput& output) noexcept;
    CameraOutput finish(CameraOutput output) noexcept;
    void remember(const CameraOutput& output) noexcept;

    CameraSettings settings_;
    CameraMode mode_ = CameraMode::first_person;
    CameraIdentity identity_{};
    bool has_identity_ = false;

    Pose last_pose_{};
    Vec3 last_pivot_{};
    float last_fov_ = 75.f;
    float last_near_plane_ = 100.f;
    bool has_last_ = false;
    bool last_custom_ = false;

    bool transition_active_ = false;
    Pose transition_from_{};
    float transition_from_fov_ = 75.f;
    float transition_from_near_ = 100.f;
    float transition_elapsed_ = 0.f;

    float third_distance_ = 0.f;
    float invalid_seconds_ = 0.f;
};

} // namespace excamera
