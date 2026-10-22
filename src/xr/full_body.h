#pragma once
#include "math.h"
#include <cstdint>

namespace fp::xr {
struct TrackerSample { XrPosef pose{{0, 0, 0, 1}, {}}; bool tracked = false; };
// Waist then left and right feet: the local emulator ABI, and the shape the real body trackers are read
// into (XR_HTCX_vive_tracker_interaction roles waist, left_foot, right_foot; Hands::sampleLower).
struct LowerSample {
    uint32_t size = sizeof(LowerSample), version = 1;
    XrTime time = 0;
    std::array<TrackerSample, 3> trackers{};
};
inline bool lowerValid(const LowerSample& sample, XrTime time) {
    if (sample.size != sizeof(LowerSample) || sample.version != 1 || sample.time != time || time <= 0) return false;
    for (const auto& tracker : sample.trackers)
        if (!tracker.tracked || !valid(tracker.pose)) return false;
    for (unsigned side = 1; side < 3; ++side) {
        const float distance = length(sub(sample.trackers[side].pose.position, sample.trackers[0].pose.position));
        if (distance > 1.8f || distance < .15f) return false;
    }
    return true;
}
}
