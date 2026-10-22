#pragma once
#include "hand_joints.h"
#include "hand_mesh.h"
#include <vector>

namespace fp::xr {
// The game's own hand mesh bent by tracked or inferred finger joints. The game skins its hands
// rigidly (one bone per hand), so the mesh is modelled with straight fingers; a rest skeleton is
// read back from the posed joints by straightening each finger along its metacarpal, every finger
// vertex is weighted to its two nearest rest segments, and each segment carries its vertices to
// the posed joints. Palm vertices stay put. All positions share the hand part's frame and units.
struct HandSource {
    const float* positions = nullptr;   // 3 floats per vertex, `count` vertices
    const float* normals = nullptr;     // optional, same layout
    const uint32_t* triangles = nullptr; // 3 per face, indices 0..count-1
    unsigned count = 0, faces = 0;
};
// The bending chain of one finger: its first joint, its segment count and its metacarpal. The thumb
// bends from its metacarpal (2..5); the other fingers from the proximal joint to the tip, their
// metacarpals staying in the palm. Every chain ends on its tip joint, inside the 26-joint array.
struct FingerChain { unsigned first, segments, meta; };
constexpr FingerChain fingerChain(unsigned finger) {
    return finger == 0 ? FingerChain{2, 3, 2} : FingerChain{2 + finger * 5, 3, 1 + finger * 5};
}
constexpr unsigned chainSegments = 3 * 5;
static_assert(fingerChain(0).first + fingerChain(0).segments == thumbTipJoint, "thumb chain");
static_assert(fingerChain(1).first + fingerChain(1).segments == indexTipJoint, "index chain");
static_assert(fingerChain(4).first + fingerChain(4).segments == littleTipJoint && littleTipJoint < handJointCount, "little chain");
// The straight direction a chain rests along: its metacarpal's (the thumb's first segment's).
inline XrVector3f chainRestDirection(const std::array<XrVector3f, handJointCount>& joint, const FingerChain& c) {
    const unsigned from = c.meta == c.first ? c.first + 1 : c.first;
    return unit(sub(joint[from], joint[c.meta]), {0, 0, -1});
}
namespace detail {
struct Segment { XrVector3f restA, restB, posedA, posedB; unsigned finger; };
inline float segmentDistance(XrVector3f p, XrVector3f a, XrVector3f b, float& t) {
    const XrVector3f ab = sub(b, a);
    const float len2 = dot(ab, ab);
    t = len2 > 1e-8f ? (std::min)(1.f, (std::max)(0.f, dot(sub(p, a), ab) / len2)) : 0.f;
    return length(sub(p, add(a, scale(ab, t))));
}
}
// `joints` are the 26 hand joints in the mesh's frame and units; `radius` the same units. Returns an
// empty mesh when the joints or the source are unusable, so the caller can fall back.
inline HandMesh bendHand(const HandSource& src, const std::array<XrVector3f, handJointCount>& joint, const std::array<float, handJointCount>& radius, uint32_t validMask) {
    HandMesh m{};
    if ((validMask & allHandJoints) != allHandJoints || !src.positions || !src.triangles || src.count < 3 || src.faces < 1) return m;
    // Rest skeleton: each finger straightened along its metacarpal, keeping the posed segment lengths.
    std::vector<detail::Segment> segments;
    segments.reserve(chainSegments);
    for (unsigned finger = 0; finger < 5; ++finger) {
        const FingerChain c = fingerChain(finger);
        const XrVector3f dir = chainRestDirection(joint, c);
        XrVector3f rest = joint[c.first];
        for (unsigned k = 0; k < c.segments; ++k) {
            const unsigned a = c.first + k, b = a + 1;
            const float len = length(sub(joint[b], joint[a]));
            const XrVector3f restB = add(rest, scale(dir, len));
            segments.push_back({rest, restB, joint[a], joint[b], finger});
            rest = restB;
        }
    }
    m.vertices.reserve(src.count);
    m.indices.reserve(size_t(src.faces) * 3);
    const float reach = (std::max)(radius[indexTipJoint], .004f) * 2.5f; // how far from a rest finger a vertex still belongs to it
    for (unsigned v = 0; v < src.count; ++v) {
        const XrVector3f p = {src.positions[v * 3], src.positions[v * 3 + 1], src.positions[v * 3 + 2]};
        const XrVector3f n = src.normals ? XrVector3f{src.normals[v * 3], src.normals[v * 3 + 1], src.normals[v * 3 + 2]} : XrVector3f{0, 1, 0};
        // the two nearest rest segments and their distances
        int best = -1, second = -1;
        float bestD = 1e9f, secondD = 1e9f, t = 0;
        for (int s = 0; s < int(segments.size()); ++s) {
            const float d = detail::segmentDistance(p, segments[s].restA, segments[s].restB, t);
            if (d < bestD) { second = best; secondD = bestD; best = s; bestD = d; }
            else if (d < secondD) { second = s; secondD = d; }
        }
        XrVector3f outP = p, outN = n;
        if (best >= 0 && bestD < reach) {
            auto carry = [&](const detail::Segment& s, XrVector3f& pos, XrVector3f& nor) {
                const XrQuaternionf q = directionRotation(unit(sub(s.restB, s.restA), {0, 0, -1}), unit(sub(s.posedB, s.posedA), {0, 0, -1}));
                pos = add(s.posedA, rotate(q, sub(p, s.restA)));
                nor = rotate(q, n);
            };
            XrVector3f p1{}, n1{};
            carry(segments[best], p1, n1);
            if (second >= 0 && secondD < reach && segments[second].finger == segments[best].finger && secondD < bestD * 2.5f) { // a neighbour on the same finger smooths the crease
                XrVector3f p2{}, n2{};
                carry(segments[second], p2, n2);
                const float w2 = bestD / (bestD + secondD) * .5f, w1 = 1 - w2;
                outP = add(scale(p1, w1), scale(p2, w2));
                outN = unit(add(scale(n1, w1), scale(n2, w2)), n);
            } else { outP = p1; outN = n1; }
        }
        const XrVector3f tangent = unit(cross(outN, {0, 0, 1}), {1, 0, 0});
        m.vertices.push_back({{outP.x, outP.y, outP.z}, {outN.x, outN.y, outN.z}, {tangent.x, tangent.y, tangent.z, 1}, {0, 0, 0}});
    }
    for (unsigned f = 0; f < src.faces; ++f)
        for (unsigned c = 0; c < 3; ++c) {
            const uint32_t i = src.triangles[f * 3 + c];
            if (i >= src.count) { m.vertices.clear(); m.indices.clear(); return m; }
            m.indices.push_back(i);
        }
    return m;
}
}
