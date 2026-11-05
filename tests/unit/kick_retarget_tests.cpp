#include "kick_clip.h"

#include <cmath>

#include "../test_support.hpp"

namespace {

using fp::kick::Vec;
using fp::kick::Quat;
namespace k = fp::kick;

bool near_equal(float a, float b, float eps = 1e-3f) { return std::isfinite(a) && std::fabs(a - b) <= eps; }
bool near_vec(Vec a, Vec b, float eps = 1e-3f) { return near_equal(a.x, b.x, eps) && near_equal(a.y, b.y, eps) && near_equal(a.z, b.z, eps); }
float angle(Vec a, Vec b) { return std::acos(std::clamp(fp::xr::dot(k::normalised(a), k::normalised(b)), -1.f, 1.f)); }
float quat_angle(Quat q) { return 2 * std::acos(std::clamp(std::fabs(q.w), 0.f, 1.f)); }

bool baked_clips_are_right_leg_kicks_in_both_sets() {
    static_assert(k::clipCount == 5);
    int sets[2]{};
    for (const auto& c : k::clips) {
        CHECK(c.count > 8 && c.fps == 30.f && c.peak > 0 && c.peak < c.count - 1 && c.set >= 0 && c.set <= 1);
        ++sets[c.set];
        for (int i = 0; i < c.count; ++i)
            for (unsigned s = 0; s < k::hipOffset; ++s) CHECK(near_equal(fp::xr::length(c.frames[i].v[s]), 1.f, 2e-3f));
        const auto& p = c.frames[c.peak];
        // At the peak the right (kicking) thigh is raised forward and the left (supporting) one hangs.
        CHECK(p.v[k::rightThigh].z > .5f && p.v[k::rightThigh].y > 0);
        CHECK(p.v[k::leftThigh].y < -.7f);
        // The hips stay above the supporting foot, at hip height.
        CHECK(p.v[k::hipOffset].y > 60 && p.v[k::hipOffset].y < 100);
        // Across vectors point to the character's left, up vectors up.
        CHECK(c.frames[0].v[k::pelvisUp].y > .8f && c.frames[0].v[k::chestUp].y > .8f);
    }
    CHECK(sets[0] == 3 && sets[1] == 2);
    return true;
}

bool weapon_classes_pick_sets_and_modes() {
    using C = k::WeaponClass;
    CHECK(k::classify(0, 0x80, false, false) == C::unarmed);
    CHECK(k::classify(0, 0xC0, false, false) == C::unarmed);
    CHECK(k::classify(1, 0x10, false, false) == C::oneHanded);
    CHECK(k::classify(1, 0x11, false, false) == C::oneHanded); // the unmasked flag word (a sword, 0x11)
    CHECK(k::classify(2, 0x90, false, false) == C::dualWield);
    CHECK(k::classify(1, 0x10, true, false) == C::oneHandedShield);
    CHECK(k::classify(0, 0x80, true, false) == C::shieldOnly);
    CHECK(k::classify(1, 0x20, false, false) == C::twoHanded);
    CHECK(k::classify(1, 0x421, false, false) == C::twoHanded); // a two-handed sword
    CHECK(k::classify(1, 0x521, false, false) == C::twoHanded); // the greatsword
    CHECK(k::classify(1, 0x23, false, false) == C::twoHanded);  // a bar or staff
    CHECK(k::classify(1, 0x30, false, false) == C::polearm);
    CHECK(k::classify(1, 0x32, false, false) == C::polearm);    // halberd, glaive, spetum...
    CHECK(k::classify(1, 0x30, false, true) == C::specialHeld);
    CHECK(k::classRules[size_t(C::twoHanded)].set == 1 && k::classRules[size_t(C::twoHanded)].mode == k::Mode::full);
    CHECK(k::classRules[size_t(C::polearm)].mode == k::Mode::lowerBody && k::classRules[size_t(C::specialHeld)].mode == k::Mode::lowerBody);
    for (const auto& r : k::classRules) CHECK(k::nextClip(k::clips, k::clipCount, r.set, -1) >= 0);
    return true;
}

bool rotation_never_repeats_within_a_set() {
    for (int set = 0; set < 2; ++set) {
        int last = -1, seen[k::clipCount]{};
        for (int n = 0; n < 12; ++n) {
            const int i = k::nextClip(k::clips, k::clipCount, set, last);
            CHECK(i >= 0 && k::clips[i].set == set && i != last);
            ++seen[i];
            last = i;
        }
        for (int i = 0; i < k::clipCount; ++i) CHECK((k::clips[i].set == set) == (seen[i] > 0));
    }
    const k::Clip one[] = {k::clips[0]};
    CHECK(k::nextClip(one, 1, k::clips[0].set, 0) == 0); // a set of one repeats
    CHECK(k::nextClip(one, 1, 1 - k::clips[0].set, -1) == -1);
    return true;
}

bool progress_maps_the_hit_to_the_clip_peak() {
    const auto& c = k::clips[0];
    CHECK(k::clipFrameAt(c, -.2f) == 0.f && k::clipFrameAt(c, 0.f) == 0.f);
    CHECK(near_equal(k::clipFrameAt(c, k::hitProgress), float(c.peak)));
    CHECK(near_equal(k::clipFrameAt(c, 1.5f), float(c.count - 1)));
    float last = -1;
    for (int i = 0; i <= 100; ++i) {
        const float f = k::clipFrameAt(c, i / 100.f);
        CHECK(f >= last);
        last = f;
    }
    CHECK(k::weightAt(-.1f) == 0.f && k::weightAt(0.f) == 0.f && k::weightAt(1.f) == 0.f && k::weightAt(.5f) == 1.f);
    CHECK(k::weightAt(.03f) > 0 && k::weightAt(.03f) < .5f && k::weightAt(.97f) > 0 && k::weightAt(.97f) < .5f);
    return true;
}

bool sampling_interpolates_and_keeps_the_offset_unscaled() {
    const auto& c = k::clips[0];
    const auto a = k::sample(c, 3.f), m = k::sample(c, 3.5f);
    CHECK(near_vec(a.v[k::rightShin], c.frames[3].v[k::rightShin]));
    CHECK(near_equal(fp::xr::length(m.v[k::rightShin]), 1.f));
    const Vec mid = fp::xr::scale(fp::xr::add(c.frames[3].v[k::hipOffset], c.frames[4].v[k::hipOffset]), .5f);
    CHECK(near_vec(m.v[k::hipOffset], mid, 1e-2f));
    CHECK(near_vec(k::sample(c, 1e6f).v[k::leftFoot], c.frames[c.count - 1].v[k::leftFoot]));
    return true;
}

bool mirror_swaps_sides_and_is_its_own_inverse() {
    const auto& f = k::clips[0].frames[k::clips[0].peak];
    const auto m = k::mirrored(f);
    CHECK(near_vec(m.v[k::leftThigh], {-f.v[k::rightThigh].x, f.v[k::rightThigh].y, f.v[k::rightThigh].z}));
    CHECK(near_vec(m.v[k::pelvisAcross], {f.v[k::pelvisAcross].x, -f.v[k::pelvisAcross].y, -f.v[k::pelvisAcross].z}));
    CHECK(near_vec(m.v[k::hipOffset], {-f.v[k::hipOffset].x, f.v[k::hipOffset].y, f.v[k::hipOffset].z}));
    const auto back = k::mirrored(m);
    for (unsigned s = 0; s < k::slotCount; ++s) CHECK(near_vec(back.v[s], f.v[s]));
    // The mirrored kick raises the left leg.
    CHECK(m.v[k::leftThigh].z > .5f && m.v[k::rightThigh].y < -.7f);
    return true;
}

bool heading_maps_clip_axes_to_the_body() {
    const auto h = k::headingFrom({0, -.2f, -2}, {-.3f, .05f, .02f}); // facing -Z, left hip toward -X
    CHECK(h.valid && near_vec(h.forward, {0, 0, -1}) && near_vec(h.left, {-1, 0, 0}));
    CHECK(near_vec(k::toWorld(h, {0, 0, 1}), {0, 0, -1}) && near_vec(k::toWorld(h, {2, 0, 0}), {-2, 0, 0}));
    const auto m = k::headingFrom({0, 0, -1}, {.3f, 0, 0}); // the other handedness: left still lands left
    CHECK(m.valid && near_vec(k::toWorld(m, {1, 0, 0}), {1, 0, 0}));
    CHECK(!k::headingFrom({0, 1, 0}, {1, 0, 0}).valid && !k::headingFrom({0, 0, 1}, {0, 0, 1}).valid);
    const auto w = k::worldFrame(k::clips[0], h, k::hitProgress, false);
    CHECK(w.v[k::rightShin].z < -.3f); // the kick goes where the character faces
    return true;
}

bool stance_decides_the_mirror() {
    k::Clip c = k::clips[0];
    c.supportLeads = true; // a rear-leg kick: its supporting (left) foot leads
    CHECK(!k::mirrorFor(c, true) && k::mirrorFor(c, false));
    c.supportLeads = false;
    CHECK(k::mirrorFor(c, true) && !k::mirrorFor(c, false));
    return true;
}

bool bone_and_frame_targets_are_capped_turns() {
    const Quat identity{0, 0, 0, 1};
    const auto q = k::boneTarget(identity, {0, -1, 0}, {0, 0, 1}, 3.2f);
    CHECK(near_vec(fp::xr::rotate(q, {0, -1, 0}), {0, 0, 1}));
    CHECK(near_equal(quat_angle(k::boneTarget(identity, {0, -1, 0}, {0, 0, 1}, .35f)), .35f));
    // A trunk turned a quarter about up: up stays, across follows.
    const auto t = k::frameTarget(identity, {0, 1, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, 3.2f);
    CHECK(near_vec(fp::xr::rotate(t, {0, 1, 0}), {0, 1, 0}) && near_vec(fp::xr::rotate(t, {1, 0, 0}), {0, 0, -1}));
    // Tilted and turned at once.
    const Vec up = k::normalised({0, 1, .3f}), across = k::normalised({0, -.3f, 1});
    const auto r = k::alignTwo({0, 1, 0}, {0, 0, 1}, up, across);
    CHECK(near_vec(fp::xr::rotate(r, {0, 1, 0}), up) && near_vec(fp::xr::rotate(r, {0, 0, 1}), across));
    CHECK(quat_angle(k::frameTarget(identity, {0, 1, 0}, {1, 0, 0}, {0, -1, 0}, {-1, 0, 0})) <= k::maxPartStep + 1e-4f);
    return true;
}

bool hip_target_scales_and_steps() {
    // A clip leg of 80 cm on a 0.9 m leg: an offset of (0, 70, 10) cm becomes (0, 0.7875, 0.1125) m.
    const auto near = k::hipTarget({1, 0, 2}, {0, 70, 10}, 80, .9f, {1, .78f, 2.1f});
    CHECK(near_vec(near, {1, .7875f, 2.1125f}));
    const auto far = k::hipTarget({0, 0, 0}, {0, 100, 0}, 100, 1, {0, .5f, 0});
    CHECK(near_vec(far, {0, .5f + k::maxHipStep, 0}));
    return true;
}

bool blend_is_endpoint_exact_and_shortest() {
    const Quat a{0, 0, 0, 1}, b = fp::xr::yawQuaternion(1.f);
    const auto s = k::blend(a, b, 0), e = k::blend(a, b, 1), m = k::blend(a, b, .5f);
    CHECK(near_equal(s.w, 1) && near_equal(e.y, b.y) && near_equal(e.w, b.w));
    CHECK(near_equal(m.y, std::sin(.25f)) && near_equal(m.w, std::cos(.25f)));
    const auto n = k::blend(a, {-b.x, -b.y, -b.z, -b.w}, .5f);
    CHECK(near_equal(std::fabs(n.y), std::sin(.25f)));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"baked clips are right-leg kicks in both sets", baked_clips_are_right_leg_kicks_in_both_sets},
        {"weapon classes pick sets and modes", weapon_classes_pick_sets_and_modes},
        {"rotation never repeats within a set", rotation_never_repeats_within_a_set},
        {"progress maps the hit to the clip peak", progress_maps_the_hit_to_the_clip_peak},
        {"sampling interpolates and keeps the offset unscaled", sampling_interpolates_and_keeps_the_offset_unscaled},
        {"mirror swaps sides and is its own inverse", mirror_swaps_sides_and_is_its_own_inverse},
        {"heading maps clip axes to the body", heading_maps_clip_axes_to_the_body},
        {"stance decides the mirror", stance_decides_the_mirror},
        {"bone and frame targets are capped turns", bone_and_frame_targets_are_capped_turns},
        {"hip target scales and steps", hip_target_scales_and_steps},
        {"blend is endpoint exact and shortest", blend_is_endpoint_exact_and_shortest},
    });
}
