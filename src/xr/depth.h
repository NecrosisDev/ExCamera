#pragma once
#include "frame_contract.h"
#include <limits>
#include <vector>

namespace fp::xr {
// Linearisation is derived from the source projection and the OpenGL depth convention, never
// from a tuned constant: m[10]/m[14] already encode near, far and the infinite-far sentinel.
inline float ndcFromStored(float stored, const DepthConvention& c) {
    if (c.reversed) stored = 1 - stored;
    const float window = (stored - c.rangeNear) / (c.rangeFar - c.rangeNear);
    return c.range == DepthConvention::Range::zeroToOne ? window : window * 2 - 1;
}
inline float storedFromNdc(float ndc, const DepthConvention& c) {
    const float window = c.range == DepthConvention::Range::zeroToOne ? ndc : (ndc + 1) * .5f;
    const float stored = c.rangeNear + window * (c.rangeFar - c.rangeNear);
    return c.reversed ? 1 - stored : stored;
}
// View-space distance along -Z for a perspective matrix; returns +inf for the far sentinel.
inline float viewDepthFromNdc(float ndc, const Matrix& projection) {
    const float denominator = ndc + projection[10];
    if (std::abs(denominator) < 1.e-12f) return std::numeric_limits<float>::infinity();
    return projection[14] / denominator;
}
inline float ndcFromViewDepth(float depth, const Matrix& projection) {
    return (projection[10] * -depth + projection[14]) / depth;
}
inline float metresFromStored(float stored, const FrameContract& frame) {
    return viewDepthFromNdc(ndcFromStored(stored, frame.depth), frame.projection) / frame.unitsPerMetre;
}
// Pixel centre to view-space point at the pixel's stored depth; the image origin follows the
// contract, so a flipped capture is corrected here rather than by the consumer.
inline XrVector3f unproject(int x, int y, float stored, const FrameContract& frame) {
    const float u = (float(x - frame.subrect.x) + .5f) / float(frame.subrect.width);
    float v = (float(y - frame.subrect.y) + .5f) / float(frame.subrect.height);
    if (!frame.originBottomLeft) v = 1 - v;
    const auto& m = frame.projection;
    const float depth = viewDepthFromNdc(ndcFromStored(stored, frame.depth), m);
    // clip.x = m0 x + m8 z with z = -depth, so x = (ndc + m8) depth / m0.
    const float ndcX = u * 2 - 1, ndcY = v * 2 - 1;
    return {(ndcX + m[8]) / m[0] * depth, (ndcY + m[9]) / m[5] * depth, -depth};
}
}
