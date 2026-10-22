#pragma once
#include "native_pose.h"
#include "upper_body.h"

namespace fp::xr {
// Clamps a motor correction to `maximum` radians so a tracking jump cannot request a violent twist.
inline XrQuaternionf limitMotorRotation(XrQuaternionf delta, float maximum) {
    if (delta.w < 0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    const float angle = 2 * std::acos(std::clamp(delta.w, 0.f, 1.f));
    if (angle <= maximum) return delta;
    const float n = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (n < .00001f) return {0, 0, 0, 1};
    const float factor = std::sin(maximum * .5f) / n;
    return {delta.x * factor, delta.y * factor, delta.z * factor, std::cos(maximum * .5f)};
}
inline NativePose motorLocal(XrQuaternionf parent, XrQuaternionf child) {
    return nativeItemPose({multiply(conjugate(parent), child), {}});
}
inline XrVector3f boundedDriverPosition(XrVector3f achieved, XrVector3f wanted, float limit = .35f) {
    const auto delta = sub(wanted, achieved);
    const float distance = length(delta);
    return distance > limit ? add(achieved, scale(delta, limit / distance)) : wanted;
}
}
