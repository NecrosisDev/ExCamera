#include "math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace excamera {
namespace {

using Quaternion = std::array<float, 4>;

float component(const Pose& pose, int row, int column) {
    const Vec3& axis = row == 0 ? pose.right : row == 1 ? pose.up : pose.back;
    return column == 0 ? axis.x : column == 1 ? axis.y : axis.z;
}

void set_component(Pose& pose, int row, int column, float value) {
    Vec3& axis = row == 0 ? pose.right : row == 1 ? pose.up : pose.back;
    if (column == 0) axis.x = value;
    else if (column == 1) axis.y = value;
    else axis.z = value;
}

bool pose_quaternion(const Pose& pose, Quaternion& quaternion) {
    const float trace = component(pose, 0, 0) + component(pose, 1, 1) + component(pose, 2, 2);
    if (trace > 0.f) {
        const float scale = 2.f * std::sqrt(trace + 1.f);
        if (!(scale > 1e-6f) || !std::isfinite(scale)) return false;
        quaternion = {
            (component(pose, 2, 1) - component(pose, 1, 2)) / scale,
            (component(pose, 0, 2) - component(pose, 2, 0)) / scale,
            (component(pose, 1, 0) - component(pose, 0, 1)) / scale,
            0.25f * scale,
        };
    } else {
        int index = component(pose, 1, 1) > component(pose, 0, 0) ? 1 : 0;
        if (component(pose, 2, 2) > component(pose, index, index)) index = 2;
        const int next = (index + 1) % 3;
        const int last = (index + 2) % 3;
        const float scale = 2.f * std::sqrt(std::max(0.f, 1.f + component(pose, index, index) -
            component(pose, next, next) - component(pose, last, last)));
        if (!(scale > 1e-6f) || !std::isfinite(scale)) return false;
        quaternion[index] = 0.25f * scale;
        quaternion[next] = (component(pose, index, next) + component(pose, next, index)) / scale;
        quaternion[last] = (component(pose, index, last) + component(pose, last, index)) / scale;
        quaternion[3] = (component(pose, last, next) - component(pose, next, last)) / scale;
    }
    const float magnitude = std::sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
    if (!(magnitude > 1e-6f) || !std::isfinite(magnitude)) return false;
    for (float& value : quaternion) value /= magnitude;
    return true;
}

Pose quaternion_pose(const Quaternion& quaternion) {
    const float x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];
    Pose result;
    result.right = {1.f - 2.f * (y * y + z * z), 2.f * (x * y - z * w), 2.f * (x * z + y * w)};
    result.up = {2.f * (x * y + z * w), 1.f - 2.f * (x * x + z * z), 2.f * (y * z - x * w)};
    result.back = {2.f * (x * z - y * w), 2.f * (y * z + x * w), 1.f - 2.f * (x * x + y * y)};
    return result;
}

} // namespace

float length(Vec3 value) noexcept {
    return std::sqrt(dot(value, value));
}

Vec3 normalized(Vec3 value, Vec3 fallback) noexcept {
    const float magnitude = length(value);
    if (!(magnitude > 1e-6f) || !std::isfinite(magnitude)) return fallback;
    return value / magnitude;
}

bool finite(Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const Pose& pose) noexcept {
    return finite(pose.right) && finite(pose.up) && finite(pose.back) && finite(pose.origin);
}

Look sanitize_look(Look look) noexcept {
    const float yaw = std::isfinite(look.yaw) ? std::remainder(look.yaw, two_pi) : 0.f;
    const float pitch = std::isfinite(look.pitch) ? std::clamp(look.pitch, -max_look_pitch, max_look_pitch) : 0.f;
    return {yaw, pitch};
}

Pose look_pose(Look look) noexcept {
    look = sanitize_look(look);
    const float sine = std::sin(look.yaw), cosine = std::cos(look.yaw);
    const float pitch_sine = std::sin(look.pitch), pitch_cosine = std::cos(look.pitch);
    return {
        {-cosine, 0.f, sine},
        {-sine * pitch_sine, pitch_cosine, -cosine * pitch_sine},
        {-sine * pitch_cosine, -pitch_sine, -cosine * pitch_cosine},
        {},
    };
}

Look look_from_pose(const Pose& pose) noexcept {
    return sanitize_look({std::atan2(-pose.back.x, -pose.back.z),
                          std::asin(std::clamp(-pose.back.y, -1.f, 1.f))});
}

Vec3 third_person_pivot(const Pose& eye, float height_cm, float shoulder_cm, Look look) noexcept {
    return eye.origin + Vec3{0.f, height_cm, 0.f} + look_pose(look).right * shoulder_cm;
}

Vec3 third_person_pivot(const Pose& eye, float height_cm, float shoulder_cm, Look look,
                        float clearance_cm) noexcept {
    const Vec3 desired = third_person_pivot(eye, height_cm, shoulder_cm, look);
    const Vec3 offset = desired - eye.origin;
    const float distance = length(offset);
    if (distance <= 1e-6f) return desired;
    if (std::isnan(clearance_cm)) return eye.origin;
    if (clearance_cm == std::numeric_limits<float>::infinity()) return desired;
    if (!std::isfinite(distance) || !std::isfinite(clearance_cm) || clearance_cm <= 0.f)
        return eye.origin;
    return eye.origin + offset * std::min(1.f, clearance_cm / distance);
}

Vec3 eye_position(const Pose& head, float setback_cm) noexcept {
    const float clamped = std::clamp(std::isfinite(setback_cm) ? setback_cm : 0.f, 0.f, 16.f);
    return head.origin + head.up * 9.60687065f + head.back * (16.5f - clamped);
}

bool eye_pose(const Pose& head, float setback_cm, Pose& output) noexcept {
    if (!finite(head)) return false;
    const Vec3 back = normalized(-head.back);
    const Vec3 head_up = normalized(head.up);
    Vec3 right = cross(head_up, back);
    if (!(length(right) > 0.05f)) return false;
    right = normalized(right);
    const Vec3 up = normalized(cross(back, right));
    if (!finite(back) || !finite(up) || !finite(right) || length(back) < 0.5f || length(up) < 0.5f) return false;
    output = {right, up, back, eye_position(head, setback_cm)};
    return finite(output);
}

Pose strided_pose(const float* values) noexcept {
    return {
        {values[0], values[1], values[2]},
        {values[4], values[5], values[6]},
        {values[8], values[9], values[10]},
        {values[12], values[13], values[14]},
    };
}

Matrix4 expand_affine(const Pose& pose) noexcept {
    Matrix4 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) result.rows[row * 4 + column] = component(pose, row, column);
        result.rows[row * 4 + 3] = 0.f;
    }
    result.rows[12] = pose.origin.x;
    result.rows[13] = pose.origin.y;
    result.rows[14] = pose.origin.z;
    result.rows[15] = 1.f;
    return result;
}

Pose blend_pose(const Pose& from, const Pose& to, float amount) noexcept {
    const float clamped = std::clamp(std::isfinite(amount) ? amount : 0.f, 0.f, 1.f);
    if (clamped == 0.f) return from;
    if (clamped == 1.f) return to;
    if (!finite(from)) return to;
    if (!finite(to)) return from;
    const float eased = clamped * clamped * (3.f - 2.f * clamped);
    Pose result = from;
    Quaternion first{}, second{};
    if (pose_quaternion(from, first) && pose_quaternion(to, second)) {
        float cosine = 0.f;
        for (int index = 0; index < 4; ++index) cosine += first[index] * second[index];
        if (cosine < 0.f) {
            cosine = -cosine;
            for (float& value : second) value = -value;
        }
        cosine = std::clamp(cosine, 0.f, 1.f);
        float from_weight = 1.f - eased, to_weight = eased;
        if (cosine < 0.9995f) {
            const float angle = std::acos(cosine);
            const float sine = std::sin(angle);
            if (std::fabs(sine) > 1e-6f) {
                from_weight = std::sin((1.f - eased) * angle) / sine;
                to_weight = std::sin(eased * angle) / sine;
            }
        }
        Quaternion blended{};
        for (int index = 0; index < 4; ++index) blended[index] = from_weight * first[index] + to_weight * second[index];
        const float magnitude = std::sqrt(blended[0] * blended[0] + blended[1] * blended[1] +
            blended[2] * blended[2] + blended[3] * blended[3]);
        if (magnitude > 1e-6f && std::isfinite(magnitude)) {
            for (float& value : blended) value /= magnitude;
            result = quaternion_pose(blended);
        }
    }
    result.origin = from.origin + (to.origin - from.origin) * eased;
    return result;
}

bool project_point(const Pose& camera, Vec3 point, float width, float height, float vertical_fov,
                   Vec2& output) noexcept {
    const Vec3 delta = point - camera.origin;
    const float x = dot(delta, camera.right);
    const float y = dot(delta, camera.up);
    const float z = dot(delta, camera.back);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z >= -0.01f ||
        !(width > 0.f) || !(height > 0.f) || !std::isfinite(vertical_fov)) return false;
    const float scale = height * 0.5f / std::tan(vertical_fov * pi / 360.f);
    if (!std::isfinite(scale) || std::fabs(scale) < 1e-6f) return false;
    output = {width * 0.5f + x / -z * scale, height * 0.5f - y / -z * scale};
    return std::isfinite(output.x) && std::isfinite(output.y);
}

} // namespace excamera
