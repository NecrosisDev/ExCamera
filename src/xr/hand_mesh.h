#pragma once
#include "hand_joints.h"
#include <vector>

namespace fp::xr {
// Our own articulated hand: a rounded, tapered sleeve per finger segment (an eight-sided prism
// between two rings with domed ends) and a rounded palm slab across the knuckles, built from joint
// positions already expressed in the drawn bone's frame. Nothing here comes from a game asset.
// The vertex layout matches the native body stream the mod already feeds.
struct HandVertex { float position[3], normal[3], tangent[4], velocity[3]; };
struct HandMesh {
    std::vector<HandVertex> vertices;
    std::vector<uint32_t> indices;
};
constexpr unsigned handSegments = 3 + 4 * 4, handRingSides = 8, handPalmCorners = 6;
constexpr unsigned handSegmentVertices = 2 * handRingSides + 2, handSegmentTriangles = 4 * handRingSides;
constexpr unsigned handPalmVertices = 2 * handPalmCorners + 2, handPalmTriangles = 4 * handPalmCorners;
constexpr unsigned handMeshVertices = handSegments * handSegmentVertices + handPalmVertices;
constexpr unsigned handMeshTriangles = handSegments * handSegmentTriangles + handPalmTriangles;

namespace detail {
inline void handVertex(HandMesh& m, XrVector3f p, XrVector3f n, XrVector3f t) {
    m.vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {t.x, t.y, t.z, 1}, {0, 0, 0}});
}
// A ring of handRingSides vertices around `centre` in the plane across `axis`; the normals lean along
// `lean` so a tapered sleeve shades like a cone rather than a tube.
inline uint32_t handRing(HandMesh& m, XrVector3f centre, XrVector3f axis, XrVector3f u, XrVector3f v, float r, float lean) {
    const uint32_t start = uint32_t(m.vertices.size());
    for (unsigned k = 0; k < handRingSides; ++k) {
        const float a = 6.2831853f * float(k) / float(handRingSides), c = std::cos(a), s = std::sin(a);
        const XrVector3f radial = add(scale(u, c), scale(v, s));
        handVertex(m, add(centre, scale(radial, r)), unit(add(radial, scale(axis, lean))), axis);
    }
    return start;
}
// One sleeve from a (radius ra) to b (radius rb), domed at both ends.
inline void handSegment(HandMesh& m, XrVector3f a, XrVector3f b, float ra, float rb, XrVector3f across) {
    const XrVector3f axis = unit(sub(b, a), {0, 0, -1});
    const XrVector3f u = unit(sub(across, scale(axis, dot(across, axis))), {1, 0, 0}), v = cross(axis, u);
    const float length = fp::xr::length(sub(b, a)), lean = length > 1e-5f ? (ra - rb) / length : 0.f;
    const uint32_t ringA = handRing(m, a, axis, u, v, ra, lean), ringB = handRing(m, b, axis, u, v, rb, lean);
    const uint32_t capA = uint32_t(m.vertices.size());
    handVertex(m, sub(a, scale(axis, ra * .6f)), scale(axis, -1), u);
    const uint32_t capB = uint32_t(m.vertices.size());
    handVertex(m, add(b, scale(axis, rb * .6f)), axis, u);
    for (uint32_t k = 0; k < handRingSides; ++k) {
        const uint32_t next = (k + 1) % handRingSides;
        m.indices.insert(m.indices.end(), {ringA + k, ringB + k, ringA + next, ringA + next, ringB + k, ringB + next}); // the side, wound outward
        m.indices.insert(m.indices.end(), {capA, ringA + next, ringA + k});                                              // the root dome
        m.indices.insert(m.indices.end(), {capB, ringB + k, ringB + next});                                              // the tip dome
    }
}
// The palm: a slab the thickness of the palm radius with a rounded outline through the wrist and the
// four knuckles, smooth-shaded so its edges read as flesh rather than a box.
inline void handPalm(HandMesh& m, const std::array<XrVector3f, handPalmCorners>& outline, XrVector3f normal, float thick) {
    XrVector3f centre{};
    for (const auto& p : outline) centre = add(centre, p);
    centre = scale(centre, 1.f / float(handPalmCorners));
    const XrVector3f up = scale(normal, thick * .5f);
    const uint32_t top = uint32_t(m.vertices.size());
    for (const auto& p : outline) handVertex(m, add(p, up), unit(add(normal, scale(unit(sub(p, centre)), .7f))), unit(sub(p, centre)));
    const uint32_t bottom = uint32_t(m.vertices.size());
    for (const auto& p : outline) handVertex(m, sub(p, up), unit(add(scale(normal, -1), scale(unit(sub(p, centre)), .7f))), unit(sub(p, centre)));
    const uint32_t topCentre = uint32_t(m.vertices.size());
    handVertex(m, add(centre, up), normal, unit(sub(outline[0], centre)));
    const uint32_t bottomCentre = uint32_t(m.vertices.size());
    handVertex(m, sub(centre, up), scale(normal, -1), unit(sub(outline[0], centre)));
    for (uint32_t k = 0; k < handPalmCorners; ++k) {
        const uint32_t next = (k + 1) % handPalmCorners;
        m.indices.insert(m.indices.end(), {topCentre, top + k, top + next});
        m.indices.insert(m.indices.end(), {bottomCentre, bottom + next, bottom + k});
        m.indices.insert(m.indices.end(), {top + k, bottom + k, top + next, top + next, bottom + k, bottom + next});
    }
}
}

// joints: positions in the bone's frame, radii in the same units. Returns an empty mesh unless
// every joint the mesh needs is valid.
inline HandMesh handMesh(const std::array<XrVector3f, handJointCount>& joint, const std::array<float, handJointCount>& radius, uint32_t validMask) {
    HandMesh m{};
    if ((validMask & allHandJoints) != allHandJoints) return m;
    m.vertices.reserve(handMeshVertices);
    m.indices.reserve(handMeshTriangles * 3);
    const XrVector3f palmAcross = sub(joint[6], joint[21]); // index to little metacarpal
    for (unsigned finger = 0; finger < 5; ++finger) {
        const unsigned first = finger == 0 ? 2 : 1 + finger * 5, count = finger == 0 ? 3 : 4;
        for (unsigned k = 0; k < count; ++k)
            detail::handSegment(m, joint[first + k], joint[first + k + 1], radius[first + k], radius[first + k + 1] * (k + 1 == count ? .85f : 1.f), palmAcross);
    }
    const XrVector3f knuckles = scale(add(add(joint[7], joint[12]), add(joint[17], joint[22])), .25f);
    const XrVector3f normal = unit(cross(sub(knuckles, joint[wristJoint]), palmAcross), {0, 1, 0});
    const std::array<XrVector3f, handPalmCorners> outline = {joint[6], joint[7], joint[12], joint[17], joint[22], joint[21]}; // round the knuckles and back along the wrist
    detail::handPalm(m, outline, normal, radius[palmJoint] * 1.2f);
    return m;
}
}
