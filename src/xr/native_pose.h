#pragma once
#include "math.h"

namespace fp::xr {
// ExCamera's camera layout: right, up and back axes followed by the origin, in game units.
using NativePose = std::array<float, 12>;

inline void requireRigid(const NativePose& pose) {
    for (float value : pose)
        if (!std::isfinite(value)) throw std::runtime_error("invalid native pose");
    for (int axis = 0; axis < 3; ++axis)
        for (int other = 0; other < 3; ++other) {
            float d = 0;
            for (int k = 0; k < 3; ++k) d += pose[axis * 3 + k] * pose[other * 3 + k];
            if (std::abs(d - (axis == other ? 1.f : 0.f)) > .001f) throw std::runtime_error("nonrigid native pose");
        }
    const float determinant = pose[0] * (pose[4] * pose[8] - pose[5] * pose[7]) -
                              pose[1] * (pose[3] * pose[8] - pose[5] * pose[6]) +
                              pose[2] * (pose[3] * pose[7] - pose[4] * pose[6]);
    if (determinant < .999f) throw std::runtime_error("reflected native pose");
}
// Composes a calibrated tracked pose (metres) onto a native anchor. The anchor must be the
// locomotion frame, never the animated head, or real and animated rotation would multiply.
inline NativePose nativeEye(const NativePose& anchor, XrPosef relative, float unitsPerMetre) {
    requireRigid(anchor);
    if (!valid(relative) || !std::isfinite(unitsPerMetre) || unitsPerMetre <= 0) throw std::runtime_error("invalid native eye");
    const auto matrix = poseMatrix(relative);
    NativePose out{};
    for (int axis = 0; axis < 3; ++axis)
        for (int k = 0; k < 3; ++k)
            for (int local = 0; local < 3; ++local) out[axis * 3 + k] += anchor[local * 3 + k] * matrix[axis * 4 + local];
    const float position[3] = {relative.position.x, relative.position.y, relative.position.z};
    for (int k = 0; k < 3; ++k) {
        out[9 + k] = anchor[9 + k];
        for (int local = 0; local < 3; ++local) out[9 + k] += anchor[local * 3 + k] * position[local] * unitsPerMetre;
    }
    for (float value : out)
        if (!std::isfinite(value)) throw std::runtime_error("unrepresentable native eye");
    return out;
}
inline NativePose nativeItemPose(XrPosef pose, float unitsPerMetre = 100) {
    return nativeEye({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, pose, unitsPerMetre);
}
inline XrPosef nativePoseMetres(const NativePose& native, float unitsPerMetre = 100) {
    requireRigid(native);
    const float m00 = native[0], m11 = native[4], m22 = native[8], trace = m00 + m11 + m22;
    XrQuaternionf q{};
    if (trace > 0) {
        const float s = 2 * std::sqrt(trace + 1);
        q = {(native[5] - native[7]) / s, (native[6] - native[2]) / s, (native[1] - native[3]) / s, s * .25f};
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2 * std::sqrt(1 + m00 - m11 - m22);
        q = {s * .25f, (native[3] + native[1]) / s, (native[6] + native[2]) / s, (native[5] - native[7]) / s};
    } else if (m11 > m22) {
        const float s = 2 * std::sqrt(1 + m11 - m00 - m22);
        q = {(native[3] + native[1]) / s, s * .25f, (native[7] + native[5]) / s, (native[6] - native[2]) / s};
    } else {
        const float s = 2 * std::sqrt(1 + m22 - m00 - m11);
        q = {(native[6] + native[2]) / s, (native[7] + native[5]) / s, s * .25f, (native[1] - native[3]) / s};
    }
    const float inverse = 1 / unitsPerMetre;
    return {q, {native[9] * inverse, native[10] * inverse, native[11] * inverse}};
}
}
