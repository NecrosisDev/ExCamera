#pragma once
// OpenTrack's UDP output: six Windows little-endian doubles, XYZ then yaw/pitch/roll (degrees).
// This is processed view intent; it may already contain a vendor's eye contribution.
#include "camera_motion.h"
#include <cstdint>
#include <cstring>

namespace fp {
// Keep OpenTrack's usual UDP INPUT port4242 available for Tobii/phone/webcam sources.
inline constexpr unsigned trackingPort = 4243;
inline constexpr uint64_t trackingStaleMs = 250;
struct TrackingSample {
    float rotation[12]{};
    uint64_t received = 0, sequence = 0;
    unsigned generation = 0;
    bool valid = false;
};
inline bool decodeTracking(const void* bytes, size_t length, float* rotation) {
    if (!bytes || length != 6 * sizeof(double)) return false;
    double axes[6];
    std::memcpy(axes, bytes, sizeof axes);
    for (double v : axes)
        if (!std::isfinite(v)) return false;
    // Translation is deliberately unused: the native head/collision still owns position.
    // Upstream's mouse output maps positive yaw right and positive pitch up.
    constexpr double radians = 3.141592653589793 / 180.;
    const float yaw = float(-std::remainder(axes[3], 360.) * radians);
    const float pitch = float(std::remainder(axes[4], 360.) * radians);
    const float roll = float(std::remainder(axes[5], 360.) * radians);
    basis(yaw, pitch, rotation);
    const float c = std::cos(roll), s = std::sin(roll);
    for (int i = 0; i < 3; ++i) {
        const float right = rotation[i], up = rotation[3 + i];
        rotation[i] = c * right + s * up;
        rotation[3 + i] = c * up - s * right;
    }
    rotation[9] = rotation[10] = rotation[11] = 0.f;
    return true;
}
// Compose camera axes, respecting basis(0,0)'s -X,+Y,-Z convention. Alias safe.
inline void composeView(const float* basePose, const float* offset, float* result) {
    float composed[12];
    for (int row = 0; row < 3; ++row)
        for (int i = 0; i < 3; ++i)
            composed[row * 3 + i] = -offset[row * 3] * basePose[i] +
                                    offset[row * 3 + 1] * basePose[3 + i] -
                                    offset[row * 3 + 2] * basePose[6 + i];
    std::memcpy(composed + 9, basePose + 9, 3 * sizeof(float));
    std::memcpy(result, composed, sizeof composed);
}
inline void relativeView(const float* neutral, const float* current, float* result) {
    // current * transpose(neutral), then the canonical camera convention.
    for (int row = 0; row < 3; ++row)
        for (int axis = 0; axis < 3; ++axis) {
            float dot = 0;
            for (int k = 0; k < 3; ++k) dot += current[row * 3 + k] * neutral[axis * 3 + k];
            result[row * 3 + axis] = dot * (axis == 1 ? 1.f : -1.f);
        }
    result[9] = result[10] = result[11] = 0;
}
struct TrackingView {
    float neutral[12]{}, shown[12]{}, correction[12]{};
    uint64_t sequence = 0;
    unsigned generation = 0, reset = 0;
    bool active = false, initialized = false;
    void clear() { *this = {}; }
    // Only once per presentation. Repeated/temporal samples reuse its output.
    void update(const TrackingSample& sample, uint64_t now, float seconds, bool enabled,
                unsigned requestedReset) {
        if (!initialized) {
            basis(0, 0, shown);
            basis(0, 0, correction);
            initialized = true;
        }
        const bool fresh = enabled && sample.valid && now >= sample.received &&
                           now - sample.received <= trackingStaleMs;
        float target[12]{};
        basis(0, 0, target);
        if (fresh) {
            if (!active || generation != sample.generation || reset != requestedReset) {
                std::memcpy(neutral, sample.rotation, sizeof neutral);
                std::memcpy(correction, shown, sizeof correction);
                generation = sample.generation;
                reset = requestedReset;
            }
            relativeView(neutral, sample.rotation, target);
            sequence = sample.sequence;
        }
        active = fresh;
        // No ongoing smoothing of deliberate head motion. Blend only connection/neutral
        // transitions and fallback; provider curves/filtering remain external.
        const float dt = std::isfinite(seconds) && seconds > 0.f ? (std::min)(seconds, .1f) : 0.f;
        if (!fresh)
            blendPose(shown, target, 1.f - std::exp(-dt / .12f), shown, false);
        else {
            float identity[12]{};
            basis(0, 0, identity);
            blendPose(correction, identity, 1.f - std::exp(-dt / .12f), correction, false);
            composeView(target, correction, shown);
        }
    }
};
} // namespace fp
