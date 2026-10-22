#pragma once
#include "motor_math.h"

namespace fp::xr {
struct EquippedOwner {
    uint64_t root = 0, scene = 0, epoch = 0, stream = 0, weapon = 0, grip = 0;
    uint32_t generation = 0;
    bool operator==(const EquippedOwner& b) const {
        return root == b.root && scene == b.scene && epoch == b.epoch && stream == b.stream && weapon == b.weapon && grip == b.grip && generation == b.generation;
    }
};
struct EquippedInput {
    std::array<XrPosef, 2> controllers{}, hands{};
    std::array<XrVector3f, 2> anchors{}; // authored grip points in weapon space
    std::array<bool, 2> tracked{}, pressed{};
    XrPosef weapon{{0, 0, 0, 1}, {}};
    unsigned primary = 1;
    bool twoHanded = false;
    // The haft: the weapon's long axis through the primary grip, in weapon space, for a second hand
    // to close on a one-handed hold (an empty off hand squeezing near the haft).
    XrVector3f axis{};
    float axisLength = 0;
    bool haftAllowed = false;
};
struct EquippedTarget { XrPosef weapon{{0, 0, 0, 1}, {}}; std::array<XrPosef, 2> hands{}; bool valid = false, twoHanded = false, clamped = false, haft = false; };
inline XrVector3f materialPoint(XrPosef pose, XrVector3f local) { return add(pose.position, rotate(pose.orientation, local)); }
// Desired weapon and hand poses from controllers holding the authored grips. Equipment ownership
// and constraints stay native; a fresh press after release is required before driving.
class EquippedDrive {
    EquippedOwner owner_{};
    bool seen_ = false, bound_ = false;
    std::array<bool, 2> armed_{};
    std::array<XrVector3f, 2> controllerAnchors_{};
    std::array<XrPosef, 2> weaponHands_{};
    XrQuaternionf rotation_{0, 0, 0, 1};
    bool haft_ = false;            // the off hand holds the haft of a one-handed weapon
    XrVector3f haftAnchor_{};      // where it closed, in weapon space
    static constexpr float haftClearance = .08f; // metres from the holding hand before the haft counts
    static constexpr float haftRadius = .06f;    // metres from the haft line a squeezing hand may be

public:
    void reset() { *this = EquippedDrive{}; }
    bool haft() const { return haft_; }
    // A free, squeezing off hand within 6 cm of the haft line (and clear of the primary grip) takes
    // the haft where it is; letting go releases it. Nothing native changes: the game's own one-hand
    // constraint keeps the weapon; the drive only steers the pose with two points instead of one.
    void haftStep(const EquippedInput& in) {
        const unsigned other = 1 - in.primary;
        if (!in.haftAllowed || in.twoHanded || !in.tracked[other] || !valid(in.controllers[other]) || !finite(in.axis) || !(in.axisLength > .1f)) { haft_ = false; return; }
        if (!in.pressed[other]) { haft_ = false; return; }
        if (haft_) return;
        const auto local = rotate(conjugate(in.weapon.orientation), sub(in.controllers[other].position, in.weapon.position));
        const float along = dot(sub(local, in.anchors[in.primary]), in.axis);
        if (std::abs(along) < haftClearance || std::abs(along) > in.axisLength) return; // not clear of the hand that holds it, or past the end
        const auto onAxis = add(in.anchors[in.primary], scale(in.axis, along));
        if (length(sub(local, onAxis)) > haftRadius) return;
        haftAnchor_ = onAxis;
        haft_ = true;
        bound_ = false; // rebind both hands to the weapon at this moment
    }
    EquippedTarget step(EquippedOwner owner, const EquippedInput& in) {
        EquippedTarget out{};
        if (!owner.root || !owner.scene || !owner.weapon || in.primary > 1 || !valid(in.weapon)) { reset(); return out; }
        if (!seen_ || !(owner == owner_)) { reset(); seen_ = true; owner_ = owner; }
        const bool haftBefore = haft_;
        haftStep(in);
        if (haftBefore && !haft_) bound_ = false; // back to one hand: rebind
        const bool twoPoints = in.twoHanded || haft_;
        std::array<XrVector3f, 2> anchors = in.anchors;
        if (haft_) anchors[1 - in.primary] = haftAnchor_;
        const unsigned required = twoPoints ? 3u : (1u << in.primary);
        for (unsigned side = 0; side < 2; ++side) {
            if (!(required & (1u << side))) continue;
            if (!in.tracked[side] || !valid(in.controllers[side]) || !valid(in.hands[side]) || !finite(in.anchors[side])) { reset(); return out; }
            if (!in.pressed[side]) { armed_[side] = true; bound_ = false; }
        }
        for (unsigned side = 0; side < 2; ++side)
            if ((required & (1u << side)) && (!armed_[side] || !in.pressed[side])) return out;
        if (!bound_) {
            rotation_ = multiply(conjugate(in.controllers[in.primary].orientation), in.weapon.orientation);
            for (unsigned side = 0; side < 2; ++side) {
                if (!(required & (1u << side))) continue;
                const auto point = materialPoint(in.weapon, anchors[side]);
                if (length(sub(point, in.controllers[side].position)) > .75f) { reset(); return out; }
                controllerAnchors_[side] = rotate(conjugate(in.controllers[side].orientation), sub(point, in.controllers[side].position));
                weaponHands_[side] = compose(inverse(in.weapon), in.hands[side]);
            }
            bound_ = true;
        }
        auto rotation = multiply(in.controllers[in.primary].orientation, rotation_);
        const auto primaryPoint = materialPoint(in.controllers[in.primary], controllerAnchors_[in.primary]);
        if (twoPoints) {
            const unsigned other = 1 - in.primary;
            const auto span = sub(materialPoint(in.controllers[other], controllerAnchors_[other]), primaryPoint);
            const auto material = sub(anchors[other], anchors[in.primary]);
            const float distance = length(span), spacing = length(material);
            if (spacing < .04f || spacing > 1.2f || distance < .04f || distance > 1.4f || std::abs(distance - spacing) > .25f) { reset(); return out; }
            rotation = multiply(directionRotation(rotate(rotation, material), span), rotation);
            out.clamped = std::abs(distance - spacing) > .001f;
        }
        rotation = multiply(limitMotorRotation(multiply(rotation, conjugate(in.weapon.orientation)), 1.f), in.weapon.orientation);
        auto point = primaryPoint;
        const auto delta = sub(point, materialPoint(in.weapon, anchors[in.primary]));
        const float distance = length(delta);
        if (distance > .35f) { point = add(materialPoint(in.weapon, anchors[in.primary]), scale(delta, .35f / distance)); out.clamped = true; }
        out.weapon = {rotation, sub(point, rotate(rotation, anchors[in.primary]))};
        for (unsigned side = 0; side < 2; ++side)
            if (required & (1u << side)) out.hands[side] = compose(out.weapon, weaponHands_[side]);
        out.valid = valid(out.weapon);
        out.twoHanded = in.twoHanded;
        out.haft = haft_;
        return out;
    }
};
}
