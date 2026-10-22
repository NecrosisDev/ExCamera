#pragma once
#include "math.h"

namespace fp::xr {
// Eye placement policy. The runtime's per-eye poses carry the headset's real IPD; an
// override re-centres both eyes on the tracked head along its right axis. Either way the
// eyes are rigid with the head: no smoothing, lag or camera-side input exists here.
struct EyeLayout {
    float ipdOverrideMetres = 0;   // 0 keeps the runtime's eye poses
    float unitsPerMetre = 100;     // game units per metre; 100 matches ExCamera's centimetre settings
    float gripAngleDegrees = 0;    // pitch added to a controller's grip pose, so a held hilt sits as the player holds it
    float hapticStrength = 1;      // scales every pulse, 0 silences them
    float panelWidthMetres = 1.2f, panelDistanceMetres = 1.5f; // the head-locked UI quad
    bool frameTimeReadout = false; // keep the status line up with the frame time on it
    bool relaxedGrab = true;       // tracked hands grasp by closing on a thing, not only by a tight fist
    bool handPanel = true;         // the open UI is held in the left hand (handPanelPose) instead of in front of the head
    static constexpr float fallbackIpdMetres = .064f;
    bool plausible() const {
        return std::isfinite(ipdOverrideMetres) && ipdOverrideMetres >= 0 && ipdOverrideMetres <= .1f &&
               std::isfinite(unitsPerMetre) && unitsPerMetre > 0 && std::isfinite(gripAngleDegrees) && std::fabs(gripAngleDegrees) <= 90 &&
               std::isfinite(hapticStrength) && hapticStrength >= 0 && hapticStrength <= 2 &&
               std::isfinite(panelWidthMetres) && panelWidthMetres >= .3f && panelWidthMetres <= 4 &&
               std::isfinite(panelDistanceMetres) && panelDistanceMetres >= .5f && panelDistanceMetres <= 5;
    }
};
// Runtime-reported eye poses are trusted only when they are rigid with the head; otherwise the
// fallback separation applies so a broken runtime cannot collapse or explode the stereo pair.
inline std::array<XrPosef, 2> eyePoses(XrPosef head, const std::array<XrPosef, 2>& runtimeEyes, EyeLayout policy) {
    if (!policy.plausible() || !valid(head)) throw std::runtime_error("invalid eye layout");
    float separation = policy.ipdOverrideMetres;
    if (separation <= 0) {
        const bool rigid = valid(runtimeEyes[0]) && valid(runtimeEyes[1]) &&
                           length(sub(runtimeEyes[0].position, head.position)) < .1f &&
                           length(sub(runtimeEyes[1].position, head.position)) < .1f;
        if (rigid) return runtimeEyes;
        separation = EyeLayout::fallbackIpdMetres;
    }
    std::array<XrPosef, 2> out{};
    for (unsigned i = 0; i < 2; ++i) {
        const float side = i ? .5f : -.5f;
        const XrQuaternionf orientation = valid(runtimeEyes[i]) ? runtimeEyes[i].orientation : head.orientation;
        out[i] = {orientation, add(head.position, rotate(head.orientation, {side * separation, 0, 0}))};
    }
    return out;
}
}
