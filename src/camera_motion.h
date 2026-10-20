#pragma once
#include <algorithm>
#include <cmath>
#include <type_traits>
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
// Capture once per FP/TP camera epoch; zero TP override inherits the selected FOV.
// Blend/hold paths retain their own explicit FOV rather than resolving settings again.
inline float resolveCameraFov(int mode, float selected, float thirdPerson) {
    return mode == 1 && thirdPerson > 0.f && std::isfinite(thirdPerson) ? thirdPerson : selected;
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
// [Camera] EyeSetback: cm the viewpoint (and the aim's origin with it) sits behind the eyes, along the head's
// forward axis, toward the head's middle where its collision keeps arms, weapons and walls furthest away. Your
// head is handled separately by camera-only render filtering; the camera stays at the centered head mount (and
// a hidden mesh still casts its shadow: the shadow maps choose casters by their own flag, not the pass mask).
// At most eyeAlongAxis3: never behind the head part's origin.
inline constexpr int eyeSetbackMax = 16;
inline int clampEyeSetback(int cm) {
    return cm < 0 ? 0 : (cm > eyeSetbackMax ? eyeSetbackMax : cm);
}
inline void eyePoint(const float* pose, float* eye, float setback = 0.f) {
    const float along =
        eyeAlongAxis3 - (setback < 0.f ? 0.f : (setback > eyeAlongAxis3 ? eyeAlongAxis3 : setback));
    for (int i = 0; i < 3; ++i) eye[i] = pose[9 + i] + pose[3 + i] * eyeAlongAxis2 + pose[6 + i] * along;
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
// Heading hold (first person, [Camera] HoldHeading): looking up or down, the game's head turns its face a few
// degrees off the aim's heading (the view slid sideways). The view is turned about the world's up by that
// small error, so it faces the aim's heading; pitch, roll and the eye stay the head's. Errors past `full`
// fade out by `none`: those are the head still catching up with a fast turn, which stays visible. Nothing
// is done near straight up or down, where a heading means little. Returns the turn applied (radians).
inline float wrapPi(float a) {
    a = std::fmod(a + 3.14159265f, 6.2831853f);
    return a < 0.f ? a + 3.14159265f : a - 3.14159265f;
}
inline constexpr float headingHoldFull = 0.07f, headingHoldNone = 0.14f; // 4 and 8 degrees
inline float holdHeading(float* pose, float aimYaw) {
    const float fy = -pose[7];
    if (!std::isfinite(aimYaw) || std::fabs(fy) > 0.97f) return 0.f;
    float err = aimYaw - std::atan2(-pose[6], -pose[8]);
    err = wrapPi(err);
    const float a = std::fabs(err);
    const float w =
        a <= headingHoldFull
            ? 1.f
            : (a >= headingHoldNone ? 0.f : (headingHoldNone - a) / (headingHoldNone - headingHoldFull));
    const float turn = err * w;
    if (turn == 0.f) return 0.f;
    const float c = std::cos(turn), s = std::sin(turn);
    for (int r = 0; r < 3; ++r) {
        float* v = pose + 3 * r;
        const float x = v[0], z = v[2];
        v[0] = c * x + s * z;
        v[2] = -s * x + c * z;
    } // about +y, as basis turns yaw
    return turn;
}
// The horizontal field of view a vertical one gives at an aspect (width / height).
inline float horizontalFov(float verticalDegrees, float aspect) {
    return 2.f * std::atan(std::tan(verticalDegrees * 3.14159265f / 360.f) * aspect) * 360.f / 6.2831853f;
}
// third person:
// an over-the-shoulder orbit. The camera looks along the mouse aim (yaw, pitch: the same angles that
// turn the head and body), from behind a pivot above the eye, shifted to the right shoulder (negative:
// left). `clear(origin, dir, length)` returns how far the camera may go from the pivot (walls, props);
// the camera pulls in at once and eases back out (0.35 s). The pivot follows the head with a short
// spring (0.08 s) so head bob does not shake the view. Modest smoothing only; first person has none.
// A rigid camera frame as a unit quaternion (x,y,z,w), using its stored row
// convention. The largest-diagonal branches stay conditioned at a half turn.
inline bool poseQuaternion(const float* p, float* q) {
    const float trace = p[0] + p[4] + p[8];
    if (trace > 0.f) {
        const float s = 2.f * std::sqrt(trace + 1.f);
        if (!(s > 1e-6f) || !std::isfinite(s)) return false;
        q[0] = (p[7] - p[5]) / s;
        q[1] = (p[2] - p[6]) / s;
        q[2] = (p[3] - p[1]) / s;
        q[3] = 0.25f * s;
    } else {
        int i = p[4] > p[0] ? 1 : 0;
        if (p[8] > p[3 * i + i]) i = 2;
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        const float s = 2.f * std::sqrt(std::fmax(0.f, 1.f + p[3 * i + i] - p[3 * j + j] - p[3 * k + k]));
        if (!(s > 1e-6f) || !std::isfinite(s)) return false;
        q[i] = 0.25f * s;
        q[j] = (p[3 * i + j] + p[3 * j + i]) / s;
        q[k] = (p[3 * i + k] + p[3 * k + i]) / s;
        q[3] = (p[3 * k + j] - p[3 * j + k]) / s;
    }
    float n = 0.f;
    for (int i = 0; i < 4; ++i) n += q[i] * q[i];
    if (!(n > 1e-8f) || !std::isfinite(n)) return false;
    n = std::sqrt(n);
    for (int i = 0; i < 4; ++i) q[i] /= n;
    return true;
}
inline void quaternionAxes(const float* q, float* p) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    p[0] = 1.f - 2.f * (y * y + z * z);
    p[1] = 2.f * (x * y - z * w);
    p[2] = 2.f * (x * z + y * w);
    p[3] = 2.f * (x * y + z * w);
    p[4] = 1.f - 2.f * (x * x + z * z);
    p[5] = 2.f * (y * z - x * w);
    p[6] = 2.f * (x * z - y * w);
    p[7] = 2.f * (y * z + x * w);
    p[8] = 1.f - 2.f * (x * x + y * y);
}
// Third person -> overhead keeps the existing smoothstep timing and position
// lerp. Blend orientation on the shortest quaternion arc: lerping camera axes
// makes up/back parallel halfway through a 180-degree heading change.
inline void blendPose(const float* from, const float* to, float t, float* out, bool ease = true) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    if (t == 0.f || t == 1.f) {
        std::memcpy(out, t == 0.f ? from : to, 48);
        return;
    }
    if (ease) t = t * t * (3.f - 2.f * t);
    float a[4], b[4], q[4];
    if (poseQuaternion(from, a) && poseQuaternion(to, b)) {
        float dot = 0.f;
        for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
        if (dot < 0.f) {
            dot = -dot;
            for (float& v : b) v = -v;
        }
        dot = std::fmin(1.f, dot);
        float wa = 1.f - t, wb = t;
        if (dot < 0.9995f) {
            const float angle = std::acos(dot), sine = std::sin(angle);
            wa = std::sin((1.f - t) * angle) / sine;
            wb = std::sin(t * angle) / sine;
        }
        float n = 0.f;
        for (int i = 0; i < 4; ++i) {
            q[i] = wa * a[i] + wb * b[i];
            n += q[i] * q[i];
        }
        n = std::sqrt(n);
        for (float& v : q) v /= n;
        quaternionAxes(q, out);
    } else
        std::memcpy(out, from, 36); // Keep the last good view if a native frame is unusable.
    for (int i = 9; i < 12; ++i) out[i] = from[i] + (to[i] - from[i]) * t;
}
// Eyes compensate ordinary simulated head motion without relocating the physical
// eye or turning large knockdowns into a detached camera. Stateless rotation
// composition keeps all temporal samples on the same mouse intent and adds no lag.
inline constexpr float eyeCompensationLimit = 0.5235987756f; // thirty degrees
inline void stabilizeEyeRotation(float* physical, float yaw, float pitch) {
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;
    float desired[12], a[4], b[4];
    basis(yaw, pitch, desired);
    for (int i = 9; i < 12; ++i) desired[i] = physical[i];
    if (!poseQuaternion(physical, a) || !poseQuaternion(desired, b)) return;
    float dot = 0.f;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    const float angle = 2.f * std::acos(std::clamp(std::fabs(dot), 0.f, 1.f));
    const float share = angle <= eyeCompensationLimit ? 1.f : eyeCompensationLimit / angle;
    blendPose(physical, desired, share, physical, false);
}
inline constexpr float tpPivotMargin =
    20.f; // cm the pivot keeps from a ceiling or floor above or below the eye
// Third-person blockers: characters, walls and floors always pull the camera in.
// Hide may suppress verified placed props only; Push pulls in for every hit.
// Smart pulls in for placed props too, but ignores loose items and the player tree.
inline constexpr int blockersHide = 0, blockersPush = 1, blockersSmart = 2;
inline int blockersFromText(const wchar_t* t) {
    if (!t) return blockersSmart;
    while (*t == L' ') ++t;
    auto is = [&](const wchar_t* w) {
        const wchar_t* p = t;
        for (; *w; ++w, ++p)
            if ((*p | 0x20) != *w) return false;
        while (*p == L' ') ++p;
        return *p == 0;
    };
    return is(L"hide") ? blockersHide : is(L"push") ? blockersPush : blockersSmart;
}
inline const wchar_t* blockersText(int mode) {
    return mode == blockersHide ? L"hide" : mode == blockersPush ? L"push" : L"smart";
}
// The free-aim region is an ellipse (or circle when both screen-space radii match).
// Mouse motion below the soft-edge band goes to the cursor. In the outer band,
// the *outward radial component* is progressively shared with camera turning.
// The tangential component keeps the cursor free to slide around the ellipse;
// we do not add a camera velocity filter, stored pressure, or drift after release.
inline constexpr float aimBoxShare = 0.2f;
inline constexpr int defaultAimEdgeSoftness = 100;
inline constexpr int fixedAimHalfExtent = 25, fixedHeadStabilization = 100;
struct AimBox {
    float x = 0.f, y = 0.f;
};
inline void aimBoxStep(AimBox& box, float& dx, float& dy, float halfWidth, float halfHeight,
                       int edgeSoftness = 0) {
    if (!std::isfinite(dx)) {
        dx = 0.f;
        box.x = 0.f;
    }
    if (!std::isfinite(dy)) {
        dy = 0.f;
        box.y = 0.f;
    }
    const bool useX = std::isfinite(halfWidth) && halfWidth > 0.f;
    const bool useY = std::isfinite(halfHeight) && halfHeight > 0.f;
    const float previousX = useX && std::isfinite(box.x) ? box.x : 0.f;
    const float previousY = useY && std::isfinite(box.y) ? box.y : 0.f;
    const float wantX = previousX + dx, wantY = previousY + dy;
    float keptX = useX ? std::clamp(wantX, -halfWidth, halfWidth) : 0.f;
    float keptY = useY ? std::clamp(wantY, -halfHeight, halfHeight) : 0.f;
    const int softness = std::clamp(edgeSoftness, 0, 100);
    if (!softness) {
        // Exactly the input21 projection. Zero softness is an exact recovery mode.
        if (useX && useY) {
            const double r = std::hypot(double(wantX) / double(halfWidth),
                                        double(wantY) / double(halfHeight));
            if (r > 1.) {
                keptX = float(double(wantX) / r);
                keptY = float(double(wantY) / r);
            }
        }
    } else if (useX || useY) {
        // Normalized radii also handle a zero-sized axis: that axis turns
        // directly, while the other gets a one-dimensional soft shoulder.
        const double x = useX ? double(previousX) / double(halfWidth) : 0.;
        const double y = useY ? double(previousY) / double(halfHeight) : 0.;
        const double vx = useX ? double(dx) / double(halfWidth) : 0.;
        const double vy = useY ? double(dy) / double(halfHeight) : 0.;
        const double length = std::hypot(x, y);
        const double deltaLength = std::hypot(vx, vy);
        const double rx = length > 1.e-8 ? x / length : (deltaLength > 0. ? vx / deltaLength : 0.);
        const double ry = length > 1.e-8 ? y / length : (deltaLength > 0. ? vy / deltaLength : 0.);
        const double outward = vx * rx + vy * ry;
        double keepOutward = outward;
        const double band = double(softness) * 0.01;
        const double start = 1. - band;
        if (outward > 0. && length >= 0. && length < 1.) {
            if (length < start && length + outward > start) {
                const double past = outward - (start - length);
                keepOutward = (start - length) + band * (-std::expm1(-past / band));
            } else if (length >= start) {
                keepOutward = (1. - length) * (-std::expm1(-outward / band));
            }
        } else if (outward > 0. && length >= 1.) {
            keepOutward = 0.; // already at the rim: outward input goes to the view
        }
        // A radial shoulder only: tangential movement still traces the oval
        // instead of getting eaten by the camera when it reaches the rim.
        double nx = x + vx + (keepOutward - outward) * rx;
        double ny = y + vy + (keepOutward - outward) * ry;
        const double r = std::hypot(nx, ny);
        if (r > 1.) {
            nx /= r;
            ny /= r;
        }
        keptX = useX ? float(nx * double(halfWidth)) : 0.f;
        keptY = useY ? float(ny * double(halfHeight)) : 0.f;
    }
    dx = wantX - keptX;
    dy = wantY - keptY;
    box.x = keptX;
    box.y = keptY;
}
// A screen offset unprojected through a view orientation. No hit depth or camera/body parallax:
// looking at nearby ground cannot reverse the horizontal heading.
inline void cursorRay(const float* pose, float x, float y, float height, float fov, float* out) {
    const float scale = projectionScale(height, fov);
    const float sx = scale > 0.f ? x / scale : 0.f, sy = scale > 0.f ? -y / scale : 0.f;
    for (int i = 0; i < 3; ++i) out[i] = -pose[6 + i] + sx * pose[i] + sy * pose[3 + i];
    unit(out);
}
inline float cursorHeading(const float* pose, const float* ray) {
    const float yaw = std::atan2(-pose[6], -pose[8]);
    const float forward = ray[0] * std::sin(yaw) + ray[2] * std::cos(yaw);
    const float side = ray[0] * std::cos(yaw) - ray[2] * std::sin(yaw);
    // A vertical cursor ray can cross a pitch pole; body heading stays in the view's forward hemisphere.
    return yaw + std::atan2(side, std::fmax(0.15f, forward));
}
inline int clampDeadzone(int share) {
    return clampRange(share, 0, 40);
}
// Move the *camera's desired direction* toward the cursor's world-space ray, then
// reproject that SAME ray to the new screen offset. A snap (middle click) skips
// the easing; continuous follow is frame-time independent. No new physics camera
// transform is manufactured: first-person still follows the real head.
inline bool recenterAimOnCursor(AimBox& box, float& yaw, float& pitch,
                               float width, float height, float fov, float dt, bool snap) {
    if (!std::isfinite(yaw) || !std::isfinite(pitch) ||
        !std::isfinite(box.x) || !std::isfinite(box.y) ||
        !(width > 40.f && height > 40.f) || !std::isfinite(fov) ||
        fov < 1.f || fov > 170.f || (std::fabs(box.x) < 1e-4f && std::fabs(box.y) < 1e-4f))
        return false;
    float pose[12]{}, ray[3];
    basis(yaw, pitch, pose);
    cursorRay(pose, box.x, box.y, height, fov, ray);
    const float desiredYaw = cursorHeading(pose, ray);
    const float desiredPitch = std::atan2(ray[1], std::hypot(ray[0], ray[2]));
    const float k = snap ? 1.f : (std::isfinite(dt) && dt > 0.f
                                     ? 1.f - std::exp(-(std::min)(dt, 0.1f) / 0.75f)
                                     : 0.f);
    if (!(k > 0.f)) return false;
    const float ny = wrapPi(yaw + wrapPi(desiredYaw - yaw) * k);
    const float np = std::clamp(pitch + (desiredPitch - pitch) * k, -1.15f, 1.15f);
    if (std::fabs(wrapPi(ny - yaw)) < 1e-6f && std::fabs(np - pitch) < 1e-6f)
        return false; // pitched against the native head limit
    float newPose[12]{}, px = 0.f, py = 0.f;
    basis(ny, np, newPose);
    if (!projectCursor(ray, newPose, width, height, fov, px, py)) return false;
    yaw = ny;
    pitch = np;
    box.x = px - width * .5f;
    box.y = py - height * .5f;
    if (std::fabs(box.x) < 0.0005f) box.x = 0.f;
    if (std::fabs(box.y) < 0.0005f) box.y = 0.f;
    return true;
}
// The body's heading toward what the free crosshair is over (positions in one frame), level; straight along yaw
// when the point is nearer than minDistance (it is then the body itself, or right under it).
inline void headingToward(const float* body, const float* point, float yaw, float* d,
                          float minDistance = 50.f) {
    const float x = point[0] - body[0], z = point[2] - body[2], l = std::sqrt(x * x + z * z);
    if (std::isfinite(l) && l >= minDistance) {
        d[0] = x / l;
        d[1] = 0.f;
        d[2] = z / l;
    } else {
        d[0] = std::sin(yaw);
        d[1] = 0.f;
        d[2] = std::cos(yaw);
    }
}
// Down, for the third-person follow: the game's own state bits say so (knocked down, falling, getting up;
// dead), held 0.3 s after they clear so getting up does not flicker. It was a height gap between the eye and
// the character's reference position, which that position's place on the body made true while standing.
inline constexpr unsigned knockdownStateBits = 0xac0 | 0x40204 | 0xa00, deadLifeBit = 0x100;
inline constexpr unsigned long long knockdownReleaseMs = 300;
struct KnockdownFollow {
    bool down = false;
    unsigned long long clearSince = 0;
};
inline bool knockdownStep(KnockdownFollow& k, unsigned stateFlags, unsigned lifeFlags,
                          unsigned long long ms) {
    if ((stateFlags & knockdownStateBits) || (lifeFlags & deadLifeBit)) {
        k.down = true;
        k.clearSince = 0;
        return true;
    }
    if (k.down) {
        if (!k.clearSince) k.clearSince = ms;
        if (ms - k.clearSince >= knockdownReleaseMs) {
            k.down = false;
            k.clearSince = 0;
        }
    }
    return k.down;
}
// The half-size of the box the camera is kept clear by (cm): bigger than the near plane's own rectangle, so
// a wall beside the view never comes inside the near plane and shows through.
inline constexpr float tpCameraRadius = 12.f;
// A cast for ThirdPerson: clear(origin, unit direction, length[, levelOnly]) -> how far is free. levelOnly asks
// for walls and floors alone (no props, no people); a three-argument clear answers every cast the same way.
template <class Clear>
inline float tpCast(Clear& clear, const float* origin, const float* dir, float length, bool levelOnly) {
    float a;
    if constexpr (std::is_invocable_v<Clear&, const float*, const float*, float, bool>)
        a = clear(origin, dir, length, levelOnly);
    else
        a = clear(origin, dir, length);
    return std::isfinite(a) && a < length ? a : length;
}
struct ThirdPerson {
    bool valid = false;
    float pivot[3]{}, distance = -1.f, anchor[3]{};
    // tight spaces (third person only): when a wall pins the camera, the shoulder swaps toward the open side
    // (eased, held 1 s before swapping back) and the framing goes closer to centre and a little higher,
    // easing back as the room opens.
    float side = 0.f, sideNow = 0.f, pinned = 0.f, swapHold = 0.f, lastSign = 0.f;
    unsigned swaps = 0;
    // feel: lag 0..100 slows the pivot's follow and leads it toward where the eye travels (up to 0.4 s ahead,
    // 50 cm at most). Knockdown: while down, the pivot follows the given body point with 0.3 s.
    float lag = 0.f;
    bool down = false;
    KnockdownFollow knock; // Belongs to this camera/actor lifetime, including its delayed release.
    float downPoint[3]{}, vel[3]{}, lastEye[3]{};
    bool haveEye = false;
    float radius = tpCameraRadius;
    // What the last update decided (the status file shows them): the room above the eye (-1 not asked), the
    // height a ceiling took away (given to the boom instead), how far the pivot was pulled back out of a wall,
    // and the free length behind the pivot.
    float lastCeilingRoom = -1.f, lastLostHeight = 0.f, lastPivotPulled = 0.f, lastAllowed = 0.f;
    void reset() {
        valid = false;
        distance = -1.f;
        side = 0.f;
        sideNow = 0.f;
        pinned = 0.f;
        swapHold = 0.f;
        haveEye = false;
        down = false;
        knock = {};
        for (float& v : vel) v = 0.f;
    }
    // How far the camera can go from `from` along dir, kept `radius` clear: the centre ray (the only one that
    // also meets props and people), and four parallel level rays through the corners of a box of that
    // half-size. A corner that would start inside a wall is pulled in toward the centre first.
    template <class Clear>
    float boxCast(Clear& clear, const float* from, const float* dir, float length, const float* pose) {
        float allowed = tpCast(clear, from, dir, length, false);
        if (radius <= 0.f) return allowed;
        for (int k = 0; k < 4; ++k) {
            const float sx = (k & 1) ? radius : -radius, sy = (k & 2) ? radius : -radius;
            float off[3], ol = 0;
            for (int i = 0; i < 3; ++i) {
                off[i] = pose[i] * sx + pose[3 + i] * sy;
                ol += off[i] * off[i];
            }
            ol = std::sqrt(ol);
            if (!(ol > 1e-3f)) continue;
            const float n[3] = {off[0] / ol, off[1] / ol, off[2] / ol};
            const float reach = tpCast(clear, from, n, ol, true);
            const float s = reach >= ol ? 1.f : std::fmax(0.f, 0.8f * reach / ol);
            const float o[3] = {from[0] + off[0] * s, from[1] + off[1] * s, from[2] + off[2] * s};
            allowed = std::fmin(allowed, tpCast(clear, o, dir, length, true));
        }
        return allowed;
    }
    template <class Clear>
    void update(const float* eye, float yaw, float pitch, float want, float shoulder, float height,
                float seconds, Clear clear, float* pose, bool tight = false,
                const float* orientation = nullptr) {
        if (orientation)
            std::memcpy(pose, orientation, 9 * sizeof(float));
        else
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
        // every pull-in ray in the ceiling and the camera collapses onto it. The height the ceiling takes away
        // is not lost: the boom behind the pivot rises by it instead, so the setting still lifts the view.
        float ceilingY = INFINITY;
        lastCeilingRoom = -1.f;
        lastLostHeight = 0.f;
        if (height > 0.f && !down) {
            const float v[3] = {0.f, 1.f, 0.f};
            const float room = tpCast(clear, eye, v, height + tpPivotMargin, true);
            lastCeilingRoom = room;
            ceilingY = eye[1] + std::fmax(0.f, room - tpPivotMargin);
            if (eye[1] + height > ceilingY) {
                lastLostHeight = eye[1] + height - ceilingY;
                height = ceilingY - eye[1];
            }
        }
        const float up[3] = {eye[0], eye[1] + height, eye[2]};
        float jump = 0;
        for (int i = 0; i < 3; ++i) jump += std::fabs(up[i] - anchor[i]);
        float goal[3] = {up[0], up[1], up[2]};
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
            goal[0] += ax;
            goal[2] += az;
        }
        std::memcpy(lastEye, eye, 12);
        haveEye = true;
        if (down) std::memcpy(goal, downPoint, 12);
        const float tau = down ? 0.3f : 0.08f + 0.002f * (lag < 0.f ? 0.f : (lag > 100.f ? 100.f : lag));
        if (!valid || !std::isfinite(seconds) || seconds < 0.f || seconds > 0.5f || jump > 200.f) {
            std::memcpy(pivot, goal, 12);
            valid = true;
            distance = -1.f;
        } else {
            const float a = 1.f - std::exp(-seconds / tau);
            for (int i = 0; i < 3; ++i) pivot[i] += (goal[i] - pivot[i]) * a;
        }
        if (pivot[1] > ceilingY && ceilingY >= eye[1])
            pivot[1] = ceilingY; // easing never carries it into a lintel either
        // The pivot itself must be reachable from the eye: the lead, the lag and the height can carry it into a
        // wall, and every ray from a pivot inside a wall misses that wall, so the camera ended up behind it.
        lastPivotPulled = 0.f;
        {
            float d[3], l = 0;
            for (int i = 0; i < 3; ++i) {
                d[i] = pivot[i] - eye[i];
                l += d[i] * d[i];
            }
            l = std::sqrt(l);
            if (l > 1e-3f) {
                const float n[3] = {d[0] / l, d[1] / l, d[2] / l};
                const float free = tpCast(clear, eye, n, l + radius, true);
                if (free < l + radius) {
                    const float keep = std::fmax(0.f, free - radius);
                    for (int i = 0; i < 3; ++i) pivot[i] = eye[i] + n[i] * keep;
                    lastPivotPulled = l - keep;
                }
            }
        }
        std::memcpy(anchor, up, 12);
        float dir[3];
        for (int i = 0; i < 3; ++i)
            dir[i] = pose[6 + i] * want + pose[i] * shoulder +
                     (i == 1 ? lastLostHeight : 0.f); // back, right, and the lifted boom
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(len > 1.f)) {
            dir[0] = pose[6];
            dir[1] = pose[7];
            dir[2] = pose[8];
            len = 1.f;
        } else
            for (float& v : dir) v /= len;
        // A minimum boom length must never overrule an actual collision. At zero
        // clearance the camera stays at the reachable pivot until space opens.
        const float allowed = std::fmax(0.f, boxCast(clear, pivot, dir, len, pose));
        lastAllowed = allowed;
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
                    const float oa = boxCast(clear, pivot, other, ol, pose);
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
