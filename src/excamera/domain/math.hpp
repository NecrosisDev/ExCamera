#pragma once

#include <array>
#include <cstdint>

namespace excamera {

inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float two_pi = 2.f * pi;
inline constexpr float max_look_pitch = 1.52f;

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

constexpr Vec3 operator+(Vec3 left, Vec3 right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}
constexpr Vec3 operator-(Vec3 left, Vec3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
constexpr Vec3 operator-(Vec3 value) {
    return {-value.x, -value.y, -value.z};
}
constexpr Vec3 operator*(Vec3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}
constexpr Vec3 operator*(float scale, Vec3 value) {
    return value * scale;
}
constexpr Vec3 operator/(Vec3 value, float scale) {
    return {value.x / scale, value.y / scale, value.z / scale};
}
constexpr float dot(Vec3 left, Vec3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
constexpr Vec3 cross(Vec3 left, Vec3 right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float length(Vec3 value) noexcept;
Vec3 normalized(Vec3 value, Vec3 fallback = {}) noexcept;
bool finite(Vec3 value) noexcept;

// Native affine convention: right/up/back rows, then origin. Camera forward is -back.
struct Pose {
    Vec3 right;
    Vec3 up;
    Vec3 back;
    Vec3 origin;
};

bool finite(const Pose& pose) noexcept;

struct Look {
    float yaw = 0.f;
    float pitch = 0.f;
};

struct Matrix4 {
    std::array<float, 16> rows{};
};

Look sanitize_look(Look look) noexcept;
Pose look_pose(Look look) noexcept;
Look look_from_pose(const Pose& pose) noexcept;

// Shared camera composition; eye and pivot units are centimetres.
Vec3 third_person_pivot(const Pose& eye, float height_cm, float shoulder_cm, Look look) noexcept;
Vec3 third_person_pivot(const Pose& eye, float height_cm, float shoulder_cm, Look look,
                        float clearance_cm) noexcept;

// The head's second row is up and third row is forward. Eye units are centimetres.
Vec3 eye_position(const Pose& head, float setback_cm) noexcept;
bool eye_pose(const Pose& head, float setback_cm, Pose& output) noexcept;

// Converts native four-float render rows to an affine pose, ignoring each row's padding.
Pose strided_pose(const float* values) noexcept;
Matrix4 expand_affine(const Pose& pose) noexcept;
Pose blend_pose(const Pose& from, const Pose& to, float amount) noexcept;

// Projects a scene-space point through a vertical-FOV camera into native client coordinates.
bool project_point(const Pose& camera, Vec3 point, float width, float height, float vertical_fov,
                   Vec2& output) noexcept;

} // namespace excamera
