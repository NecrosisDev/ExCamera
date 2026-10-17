#pragma once
// Active head stabiliser (all steadying is on the physics head; the camera follows the head
// exactly). The head is a Verlet tetrahedron of 4 points; once per physics tick, after the game's 20
// constraint passes and before it turns the points into the head pose (0x89800 -> 0x7ECD0), the points
// are turned about their centre:
// - damping: the part of the head's turn this tick that the gaze did not ask for (sway, steps, bumps)
// is taken out, per axis (yaw about world up, pitch about the head's right, roll about its forward);
// - spring: the head's roll is eased back toward level (the gaze never asks for roll);
// - bob: the head's height is eased toward its own smoothed height.
// Turning the points (not the previous ones) also lowers the Verlet velocity they carry into the next
// tick, which is what makes it damping. Every correction is clamped per tick. Mouse input is untouched:
// the gaze (root+0xCB0) is only read.
#include <cmath>
#include <cstring>

namespace fp {

struct HeadSteadyState {
    bool primed = false;
    float forward[3]{}, up[3]{}, right[3]{}, gaze[3]{};
    float height = 0;
};
struct HeadSteadyStrength {
    int roll = 0, pitch = 0, yaw = 0, bob = 0;
}; // 0..100 each

inline constexpr float steadyDampMax = 0.85f;      // share of unwanted turn removed per tick at 100
inline constexpr float steadyRollSpringMax = 0.2f; // share of the roll error removed per tick at 100
inline constexpr float steadyTurnClamp = 0.035f;   // radians per tick (2 degrees): never a snap
inline constexpr float steadyBobMax = 0.35f;       // share of the height error removed per tick at 100
inline constexpr float steadyBobSmooth = 0.12f; // the smoothed height follows the head by this share a tick
inline constexpr float steadyBobClamp = 1.5f;   // cm per tick

inline float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool unit3(float* v) {
    const float n = std::sqrt(dot3(v, v));
    if (!(n > 1e-6f) || !std::isfinite(n)) return false;
    for (int i = 0; i < 3; ++i) v[i] /= n;
    return true;
}

// The pose the game builds from the points (EXP_tetra_to_mesh_affine 0x7ECD0): row i = sum_k coeff[i][k] *
// point k; rows 0..2 axes (1 up, 2 forward), row 3 position.
inline void tetraPose(const float (*points)[3], const float* coeffs, float* pose) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += coeffs[i * 4 + k] * points[k][j];
            pose[i * 3 + j] = s;
        }
}
// Rodrigues: v turned by rotation vector r (radians).
inline void turn3(const float* r, const float* v, float* o) {
    const float a = std::sqrt(dot3(r, r));
    if (!(a > 1e-9f)) {
        std::memcpy(o, v, 12);
        return;
    }
    const float k[3] = {r[0] / a, r[1] / a, r[2] / a}, c = std::cos(a), s = std::sin(a);
    float kv[3];
    cross3(k, v, kv);
    const float kd = dot3(k, v);
    for (int i = 0; i < 3; ++i) o[i] = v[i] * c + kv[i] * s + k[i] * kd * (1 - c);
}
inline float steadyHeading(float (*points)[3], const float* coeffs, const float* gaze, int strength) {
    float pose[12];
    tetraPose(points, coeffs, pose);
    for (float v : pose)
        if (!std::isfinite(v)) return 0.f;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(points[k][i])) return 0.f;
    const float horizontal = pose[6] * pose[6] + pose[8] * pose[8];
    const float wanted = gaze[0] * gaze[0] + gaze[2] * gaze[2];
    if (!(horizontal > 1e-4f && wanted > 1e-4f) || strength <= 0) return 0.f;
    float error = std::remainder(std::atan2(gaze[0], gaze[2]) - std::atan2(pose[6], pose[8]), 6.28318530718f);
    // Only the small steady bias; large gaps belong to the game's ordinary head turn.
    if (!std::isfinite(error) || std::fabs(error) > 0.35f) return 0.f;
    float turn = error * float(strength > 100 ? 100 : strength) * 0.01f * 0.25f;
    turn = turn < -steadyTurnClamp ? -steadyTurnClamp : (turn > steadyTurnClamp ? steadyTurnClamp : turn);
    if (std::fabs(turn) < 1e-6f) return 0.f;
    float centre[3]{};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i) centre[i] += points[k][i] * 0.25f;
    for (float v : centre)
        if (!std::isfinite(v)) return 0.f;
    const float rotation[3] = {0, turn, 0};
    for (int k = 0; k < 4; ++k) {
        float v[3], out[3];
        for (int i = 0; i < 3; ++i) v[i] = points[k][i] - centre[i];
        turn3(rotation, v, out);
        for (int i = 0; i < 3; ++i) points[k][i] = centre[i] + out[i];
    }
    return turn;
}
// The rotation vector taking frame (a0, a1, a2) to (b0, b1, b2), for small turns: half the sum of a_i x b_i.
inline void frameTurn(const float* const* a, const float* const* b, float* r) {
    r[0] = r[1] = r[2] = 0;
    for (int i = 0; i < 3; ++i) {
        float c[3];
        cross3(a[i], b[i], c);
        for (int j = 0; j < 3; ++j) r[j] += 0.5f * c[j];
    }
}

// One tick. points: the head's 4 working points (in/out). gaze: the unit direction the mouse commands.
// Returns false (and resets) when anything is unusable; nothing is changed then.
inline bool steadyHead(HeadSteadyState& st, float (*points)[3], const float* coeffs, const float* gaze,
                       const HeadSteadyStrength& s) {
    float pose[12];
    tetraPose(points, coeffs, pose);
    float f[3] = {pose[6], pose[7], pose[8]}, u[3] = {pose[3], pose[4], pose[5]},
          g[3] = {gaze[0], gaze[1], gaze[2]};
    if (!unit3(f) || !unit3(g)) {
        st.primed = false;
        return false;
    }
    {
        const float d = dot3(u, f);
        for (int i = 0; i < 3; ++i) u[i] -= d * f[i];
    }
    if (!unit3(u)) {
        st.primed = false;
        return false;
    }
    float rgt[3];
    cross3(u, f, rgt);
    float centre[3]{};
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 3; ++j) centre[j] += 0.25f * points[k][j];
    for (float v : centre)
        if (!std::isfinite(v)) {
            st.primed = false;
            return false;
        }
    if (!st.primed) {
        std::memcpy(st.forward, f, 12);
        std::memcpy(st.up, u, 12);
        std::memcpy(st.right, rgt, 12);
        std::memcpy(st.gaze, g, 12);
        st.height = centre[1];
        st.primed = true;
        return true;
    }
    // this tick's turn of the head, and the turn the gaze asked for
    const float* was[3] = {st.right, st.up, st.forward};
    const float* now[3] = {rgt, u, f};
    float headTurn[3];
    frameTurn(was, now, headTurn);
    float gazeTurn[3];
    cross3(st.gaze, g, gazeTurn);
    float e[3];
    for (int i = 0; i < 3; ++i) e[i] = headTurn[i] - gazeTurn[i];
    // a turn toward where the mouse points (the head catching up after a turn) is never slowed
    float toward[3];
    cross3(f, g, toward);
    const float gap = std::sqrt(dot3(toward, toward));
    if (gap > 1e-4f) {
        for (float& v : toward) v /= gap;
        const float along = dot3(e, toward);
        const float keep = along > 0 ? (along < gap ? along : gap) : 0.f;
        for (int i = 0; i < 3; ++i) e[i] -= keep * toward[i];
    } else
        toward[0] = toward[1] = toward[2] = 0;
    const float worldUp[3] = {0, 1, 0};
    const float ky = steadyDampMax * float(s.yaw) / 100.f, kp = steadyDampMax * float(s.pitch) / 100.f,
                kr = steadyDampMax * float(s.roll) / 100.f;
    float c[3];
    const float ey = dot3(e, worldUp), ep = dot3(e, rgt), er = dot3(e, f);
    for (int i = 0; i < 3; ++i) c[i] = -(ky * ey * worldUp[i] + kp * ep * rgt[i] + kr * er * f[i]);
    // a push toward where the mouse points never reaches past it: at most half the gap left (after a flick the
    // damping would otherwise carry the head beyond the gaze)
    {
        const float push = dot3(c, toward), most = 0.5f * gap;
        if (push > most)
            for (int i = 0; i < 3; ++i) c[i] -= (push - most) * toward[i];
    }
    // roll back toward level: the head's up toward world up, about the forward axis (not near straight up or down)
    if (std::fabs(f[1]) < 0.94f) {
        float level[3] = {-f[1] * f[0], 1.f - f[1] * f[1], -f[1] * f[2]};
        if (unit3(level)) {
            float x[3];
            cross3(u, level, x);
            const float roll = dot3(x, f);
            for (int i = 0; i < 3; ++i) c[i] += steadyRollSpringMax * float(s.roll) / 100.f * roll * f[i];
        }
    }
    const float cl = std::sqrt(dot3(c, c));
    if (cl > steadyTurnClamp)
        for (float& v : c) v *= steadyTurnClamp / cl;
    // height toward its smoothed value
    float lift = 0;
    if (s.bob > 0) {
        st.height += (centre[1] - st.height) * steadyBobSmooth;
        lift = (st.height - centre[1]) * steadyBobMax * float(s.bob) / 100.f;
        lift = lift > steadyBobClamp ? steadyBobClamp : (lift < -steadyBobClamp ? -steadyBobClamp : lift);
    } else
        st.height = centre[1];
    for (int k = 0; k < 4; ++k) {
        float d[3];
        for (int j = 0; j < 3; ++j) d[j] = points[k][j] - centre[j];
        float t[3];
        turn3(c, d, t);
        for (int j = 0; j < 3; ++j) points[k][j] = centre[j] + t[j] + (j == 1 ? lift : 0.f);
    }
    turn3(c, f, st.forward);
    turn3(c, u, st.up);
    turn3(c, rgt, st.right);
    std::memcpy(st.gaze, g, 12);
    return true;
}

// The chest and neck under the head: the same damping, pitch and roll only (yaw is turning and swinging),
// relative to the world. strength 0..100.
struct BodySteadyState {
    bool primed = false;
    float axes[3][3]{};
};
inline bool steadyBodyPart(BodySteadyState& st, float (*points)[3], const float* coeffs, int strength) {
    float pose[12];
    tetraPose(points, coeffs, pose);
    float a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a[i][j] = pose[i * 3 + j];
        if (!unit3(a[i])) {
            st.primed = false;
            return false;
        }
    }
    float centre[3]{};
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 3; ++j) centre[j] += 0.25f * points[k][j];
    if (!st.primed) {
        std::memcpy(st.axes, a, sizeof a);
        st.primed = true;
        return true;
    }
    const float* was[3] = {st.axes[0], st.axes[1], st.axes[2]};
    const float* now[3] = {a[0], a[1], a[2]};
    float turn[3];
    frameTurn(was, now, turn);
    const float k = steadyDampMax * 0.6f * float(strength) / 100.f;
    float c[3] = {-k * turn[0], 0.f, -k * turn[2]}; // world x and z: tilting; world y (yaw) left alone
    const float cl = std::sqrt(dot3(c, c));
    if (cl > steadyTurnClamp)
        for (float& v : c) v *= steadyTurnClamp / cl;
    for (int kk = 0; kk < 4; ++kk) {
        float d[3];
        for (int j = 0; j < 3; ++j) d[j] = points[kk][j] - centre[j];
        float t[3];
        turn3(c, d, t);
        for (int j = 0; j < 3; ++j) points[kk][j] = centre[j] + t[j];
    }
    for (int i = 0; i < 3; ++i) turn3(c, a[i], st.axes[i]);
    return true;
}

// Spine follows aim: looking down bends the spine forward, looking up bends it back, so a swing aims where
// you look. The game's pose builder 0x1760F0 multiplies a part's extra rotation part+0x5EC (x y z w, applied
// when part+0xC has 0x4000) into its animation pose before turning it into joint targets
// , so the bend rides on whatever animation plays, combat included, and
// the joint motors make it soft. The bend is a share of the gaze's pitch, split between the chest and the
// spine part below it. Its axis is the part's own X; which way +X turns a part is read from its pose each
// tick, and a part whose X is not its side axis is not bent.
inline constexpr float spineBendShare = 0.6f;           // of the gaze pitch at 100 %
inline constexpr float spineBendMax = 0.7f;             // radians (40 degrees), the whole spine
inline constexpr float spineLowerShare = 0.4f;          // the part below the chest; the chest takes the rest
// Extra rotations are in the parent frame. Preserve an existing native extra
// rotation by applying the mod's bend after it: qMod * qExisting * qAnimation.
inline bool composeSpineRotation(const float* mod, const float* existing, float* out) {
    float norm = 0.f;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(mod[i]) || !std::isfinite(existing[i])) return false;
        norm += existing[i] * existing[i];
    }
    if (!(norm > 1e-8f) || !std::isfinite(norm)) return false;
    const float q[4] = {
        mod[3] * existing[0] + mod[0] * existing[3] + mod[1] * existing[2] - mod[2] * existing[1],
        mod[3] * existing[1] - mod[0] * existing[2] + mod[1] * existing[3] + mod[2] * existing[0],
        mod[3] * existing[2] + mod[0] * existing[1] - mod[1] * existing[0] + mod[2] * existing[3],
        mod[3] * existing[3] - mod[0] * existing[0] - mod[1] * existing[1] - mod[2] * existing[2]};
    for (float v : q)
        if (!std::isfinite(v)) return false;
    std::memcpy(out, q, sizeof q);
    return true;
}
inline float spineBend(const float* gaze, int follow) { // radians, positive forward (looking down)
    const float y = gaze[1] < -1.f ? -1.f : (gaze[1] > 1.f ? 1.f : gaze[1]);
    const float bend =
        -std::asin(y) * spineBendShare * float(follow < 0 ? 0 : (follow > 100 ? 100 : follow)) / 100.f;
    return bend > spineBendMax ? spineBendMax : (bend < -spineBendMax ? -spineBendMax : bend);
}
// 0x1CBD0(qNative,qExtra,out) computes Hamilton(qExtra,qNative); 0x1760F0 then
// composes the result with the parent's pose. Extra axes are therefore in the PARENT frame.
// Using the already-bent part frame both changes the axis and made the old 0.9 agreement gate
// switch the bend off once it had succeeded. The achieved part pose is deliberately not fed back.
inline bool spineBendQuat(const float* /*pose*/, const float* parentPose, const float* gaze, float angle,
                          float* quat) {
    float heading[3] = {gaze[0], 0, gaze[2]};
    if (!unit3(heading) || !std::isfinite(angle)) return false;
    const float up[3] = {0, 1, 0};
    float across[3], axis[3];
    cross3(up, heading, across);
    for (int i = 0; i < 3; ++i) axis[i] = dot3(parentPose + 3 * i, across);
    if (!unit3(axis)) return false;
    const float sn = std::sin(angle * .5f);
    for (int i = 0; i < 3; ++i) quat[i] = axis[i] * sn;
    quat[3] = std::cos(angle * .5f);
    return true;
}
// Lean: while the lean key is held the mouse bends the upper body instead of turning the view: sideways with
// its x, forward and back with its y, through the same chest and spine rotation (the head, and so the first-
// person eye, go with it). Let go and it eases back upright. It rides on the animation and the joints, so
// whether a lean reaches past a wall or round a corner is the game's collision's say.
inline constexpr float leanRadiansPerPixel = 0.004f, leanSideMax = 0.5f, leanForwardMax = 0.35f,
                       leanReturnSeconds = 0.15f;
struct Lean {
    float side = 0, forward = 0; // radians; side positive: to the right; forward positive: forward
    void step(float dx, float dy, bool held, float seconds) {
        auto clampTo = [](float v, float m) {
            return v < -m ? -m : (v > m ? m : v);
        };
        if (held) {
            side = clampTo(side + dx * leanRadiansPerPixel, leanSideMax);
            forward = clampTo(forward - dy * leanRadiansPerPixel, leanForwardMax);
        } else {
            const float keep =
                std::isfinite(seconds) && seconds > 0.f ? std::exp(-seconds / leanReturnSeconds) : 0.f;
            side *= keep;
            forward *= keep;
            if (std::fabs(side) < 1e-4f) side = 0;
            if (std::fabs(forward) < 1e-4f) forward = 0;
        }
        if (!std::isfinite(side) || !std::isfinite(forward)) side = forward = 0;
    }
    void keys(int sideHeld, float seconds) {
        const float k =
            seconds > 0.f && std::isfinite(seconds) ? 1.f - std::exp(-seconds / leanReturnSeconds) : 1.f;
        side += (float(sideHeld) * leanSideMax - side) * k;
        forward *= 1.f - k;
        if (std::fabs(side) < 1e-4f) side = 0;
        if (std::fabs(forward) < 1e-4f) forward = 0;
    }
    bool active() const { return side != 0 || forward != 0; }
};
// The extra rotation for one part with a lean: `forward` about the side axis (as spineBendQuat), then `side`
// about the heading. Both axes are expressed in the parent's frame, as the game's composition requires.
inline bool spineLeanQuat(const float* /*pose*/, const float* parentPose, const float* gaze, float forward,
                          float side, float* quat) {
    float heading[3] = {gaze[0], 0, gaze[2]};
    if (!unit3(heading) || !std::isfinite(forward) || !std::isfinite(side)) return false;
    const float up[3] = {0, 1, 0};
    float across[3];
    cross3(up, heading, across);
    auto local = [](const float* frame, const float* v, float* out) {
        for (int i = 0; i < 3; ++i)
            out[i] = frame[3 * i] * v[0] + frame[3 * i + 1] * v[1] + frame[3 * i + 2] * v[2];
        return unit3(out);
    };
    float a[3], h[3];
    if (!local(parentPose, across, a) || !local(parentPose, heading, h)) return false;
    const float sf = std::sin(forward * 0.5f), cf = std::cos(forward * 0.5f), ss = std::sin(side * 0.5f),
                cs = std::cos(side * 0.5f);
    const float p[4] = {a[0] * sf, a[1] * sf, a[2] * sf, cf}, q[4] = {h[0] * ss, h[1] * ss, h[2] * ss, cs};
    quat[0] = p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1]; // p * q: the lean, then the bend
    quat[1] = p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0];
    quat[2] = p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3];
    quat[3] = p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2];
    return true;
}
} // namespace fp
