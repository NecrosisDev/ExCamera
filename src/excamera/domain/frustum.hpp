#pragma once

#include <algorithm>
#include <cmath>

#include "collision.hpp"

namespace excamera {

struct CameraFrustum {
    Pose orientation;
    float vertical_fov = 82.f;
    float aspect = 16.f / 9.f;
    float near_plane = 3.f;
};

// Two refinements after the initial corner evaluation. Failure to find a
// verified candidate within that budget fails inward rather than guessing.
inline constexpr int frustum_refinement_steps = 2;

namespace detail {

inline bool valid_frustum(const CameraFrustum& frustum) noexcept {
    const Pose& pose = frustum.orientation;
    return finite(pose) && valid_ray({}, pose.right, 1.f) && valid_ray({}, pose.up, 1.f) &&
        valid_ray({}, pose.back, 1.f) && std::fabs(dot(pose.right, pose.up)) <= 0.002f &&
        std::fabs(dot(pose.right, pose.back)) <= 0.002f && std::fabs(dot(pose.up, pose.back)) <= 0.002f &&
        std::isfinite(frustum.vertical_fov) && frustum.vertical_fov > 0.f && frustum.vertical_fov < 170.f &&
        std::isfinite(frustum.aspect) && frustum.aspect > 0.f &&
        std::isfinite(frustum.near_plane) && frustum.near_plane >= 0.f;
}

} // namespace detail

// Checks the central boom and the four near-plane corners against static level
// geometry. Every accepted candidate is verified at its actual near-plane size.
template <typename Probe>
float frustum_clearance(Vec3 pivot, const CameraFrustum& frustum, float requested,
                        Probe&& probe) noexcept {
    if (!detail::valid_frustum(frustum) || !detail::valid_ray(pivot, frustum.orientation.back, requested))
        return 0.f;
    const float vertical = std::tan(frustum.vertical_fov * pi / 360.f);
    const float horizontal = vertical * frustum.aspect;
    if (!std::isfinite(vertical) || !std::isfinite(horizontal) || vertical <= 0.f || horizontal <= 0.f)
        return 0.f;
    const Pose& orientation = frustum.orientation;
    const auto central_probe = [&](float length) { return probe(pivot, orientation.back, length); };
    float candidate = segment_clearance(pivot, orientation.back, requested, central_probe);
    if (frustum.near_plane <= 1e-6f) return candidate;
    if (candidate <= frustum.near_plane) return 0.f;

    for (int pass = 0; pass <= frustum_refinement_steps; ++pass) {
        if (candidate <= frustum.near_plane) return 0.f;
        float next = candidate;
        bool obstructed = false;
        for (int corner = 0; corner < 4; ++corner) {
            const float x = (corner & 1) ? horizontal : -horizontal;
            const float y = (corner & 2) ? vertical : -vertical;
            const Vec3 point = pivot + orientation.back * (candidate - frustum.near_plane) +
                orientation.right * (x * frustum.near_plane) + orientation.up * (y * frustum.near_plane);
            const Vec3 delta = point - pivot;
            const float distance = length(delta);
            const Vec3 direction = normalized(delta);
            if (!detail::valid_ray(pivot, direction, distance)) return 0.f;
            const auto corner_probe = [&](float length) { return probe(pivot, direction, length); };
            const float clearance = segment_clearance(pivot, direction, distance, corner_probe);
            if (clearance >= distance) continue;
            obstructed = true;
            const float fraction = clearance / distance;
            next = std::min(next, frustum.near_plane + (candidate - frustum.near_plane) * fraction);
        }
        if (!obstructed) return candidate;
        if (!std::isfinite(next) || next < 0.f) return 0.f;
        if (next >= candidate)
            next = std::max(frustum.near_plane, candidate - std::max(1.f, candidate * 0.125f));
        candidate = next;
    }
    return 0.f;
}

} // namespace excamera
