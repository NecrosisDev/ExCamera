#include "jump_clip.h"
#include "camera_motion.h"

#include <cmath>
#include <string_view>

#include "../test_support.hpp"

namespace {

namespace j = fp::jump;
namespace k = fp::kick;

bool near_equal(float a, float b, float eps = 1e-3f) { return std::isfinite(a) && std::fabs(a - b) <= eps; }
bool near_vec(k::Vec a, k::Vec b, float eps = 1e-3f) { return near_equal(a.x, b.x, eps) && near_equal(a.y, b.y, eps) && near_equal(a.z, b.z, eps); }

bool baked_phases_come_from_the_clips() {
    static_assert(j::clipCount == 2);
    const auto& s = j::clips[j::standingClip];
    const auto& r = j::clips[j::travellingClip];
    CHECK(std::string_view(s.name) == "jumping" && std::string_view(r.name) == "runningJump");
    // Jumping: the hips' dip ends at 10, peak at 23, both feet down again at 33, settled by 50.
    CHECK(s.count == 57 && s.fps == 30.f && s.takeoff == 10 && s.peak == 23 && s.touchdown == 33 && s.end == 50);
    // Running Jump: rising from the first frame, peak at 10, one foot down at 18, settled by 26.
    CHECK(r.count == 28 && r.takeoff == 0 && r.peak == 10 && r.touchdown == 18 && r.end == 26);
    for (const auto& c : j::clips) {
        CHECK(c.takeoff < c.peak && c.peak < c.touchdown && c.touchdown <= c.end && c.end < c.count);
        for (int i = 0; i < c.count; ++i) {
            for (unsigned slot = 0; slot < k::hipOffset; ++slot) CHECK(near_equal(fp::xr::length(c.frames[i].v[slot]), 1.f, 2e-3f));
            CHECK(near_vec(c.frames[i].v[k::hipOffset], {0, 0, 0}, 0)); // the hips' position is never driven
            // The chords and bone lengths rebuild the authored drop.
            CHECK(near_equal(j::dropBelowHips(c, c.frames[i]), c.drops[i], .5f));
        }
        CHECK(c.frames[0].v[k::pelvisUp].y > .8f && c.frames[0].v[k::chestUp].y > .8f);
    }
    // The standing peak tucks the legs: the feet hang far less below the hips than at the start.
    CHECK(s.drops[0] - s.drops[s.peak] > 25.f);
    return true;
}

bool standing_tuck_default_keeps_the_feet_in_the_margin() {
    const auto& s = j::clips[j::standingClip];
    const float most = j::maxTuck(s, fp::jumpPeakCm, fp::jumpInAirMarginCm);
    CHECK(near_equal(most, j::standingTuckDefault, .011f));
    static_assert(j::standingTuckDefault > 0 && j::standingTuckDefault < 1);
    float worst = 0, authored = 0;
    for (int f = s.takeoff; f <= s.touchdown; ++f) {
        worst = std::fmax(worst, j::footClearance(s, f, j::standingTuckDefault, fp::jumpPeakCm));
        authored = std::fmax(authored, j::footClearance(s, f, 1.f, fp::jumpPeakCm));
    }
    CHECK(worst <= fp::jumpInAirMarginCm && authored > fp::jumpInAirMarginCm);
    CHECK(j::footClearance(s, s.takeoff, 0, fp::jumpPeakCm) < 1e-3f); // untucked at takeoff: on the ground
    CHECK(near_equal(j::flightRise(s, float(s.peak), 15), 15) && j::flightRise(s, float(s.touchdown), 15) == 0);
    return true;
}

bool tuck_scales_only_the_legs() {
    const auto& s = j::clips[j::standingClip];
    const auto& f0 = s.frames[0];
    const auto& p = s.frames[s.peak];
    const auto full = j::tucked(f0, p, 1), none = j::tucked(f0, p, 0), half = j::tucked(f0, p, .5f);
    for (unsigned slot = 0; slot < k::slotCount; ++slot) CHECK(near_vec(full.v[slot], p.v[slot]));
    for (unsigned slot : j::legSlots) CHECK(near_vec(none.v[slot], f0.v[slot]));
    CHECK(near_vec(none.v[k::chestUp], p.v[k::chestUp]) && near_vec(half.v[k::leftUpperArm], p.v[k::leftUpperArm]));
    CHECK(j::dropBelowHips(s, half) > j::dropBelowHips(s, full) && j::dropBelowHips(s, half) < j::dropBelowHips(s, none));
    return true;
}

bool flight_maps_are_monotone_and_hit_their_ends() {
    const auto& s = j::clips[j::standingClip];
    const float g = fp::jumpGravity, v = fp::jumpTakeoffSpeed(15);
    CHECK(near_equal(j::riseFrame(s, 0, v, g), float(s.takeoff)) && near_equal(j::riseFrame(s, .1f, 0, g), float(s.peak)));
    // A ballistic flight maps time linearly: halfway to the peak is halfway through the rising frames.
    const float top = v / g;
    CHECK(near_equal(j::riseFrame(s, top / 2, v - g * top / 2, g), (s.takeoff + s.peak) / 2.f, 1e-2f));
    CHECK(near_equal(j::fallFrame(s, 0, 0, 15, g), float(s.peak)) && near_equal(j::fallFrame(s, top, -v, 0, g), float(s.touchdown)));
    CHECK(near_equal(j::fallFrame(s, top / 2, -g * top / 2, 15 - .5f * g * (top / 2) * (top / 2), g), (s.peak + s.touchdown) / 2.f, 1e-2f));
    return true;
}

// A ballistic hop to 15 cm, polled at 83 Hz, through the whole warp.
bool standing_warp_runs_every_phase_in_order() {
    const auto& s = j::clips[j::standingClip];
    const float g = fp::jumpGravity, v = fp::jumpTakeoffSpeed(15), dt = 1 / 83.f;
    auto w = j::startWarp(s, true);
    CHECK(w.phase == j::Phase::windup && w.frame == 0);
    int takeoffs = 0;
    float t = 0, takeoffAt = -1, last = -1, peakFrame = -1, touchFrame = -1;
    j::Phase seen = w.phase;
    for (int i = 0; i < 400 && w.phase != j::Phase::none; ++i, t += dt) {
        j::Flight in{};
        if (takeoffAt >= 0) {
            const float tf = t - takeoffAt;
            in.risen = v * tf - .5f * g * tf * tf;
            in.speed = v - g * tf;
            in.takeoffSpeed = v;
            in.pushing = tf < .05f;
            in.tracking = in.risen > 0 || tf < .01f;
        }
        if (j::advanceWarp(w, s, t, in, g)) {
            ++takeoffs;
            takeoffAt = t;
            CHECK(near_equal(t, j::windupSeconds(s), dt + 1e-4f) && w.frame == float(s.takeoff));
        }
        CHECK(w.frame >= last || w.phase == j::Phase::none);
        last = w.frame;
        CHECK(int(w.phase) >= int(seen) || w.phase == j::Phase::none); // phases only ever go forward
        if (w.phase == j::Phase::fall && seen != j::Phase::fall) peakFrame = w.frame;
        if (w.phase == j::Phase::landing && seen != j::Phase::landing) touchFrame = w.frame;
        seen = w.phase;
        CHECK(w.weight >= 0 && w.weight <= 1);
    }
    CHECK(takeoffs == 1 && w.phase == j::Phase::none && w.weight == 0);
    CHECK(near_equal(peakFrame, float(s.peak), .5f) && near_equal(touchFrame, float(s.touchdown), .5f));
    // The landing plays at the clip's own rate: touchdown -> end in (end - touchdown) / fps, then the blend out.
    const float flight = 2 * v / g;
    CHECK(t < j::windupSeconds(s) + flight + (s.end - s.touchdown) / s.fps + j::blendOutSeconds + 6 * dt);
    return true;
}

bool running_warp_starts_in_the_air_and_cancels_cleanly() {
    const auto& r = j::clips[j::travellingClip];
    auto w = j::startWarp(r, true); // no frames before its takeoff: no windup, the push is at the press
    CHECK(w.phase == j::Phase::rise && w.frame == 0);
    j::Flight in{0, 0, 300, true, true};
    CHECK(!j::advanceWarp(w, r, .05f, in, fp::jumpGravity) && w.weight > 0 && w.weight < 1 && w.frame < float(r.peak));
    CHECK(j::advanceWarp(w, r, .2f, in, fp::jumpGravity) == false && w.weight == 1);
    j::cancelWarp(w, .2f);
    CHECK(w.phase == j::Phase::blendOut);
    j::advanceWarp(w, r, .2f + j::blendOutSeconds / 2, in, fp::jumpGravity);
    CHECK(w.weight > 0 && w.weight < 1);
    j::advanceWarp(w, r, .2f + j::blendOutSeconds, in, fp::jumpGravity);
    CHECK(w.phase == j::Phase::none && w.weight == 0);
    // A windup cancelled before takeoff never asks for the push.
    auto s = j::startWarp(j::clips[j::standingClip], true);
    j::cancelWarp(s, .1f);
    CHECK(!j::advanceWarp(s, j::clips[j::standingClip], 1.f, {}, fp::jumpGravity) && s.phase == j::Phase::none);
    // Skipping the windup (Classic, or a refused body) starts at the takeoff frame.
    CHECK(j::startWarp(j::clips[j::standingClip], false).phase == j::Phase::rise &&
          j::startWarp(j::clips[j::standingClip], false).frame == float(j::clips[j::standingClip].takeoff));
    return true;
}

bool stance_decides_the_mirror() {
    const auto& s = j::clips[j::standingClip]; // left foot leads
    CHECK(!j::mirrorFor(s, true) && j::mirrorFor(s, false));
    const auto& r = j::clips[j::travellingClip]; // pushes off the right (trailing) foot
    CHECK(!j::mirrorFor(r, false, 1) && j::mirrorFor(r, true, 0) && !j::mirrorFor(r, true, -1));
    const auto h = k::headingFrom({0, 0, 1}, {1, 0, 0});
    const auto f = j::worldFrame(s, h, float(s.peak), 1, false);
    CHECK(near_vec(f.v[k::leftShin], s.frames[s.peak].v[k::leftShin]) && near_vec(f.v[k::hipOffset], {0, 0, 0}, 0));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"baked phases come from the clips", baked_phases_come_from_the_clips},
        {"standing tuck default keeps the feet in the margin", standing_tuck_default_keeps_the_feet_in_the_margin},
        {"tuck scales only the legs", tuck_scales_only_the_legs},
        {"flight maps are monotone and hit their ends", flight_maps_are_monotone_and_hit_their_ends},
        {"standing warp runs every phase in order", standing_warp_runs_every_phase_in_order},
        {"running warp starts in the air and cancels cleanly", running_warp_starts_in_the_air_and_cancels_cleanly},
        {"stance decides the mirror", stance_decides_the_mirror},
    });
}
