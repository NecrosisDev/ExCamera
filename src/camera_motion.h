#pragma once
#include <cmath>
#include <cstring>

namespace fp {
// The game's field of view is vertical (EXP_build_projection 0x9F2E0 scales the width by the aspect).
inline constexpr int fovMin = 30, fovMax = 130;
// Third-person camera placement, cm: behind the eye, to the right (negative: left), above the eye.
inline constexpr int tpDistanceMin = 50, tpDistanceMax = 1200, tpShoulderMax = 160, tpHeightMin = -100,
                     tpHeightMax = 300;
inline int clampRange(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline int clampFov(int degrees) {
    return degrees < fovMin ? fovMin : (degrees > fovMax ? fovMax : degrees);
}
inline float projectionScale(float height, float degrees) {
    return height * 0.5f / std::tan(degrees * 3.14159265359f / 360.f);
}
inline bool projectCursor(const float* delta, const float* pose, float width, float height, float fov,
                          float& px, float& py) {
    float x = 0, y = 0, z = 0;
    for (int i = 0; i < 3; i++) {
        x += delta[i] * pose[i];
        y += delta[i] * pose[3 + i];
        z += delta[i] * pose[6 + i];
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z >= -.01f || width <= 0 ||
        height <= 0)
        return false;
    const float scale = projectionScale(height, fov);
    px = width * .5f + x / (-z) * scale;
    py = height * .5f - y / (-z) * scale;
    return std::isfinite(px) && std::isfinite(py);
}
inline bool travel(unsigned mask, float yaw, float* out) {
    const float forward = float(bool(mask & 1)) - float(bool(mask & 4));
    const float left = float(bool(mask & 2)) - float(bool(mask & 8));
    const float length = std::sqrt(forward * forward + left * left);
    out[0] = out[1] = out[2] = 0;
    if (length == 0) return false;
    out[0] = (std::sin(yaw) * forward + std::cos(yaw) * left) / length;
    out[2] = (std::cos(yaw) * forward - std::sin(yaw) * left) / length;
    return true;
}
// World-up basis: physical head roll and gait never rotate the rendered horizon.
inline void basis(float yaw, float pitch, float* pose) {
    const float s = std::sin(yaw), c = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    pose[0] = -c;
    pose[1] = 0;
    pose[2] = s;
    pose[3] = -s * sp;
    pose[4] = cp;
    pose[5] = -c * sp;
    pose[6] = -s * cp;
    pose[7] = -sp;
    pose[8] = -c * cp;
}
inline float approachAngle(float value, float target, float seconds) {
    const float delta = std::remainder(target - value, 6.28318530718f);
    return value + delta * (1.f - std::exp(-seconds / 0.055f));
}
inline void unit(float* v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1.e-6f)
        for (int i = 0; i < 3; i++) v[i] /= n;
}
inline void cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
// The eye's place in the head part's frame: cm along its second and third axes (pose rows 3-5 and 6-8).
inline constexpr float eyeAlongAxis2 = 9.60687065f, eyeAlongAxis3 = 16.5f;
// The eye in the frame a 3x4 pose (three axes, then the origin) is given in.
inline void eyePoint(const float* pose, float* eye) {
    for (int i = 0; i < 3; ++i)
        eye[i] = pose[9 + i] + pose[3 + i] * eyeAlongAxis2 + pose[6 + i] * eyeAlongAxis3;
}
// the camera is exactly the
// head, every frame. Its back
// axis is the head's forward reversed and its up axis the head's up (made unit length and square to
// each other, forward kept), so it turns, tilts and rolls with the head; its position is the head's eye
// mount. No smoothing, no world-up levelling, no offset of its own: the only steadying is Head
// stabilisation (and its roll comfort), on the head. retires DirectionFilter,
// PositionFilter, YawActivity and the knockdown blend (poseQuat/quatPose/blendPose).
// false: the head's up and forward are (nearly) parallel, a matrix no view can be made from; the
// caller treats it as a bad head frame (hold, then park).
inline bool headPose(const float* head, const float* eye, float* pose) {
    float back[3] = {-head[6], -head[7], -head[8]}, up[3] = {head[3], head[4], head[5]};
    unit(back);
    unit(up);
    float right[3];
    cross(up, back, right);
    if (!(std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]) > 0.05f)) return false;
    std::memcpy(pose + 6, back, 12);
    std::memcpy(pose, right, 12);
    unit(pose);
    cross(pose + 6, pose, pose + 3);
    std::memcpy(pose + 9, eye, 12);
    return true;
}
// highlight: the lines of an object's box (centre/half in its own space, cm) with its node pose
// (rows of the 3x3, then the position), plus move handles (its right and forward axes) and a turn ring
// in its ground plane: world-space segment end points, 2 per segment. Returns the point count.
inline int highlightLines(const float* node, const float* centre, const float* half, float* out,
                          int capacity) {
    int n = 0;
    auto world = [&](float x, float y, float z, float* w) {
        for (int i = 0; i < 3; ++i) w[i] = node[9 + i] + x * node[i] + y * node[3 + i] + z * node[6 + i];
    };
    auto seg = [&](const float* a, const float* b) {
        if (n + 2 > capacity) return;
        std::memcpy(out + n * 3, a, 12);
        std::memcpy(out + (n + 1) * 3, b, 12);
        n += 2;
    };
    float c[8][3];
    for (int k = 0; k < 8; ++k)
        world(centre[0] + ((k & 1) ? half[0] : -half[0]), centre[1] + ((k & 2) ? half[1] : -half[1]),
              centre[2] + ((k & 4) ? half[2] : -half[2]), c[k]);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) seg(c[e[0]], c[e[1]]);
    const float r = 1.35f * (half[0] > half[2] ? half[0] : half[2]) + 10.f;
    float o[3], a[3];
    world(centre[0], centre[1], centre[2], o);
    world(centre[0] + r, centre[1], centre[2], a);
    seg(o, a); // move handle: the object's right
    world(centre[0], centre[1], centre[2] + r, a);
    seg(o, a); // move handle: the object's forward
    float prev[3];
    world(centre[0] + r, centre[1] - half[1], centre[2], prev); // turn ring at its base
    for (int i = 1; i <= 24; ++i) {
        const float t = 6.28318530718f * float(i) / 24.f;
        float p[3];
        world(centre[0] + r * std::cos(t), centre[1] - half[1], centre[2] + r * std::sin(t), p);
        seg(prev, p);
        std::memcpy(prev, p, 12);
    }
    return n;
}
// the terrain brush ring: a circle of `radius` (cm) around `at`, flat at its height, plus a short
// upright tick at the centre; 2 points per segment. Returns the point count.
inline int ringLines(const float* at, float radius, float* out, int capacity) {
    int n = 0;
    auto seg = [&](const float* a, const float* b) {
        if (n + 2 > capacity) return;
        std::memcpy(out + n * 3, a, 12);
        std::memcpy(out + (n + 1) * 3, b, 12);
        n += 2;
    };
    float prev[3] = {at[0] + radius, at[1] + 5.f, at[2]};
    for (int i = 1; i <= 40; ++i) {
        const float t = 6.28318530718f * float(i) / 40.f;
        const float p[3] = {at[0] + radius * std::cos(t), at[1] + 5.f, at[2] + radius * std::sin(t)};
        seg(prev, p);
        std::memcpy(prev, p, 12);
    }
    const float lo[3] = {at[0], at[1], at[2]}, hi[3] = {at[0], at[1] + 40.f, at[2]};
    seg(lo, hi);
    return n;
}
// third person:
// an over-the-shoulder orbit. The camera looks along the mouse aim (yaw, pitch: the same angles that
// turn the head and body), from behind a pivot above the eye, shifted to the right shoulder (negative:
// left). `clear(origin, dir, length)` returns how far the camera may go from the pivot (walls, props);
// the camera pulls in at once and eases back out (0.35 s). The pivot follows the head with a short
// spring (0.08 s) so head bob does not shake the view. Modest smoothing only; first person has none.
// (polish list item 7): third person -> the game's overhead view in a short blend. Rows 0..2 are
// right, up, back; each is lerped and the frame made orthonormal again (back kept, right rebuilt), the
// position lerped; t in 0..1 is eased (smoothstep).
inline void blendPose(const float* from, const float* to, float t, float* out) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    t = t * t * (3.f - 2.f * t);
    float r[12];
    for (int i = 0; i < 12; ++i) r[i] = from[i] + (to[i] - from[i]) * t;
    auto norm = [](float* v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f)
            for (int i = 0; i < 3; ++i) v[i] /= l;
    };
    float* right = r;
    float* up = r + 3;
    float* back = r + 6;
    norm(back);
    right[0] = up[1] * back[2] - up[2] * back[1];
    right[1] = up[2] * back[0] - up[0] * back[2];
    right[2] = up[0] * back[1] - up[1] * back[0];
    norm(right);
    up[0] = back[1] * right[2] - back[2] * right[1];
    up[1] = back[2] * right[0] - back[0] * right[2];
    up[2] = back[0] * right[1] - back[1] * right[0];
    norm(up);
    std::memcpy(out, r, 48);
}
inline constexpr float tpPivotMargin =
    20.f; // cm the pivot keeps from a ceiling or floor above or below the eye
struct ThirdPerson {
    bool valid = false;
    float pivot[3]{}, distance = -1.f, anchor[3]{};
    // tight spaces (polish list item 3; third person only): when a wall pins the camera, the shoulder
    // swaps toward the open side (eased, held 1 s before swapping back) and the framing goes closer to
    // centre and a little higher, easing back as the room opens.
    float side = 0.f, sideNow = 0.f, pinned = 0.f, swapHold = 0.f, lastSign = 0.f;
    unsigned swaps = 0;
    // feel: lag 0..100 slows the pivot's follow
    // and leads it toward where the eye travels (up to 0.4 s ahead,
    // 50 cm at most). Knockdown (item 5): while down, the pivot follows the given body point with 0.3 s.
    float lag = 0.f;
    bool down = false;
    float downPoint[3]{}, vel[3]{}, lastEye[3]{};
    bool haveEye = false;
    void reset() {
        valid = false;
        distance = -1.f;
        side = 0.f;
        sideNow = 0.f;
        pinned = 0.f;
        swapHold = 0.f;
        haveEye = false;
        for (float& v : vel) v = 0.f;
    }
    template <class Clear>
    void update(const float* eye, float yaw, float pitch, float want, float shoulder, float height,
                float seconds, Clear clear, float* pose, bool tight = false) {
        basis(yaw, pitch, pose);
        const bool fresh = !valid || !std::isfinite(seconds) || seconds < 0.f || seconds > 0.5f;
        const float ease = fresh ? 1.f : 1.f - std::exp(-seconds / 0.35f);
        const float chosen = shoulder < 0.f ? -1.f : 1.f;
        if (!tight || shoulder == 0.f) {
            side = chosen;
            sideNow = side;
            lastSign = chosen;
            pinned = 0.f;
            swapHold = 0.f;
        } else {
            if (side == 0.f || lastSign != chosen) {
                side = chosen;
                sideNow = side;
                lastSign = chosen;
            } // the setting changed: start on it
            if (swapHold > 0.f && std::isfinite(seconds)) swapHold -= seconds;
            sideNow += (side - sideNow) * ease;
        }
        const float mag = std::fabs(shoulder) * (tight ? 1.f - 0.5f * pinned : 1.f);
        shoulder = tight ? mag * sideNow : shoulder;
        height += tight ? 18.f * pinned : 0.f;
        // The pivot rises above the eye only as far as the level allows: a pivot inside a low ceiling starts
        // every pull-in ray in the ceiling and the camera collapses onto it. (Below the eye the floor is
        // further than the lowest setting.) The caller's clear answers a straight-up ray from the level alone.
        float ceilingY = INFINITY;
        if (height > 0.f && !down) {
            const float v[3] = {0.f, 1.f, 0.f};
            const float room = clear(eye, v, height + tpPivotMargin);
            if (std::isfinite(room)) {
                ceilingY = eye[1] + std::fmax(0.f, room - tpPivotMargin);
                if (eye[1] + height > ceilingY) height = ceilingY - eye[1];
            }
        }
        const float up[3] = {eye[0], eye[1] + height, eye[2]};
        float jump = 0;
        for (int i = 0; i < 3; ++i) jump += std::fabs(up[i] - anchor[i]);
        float aim[3] = {up[0], up[1], up[2]};
        if (fresh)
            for (float& v : vel) v = 0.f; // no lead carried over a hitch or a teleport
        if (lag > 0.f && haveEye && !fresh &&
            seconds > 0.f) { // where the eye travels (horizontal), smoothed over 0.3 s
            const float b = 1.f - std::exp(-seconds / 0.3f);
            for (int i = 0; i < 3; i += 2) {
                const float v = (eye[i] - lastEye[i]) / seconds;
                if (std::isfinite(v) && std::fabs(v) < 2000.f) vel[i] += (v - vel[i]) * b;
            }
            const float lead = 0.004f * lag;
            float ax = vel[0] * lead, az = vel[2] * lead;
            const float l = std::sqrt(ax * ax + az * az);
            if (l > 50.f) {
                ax *= 50.f / l;
                az *= 50.f / l;
            }
            aim[0] += ax;
            aim[2] += az;
        }
        std::memcpy(lastEye, eye, 12);
        haveEye = true;
        if (down) std::memcpy(aim, downPoint, 12);
        const float tau = down ? 0.3f : 0.08f + 0.002f * (lag < 0.f ? 0.f : (lag > 100.f ? 100.f : lag));
        if (!valid || !std::isfinite(seconds) || seconds < 0.f || seconds > 0.5f || jump > 200.f) {
            std::memcpy(pivot, aim, 12);
            valid = true;
            distance = -1.f;
        } else {
            const float a = 1.f - std::exp(-seconds / tau);
            for (int i = 0; i < 3; ++i) pivot[i] += (aim[i] - pivot[i]) * a;
        }
        if (pivot[1] > ceilingY && ceilingY >= eye[1])
            pivot[1] = ceilingY; // easing never carries it into a lintel either
        std::memcpy(anchor, up, 12);
        float dir[3];
        for (int i = 0; i < 3; ++i)
            dir[i] = pose[6 + i] * want +
                     pose[i] * shoulder; // back (row 2 is the camera's back) and right (row 0)
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(len > 1.f)) {
            dir[0] = pose[6];
            dir[1] = pose[7];
            dir[2] = pose[8];
            len = 1.f;
        } else
            for (float& v : dir) v /= len;
        float allowed = clear(pivot, dir, len);
        if (!std::isfinite(allowed) || allowed > len) allowed = len;
        if (allowed < 12.f) allowed = 12.f;
        if (tight && mag > 1.f) {
            const float p =
                std::fmin(1.f, std::fmax(0.f, (0.6f - allowed / len) / 0.4f)); // 0 open .. 1 pinned
            pinned += (p - pinned) * (p > pinned ? (fresh ? 1.f : 1.f - std::exp(-seconds / 0.12f)) : ease);
            const bool onChosen = side == chosen;
            if (swapHold <= 0.f &&
                (p > 0.25f ||
                 !onChosen)) { // pinned: try the other shoulder; swapped: go back once the chosen one is open
                float other[3];
                for (int i = 0; i < 3; ++i)
                    other[i] =
                        pose[6 + i] * want -
                        pose[i] * mag * side; // the mirror of the side we are going to, not the eased one
                float ol = std::sqrt(other[0] * other[0] + other[1] * other[1] + other[2] * other[2]);
                if (ol > 1.f) {
                    for (float& v : other) v /= ol;
                    float oa = clear(pivot, other, ol);
                    if (!std::isfinite(oa) || oa > ol) oa = ol;
                    if (onChosen ? oa > allowed * 1.4f + 20.f : oa >= 0.9f * ol) {
                        side = -side;
                        swapHold = 1.f;
                        ++swaps;
                    }
                }
            }
        }
        if (distance < 0.f || allowed < distance || !std::isfinite(seconds) || seconds < 0.f ||
            seconds > 0.5f)
            distance = allowed;
        else
            distance += (allowed - distance) * (1.f - std::exp(-seconds / 0.35f));
        for (int i = 0; i < 3; ++i) pose[9 + i] = pivot[i] + dir[i] * distance;
    }
};
// dynamic near plane. clearance = view depth to the nearest level geometry in view (<0
// unknown). Tightens at once (nothing slices into view), widens gently (no pop), stays within
// [minNear, maxNear]; a wider near plane gives the depth buffer more precision far away.
// The floor of the dynamic near plane, in tenths of a centimetre (F8 View "Near clip", [Camera] NearClip):
// lower lets the eye come closer to walls without them slicing into view, at the cost of depth precision
// far away.
inline constexpr int nearClipMinTenths = 5, nearClipMaxTenths = 10, nearClipDefaultTenths = 5;
inline int clampNearClip(int tenths) {
    return tenths < nearClipMinTenths ? nearClipMinTenths
                                      : (tenths > nearClipMaxTenths ? nearClipMaxTenths : tenths);
}
// The smallest depth difference a depth buffer of `bits` can tell apart at distance z, for a perspective
// projection with these near and far planes (d = f/(f-n) * (1 - n/z), one step of 2^-bits).
inline float depthResolution(float nearPlane, float farPlane, int bits, float z) {
    return std::ldexp(1.f, -bits) * (farPlane - nearPlane) * z * z / (farPlane * nearPlane);
}
struct NearPlane {
    bool valid = false;
    float value = 1.f;
    void reset() { valid = false; }
    float update(float clearance, float seconds, float minNear = 1.f, float maxNear = 4.f) {
        const float want = clearance < 0.f || !std::isfinite(clearance)
                               ? minNear
                               : std::fmin(maxNear, std::fmax(minNear, clearance * 0.5f));
        if (!valid || !std::isfinite(seconds) || seconds < 0.f || seconds > 0.5f || want < value) {
            value = want;
            valid = true;
            return value;
        }
        value += (want - value) * (1.f - std::exp(-seconds / 0.35f));
        if (value > maxNear) value = maxNear;
        return value;
    }
};
} // namespace fp
