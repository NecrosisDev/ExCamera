#pragma once
#include "native_pose.h"
#include <cstdint>

namespace fp::xr {
// How a stored depth value maps to normalised device depth: glClipControl range plus
// glDepthRange, and whether the application stored 1-d (reversed depth).
struct DepthConvention {
    enum class Range { negativeOneToOne, zeroToOne } range = Range::negativeOneToOne;
    float rangeNear = 0, rangeFar = 1;
    bool reversed = false;
    bool plausible() const {
        return std::isfinite(rangeNear) && std::isfinite(rangeFar) && rangeNear >= 0 && rangeFar <= 1 && rangeNear < rangeFar;
    }
};
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
// Description of one copied world frame: immutable colour plus matching sampleable depth, with
// everything a reconstruction needs to interpret it. Captured before UI/overlays and before the
// depth clear, from the world target itself, never from a shadow map or an eye attachment.
struct FrameContract {
    int width = 0, height = 0;
    Rect subrect{};
    bool originBottomLeft = true;
    unsigned samples = 1;
    DepthConvention depth{};
    Matrix projection{};
    NativePose view{};
    float unitsPerMetre = 100, clipNear = 0, clipFar = 0;
    uint64_t serial = 0, sceneGeneration = 0, contextGeneration = 0;
    int64_t predictedTime = 0;
    uint32_t colorTexture = 0, depthTexture = 0;
    // The reason this frame cannot be consumed, or null when it can.
    const char* validate() const {
        if (width < 1 || height < 1 || width > 16384 || height > 16384) return "frame dimensions";
        if (subrect.width < 1 || subrect.height < 1 || subrect.x < 0 || subrect.y < 0 ||
            subrect.x + subrect.width > width || subrect.y + subrect.height > height) return "frame subrect";
        if (samples != 1) return "multisampled source needs an explicit resolve";
        if (!depth.plausible()) return "depth convention";
        for (float value : projection)
            if (!std::isfinite(value)) return "projection";
        if (!(projection[0] > 0 && projection[5] > 0 && projection[11] == -1 && projection[15] == 0)) return "projection is not perspective";
        try { requireRigid(view); } catch (const std::exception&) { return "view pose"; }
        if (!std::isfinite(unitsPerMetre) || unitsPerMetre <= 0) return "metre scale";
        if (!(clipNear > 0) || !std::isfinite(clipNear) || (clipFar != 0 && !(clipFar > clipNear))) return "clip range";
        if (!serial || !sceneGeneration || !contextGeneration) return "frame identity";
        if (!colorTexture || !depthTexture) return "missing colour or depth texture";
        return nullptr;
    }
};
}
