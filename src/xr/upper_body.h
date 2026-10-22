#pragma once
#include "math.h"
#include "hand_joints.h"
#include <cstdint>

namespace fp::xr {
// Grip pose with the grasp button, plus the controller's stick, trigger and menu button so
// walking, attacking and the game's menu need no desktop input.
struct HandSample {
    XrPosef grip{{0, 0, 0, 1}, {}};
    bool tracked = false, pressed = false;
    XrVector2f stick{};
    bool stickActive = false, trigger = false, menu = false;
    HandJoints joints{}; // tracked joints, or joints inferred from the grip and grasp
    XrPosef aim{{0, 0, 0, 1}, {}}; // the controller's aim pose: a ray along -z for pointing at panels
    bool aimTracked = false;
    bool kick = false;             // the stick pressed in: the VR kick
};
struct BodySample {
    XrPosef head{{0, 0, 0, 1}, {}};
    std::array<HandSample, 2> hands{};
    XrTime time = 0;
    uint64_t epoch = 0; // tracking-space generation; a change invalidates every derived hold
    bool focused = false;
};
// The menu held in the left hand: a panel just above and ahead of the palm, turned to face the head
// (its +z towards the eyes, its up near the world's), while that hand is tracked. The host draws the
// game's UI there and the right hand's pointer aims at the same pose, so both read this one function.
constexpr float handPanelWidth = .42f;
inline bool handPanelPose(const BodySample& s, XrPosef& out) {
    const auto& hand = s.hands[0];
    if (!hand.tracked || !valid(hand.grip) || !valid(s.head)) return false;
    const XrVector3f at = add(hand.grip.position, rotate(hand.grip.orientation, {0, .07f, -.12f}));
    const XrVector3f toHead = sub(s.head.position, at);
    if (length(toHead) < .1f) return false;
    const XrVector3f z = unit(toHead, {0, 0, 1}), x = unit(cross({0, 1, 0}, z), {1, 0, 0}), y = cross(z, x);
    // the rotation whose columns are x, y, z
    const float m00 = x.x, m11 = y.y, m22 = z.z, trace = m00 + m11 + m22;
    XrQuaternionf q{};
    if (trace > 0) {
        const float s4 = std::sqrt(trace + 1) * 2;
        q = {(y.z - z.y) / s4, (z.x - x.z) / s4, (x.y - y.x) / s4, s4 / 4};
    } else if (m00 > m11 && m00 > m22) {
        const float s4 = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {s4 / 4, (y.x + x.y) / s4, (z.x + x.z) / s4, (y.z - z.y) / s4};
    } else if (m11 > m22) {
        const float s4 = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(y.x + x.y) / s4, s4 / 4, (z.y + y.z) / s4, (z.x - x.z) / s4};
    } else {
        const float s4 = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(z.x + x.z) / s4, (z.y + y.z) / s4, s4 / 4, (x.y - y.x) / s4};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f)) return false;
    out = {{q.x / n, q.y / n, q.z / n, q.w / n}, at};
    return valid(out);
}
struct Arm { XrVector3f shoulder{}, elbow{}, wrist{}; XrQuaternionf orientation{0, 0, 0, 1}; bool valid = false, clamped = false; };
struct UpperBody { XrPosef chest{{0, 0, 0, 1}, {}}; std::array<Arm, 2> arms{}; bool valid = false; };

// Two-bone IK with a pole hint. Reach is clamped to the chain so the elbow stays representable.
inline Arm solveArm(XrVector3f shoulder, XrVector3f wrist, XrVector3f pole, float upper, float lower) {
    Arm arm{};
    if (!finite(shoulder) || !finite(wrist) || !finite(pole) || !(upper > .05f && upper < .8f && lower > .05f && lower < .8f)) return arm;
    const auto delta = sub(wrist, shoulder);
    const float distance = length(delta);
    if (!std::isfinite(distance) || distance > 2) return arm;
    const auto direction = unit(delta);
    const float reach = std::clamp(distance, std::abs(upper - lower) + .001f, upper + lower - .001f);
    auto bend = sub(pole, scale(direction, dot(pole, direction)));
    if (length(bend) < .001f) bend = cross(direction, std::abs(direction.y) < .9f ? XrVector3f{0, 1, 0} : XrVector3f{1, 0, 0});
    const float along = (upper * upper - lower * lower + reach * reach) / (2 * reach);
    arm.shoulder = shoulder;
    arm.wrist = add(shoulder, scale(direction, reach));
    arm.elbow = add(add(shoulder, scale(direction, along)), scale(unit(bend), std::sqrt((std::max)(0.f, upper * upper - along * along))));
    arm.valid = true;
    arm.clamped = std::abs(reach - distance) > .00001f;
    return arm;
}
// Head and two grips do not observe the torso; the chest hangs below the head at its yaw.
inline UpperBody solveUpper(const BodySample& sample, float upper = .30f, float lower = .27f) {
    UpperBody result{};
    float yaw = 0;
    if (!sample.focused || !valid(sample.head) || !headingYaw(sample.head.orientation, yaw)) return result;
    result.chest = {yawQuaternion(yaw), add(sample.head.position, {0, -.24f, .02f})};
    result.valid = true;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& hand = sample.hands[i];
        if (!hand.tracked || !valid(hand.grip)) continue;
        const float side = i ? 1.f : -1.f;
        const auto shoulder = add(result.chest.position, rotate(result.chest.orientation, {side * .19f, .06f, 0}));
        const auto pole = rotate(result.chest.orientation, {side * .65f, -.75f, .2f});
        auto& arm = result.arms[i];
        arm = solveArm(shoulder, hand.grip.position, pole, upper, lower);
        arm.orientation = hand.grip.orientation;
    }
    return result;
}

struct InteractionOwner {
    uint64_t scene = 0, root = 0, generation = 0;
    bool operator==(const InteractionOwner& b) const { return scene == b.scene && root == b.root && generation == b.generation; }
};
struct ItemTarget {
    uint64_t object = 0;
    XrPosef pose{{0, 0, 0, 1}, {}};
    XrVector3f anchor{}, linearVelocity{};
    bool held = false, released = false, cancelled = false, twoHanded = false;
};
// Produces desired item poses from a bound grip; physics and picking owners decide what is accepted.
// The bind keeps the accepted material point and grip-to-item rotation so off-centre grabs hold still.
class HandHold {
    InteractionOwner owner_{};
    uint64_t epoch_ = 0, object_ = 0;
    unsigned hand_ = 0;
    XrPosef offset_{{0, 0, 0, 1}, {}};
    XrVector3f localPoint_{}, gripPoint_{}, last_{}, secondaryDirection_{};
    XrQuaternionf secondaryOffset_{0, 0, 0, 1}, lastOrientation_{0, 0, 0, 1};
    XrTime lastTime_ = 0;
    bool armed_[2]{false, false}, secondary_ = false; // a grip must be seen released before it may bind

public:
    void reset() { object_ = 0; secondary_ = false; lastTime_ = 0; armed_[0] = armed_[1] = false; }
    bool holding() const { return object_ != 0; }
    unsigned hand() const { return hand_; }
    bool bind(const BodySample& sample, InteractionOwner owner, unsigned hand, uint64_t object, XrPosef pose, XrVector3f acceptedPoint) {
        if (hand > 1 || object_ || !object || !owner.scene || !owner.root || !sample.focused) return false;
        const auto& grip = sample.hands[hand];
        if (!grip.tracked || !grip.pressed || !armed_[hand] || !valid(pose) || !finite(acceptedPoint) || !valid(grip.grip)) return false;
        if (length(sub(acceptedPoint, grip.grip.position)) > .35f) return false;
        owner_ = owner; epoch_ = sample.epoch; object_ = object; hand_ = hand;
        localPoint_ = rotate(conjugate(pose.orientation), sub(acceptedPoint, pose.position));
        gripPoint_ = rotate(conjugate(grip.grip.orientation), sub(acceptedPoint, grip.grip.position));
        offset_ = compose(inverse(grip.grip), pose);
        last_ = pose.position; lastTime_ = sample.time; secondary_ = false;
        return true;
    }
    ItemTarget step(const BodySample& sample, InteractionOwner owner) {
        ItemTarget out{};
        out.object = object_;
        for (unsigned i = 0; i < 2; ++i)
            if (!sample.hands[i].pressed) armed_[i] = true;
        if (!object_) return out;
        const auto& hand = sample.hands[hand_];
        const bool bad = sample.epoch != epoch_ || !(owner == owner_) || !sample.focused || !hand.tracked || !valid(hand.grip) ||
                         sample.time <= lastTime_ || sample.time - lastTime_ > 100000000;
        if (bad || !hand.pressed) { out.released = true; out.cancelled = bad; reset(); return out; }
        auto pose = compose(hand.grip, offset_);
        const unsigned other = 1 - hand_;
        const auto& second = sample.hands[other];
        const bool secondWanted = armed_[other] && second.tracked && second.pressed && valid(second.grip) &&
                                  length(sub(second.grip.position, pose.position)) < .5f &&
                                  length(sub(second.grip.position, hand.grip.position)) > .08f;
        if (secondWanted) {
            const auto direction = unit(sub(second.grip.position, hand.grip.position));
            if (!secondary_) { secondaryDirection_ = direction; secondaryOffset_ = pose.orientation; secondary_ = true; }
            pose.orientation = multiply(directionRotation(secondaryDirection_, direction), secondaryOffset_);
            out.twoHanded = true;
        } else if (secondary_) {
            pose.orientation = lastOrientation_;
            offset_.orientation = multiply(conjugate(hand.grip.orientation), pose.orientation);
            secondary_ = false;
        }
        out.anchor = add(hand.grip.position, rotate(hand.grip.orientation, gripPoint_));
        pose.position = sub(out.anchor, rotate(pose.orientation, localPoint_));
        const float dt = float(sample.time - lastTime_) * 1.e-9f;
        const auto delta = sub(pose.position, last_);
        if (!valid(pose) || length(delta) > .25f) { out.released = true; out.cancelled = true; reset(); return out; }
        out.pose = pose; out.held = true;
        out.linearVelocity = scale(delta, 1 / dt);
        const float speed = length(out.linearVelocity);
        if (speed > 3) out.linearVelocity = scale(out.linearVelocity, 3 / speed);
        last_ = pose.position; lastOrientation_ = pose.orientation; lastTime_ = sample.time;
        return out;
    }
};
}
