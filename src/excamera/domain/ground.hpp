#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "collision.hpp"

namespace excamera {

inline constexpr uint32_t ground_support_flag = 0x2;
// Conservative space above supported feet, including body and near-plane clearance.
inline constexpr float ground_margin_cm = 80.f;
inline constexpr std::size_t max_ground_contacts = 16;

struct GroundContact {
    Vec3 point;
    uint32_t flags = 0;
};

struct GroundConstraint {
    float support_height = -std::numeric_limits<float>::infinity();
    bool available = false;
};

// Contact points are in the camera's global level frame. This is a conservative
// support envelope, not a claim that every point is a floor-triangle intersection.
inline GroundConstraint evaluate_ground_contacts(const GroundContact* contacts,
    std::size_t count, Vec3 eye) noexcept {
    GroundConstraint result;
    if (!finite(eye) || (count && !contacts) || count > max_ground_contacts) return result;
    for (std::size_t index = 0; index < count; ++index) {
        const GroundContact& contact = contacts[index];
        if (!(contact.flags & ground_support_flag) || !finite(contact.point)) continue;
        if (std::fabs(contact.point.x - eye.x) > 200.f ||
            std::fabs(contact.point.z - eye.z) > 200.f ||
            std::fabs(contact.point.y - eye.y) > 400.f)
            continue;
        result.support_height = std::max(result.support_height, contact.point.y);
        result.available = true;
    }
    return result;
}

// Limit only a downward boom. Unknown support never authorizes a downward move.
inline float ground_clearance(Vec3 pivot, Vec3 back, float requested,
                              GroundConstraint ground) noexcept {
    if (!detail::valid_ray(pivot, back, requested)) return 0.f;
    if (back.y >= -1e-6f) return requested;
    if (!ground.available || !std::isfinite(ground.support_height)) return 0.f;
    const float allowed = (pivot.y - ground.support_height - ground_margin_cm) / -back.y;
    if (!std::isfinite(allowed) || allowed <= 0.f) return 0.f;
    return std::min(requested, allowed);
}

} // namespace excamera
