#pragma once
#include "full_body.h"

namespace fp::xr {
struct MotionOwner {
    uint64_t root = 0, scene = 0, epoch = 0, stream = 0, sector = 0;
    uint32_t generation = 0;
    bool operator==(const MotionOwner& b) const {
        return root == b.root && scene == b.scene && epoch == b.epoch && stream == b.stream && sector == b.sector && generation == b.generation;
    }
};
struct MotionTarget { XrVector3f direction{}, goal{}; float distance = 0, drop = 0, duck = 0; bool valid = false, moving = false; };
// Turns real walking into root travel intent. The stage-to-world basis is fixed at bind so the
// moving render anchor cannot feed back into intent; a tracking jump recenters instead of walking.
class Locomotion {
    MotionOwner owner_{};
    XrVector3f waist_{}, head_{}, root_{};
    XrQuaternionf basis_{0, 0, 0, 1};
    XrTime time_ = 0;
    float drop_ = 0;
    bool bound_ = false, moving_ = false;

public:
    void reset() { *this = Locomotion{}; }
    MotionTarget step(MotionOwner owner, const LowerSample& lower, XrPosef head, XrVector3f nativeRoot, XrQuaternionf basis, bool roomClear) {
        MotionTarget out{};
        if (!owner.root || !owner.scene || !owner.sector || !lowerValid(lower, lower.time) || !valid(head) || !finite(nativeRoot) || !valid({basis, {}})) { reset(); return out; }
        if (bound_ && (lower.time < time_ || lower.time - time_ > 100000000)) { reset(); return out; }
        if (!bound_ || !(owner == owner_)) {
            float yaw = 0;
            if (!headingYaw(basis, yaw)) { reset(); return out; }
            reset();
            bound_ = true; owner_ = owner;
            waist_ = lower.trackers[0].pose.position; head_ = head.position; root_ = nativeRoot;
            basis_ = yawQuaternion(yaw);
        }
        time_ = lower.time;
        auto offset = sub(lower.trackers[0].pose.position, waist_);
        if (length(offset) > 1.5f) { reset(); return out; }
        const float waistDrop = waist_.y - lower.trackers[0].pose.position.y, headDrop = head_.y - head.position.y;
        const float requested = std::clamp((std::min)(waistDrop, headDrop), 0.f, .4f);
        drop_ = requested < drop_ && !roomClear ? drop_ : requested; // standing up needs headroom
        offset.y = 0;
        const auto target = add(root_, rotate(basis_, offset));
        auto error = sub(target, nativeRoot);
        error.y = 0;
        const float distance = length(error);
        if (!std::isfinite(distance) || distance > 1.5f) { reset(); return out; }
        moving_ = distance > (moving_ ? .025f : .06f);
        out.goal = target; out.valid = true; out.moving = moving_; out.distance = distance;
        out.direction = moving_ ? unit(error) : XrVector3f{};
        out.drop = drop_; out.duck = drop_ / .4f;
        return out;
    }
};
}
