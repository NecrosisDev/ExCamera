#pragma once
#include "native_pose.h"

namespace fp::xr {
using Point = std::array<float, 3>;
using Corners = std::array<Point, 8>;
struct Eye { NativePose pose{}; Matrix projection{}; };
struct StereoFrame {
    std::array<Eye, 2> eyes{};
    Eye center{}; // head pose with a frustum covering both eyes; the single view reconstruction draws
    uint64_t serial = 0;
    float nearPlane = 1, farPlane = 10000;
};
inline Point transform(const NativePose& p, Point v) {
    Point out{};
    for (unsigned k = 0; k < 3; ++k) out[k] = p[9 + k] + p[k] * v[0] + p[3 + k] * v[1] + p[6 + k] * v[2];
    return out;
}
// Near face first, counter-clockwise from bottom-left; the far face follows in the same order.
inline Corners corners(const Eye& eye, float nearPlane, float farPlane) {
    const auto& m = eye.projection;
    if (!(nearPlane > 0 && farPlane > nearPlane && std::isfinite(farPlane) && m[0] > 0 && m[5] > 0))
        throw std::runtime_error("invalid stereo frustum");
    const float left = (m[8] - 1) / m[0], right = (m[8] + 1) / m[0], bottom = (m[9] - 1) / m[5], top = (m[9] + 1) / m[5];
    Corners out{};
    unsigned at = 0;
    for (float depth : {nearPlane, farPlane})
        for (Point v : {Point{left * depth, bottom * depth, -depth}, Point{right * depth, bottom * depth, -depth},
                        Point{right * depth, top * depth, -depth}, Point{left * depth, top * depth, -depth}}) {
            out[at] = transform(eye.pose, v);
            for (float value : out[at])
                if (!std::isfinite(value)) throw std::runtime_error("nonfinite stereo corner");
            ++at;
        }
    return out;
}
struct Plane { Point normal{}; float distance = 0; };
inline std::array<Plane, 6> planes(const Corners& c) {
    constexpr unsigned faces[6][3] = {{0, 1, 2}, {4, 7, 6}, {4, 0, 3}, {1, 5, 6}, {1, 0, 4}, {3, 2, 6}};
    Point inside{};
    for (auto p : c)
        for (unsigned k = 0; k < 3; ++k) inside[k] += p[k] / 8;
    std::array<Plane, 6> out{};
    for (unsigned i = 0; i < 6; ++i) {
        Point a{}, b{};
        for (unsigned k = 0; k < 3; ++k) {
            a[k] = c[faces[i][1]][k] - c[faces[i][0]][k];
            b[k] = c[faces[i][2]][k] - c[faces[i][0]][k];
        }
        auto& plane = out[i];
        plane.normal = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        float n = 0;
        for (float value : plane.normal) n += value * value;
        n = std::sqrt(n);
        if (!(n > 1.e-8f && std::isfinite(n))) throw std::runtime_error("degenerate stereo plane");
        float side = 0;
        for (unsigned k = 0; k < 3; ++k) {
            plane.normal[k] /= n;
            plane.distance -= plane.normal[k] * c[faces[i][0]][k];
            side += plane.normal[k] * inside[k];
        }
        if (side + plane.distance > 0) {
            for (auto& value : plane.normal) value = -value;
            plane.distance = -plane.distance;
        }
    }
    return out;
}
// Exanima's bounds operand at mesh+20: centre, radius, half extents, padding, three axes.
// Unreadable or negative bounds stay visible; culling must never hide a mesh it cannot judge.
inline bool visible(const std::array<Plane, 6>& frustum, const float* bounds, Point sectorOrigin) {
    for (unsigned i = 0; i < 17; ++i)
        if (!std::isfinite(bounds[i])) return true;
    if (bounds[3] < 0 || bounds[4] < 0 || bounds[5] < 0 || bounds[6] < 0) return true;
    for (const auto& plane : frustum) {
        float distance = plane.distance;
        for (unsigned k = 0; k < 3; ++k) distance += plane.normal[k] * (bounds[k] + sectorOrigin[k]);
        if (distance > bounds[3]) return false;
        float support = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            float d = 0;
            for (unsigned k = 0; k < 3; ++k) d += plane.normal[k] * bounds[8 + axis * 3 + k];
            support += std::abs(d) * bounds[4 + axis];
        }
        if (distance > support) return false;
    }
    return true;
}
inline Matrix poseMatrix(const NativePose& p) {
    Matrix out{};
    out[15] = 1;
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned k = 0; k < 3; ++k) out[axis * 4 + k] = p[axis * 3 + k];
    for (unsigned k = 0; k < 3; ++k) out[12 + k] = p[9 + k];
    return out;
}
inline Matrix viewMatrix(const NativePose& p) {
    Matrix out{};
    out[15] = 1;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col) out[col * 4 + row] = p[row * 3 + col];
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned k = 0; k < 3; ++k) out[12 + row] -= p[row * 3 + k] * p[9 + k];
    return out;
}
// Conservative union in the first eye's frame, so native preparation admits both eyes once.
inline Corners unionCorners(const StereoFrame& frame) {
    const auto& parent = frame.eyes[0].pose;
    float left = 1.e20f, right = -1.e20f, bottom = 1.e20f, top = -1.e20f, nearPlane = 1.e20f, farPlane = 0;
    for (const auto& eye : frame.eyes)
        for (auto p : corners(eye, frame.nearPlane, frame.farPlane)) {
            Point local{};
            for (unsigned axis = 0; axis < 3; ++axis)
                for (unsigned k = 0; k < 3; ++k) local[axis] += (p[k] - parent[9 + k]) * parent[axis * 3 + k];
            const float depth = -local[2];
            if (!(depth > .0001f)) throw std::runtime_error("eyes do not share a forward frustum");
            const float x = local[0] / depth, y = local[1] / depth;
            left = (std::min)(left, x); right = (std::max)(right, x);
            bottom = (std::min)(bottom, y); top = (std::max)(top, y);
            nearPlane = (std::min)(nearPlane, depth); farPlane = (std::max)(farPlane, depth);
        }
    Eye unionEye{};
    unionEye.pose = parent;
    unionEye.projection[0] = 2 / (right - left);
    unionEye.projection[5] = 2 / (top - bottom);
    unionEye.projection[8] = (right + left) / (right - left);
    unionEye.projection[9] = (top + bottom) / (top - bottom);
    return corners(unionEye, nearPlane, farPlane);
}
}
