#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "math.hpp"

namespace excamera {

inline constexpr float clearance_margin_cm = 8.f;

// Distances are measured from the respective endpoint, in centimetres.
// Infinity means no hit. Both directions are needed for one-sided triangles.
struct SegmentHits {
    float from_origin = std::numeric_limits<float>::infinity();
    float from_end = std::numeric_limits<float>::infinity();
    bool failed = false;
};

namespace detail {

inline bool valid_ray(Vec3 origin, Vec3 direction, float length) noexcept {
    const float magnitude_squared = dot(direction, direction);
    return finite(origin) && std::isfinite(magnitude_squared) &&
        std::fabs(magnitude_squared - 1.f) <= 0.002f &&
        std::isfinite(length) && length > 0.f && finite(origin + direction * length);
}

} // namespace detail

// One bounded probe supplies the nearest hit from each endpoint. The domain
// combines them into a conservative clearance without native calls or allocation.
template <typename Probe>
float segment_clearance(Vec3 origin, Vec3 direction, float length, Probe&& probe) noexcept {
    if (!detail::valid_ray(origin, direction, length)) return 0.f;
    const SegmentHits hits = probe(length);
    if (hits.failed) return 0.f;
    float nearest = length;
    bool obstructed = false;
    if (std::isnan(hits.from_origin) || std::isnan(hits.from_end)) return 0.f;
    if (std::isfinite(hits.from_origin)) {
        if (hits.from_origin < 0.f || hits.from_origin > length) return 0.f;
        nearest = std::min(nearest, hits.from_origin);
        obstructed = true;
    }
    if (std::isfinite(hits.from_end)) {
        if (hits.from_end < 0.f || hits.from_end > length) return 0.f;
        nearest = std::min(nearest, length - hits.from_end);
        obstructed = true;
    }
    return obstructed ? std::max(nearest - clearance_margin_cm, 0.f) : length;
}

} // namespace excamera
