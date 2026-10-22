#pragma once
#if __has_include(<openxr/openxr.h>)
#include <openxr/openxr.h>
#else
// A build without the OpenXR SDK still lays out hands (the finger poses): the four value types this
// header and the hand model use, as the SDK declares them.
typedef struct XrVector3f { float x, y, z; } XrVector3f;
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrPosef { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace fp::xr {
using Matrix = std::array<float, 16>; // column-major, OpenGL convention

inline bool finite(XrVector3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool valid(XrPosef p) {
    const auto q = p.orientation;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return finite(p.position) && std::isfinite(n) && std::abs(n - 1.f) < .001f;
}
inline XrVector3f add(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XrVector3f scale(XrVector3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline XrVector3f cross(XrVector3f a, XrVector3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(XrVector3f a) { return std::sqrt(dot(a, a)); }
inline XrVector3f unit(XrVector3f a, XrVector3f fallback = {0, 0, -1}) {
    const float n = length(a);
    return n > 1.e-6f ? scale(a, 1 / n) : fallback;
}
inline XrQuaternionf conjugate(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }
inline XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    const auto r = multiply(multiply(q, {v.x, v.y, v.z, 0}), conjugate(q));
    return {r.x, r.y, r.z};
}
inline XrQuaternionf yawQuaternion(float yaw) { return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; }
inline XrQuaternionf axisAngle(XrVector3f unitAxis, float angle) { const float s = std::sin(angle / 2); return {unitAxis.x * s, unitAxis.y * s, unitAxis.z * s, std::cos(angle / 2)}; }
// A controller grip tilted about its own x axis: the hilt angle a player prefers. Position untouched.
inline XrPosef gripAngled(XrPosef grip, float degrees) {
    const float half = degrees * .0174532925f * .5f;
    return {multiply(grip.orientation, {std::sin(half), 0, 0, std::cos(half)}), grip.position};
}
inline XrPosef compose(XrPosef a, XrPosef b) {
    return {multiply(a.orientation, b.orientation), add(a.position, rotate(a.orientation, b.position))};
}
inline XrPosef inverse(XrPosef p) {
    const auto q = conjugate(p.orientation);
    return {q, rotate(q, scale(p.position, -1))};
}
// Shortest rotation taking direction `from` onto `to`; antiparallel inputs pick any perpendicular axis.
inline XrQuaternionf directionRotation(XrVector3f from, XrVector3f to) {
    from = unit(from);
    to = unit(to);
    const float d = std::clamp(dot(from, to), -1.f, 1.f);
    if (d < -.9999f) {
        const auto a = unit(cross(from, std::abs(from.y) < .9f ? XrVector3f{0, 1, 0} : XrVector3f{1, 0, 0}));
        return {a.x, a.y, a.z, 0};
    }
    const auto axis = cross(from, to);
    const float n = std::sqrt(2 * (1 + d));
    return {axis.x / n, axis.y / n, axis.z / n, n * .5f};
}
// Yaw of the forward (-Z) direction about +Y; false when looking straight up or down.
inline bool headingYaw(XrQuaternionf orientation, float& yaw) {
    const auto forward = rotate(orientation, {0, 0, -1});
    if (forward.x * forward.x + forward.z * forward.z < .0001f) return false;
    yaw = std::atan2(-forward.x, -forward.z);
    return true;
}
inline Matrix poseMatrix(XrPosef p) {
    if (!valid(p)) throw std::runtime_error("invalid rigid pose");
    const auto x = rotate(p.orientation, {1, 0, 0});
    const auto y = rotate(p.orientation, {0, 1, 0});
    const auto z = rotate(p.orientation, {0, 0, 1});
    return {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, p.position.x, p.position.y, p.position.z, 1};
}
inline Matrix product(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned k = 0; k < 4; ++k) out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return out;
}
// Off-axis OpenGL projection; NDC depth -1 at clipNear, +1 at clipFar. A zero clipFar is the
// engine's infinite-far sentinel and builds the limit matrix (m[10] = -1, m[14] = -2 near).
inline Matrix projection(XrFovf fov, float clipNear, float clipFar) {
    constexpr float halfPi = 1.57079632679f;
    for (float a : {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown})
        if (!std::isfinite(a) || std::abs(a) >= halfPi) throw std::runtime_error("invalid eye FOV");
    if (!std::isfinite(clipNear) || !std::isfinite(clipFar) || clipNear <= 0 || (clipFar != 0 && clipFar <= clipNear))
        throw std::runtime_error("invalid clip range");
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
    if (r - l <= 0 || u - d <= 0) throw std::runtime_error("reversed eye FOV");
    Matrix m{};
    m[0] = 2 / (r - l);
    m[5] = 2 / (u - d);
    m[8] = (r + l) / (r - l);
    m[9] = (u + d) / (u - d);
    m[10] = clipFar == 0 ? -1 : -(clipFar + clipNear) / (clipFar - clipNear);
    m[11] = -1;
    m[14] = clipFar == 0 ? -2 * clipNear : -2 * clipFar * clipNear / (clipFar - clipNear);
    for (float value : m)
        if (!std::isfinite(value)) throw std::runtime_error("unrepresentable eye projection");
    return m;
}

// Tracked motion is expressed relative to a yaw-only calibration so gravity and real head tilt survive.
// The anchor supplies the locomotion/world frame; the game adapter owns the axis conversion.
class HeadCamera {
    XrPosef origin_{{0, 0, 0, 1}, {0, 0, 0}};
    bool calibrated_ = false;

public:
    void invalidate() { calibrated_ = false; }
    bool calibrated() const { return calibrated_; }
    XrPosef origin() const { return origin_; } // the recentred head: yaw and position in the local space
    bool place(XrPosef origin) { // a remembered origin, already yaw-only and in the local space
        float yaw = 0;
        if (!valid(origin) || !headingYaw(origin.orientation, yaw)) return false;
        origin_ = {yawQuaternion(yaw), origin.position};
        calibrated_ = true;
        return true;
    }
    bool recenter(XrPosef head) {
        float yaw = 0;
        if (!valid(head) || !headingYaw(head.orientation, yaw)) return false;
        origin_ = {yawQuaternion(yaw), head.position};
        calibrated_ = true;
        return true;
    }
    XrPosef relative(XrPosef tracked) const {
        if (!calibrated_ || !valid(tracked)) throw std::runtime_error("head camera unavailable");
        return compose(inverse(origin_), tracked);
    }
    XrPosef eye(XrPosef anchor, XrPosef trackedEye, float unitsPerMetre) const {
        if (!valid(anchor) || !std::isfinite(unitsPerMetre) || unitsPerMetre <= 0)
            throw std::runtime_error("head camera unavailable");
        auto delta = relative(trackedEye);
        delta.position = scale(delta.position, unitsPerMetre);
        const auto result = compose(anchor, delta);
        if (!valid(result)) throw std::runtime_error("unrepresentable head camera pose");
        return result;
    }
};
}
