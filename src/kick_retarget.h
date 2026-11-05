#pragma once
// The Animated kick's maths (docs/MIXAMO-KICK-DESIGN.md). A baked full-body track (kick_clip.h) is sampled by
// the game's own kick progress, mirrored to the stance, mapped into the game's world and turned into per-part
// orientation targets, which pose_drive.inl blends with the game's own motor targets. Pure: no game memory,
// unit-tested (tests/unit/kick_retarget_tests.cpp).
#include "xr/math.h"
#include <cstdint>

namespace fp::kick {
using Vec = XrVector3f;
using Quat = XrQuaternionf;

// A frame's slots (tools/kick_bake.py SLOTS): chords between joints both skeletons have, in the clip heading frame
// (+X left, +Y up, +Z forward), always a right-leg kick. Across vectors point from the right joint to the left one;
// hipOffset is the mid-hip from the supporting (left) foot, in cm; the rest are unit directions.
enum Slot : unsigned {
    pelvisUp, pelvisAcross, chestUp, chestAcross,
    leftShoulder, leftUpperArm, leftForearm, rightShoulder, rightUpperArm, rightForearm,
    leftThigh, leftShin, leftFoot, rightThigh, rightShin, rightFoot, hipOffset, slotCount
};
struct Frame { Vec v[slotCount]; };
struct Clip { const char* name; const Frame* frames; int count; float fps; int peak; float legLengthCm; int set; bool supportLeads; };

// What drives the body for the weapon in hand: the clip set and how much of the body follows it.
enum class Mode : uint8_t { full, lowerBody };
enum class WeaponClass : uint8_t { unarmed, oneHanded, twoHanded, polearm, count };
struct ClassRule { int set; Mode mode; };
// Per weapon class (Hunter, 2026-11-05). Polearms keep their hold: legs and hips only, from the unarmed set,
// until a polearm clip is added. (Nothing the mod reads tells a polearm from a two-handed sword yet; classify.)
inline constexpr ClassRule classRules[size_t(WeaponClass::count)] = {
    {0, Mode::full}, {0, Mode::full}, {1, Mode::full}, {0, Mode::lowerBody}};
inline constexpr const char* classNames[] = {"unarmed", "one_handed", "two_handed", "polearm"};
// The class from what the character holds: weapon count, the items in the two hands, and the grip class of
// its weapon (character::gripFlags: 0x10 one hand, 0x20/0x30 two hands with a right/left lead).
inline WeaponClass classify(int weaponCount, uintptr_t heldA, uintptr_t heldB, unsigned grip) {
    if (weaponCount < 1 || (!heldA && !heldB)) return WeaponClass::unarmed;
    if (weaponCount == 1 && ((heldA && heldA == heldB) || grip == 0x20 || grip == 0x30)) return WeaponClass::twoHanded;
    return WeaponClass::oneHanded;
}
// The next clip of a set, never the one just played when the set has another (a rotation).
inline int nextClip(const Clip* clips, int count, int set, int last) {
    int first = -1;
    for (int k = 1; k <= count; ++k) {
        const int i = ((last < 0 ? -1 : last) + k + count) % count;
        if (clips[i].set != set) continue;
        if (i != last) return i;
        if (first < 0) first = i;
    }
    return first;
}

// Tuning (Phase 3 adjusts these from play checks).
inline constexpr float hitProgress = .35f;  // native kick progress where the clip's foot peak lands (assumed; measured later)
inline constexpr float blendIn = .12f, blendOut = .15f; // shares of the kick's progress spent fading in and out
inline constexpr float maxPartStep = .35f;  // radians a target may lead the achieved part in one step (safety only)
inline constexpr float maxHipStep = .10f;   // metres the hip target may lead the achieved hips in one step
inline constexpr float heldArmWeight = .6f; // an arm holding something follows the clip this much (the grip stays native)

inline Vec normalised(Vec v, Vec fallback = {0, -1, 0}) {
    const float n = fp::xr::length(v);
    return n > 1e-6f && std::isfinite(n) ? fp::xr::scale(v, 1 / n) : fallback;
}
inline float smooth(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3 - 2 * x);
}
// The clip frame (fractional) for the game's kick progress: [0, hit, 1] -> [0, peak, last].
inline float clipFrameAt(const Clip& c, float progress, float hit = hitProgress) {
    const float last = float(c.count - 1), peak = float(c.peak);
    if (!(progress > 0)) return 0;
    if (progress >= 1) return last;
    return progress < hit ? peak * progress / hit : peak + (last - peak) * (progress - hit) / (1 - hit);
}
// How much of the clip drives the body: 0 before the kick and in its start dwell, 1 in the middle.
inline float weightAt(float progress) {
    if (!(progress > 0) || progress >= 1) return 0;
    return (std::min)(smooth(progress / blendIn), smooth((1 - progress) / blendOut));
}
inline Frame sample(const Clip& c, float at) {
    at = std::clamp(at, 0.f, float(c.count - 1));
    const int a = (std::min)(int(at), c.count - 1), b = (std::min)(a + 1, c.count - 1);
    const float t = at - float(a);
    Frame out{};
    for (unsigned s = 0; s < slotCount; ++s) {
        const Vec m = fp::xr::add(fp::xr::scale(c.frames[a].v[s], 1 - t), fp::xr::scale(c.frames[b].v[s], t));
        out.v[s] = s == hipOffset ? m : normalised(m);
    }
    return out;
}
// The mirror image: a left-leg kick from the stored right-leg one. Positions negate X and left and right swap, so
// a direction negates X, an across vector (left minus right) negates Y and Z instead, and the offset from the
// supporting foot (now the right one) negates X.
inline Frame mirrored(const Frame& f) {
    Frame out = f;
    auto flipX = [](Vec v) { return Vec{-v.x, v.y, v.z}; };
    for (unsigned s : {pelvisAcross, chestAcross}) out.v[s] = {f.v[s].x, -f.v[s].y, -f.v[s].z};
    for (unsigned s : {pelvisUp, chestUp, hipOffset}) out.v[s] = flipX(f.v[s]);
    const unsigned left[] = {leftShoulder, leftUpperArm, leftForearm, leftThigh, leftShin, leftFoot};
    const unsigned right[] = {rightShoulder, rightUpperArm, rightForearm, rightThigh, rightShin, rightFoot};
    for (unsigned i = 0; i < 6; ++i) {
        out.v[left[i]] = flipX(f.v[right[i]]);
        out.v[right[i]] = flipX(f.v[left[i]]);
    }
    return out;
}
// The heading frame at kick start, in game world axes (+Y up): forward from the character's facing, left from
// its hips (left hip minus right hip), both levelled. The left axis is measured, so the clip's left lands on the
// body's left whatever the game's handedness.
struct Heading { Vec left{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1}; bool valid = false; };
inline Heading headingFrom(Vec facing, Vec leftHipMinusRight) {
    Heading h{};
    const Vec f{facing.x, 0, facing.z};
    if (fp::xr::length(f) < 1e-3f) return h;
    h.forward = normalised(f);
    Vec l{leftHipMinusRight.x, 0, leftHipMinusRight.z};
    l = fp::xr::sub(l, fp::xr::scale(h.forward, fp::xr::dot(l, h.forward)));
    if (fp::xr::length(l) < 1e-3f) return h;
    h.left = normalised(l);
    h.valid = true;
    return h;
}
inline Vec toWorld(const Heading& h, Vec v) {
    return fp::xr::add(fp::xr::add(fp::xr::scale(h.left, v.x), fp::xr::scale(h.up, v.y)), fp::xr::scale(h.forward, v.z));
}
// The clip's frame at this progress in world axes: the stored kick, mirrored for a left-leg kick.
inline Frame worldFrame(const Clip& c, const Heading& h, float progress, bool leftLeg) {
    Frame f = sample(c, clipFrameAt(c, progress));
    if (leftLeg) f = mirrored(f);
    for (unsigned s = 0; s < slotCount; ++s) f.v[s] = s == hipOffset ? toWorld(h, f.v[s]) : normalised(toWorld(h, f.v[s]));
    return f;
}
// Whether the clip must be mirrored for this stance: its supporting foot leads the hips (a rear-leg kick) or
// not, against which of the game character's feet leads (the one further along its facing).
inline bool mirrorFor(const Clip& c, bool leftFootLeads) { return c.supportLeads != leftFootLeads; }

// Caps a rotation at `limit` radians.
inline Quat limited(Quat q, float limit) {
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const float angle = 2 * std::acos(std::clamp(q.w, 0.f, 1.f));
    if (angle <= limit) return q;
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-6f) return {0, 0, 0, 1};
    const float s = std::sin(limit / 2) / n;
    return {q.x * s, q.y * s, q.z * s, std::cos(limit / 2)};
}
// A limb part's target: the achieved orientation turned so its bone points along `wanted` (twist kept), capped.
inline Quat boneTarget(Quat achieved, Vec achievedDirection, Vec wanted, float limit = maxPartStep) {
    const Vec from = normalised(achievedDirection), to = normalised(wanted, from);
    return fp::xr::multiply(limited(fp::xr::directionRotation(from, to), limit), achieved);
}
// The rotation taking a frame given by an up and an across vector onto another: up onto up, then about it so the
// across vectors agree. Only rotations between measured vectors, no cross-product handedness.
inline Quat alignTwo(Vec fromUp, Vec fromAcross, Vec toUp, Vec toAcross) {
    fromUp = normalised(fromUp);
    toUp = normalised(toUp, fromUp);
    const Quat tilt = fp::xr::directionRotation(fromUp, toUp);
    auto level = [&](Vec v) { return fp::xr::sub(v, fp::xr::scale(toUp, fp::xr::dot(v, toUp))); };
    const Vec a = level(fp::xr::rotate(tilt, fromAcross)), b = level(toAcross);
    if (fp::xr::length(a) < 1e-4f || fp::xr::length(b) < 1e-4f) return tilt;
    return fp::xr::multiply(fp::xr::directionRotation(normalised(a), normalised(b)), tilt);
}
// A trunk part's target (pelvis, belly, chest): the achieved orientation turned so its measured up and across
// vectors match the clip's, capped.
inline Quat frameTarget(Quat achieved, Vec achievedUp, Vec achievedAcross, Vec wantedUp, Vec wantedAcross, float limit = maxPartStep) {
    return fp::xr::multiply(limited(alignTwo(achievedUp, achievedAcross, wantedUp, wantedAcross), limit), achieved);
}
// Where the mid-hip should be: above the supporting foot by the clip's offset, scaled from the clip's leg to the
// character's, and at most maxHipStep from where it is.
inline Vec hipTarget(Vec supportFoot, Vec worldOffsetCm, float clipLegCm, float legMetres, Vec achievedHip) {
    const float scale = clipLegCm > 1 && legMetres > .1f ? legMetres / (clipLegCm * .01f) : 1.f;
    const Vec wanted = fp::xr::add(supportFoot, fp::xr::scale(worldOffsetCm, .01f * scale));
    const Vec delta = fp::xr::sub(wanted, achievedHip);
    const float d = fp::xr::length(delta);
    return d > maxHipStep ? fp::xr::add(achievedHip, fp::xr::scale(delta, maxHipStep / d)) : wanted;
}
// Shortest-path spherical blend: 0 gives a, 1 gives b.
inline Quat blend(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    float wa = 1 - t, wb = t;
    if (d < .9995f) {
        const float theta = std::acos(std::clamp(d, -1.f, 1.f)), s = std::sin(theta);
        wa = std::sin((1 - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    Quat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : a;
}
}
