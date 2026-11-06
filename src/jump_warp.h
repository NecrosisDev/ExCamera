#pragma once
// The Animated jump's maths (docs/GAME-BALANCE-PHYSICS.md, "The Animated jump"). A baked full-body jump (jump_clip.h)
// is played against the jump's own physics: the windup at the clip's rate, then the flight time-warped onto the push's
// measured rise and fall, then the landing at the clip's rate. The chords are the kick's (kick_retarget.h); the hips'
// position is never driven. Pure: no game memory, unit-tested (tests/unit/jump_warp_tests.cpp).
#include "kick_retarget.h"
#include <algorithm>
#include <cmath>

namespace fp::jump {
using kick::Frame;
using kick::Vec;

// A baked clip (tools/jump_bake.py): frames as authored, the per-frame drop (mid-hip above the lowest foot point, cm),
// the phase frames found from the data, and the clip's bone lengths (frame 0, cm) for the tuck sums.
struct Clip {
    const char* name;
    const Frame* frames;
    const float* drops;
    int count;
    float fps;
    int takeoff, peak, touchdown, end;
    float legLengthCm, hipHalfWidthCm, thighCm, shinCm, footCm;
    bool leftLeads;
};
inline constexpr int standingClip = 0, travellingClip = 1; // jump_bake.py CLIPS order

// Tuning, all ours.
inline constexpr float blendInSeconds = .10f;  // the clip fades in from the press
inline constexpr float blendOutSeconds = .20f; // and out after the landing's end frame, or at a cancel
inline constexpr float tuckMin = 0.f, tuckMax = 1.f; // StandingJumpTuck's range (1 = as authored)

inline Frame sample(const Clip& c, float at) {
    at = std::clamp(at, 0.f, float(c.count - 1));
    const int a = (std::min)(int(at), c.count - 1), b = (std::min)(a + 1, c.count - 1);
    const float t = at - float(a);
    Frame out{};
    for (unsigned s = 0; s < kick::slotCount; ++s) {
        const Vec m = fp::xr::add(fp::xr::scale(c.frames[a].v[s], 1 - t), fp::xr::scale(c.frames[b].v[s], t));
        out.v[s] = s == kick::hipOffset ? m : kick::normalised(m);
    }
    return out;
}

// The tuck: each leg chord's deviation from frame 0 scaled by s (1 as authored, 0 frame 0's legs), renormalised.
inline constexpr unsigned legSlots[] = {kick::leftThigh, kick::leftShin, kick::leftFoot, kick::rightThigh, kick::rightShin, kick::rightFoot};
inline Frame tucked(const Frame& f0, const Frame& f, float s) {
    Frame out = f;
    for (unsigned k : legSlots) out.v[k] = kick::normalised(fp::xr::add(f0.v[k], fp::xr::scale(fp::xr::sub(f.v[k], f0.v[k]), s)));
    return out;
}
// How far the lowest foot point (ankle or toe) hangs below the mid-hip, cm, rebuilt from a frame's chords and the
// clip's bone lengths (matches the authored drop within a millimetre or so).
inline float dropBelowHips(const Clip& c, const Frame& f) {
    float low = 1e9f;
    const unsigned thigh[2] = {kick::leftThigh, kick::rightThigh}, shin[2] = {kick::leftShin, kick::rightShin}, foot[2] = {kick::leftFoot, kick::rightFoot};
    for (unsigned side = 0; side < 2; ++side) {
        const float hip = (side ? -1.f : 1.f) * c.hipHalfWidthCm * f.v[kick::pelvisAcross].y;
        const float ankle = hip + c.thighCm * f.v[thigh[side]].y + c.shinCm * f.v[shin[side]].y;
        low = (std::min)({low, ankle, ankle + c.footCm * f.v[foot[side]].y});
    }
    return -low;
}
// The root's rise at a flight frame for a ballistic hop of peakCm, with the flight warped as the warp warps it
// (takeoff -> peak and peak -> touchdown linear in time).
inline float flightRise(const Clip& c, float frame, float peakCm) {
    if (frame >= float(c.takeoff) && frame <= float(c.peak) && c.peak > c.takeoff) {
        const float u = (frame - float(c.takeoff)) / float(c.peak - c.takeoff);
        return peakCm * (1 - (1 - u) * (1 - u));
    }
    if (frame > float(c.peak) && frame <= float(c.touchdown)) {
        const float u = (frame - float(c.peak)) / float(c.touchdown - c.peak);
        return peakCm * (1 - u * u);
    }
    return 0;
}
// The lowest foot's height above the ground at a frame: the hop's rise plus how much higher the tucked legs hang than
// at frame 0 (the game's standing pose taken as the clip's frame 0, its leg as the clip's).
inline float footClearance(const Clip& c, int frame, float s, float peakCm) {
    const Frame& f0 = c.frames[0];
    return flightRise(c, float(frame), peakCm) + dropBelowHips(c, f0) - dropBelowHips(c, tucked(f0, c.frames[frame], s));
}
// The largest tuck, in 0.01 steps, keeping the lowest foot within marginCm at every flight frame (takeoff..touchdown).
inline float maxTuck(const Clip& c, float peakCm, float marginCm) {
    float best = 0;
    for (int k = 0; k <= 100; ++k) {
        const float s = float(k) / 100.f;
        for (int f = c.takeoff; f <= c.touchdown; ++f)
            if (footClearance(c, f, s, peakCm) > marginCm) return best;
        best = s;
    }
    return best;
}

// Mirrored when the clip's stance is the other way round from the character's. supported: the one foot the
// character has down (0 left, 1 right), or -1 for both or neither; then the leading foot decides. The clip stands on
// its trailing foot at frame 0 (the running jump's push-off foot).
inline bool mirrorFor(const Clip& c, bool leftLeads, int supported = -1) {
    if (supported == 0 || supported == 1) return (supported == 0) != !c.leftLeads;
    return c.leftLeads != leftLeads;
}
// The clip's frame in world axes: tucked (legs only, against frame 0), mirrored as needed, mapped by the heading.
inline Frame worldFrame(const Clip& c, const kick::Heading& h, float at, float tuck, bool mirror) {
    Frame f = tucked(c.frames[0], sample(c, at), tuck);
    if (mirror) f = kick::mirrored(f);
    for (unsigned s = 0; s < kick::slotCount; ++s) f.v[s] = s == kick::hipOffset ? Vec{} : kick::normalised(kick::toWorld(h, f.v[s]));
    return f;
}

// The phases (docs: "The Animated jump"). windup: frames 0 -> takeoff at the clip's rate, no push yet; rise: takeoff
// -> peak onto the push's takeoff -> measured peak; fall: peak -> touchdown onto the measured peak -> back down;
// landing: touchdown -> end at the clip's rate (the brace); blendOut: the clip fades out (also after a cancel).
enum class Phase : uint8_t { none, windup, rise, fall, landing, blendOut, count };
inline constexpr const char* phaseNames[] = {"none", "windup", "rise", "fall", "landing", "blend_out"};
static_assert(sizeof phaseNames / sizeof *phaseNames == size_t(Phase::count));

inline float windupSeconds(const Clip& c) { return c.fps > 0 ? float(c.takeoff) / c.fps : 0.f; }
// Rising: the time since takeoff against the time still to the peak at the upward speed now (up / g).
inline float riseFrame(const Clip& c, float sinceTakeoff, float upSpeed, float gravity) {
    const float left = std::fmax(upSpeed, 0.f) / gravity, total = sinceTakeoff + left;
    const float u = total > 1e-6f ? std::clamp(sinceTakeoff / total, 0.f, 1.f) : 1.f;
    return float(c.takeoff) + u * float(c.peak - c.takeoff);
}
// Falling: the time since the peak against the time still to fall back to the takeoff height, the root of
// risen + v t - g t^2 / 2 = 0 (v upward, negative falling).
inline float fallFrame(const Clip& c, float sincePeak, float speed, float risenCm, float gravity) {
    const float left = (speed + std::sqrt(speed * speed + 2 * gravity * std::fmax(risenCm, 0.f))) / gravity;
    const float total = sincePeak + std::fmax(left, 0.f);
    const float u = total > 1e-6f ? std::clamp(sincePeak / total, 0.f, 1.f) : 1.f;
    return float(c.peak) + u * float(c.touchdown - c.peak);
}

// What the warp reads from the push each poll (fp::JumpPush): the measured upward speed and rise (cm/s, cm, from
// the takeoff height), whether the push is still adding speed and its takeoff speed, and whether it still follows
// the flight (false once the root is back down, or the push gave up).
struct Flight { float speed = 0, risen = 0, takeoffSpeed = 0; bool pushing = false, tracking = false; };
struct Warp {
    Phase phase = Phase::none;
    float frame = 0, phaseAt = 0, weight = 0, fadeFrom = 0;
};
// The press: a windup first when there is one to play, else straight to the rise (the push starts at the press).
inline Warp startWarp(const Clip& c, bool windup) {
    Warp w;
    w.phase = windup && c.takeoff > 0 ? Phase::windup : Phase::rise;
    w.frame = w.phase == Phase::rise ? float(c.takeoff) : 0.f;
    return w;
}
inline void cancelWarp(Warp& w, float t) {
    if (w.phase == Phase::none || w.phase == Phase::blendOut) return;
    w.fadeFrom = w.weight;
    w.phase = Phase::blendOut;
    w.phaseAt = t;
}
// One poll, t seconds after the press. true exactly once: the windup has ended and the push must start now.
inline bool advanceWarp(Warp& w, const Clip& c, float t, const Flight& in, float gravity) {
    bool takeoff = false;
    switch (w.phase) {
    case Phase::none: break;
    case Phase::windup:
        w.frame = std::fmin(t * c.fps, float(c.takeoff));
        if (t >= windupSeconds(c)) {
            w.phase = Phase::rise;
            w.phaseAt = t;
            w.frame = float(c.takeoff);
            takeoff = true;
        }
        break;
    case Phase::rise:
        if (!in.tracking) { // back down (or never up): the landing
            w.phase = Phase::landing;
            w.phaseAt = t;
            w.frame = std::fmax(w.frame, float(c.touchdown));
        } else if (!in.pushing && t > w.phaseAt && in.speed <= 0) { // the measured peak
            w.phase = Phase::fall;
            w.phaseAt = t;
            w.frame = std::fmax(w.frame, float(c.peak));
        } else
            w.frame = std::fmax(w.frame, riseFrame(c, t - w.phaseAt, in.pushing ? std::fmax(in.speed, in.takeoffSpeed) : in.speed, gravity));
        break;
    case Phase::fall:
        if (!in.tracking || in.risen <= 0) {
            w.phase = Phase::landing;
            w.phaseAt = t;
            w.frame = std::fmax(w.frame, float(c.touchdown));
        } else
            w.frame = std::fmax(w.frame, fallFrame(c, t - w.phaseAt, in.speed, in.risen, gravity));
        break;
    case Phase::landing:
        w.frame = std::fmax(w.frame, std::fmin(float(c.touchdown) + (t - w.phaseAt) * c.fps, float(c.end)));
        if (w.frame >= float(c.end)) {
            w.fadeFrom = w.weight;
            w.phase = Phase::blendOut;
            w.phaseAt = t;
        }
        break;
    case Phase::blendOut:
        if (t - w.phaseAt >= blendOutSeconds) w.phase = Phase::none;
        break;
    default: break;
    }
    if (w.phase == Phase::none) w.weight = 0;
    else if (w.phase == Phase::blendOut) w.weight = w.fadeFrom * (1 - kick::smooth((t - w.phaseAt) / blendOutSeconds));
    else w.weight = kick::smooth(t / blendInSeconds);
    return takeoff;
}
}
