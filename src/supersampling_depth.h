#pragma once

#include <cmath>
#include <cstring>
#include <limits>

namespace fp {

// Native 0x1DD50 builds this perspective depth column; 0x18C70 combines it
// with each sampled view. Reducing near changes P14, so an authored addition
// to P10 must change by the same ratio to retain its world-space depth effect.
// XY jitter and sector cropping are independent and remain byte-for-byte intact.
//
// farPlane == 0 is the native infinite-far sentinel. For finite far, the same
// sample matrix supplies its unbiased P10 through P14 / far - 1. The current
// camera projection can belong to a later temporal sample and is not a baseline.
//
// During the terrain pass, entryP10 includes any enclosing bias. Only the
// terrain-added part also receives distanceRatio = Dclose^2 / Dstock^2; its native
// 100 / squared-camera-target-distance spread otherwise grows when the camera moves
// from its stock orbit to the eye. Other authored depth effects use only the
// near coefficient ratio. The renderer adapter owns view/pass eligibility.
//
// Returns true only when P10 changes. A false result leaves a byte-identical
// copy of input in output, including invalid matrices. Exact in-place use is
// supported; no native matrix or persistent state is borrowed or modified here.
inline bool correctSampleDepth(const float input[16], float stockNear, float farPlane, bool terrainActive,
                               float terrainEntryP10, float terrainDistanceRatio, float output[16]) {
    if (!input || !output) return false;
    if (input != output) std::memmove(output, input, 16 * sizeof(float));

    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(output[i])) return false;
    if (!std::isfinite(stockNear) || stockNear <= 0.f || !std::isfinite(farPlane) || farPlane < 0.f ||
        (farPlane != 0.f && farPlane <= stockNear))
        return false;
    // Only a perspective depth/w column is understood. Off-axis/cropped XY
    // terms are allowed, but orthographic and oblique-depth projections are not.
    if (output[0] == 0.f || output[5] == 0.f || output[2] != 0.f || output[6] != 0.f ||
        output[3] != 0.f || output[7] != 0.f || output[11] != -1.f || output[15] != 0.f ||
        output[14] >= 0.f)
        return false;
    if (terrainActive && (!std::isfinite(terrainEntryP10) || !std::isfinite(terrainDistanceRatio) ||
                          terrainDistanceRatio <= 0.f))
        return false;

    const double depthTranslation = output[14], farDistance = farPlane, nativeNear = stockNear;
    const double stockTranslation =
        farPlane == 0.f ? -2.0 * nativeNear : -2.0 * farDistance * nativeNear / (farDistance - nativeNear);
    double scale = depthTranslation / stockTranslation;
    if (!std::isfinite(scale) || scale <= 0.0) return false;

    constexpr double identityTolerance = 2.0 * std::numeric_limits<float>::epsilon();
    const bool stockCoefficient = std::fabs(scale - 1.0) <= identityTolerance;
    const bool stockDistance = !terrainActive ||
                               std::fabs(double(terrainDistanceRatio) - 1.0) <= identityTolerance;
    if (stockCoefficient && stockDistance) return false;
    if (stockCoefficient) scale = 1.0;

    const double baseValue = farPlane == 0.f ? -1.0 : depthTranslation / farDistance - 1.0;
    constexpr double maxFinite = (std::numeric_limits<float>::max)();
    if (!std::isfinite(baseValue) || baseValue < -maxFinite) return false;
    float base = static_cast<float>(baseValue);
    // Native computes -(far + near)/(far - near) directly. Reconstructing it
    // from rounded P14 can differ by an ULP; do not invent a bias on ordinary
    // opaque draws or on the centre sample because of that rounding alone.
    const auto roundingOnly = [](float actual, float reference) {
        if (actual == reference) return true;
        const float direction = actual < reference ? -std::numeric_limits<float>::infinity()
                                                   : std::numeric_limits<float>::infinity();
        const float limit = std::nextafter(std::nextafter(reference, direction), direction);
        return actual < reference ? actual >= limit : actual <= limit;
    };
    const float original = output[10];
    double adjustedDelta = 0.0;
    if (terrainActive) {
        double ambient = double(terrainEntryP10) - base;
        const double terrain = double(original) - terrainEntryP10;
        const bool entryIsUnbiased = roundingOnly(terrainEntryP10, base);
        if (entryIsUnbiased && roundingOnly(original, terrainEntryP10)) return false;
        if (entryIsUnbiased) {
            // The pass entry is the native unbiased value in this case.
            base = terrainEntryP10;
            ambient = 0.0;
        }
        adjustedDelta = scale * (ambient + terrain * double(terrainDistanceRatio));
    } else {
        const double authored = double(original) - base;
        if (roundingOnly(original, base)) return false;
        adjustedDelta = scale * authored;
    }

    const double corrected = double(base) + adjustedDelta;
    if (!std::isfinite(corrected) || std::fabs(corrected) > maxFinite) return false;
    const float candidate = static_cast<float>(corrected);
    if (candidate == original) return false;
    output[10] = candidate;
    return true;
}

} // namespace fp
