#pragma once
#include "motor_math.h"

namespace fp::xr {
struct BodyDriverOwner {
    uint64_t root = 0, scene = 0, sector = 0, epoch = 0, stream = 0;
    uint32_t generation = 0;
    bool operator==(const BodyDriverOwner& b) const {
        return root == b.root && scene == b.scene && sector == b.sector && epoch == b.epoch && stream == b.stream && generation == b.generation;
    }
};
enum Joint : unsigned { head, leftHand, rightHand, waist, leftFoot, rightFoot, jointCount };
struct BodyDriverPose { std::array<XrPosef, jointCount> joints{}; bool valid = false; };
// Desired joint poses only: the physical character's native motors and contacts solve achieved
// motion. Tracked deltas are applied to the bind-time physical pose so achieved drift never
// feeds back into the tracking origin.
class BodyPoseDriver {
    BodyDriverOwner owner_{};
    std::array<XrPosef, jointCount> stage_{}, native_{};
    XrQuaternionf basis_{0, 0, 0, 1};
    bool bound_ = false;

public:
    void reset() { *this = BodyPoseDriver{}; }
    BodyDriverPose step(BodyDriverOwner owner, const std::array<XrPosef, jointCount>& stage, const std::array<XrPosef, jointCount>& physical, XrQuaternionf basis) {
        BodyDriverPose out{};
        if (!owner.root || !owner.scene || !owner.sector || !valid(XrPosef{basis, {}})) { reset(); return out; }
        for (unsigned i = 0; i < jointCount; ++i)
            if (!valid(stage[i]) || !valid(physical[i])) { reset(); return out; }
        if (!bound_ || !(owner == owner_)) { stage_ = stage; native_ = physical; basis_ = basis; owner_ = owner; bound_ = true; }
        if (length(sub(stage[waist].position, stage_[waist].position)) > 1.5f) return out;
        for (unsigned i = 0; i < jointCount; ++i) {
            const auto delta = sub(stage[i].position, stage_[i].position);
            if (length(delta) > 2.f) return {};
            out.joints[i].position = add(native_[i].position, rotate(basis_, delta));
            const auto rotation = multiply(multiply(basis_, stage[i].orientation), conjugate(multiply(basis_, stage_[i].orientation)));
            out.joints[i].orientation = multiply(rotation, native_[i].orientation);
            if (!valid(out.joints[i])) return {};
        }
        out.valid = true;
        return out;
    }
};
// Headset ownership survives a lost foot or hand: retiring body targets must not add the moving
// physical head back into a still-valid headset camera.
class BodyCameraDriver {
    BodyDriverOwner owner_{};
    XrPosef stage_{{0, 0, 0, 1}, {}}, native_{{0, 0, 0, 1}, {}};
    XrQuaternionf basis_{0, 0, 0, 1};
    bool bound_ = false;

public:
    XrPosef step(BodyDriverOwner owner, XrPosef stageHead, XrPosef nativeHead, XrQuaternionf basis, bool admitBind) {
        if (!valid(stageHead) || !valid(nativeHead)) throw std::runtime_error("body camera pose");
        if (bound_ && !(owner == owner_)) bound_ = false;
        if (!bound_) {
            if (!admitBind) return nativeHead;
            owner_ = owner; stage_ = stageHead; native_ = nativeHead; basis_ = basis; bound_ = true;
        }
        const auto delta = multiply(multiply(basis_, stageHead.orientation), conjugate(multiply(basis_, stage_.orientation)));
        return {multiply(delta, native_.orientation), add(native_.position, rotate(basis_, sub(stageHead.position, stage_.position)))};
    }
};
}
