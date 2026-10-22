#pragma once
#include "math.h"
#include <array>
#include <cstdint>

namespace fp::xr {
// The 26 XR_EXT_hand_tracking joints of one hand in LOCAL space. The character's skeleton has
// one rigid bone per hand, so joints drive that bone through the palm and turn finger shape
// into the grasp and attack inputs; no finger geometry exists to pose.
constexpr unsigned handJointCount = 26;
enum HandJoint : unsigned { palmJoint = 0, wristJoint = 1, thumbTipJoint = 5, indexTipJoint = 10, middleTipJoint = 15, ringTipJoint = 20, littleTipJoint = 25 };
struct HandJoints {
    std::array<XrPosef, handJointCount> pose{};
    std::array<float, handJointCount> radius{};
    uint32_t validMask = 0; // bit per joint with a valid, tracked pose
    bool tracked = false;   // true only when a hand tracker produced these
    bool valid(unsigned j) const { return j < handJointCount && ((validMask >> j) & 1u); }
};
constexpr uint32_t allHandJoints = (1u << handJointCount) - 1;

// Hand axes in the palm's frame: fingers along -z, palm normal -y (into the palm), thumb +x on
// the right hand and -x on the left, matching the extension's joint conventions.
inline XrVector3f fingerBase(unsigned finger, bool right) {
    const float side = right ? 1.f : -1.f;
    constexpr float spread[5] = {.035f, .025f, .008f, -.010f, -.028f};
    return {side * spread[finger], 0, finger == 0 ? -.01f : -.04f};
}

// What a controller hand is doing, for the inferred fingers: each pose is a curl of the fingers
// (0 straight, 1 closed on the palm) and of the thumb, so a held hilt shows a wrapped fist with the
// thumb over, a carried object a looser hold, a strapped shield a flat hand, a dead hand a slack half-curl.
enum class HandPose { open, fist, hilt, hold, flat, limp };
struct HandCurl { float fingers, thumb; };
inline HandCurl handCurl(HandPose pose) {
    switch (pose) {
    case HandPose::fist: return {1.f, .9f};
    case HandPose::hilt: return {.8f, .85f};
    case HandPose::hold: return {.55f, .6f};
    case HandPose::flat: return {.05f, 0.f};
    case HandPose::limp: return {.35f, .3f};
    default: return {0.f, 0.f};
    }
}
// A plausible hand from a controller grip pose: fingers extended when open, bent joint by joint
// towards the palm as the curl rises (the thumb bends across the palm instead). Joint radii are a
// typical adult's. All poses share the grip orientation.
// itemRadius (metres): when the hand holds something of that thickness, the fingers wrap it, closing as far as
// their length around it allows instead of to the pose's fixed curl.
inline HandJoints inferJoints(XrPosef grip, HandPose pose, bool right, float itemRadius = 0) {
    HandJoints h{};
    if (!valid(grip)) return h;
    h.pose[palmJoint] = grip;
    h.pose[wristJoint] = compose(grip, {{0, 0, 0, 1}, {0, 0, .06f}});
    HandCurl curl = handCurl(pose);
    if ((pose == HandPose::hilt || pose == HandPose::hold) && itemRadius > 0) {
        constexpr float fingerLength = .09f, fingerRadius = .009f, fullCurl = 2.2f;
        const float wrap = fingerLength / (itemRadius + fingerRadius) / fullCurl; // the arc the fingers can lay round the item
        curl.fingers = (std::min)(1.f, (std::max)(.3f, wrap));
        curl.thumb = (std::min)(1.f, curl.fingers + .1f);
    }
    const float side = right ? 1.f : -1.f;
    for (unsigned finger = 0; finger < 5; ++finger) {
        const XrVector3f root = fingerBase(finger, right);
        const float reach = finger == 0 ? .06f : .09f;
        const unsigned first = finger == 0 ? 2 : 1 + finger * 5, count = finger == 0 ? 4 : 5; // the thumb has no intermediate joint
        const float bend = (finger == 0 ? curl.thumb : curl.fingers) * 2.2f / float(count - 1); // radians per joint at full curl
        XrVector3f at = root;
        float angle = 0;
        for (unsigned k = 0; k < count; ++k) { // metacarpal, proximal, (intermediate,) distal, tip
            if (k) {
                angle += bend;
                const float step = reach / float(count - 1);
                // fingers fold down (-y) while running forward (-z); the thumb folds across the palm (towards the fingers)
                const XrVector3f dir = finger == 0 ? XrVector3f{-side * std::sin(angle), -.35f * std::sin(angle), -std::cos(angle)} : XrVector3f{0, -std::sin(angle), -std::cos(angle)};
                at = add(at, scale(unit(dir, {0, 0, -1}), step));
            }
            const unsigned j = first + k;
            h.pose[j] = compose(grip, {{0, 0, 0, 1}, at});
            h.radius[j] = .009f;
        }
    }
    h.radius[palmJoint] = .03f;
    h.radius[wristJoint] = .025f;
    h.validMask = allHandJoints;
    h.tracked = false;
    return h;
}
inline HandJoints inferJoints(XrPosef grip, bool closed, bool right) { return inferJoints(grip, closed ? HandPose::fist : HandPose::open, right); }

// Distance between two joint origins, or a large value when either is missing.
inline float jointGap(const HandJoints& h, unsigned a, unsigned b) {
    if (!h.valid(a) || !h.valid(b)) return 1e9f;
    return length(sub(h.pose[a].position, h.pose[b].position));
}
// Fingers curled onto the palm: the four fingertips all within reach of the palm joint. A gesture
// already held keeps a wider threshold, so a hand resting near the edge does not chatter.
inline bool fist(const HandJoints& h, bool held = false) {
    if (!h.valid(palmJoint)) return false;
    for (unsigned tip : {indexTipJoint, middleTipJoint, ringTipJoint, littleTipJoint})
        if (jointGap(h, palmJoint, tip) > (held ? .07f : .055f)) return false;
    return true;
}
// A hand closing on something it holds: the four tips drawn in but kept off the palm by the thing,
// and the thumb across the fingers. Looser than a fist, so a tracked grab lands on a real object.
inline bool closing(const HandJoints& h, bool held = false) {
    if (!h.valid(palmJoint)) return false;
    for (unsigned tip : {indexTipJoint, middleTipJoint, ringTipJoint, littleTipJoint})
        if (jointGap(h, palmJoint, tip) > (held ? .10f : .08f)) return false;
    return jointGap(h, thumbTipJoint, 12) < (held ? .075f : .06f); // the thumb over the middle finger's base
}
// Thumb and index tips touching, with the other fingers free.
inline bool pinch(const HandJoints& h, bool held = false) { return jointGap(h, thumbTipJoint, indexTipJoint) < (held ? .03f : .02f) && !fist(h, held); }
}
