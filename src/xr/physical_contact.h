#pragma once
#include "math.h"
#include <cstdint>

namespace fp::xr {
// Velocity of a material point inside a native physics tetra, from its per-step vertex
// velocities. Controller velocity or a ray hit would describe intent, not what the body did.
inline bool contactVelocity(const std::array<XrVector3f, 4>& points, const std::array<XrVector3f, 4>& velocities, XrVector3f contact, XrVector3f& out) {
    for (unsigned i = 0; i < 4; ++i)
        if (!finite(points[i]) || !finite(velocities[i])) return false;
    if (!finite(contact)) return false;
    const auto a = sub(points[1], points[0]), b = sub(points[2], points[0]), c = sub(points[3], points[0]), d = sub(contact, points[0]);
    const double determinant = dot(a, cross(b, c));
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.e-5) return false;
    const float u = float(dot(d, cross(b, c)) / determinant), v = float(dot(a, cross(d, c)) / determinant), w = float(dot(a, cross(b, d)) / determinant);
    const std::array<float, 4> weights{1 - u - v - w, u, v, w};
    out = {};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(weights[i]) || std::abs(weights[i]) > 16) return false;
        out = add(out, scale(velocities[i], weights[i]));
    }
    return finite(out) && length(out) <= 100.f;
}
// One impact per separation per part/target pair, above a speed that distinguishes a strike
// from a push. Sustained overlap never repeats an impact.
class PhysicalContactLedger {
    struct Entry { uint64_t part = 0, target = 0, lastSeen = 0, lastHit = 0; };
    std::array<Entry, 16> entries_{};

public:
    void reset() { entries_ = {}; }
    bool accept(uint64_t part, uint64_t target, uint64_t tick, float relativeSpeed) {
        if (!part || !target || part == target || !std::isfinite(relativeSpeed)) return false;
        auto* entry = &entries_[0];
        for (auto& item : entries_) {
            if (item.part == part && item.target == target) { entry = &item; break; }
            if (item.lastSeen < entry->lastSeen) entry = &item;
        }
        if (entry->part != part || entry->target != target) *entry = {part, target, 0, 0};
        const bool separated = !entry->lastSeen || tick - entry->lastSeen > 100;
        entry->lastSeen = tick;
        if (relativeSpeed < 1.2f || (entry->lastHit && (!separated || tick - entry->lastHit < 200))) return false;
        entry->lastHit = tick;
        return true;
    }
};
}
