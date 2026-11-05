#include "camera_motion.h"
#include "engine_text.h"
#include "hook_health.h"
#include "input_policy.h"
#include "native_contracts.h"
#include "pointer_set.h"
#include "status_json.h"
#include "xr/math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

#include "../test_support.hpp"

namespace {

constexpr float kPi = 3.14159265f;

bool near_equal(float a, float b, float eps = 1e-4f) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= eps;
}

inline fp::HealthVerdict step_health(
    fp::HookHealth& h, bool inPlay, uint32_t clockMs, const uint32_t (&hits)[fp::healthHooks]
) {
    return fp::healthStep(h, inPlay, clockMs, hits);
}

bool input_button_handoff_and_raw_look() {
    fp::ButtonHandoff handoff;
    // Held across a UI/mode transition: button 1 stays blocked until released, while button 2 works.
    CHECK(handoff.sample(false, 1u) == 0u);
    CHECK(handoff.sample(true, 1u) == 0u);
    CHECK(handoff.sample(true, 3u) == 2u);
    CHECK(handoff.sample(true, 0u) == 0u);
    CHECK(handoff.sample(true, 1u) == 1u);

    // Absolute-only device without prior relative packets leaves take() inactive.
    fp::RawLook absoluteOnly;
    absoluteOnly.event(true, 16000, 9000, true);
    long rx = 0, ry = 0;
    CHECK(!absoluteOnly.take(rx, ry));

    // Spurious MOUSE_MOVE_ABSOLUTE packets must be ignored without wiping accumulated relative counts.
    fp::RawLook raw;
    raw.event(false, 12, -8, true);
    raw.event(true, 32000, 16000, true);
    raw.event(false, 3, 2, true);
    CHECK(raw.take(rx, ry));
    CHECK(rx == 15 && ry == -6);

    raw.event(false, 9, -4, true);
    raw.event(true, 4096, 2048, true);
    CHECK(raw.take(rx, ry));
    CHECK(rx == 9 && ry == -4);

    // Unowned relative motion does not accumulate gameplay look deltas.
    raw.event(false, 25, -15, false);
    CHECK(raw.take(rx, ry));
    CHECK(rx == 0 && ry == 0);
    return true;
}

bool input_angle_wrapping_and_vr_turning_text() {
    CHECK(near_equal(fp::wrapHeading(0.f), 0.f));
    CHECK(near_equal(fp::wrapHeading(3.5f * kPi), -0.5f * kPi, 1e-3f));
    CHECK(near_equal(fp::wrapHeading(-3.5f * kPi), 0.5f * kPi, 1e-3f));

    CHECK(near_equal(fp::angleDelta(10.f, 350.f), 20.f, 1e-3f));
    CHECK(near_equal(fp::angleDelta(350.f, 10.f), -20.f, 1e-3f));

    // Huge magnitudes and non-finite inputs must return promptly without hanging in a subtraction loop.
    const float hugePos = fp::wrapHeading(1.0e20f);
    const float hugeNeg = fp::wrapHeading(-1.0e20f);
    CHECK(std::isfinite(hugePos) && std::fabs(hugePos) <= kPi + 1e-3f);
    CHECK(std::isfinite(hugeNeg) && std::fabs(hugeNeg) <= kPi + 1e-3f);

    const float hugeDelta = fp::angleDelta(1.0e20f, -1.0e20f);
    CHECK(std::isfinite(hugeDelta) && std::fabs(hugeDelta) <= 180.01f);

    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    (void)fp::wrapHeading(qnan);
    (void)fp::wrapHeading(inf);
    (void)fp::wrapHeading(-inf);
    (void)fp::angleDelta(qnan, 0.f);
    (void)fp::angleDelta(0.f, qnan);
    (void)fp::angleDelta(inf, 0.f);

    // Regression test for short L"sn" string (must never read past the NUL terminator at t[2]).
    CHECK(fp::vrTurningFromText(nullptr) == 1);
    CHECK(fp::vrTurningFromText(L"") == 1);
    CHECK(fp::vrTurningFromText(L"s") == 1);
    CHECK(fp::vrTurningFromText(L"sn") == 1);
    CHECK(fp::vrTurningFromText(L"sna") == 1);
    CHECK(fp::vrTurningFromText(L"snap") == 1);
    CHECK(fp::vrTurningFromText(L"snap30") == 1);
    CHECK(fp::vrTurningFromText(L"snap45") == 2);
    CHECK(fp::vrTurningFromText(L"SNAP45") == 2);
    CHECK(fp::vrTurningFromText(L"smooth") == 3);
    CHECK(fp::vrTurningFromText(L"off") == 0);
    CHECK(fp::vrTurningFromText(L"0") == 0);
    CHECK(fp::vrTurningFromText(L"2") == 2);
    CHECK(fp::vrTurningFromText(L"3") == 3);
    return true;
}

bool input_f6_cycle_policy() {
    using fp::F6Step;
    // The full cycle acts on the press, as before: first -> third -> overhead blend, then off once it settles.
    CHECK(fp::f6PressStep(false, true, 0, 9999) == F6Step::toThird);
    CHECK(fp::f6PressStep(false, true, 1, 9999) == F6Step::toOverhead);
    CHECK(fp::f6PressStep(false, true, 2, 100) == F6Step::none);
    CHECK(fp::f6PressStep(false, true, 2, fp::f6BlendSettleMs) == F6Step::toggle);
    CHECK(fp::f6PressStep(false, false, 0, 0) == F6Step::toggle);
    // Two-way: first and third wait for the release; off and the blend behave as in the full cycle.
    CHECK(fp::f6PressStep(true, true, 0, 9999) == F6Step::holdPending);
    CHECK(fp::f6PressStep(true, true, 1, 9999) == F6Step::holdPending);
    CHECK(fp::f6PressStep(true, true, 2, 100) == F6Step::none);
    CHECK(fp::f6PressStep(true, false, 1, 0) == F6Step::toggle);
    // A tap flips the two views; a hold of f6HoldMs or longer goes to the game's camera; never from the blend.
    CHECK(fp::f6ReleaseStep(120, 0) == F6Step::toThird);
    CHECK(fp::f6ReleaseStep(120, 1) == F6Step::toFirst);
    CHECK(fp::f6ReleaseStep(fp::f6HoldMs - 1, 1) == F6Step::toFirst);
    CHECK(fp::f6ReleaseStep(fp::f6HoldMs, 0) == F6Step::toOverhead);
    CHECK(fp::f6ReleaseStep(5000, 1) == F6Step::toOverhead);
    CHECK(fp::f6ReleaseStep(120, 2) == F6Step::none);
    return true;
}

bool input_game_binding_keys() {
    // Modifier families: Alt, Ctrl and Shift match their left/right codes both ways; other keys only themselves.
    CHECK(fp::sameKey(VK_MENU, VK_MENU));
    CHECK(fp::sameKey(VK_MENU, VK_LMENU) && fp::sameKey(VK_RMENU, VK_MENU));
    CHECK(fp::sameKey(VK_CONTROL, VK_RCONTROL) && fp::sameKey(VK_LSHIFT, VK_SHIFT));
    CHECK(!fp::sameKey(VK_LMENU, VK_RMENU)); // two physical keys
    CHECK(!fp::sameKey(VK_MENU, VK_CONTROL));
    CHECK(!fp::sameKey(0, 0) && !fp::sameKey(VK_MENU, 0) && !fp::sameKey(300, 300));
    // Hunter's case: the game's alternate attack (the thrust modifier) is Alt, and Kick was bound to Alt.
    const unsigned bindings[10] = {'W', 'A', 'S', 'D', VK_SHIFT, 'X', VK_LMENU, 'C', VK_SPACE, 'E'};
    CHECK(fp::gameUsesKey(VK_MENU, bindings));
    CHECK(fp::gameUsesKey(VK_SPACE, bindings));
    CHECK(fp::gameUsesKey(VK_RSHIFT, bindings));
    CHECK(!fp::gameUsesKey(VK_CONTROL, bindings));
    CHECK(!fp::gameUsesKey('G', bindings));
    CHECK(!fp::gameUsesKey(0, bindings));
    return true;
}

bool input_motion_refusals() {
    using R = fp::MotionRefusal;
    constexpr R play = R::count;
    // The base state with balance plays, whatever the health, magic or stance (the game asks none of them).
    CHECK(fp::motionGate(false, false, 0, 1.f, 1.f) == play);
    CHECK(fp::motionGate(false, false, 0, .96f, 1.f) == play);
    // Each reason, in the checks' order.
    CHECK(fp::motionGate(true, true, 0x800, 0.f, 1.f) == R::cursorFree);
    CHECK(fp::motionGate(false, true, 0x800, 0.f, 1.f) == R::busy);
    CHECK(fp::motionGate(false, false, 0x800, 0.f, 1.f) == R::dead);
    CHECK(fp::motionGate(false, false, 8, 0.f, 1.f) == R::otherState);
    CHECK(fp::motionGate(false, false, 0x8000, 1.f, 1.f) == R::otherState);
    CHECK(fp::motionGate(false, false, 0, .9f, 1.f) == R::balance); // below max - 0.05 the game starts its recover
    CHECK(fp::motionGate(false, false, 0, NAN, 1.f) == R::balance);
    // Notices: something to do for the player's reasons, silence for the others; kick and jump each name theirs.
    for (R r : {R::busy, R::otherState, R::balance, R::cursorFree, R::gameKey, R::unavailable, R::fault}) {
        CHECK(fp::motionRefusalNotice(false, r) != nullptr);
        CHECK(std::wcsncmp(fp::motionRefusalNotice(false, r), L"Kick", 4) == 0);
        CHECK(std::wcsncmp(fp::motionRefusalNotice(true, r), L"Jump", 4) == 0);
    }
    for (R r : {R::tooSoon, R::dead, R::notPlayer, R::count})
        CHECK(fp::motionRefusalNotice(false, r) == nullptr && fp::motionRefusalNotice(true, r) == nullptr);
    CHECK(std::wcsstr(fp::motionRefusalNotice(false, R::gameKey), L"Alt") != nullptr);
    CHECK(std::strcmp(fp::motionRefusalNames[size_t(R::otherState)], "state") == 0 && std::strcmp(fp::motionRefusalNames[size_t(R::balance)], "balance") == 0);
    return true;
}

bool input_cursor_panels() {
    CHECK(fp::cursorPanelOf(fp::windowInventory, false) == fp::cursorPanelInventory);
    CHECK(fp::cursorPanelOf(fp::windowLoot, false) == fp::cursorPanelInventory);
    CHECK(fp::cursorPanelOf(fp::windowContainer, false) == fp::cursorPanelInventory);
    CHECK(fp::cursorPanelOf(fp::windowShop, false) == fp::cursorPanelCommerce);
    CHECK(fp::cursorPanelOf(fp::windowArsenal, false) == fp::cursorPanelCommerce);
    CHECK(fp::cursorPanelOf(fp::windowMatches, false) == fp::cursorPanelArena);
    CHECK(fp::cursorPanelOf(fp::windowCompanyStatus, false) == fp::cursorPanelArena);
    CHECK(fp::cursorPanelOf(fp::windowDescription, false) == fp::cursorPanelDescription);
    CHECK(fp::cursorPanelOf(0x12345678u, false) == 0u);
    // The post-fight result form never frees the pointer by itself, whatever kind it carries: look stays.
    CHECK(fp::cursorPanelOf(fp::windowMatches, true) == 0u);
    CHECK(fp::cursorPanelOf(fp::windowInventory, true) == 0u);
    CHECK(fp::cursorPanelOf(0u, true) == 0u);
    return true;
}

bool input_combat_cues_cursor_and_ray_helpers() {
    fp::SwingGesture gesture;
    gesture.move(2.5f, 0.f, 1000.f);
    CHECK(gesture.side == 0);
    gesture.move(2.5f, 0.f, 1000.f);
    CHECK(gesture.side == 1);
    // Primarily vertical motion clears pending without flipping side.
    gesture.move(-2.f, 10.f, 1000.f);
    CHECK(gesture.side == 1 && gesture.pending == 0.f);
    gesture.move(-5.f, 0.f, 1000.f);
    CHECK(gesture.side == -1);

    fp::CueInputs cue{};
    CHECK(fp::originCue(cue) == fp::OriginCue::neutral);
    cue.owned = true;
    cue.combat = true;
    cue.upright = true;
    cue.clock = 500;
    cue.lastPress = 0;
    CHECK(fp::originCue(cue) == fp::OriginCue::defaultSide);
    cue.aimHeading = 0.5f;
    cue.bodyHeading = 0.f;
    CHECK(fp::originCue(cue) == fp::OriginCue::oppositeSide);
    cue.motion = 12;
    CHECK(fp::originCue(cue) == fp::OriginCue::overhead);
    cue.motion = 11;
    CHECK(fp::originCue(cue) == fp::OriginCue::neutral);

    CHECK(!fp::crosshairDrawn(false, false, false, false));
    CHECK(fp::crosshairDrawn(false, false, false, true));
    CHECK(fp::crosshairDrawn(false, false, false, false, true));
    CHECK(!fp::crosshairDrawn(false, false, false, true, false, true));
    CHECK(fp::crosshairDrawn(true, true, false, false));
    CHECK(!fp::crosshairDrawn(true, true, true, false));

    fp::RightMouseFree rmb{};
    bool changed = false;
    CHECK(fp::rmbStep(rmb, 1, fp::RmbEvent::down, true, changed) && changed && rmb.on);
    CHECK(fp::rmbStep(rmb, 1, fp::RmbEvent::up, true, changed) && changed && !rmb.on);
    CHECK(fp::rmbStep(rmb, 2, fp::RmbEvent::down, true, changed) && changed && rmb.on);
    CHECK(fp::rmbStep(rmb, 2, fp::RmbEvent::up, true, changed) && !changed && rmb.on);
    CHECK(!fp::rmbStep(rmb, 2, fp::RmbEvent::focusLost, true, changed) && changed && !rmb.on && !rmb.swallowUp);

    fp::SwapWatch watch{};
    CHECK(!fp::swapWatchStep(watch, 0, 10, true));  // initial baseline at swaps=10
    CHECK(!fp::swapWatchStep(watch, 25, 10, true)); // flat 1
    CHECK(!fp::swapWatchStep(watch, 50, 10, true)); // flat 2
    CHECK(fp::swapWatchStep(watch, 75, 10, true));  // flat 3 -> starved!
    CHECK(!fp::swapWatchStep(watch, 100, 10, true)); // reported once

    const float origin[3] = {0.f, 0.f, 0.f};
    const float dirZ[3] = {0.f, 0.f, 1.f};
    const float missOrigin[3] = {20.f, 0.f, 0.f};
    CHECK(near_equal(fp::rayInSphere(origin, dirZ, origin, 10.f), 10.f));
    CHECK(fp::rayInSphere(missOrigin, dirZ, origin, 5.f) < 0.f);

    const float eye[3] = {0.f, 10.f, 0.f};
    const float angledRay[3] = {0.f, -0.6f, 0.8f};
    const float floorPlane[4] = {0.f, 1.f, 0.f, 0.f};
    float planePoint[3]{};
    CHECK(fp::heldPointOnPlane(eye, angledRay, 50.f, floorPlane, planePoint));
    CHECK(near_equal(planePoint[0], 0.f) && near_equal(planePoint[1], 0.f) && near_equal(planePoint[2], 50.f, 1e-3f));
    const float normalRay[3] = {0.f, -1.f, 0.f};
    CHECK(!fp::heldPointOnPlane(eye, normalRay, 50.f, floorPlane, planePoint));
    return true;
}

bool camera_motion_fov_aimbox_nearplane_and_eye_stabilization() {
    CHECK(fp::clampFov(5) == fp::fovMin);
    CHECK(fp::clampFov(85) == 85);
    CHECK(fp::clampFov(500) == fp::fovMax);

    CHECK(near_equal(fp::resolveCameraFov(0, 75.f, 95.f), 75.f));
    CHECK(near_equal(fp::resolveCameraFov(1, 75.f, 0.f), 75.f));
    CHECK(near_equal(fp::resolveCameraFov(1, 75.f, 95.f), 95.f));

    // Hard-edge aimBoxStep: stays inside ellipse until crossing rim, conserving total motion.
    fp::AimBox hard{};
    float dx = 30.f, dy = 0.f;
    fp::aimBoxStep(hard, dx, dy, 100.f, 100.f, 0);
    CHECK(near_equal(hard.x, 30.f) && near_equal(dx, 0.f));
    dx = 120.f;
    dy = 0.f;
    fp::aimBoxStep(hard, dx, dy, 100.f, 100.f, 0);
    CHECK(near_equal(hard.x, 100.f) && near_equal(dx, 50.f));

    // Soft-edge aimBoxStep: feathers outward motion smoothly between cursor and camera.
    fp::AimBox soft{};
    float sdx = 40.f, sdy = 0.f;
    fp::aimBoxStep(soft, sdx, sdy, 100.f, 100.f, 100);
    CHECK(soft.x > 0.f && soft.x < 40.f);
    CHECK(sdx > 0.f && near_equal(soft.x + sdx, 40.f));

    // NearPlane: tightens immediately on wall approach and widens smoothly on retreat.
    fp::NearPlane nearPlane;
    CHECK(near_equal(nearPlane.update(10.f, 0.016f, 1.f, 4.f), 4.f));
    CHECK(near_equal(nearPlane.update(3.f, 0.016f, 1.f, 4.f), 1.5f));
    const float widened = nearPlane.update(10.f, 0.05f, 1.f, 4.f);
    CHECK(widened > 1.5f && widened < 4.f);
    CHECK(near_equal(nearPlane.update(-1.f, 0.016f, 1.f, 4.f), 1.f));

    // stabilizeEyeRotation: full compensation inside inner 10-degree cone, bounded beyond 40 degrees.
    float physical[12]{};
    fp::basis(0.f, 0.08f, physical);
    physical[9] = 10.f;
    physical[10] = 20.f;
    physical[11] = -30.f;
    float desired[12]{};
    fp::basis(0.f, 0.f, desired);
    fp::stabilizeEyeRotation(physical, 0.f, 0.f);
    for (int i = 0; i < 9; ++i) CHECK(near_equal(physical[i], desired[i], 1e-3f));
    CHECK(near_equal(physical[9], 10.f) && near_equal(physical[10], 20.f) && near_equal(physical[11], -30.f));

    float knocked[12]{};
    fp::basis(0.f, 1.0f, knocked);
    fp::stabilizeEyeRotation(knocked, 0.f, 0.f);
    CHECK(std::fabs(knocked[7] - desired[7]) > 0.1f); // capped at 40 degrees, not fully snapped

    CHECK(fp::blockersFromText(nullptr) == fp::blockersSmart);
    CHECK(fp::blockersFromText(L" hide ") == fp::blockersHide);
    CHECK(fp::blockersFromText(L"PUSH") == fp::blockersPush);
    CHECK(fp::blockersFromText(L"smart") == fp::blockersSmart);
    return true;
}

bool camera_motion_shoulder_slide() {
    // The pure slide: eased, finished after shoulderSlideSeconds, never past its target.
    float left = fp::shoulderSlideSeconds;
    const float first = fp::shoulderSlide(1.f, -1.f, left, 0.016f);
    CHECK(first < 1.f && first > 0.95f);
    const float half = fp::shoulderSlide(1.f, -1.f, left, fp::shoulderSlideSeconds / 2.f - 0.016f);
    CHECK(near_equal(half, 0.f, 1e-3f));
    CHECK(near_equal(fp::shoulderSlide(1.f, -1.f, left, 1.f), -1.f));
    CHECK(left == 0.f);
    CHECK(near_equal(fp::shoulderSlide(1.f, -1.f, left, NAN), -1.f));

    // ThirdPerson: a change of the chosen side slides the camera across in both modes, where it used to jump.
    auto open = [](const float*, const float*, float length) { return length; };
    for (bool tight : {false, true}) {
        fp::ThirdPerson tp;
        const float eye[3] = {0.f, 100.f, 0.f};
        float pose[12]{};
        for (int i = 0; i < 30; ++i) tp.update(eye, 0.f, 0.f, 250.f, 40.f, 0.f, 0.016f, open, pose, tight);
        const float before = pose[9];
        CHECK(std::fabs(before) > 30.f);
        tp.update(eye, 0.f, 0.f, 250.f, -40.f, 0.f, 0.016f, open, pose, tight);
        CHECK(std::fabs(pose[9] - before) < 0.1f * std::fabs(before)); // one frame in: barely moved
        for (int i = 0; i < 30; ++i) tp.update(eye, 0.f, 0.f, 250.f, -40.f, 0.f, 0.016f, open, pose, tight);
        CHECK(near_equal(pose[9], -before, 1.f)); // across, mirrored (the boom length itself eases)
        if (tight) CHECK(tp.swapHold > 0.f || tp.side == -1.f);
    }
    // A fresh camera starts on its side at once.
    fp::ThirdPerson fresh;
    const float eye[3] = {0.f, 100.f, 0.f};
    float pose[12]{};
    fresh.update(eye, 0.f, 0.f, 250.f, -40.f, 0.f, 0.016f, open, pose, false);
    CHECK(pose[9] > 30.f);
    return true;
}

bool camera_motion_look_follow_framing() {
    // Pitch is positive looking up: down gives a positive look, saturating at the full angle.
    CHECK(near_equal(fp::lookFollowTarget(0.f), 0.f));
    CHECK(near_equal(fp::lookFollowTarget(-fp::lookFollowFullPitch), 1.f));
    CHECK(near_equal(fp::lookFollowTarget(-3.f), 1.f));
    CHECK(near_equal(fp::lookFollowTarget(fp::lookFollowFullPitch / 2.f), -0.5f));
    CHECK(near_equal(fp::lookFollowTarget(NAN), 0.f));
    float scale = 0.f, height = 0.f;
    fp::lookFollowFraming(0.f, scale, height);
    CHECK(near_equal(scale, 1.f) && near_equal(height, 0.f));
    fp::lookFollowFraming(1.f, scale, height); // full down: a quarter farther, 30 cm up
    CHECK(near_equal(scale, 1.25f) && near_equal(height, 30.f));
    fp::lookFollowFraming(-1.f, scale, height); // full up: a fifth closer, lower
    CHECK(near_equal(scale, 0.8f) && height < 0.f);
    fp::lookFollowFraming(5.f, scale, height);
    CHECK(near_equal(scale, 1.25f));
    fp::lookFollowFraming(NAN, scale, height);
    CHECK(near_equal(scale, 1.f) && near_equal(height, 0.f));
    // Continuous through level: no jump between a little up and a little down.
    float up = 0.f, down = 0.f, hu = 0.f, hd = 0.f;
    fp::lookFollowFraming(-1e-4f, up, hu);
    fp::lookFollowFraming(1e-4f, down, hd);
    CHECK(near_equal(up, down, 1e-3f) && near_equal(hu, hd, 1e-2f));

    // A camera pinned by a wall keeps its distance while the wanted length grows: no pumping.
    auto wall = [](const float*, const float*, float length) { return std::fmin(length, 120.f); };
    fp::ThirdPerson tp;
    const float eye[3] = {0.f, 100.f, 0.f};
    float pose[12]{};
    for (int i = 0; i < 60; ++i) tp.update(eye, 0.f, 0.f, 250.f, 0.f, 0.f, 0.016f, wall, pose);
    const float pinned = tp.distance;
    for (int i = 0; i < 60; ++i) tp.update(eye, 0.f, 0.f, 250.f * 1.25f, 0.f, 0.f, 0.016f, wall, pose);
    CHECK(near_equal(tp.distance, pinned, 0.5f));
    return true;
}

bool camera_motion_knockdown_view() {
    using Step = fp::KnockdownViewStep;
    const bool knocked = true, dead = true;
    // Down in first person: third person at once, first person again after the settle, not before.
    fp::KnockdownView k;
    CHECK(fp::knockdownViewStep(k, true, 0, false, false, 0, 1000) == Step::none);
    CHECK(fp::knockdownViewStep(k, true, 0, knocked, false, 0, 1100) == Step::toThird);
    CHECK(fp::knockdownViewStep(k, true, 1, knocked, false, 0, 1200) == Step::none);
    CHECK(fp::knockdownViewStep(k, true, 1, false, false, 0, 2000) == Step::none);
    CHECK(fp::knockdownViewStep(k, true, 1, knocked, false, 0, 2100) == Step::none); // down again: no flicker
    CHECK(fp::knockdownViewStep(k, true, 1, false, false, 0, 2200) == Step::none);
    CHECK(fp::knockdownViewStep(k, true, 1, false, false, 0, 2200 + fp::knockdownViewSettleMs - 1) == Step::none);
    CHECK(fp::knockdownViewStep(k, true, 1, false, false, 0, 2200 + fp::knockdownViewSettleMs) == Step::toFirst);
    CHECK(fp::knockdownViewStep(k, true, 0, false, false, 0, 5000) == Step::none);

    // Off, or already in third person: nothing.
    fp::KnockdownView off;
    CHECK(fp::knockdownViewStep(off, false, 0, knocked, false, 0, 100) == Step::none);
    fp::KnockdownView third;
    CHECK(fp::knockdownViewStep(third, true, 1, knocked, false, 0, 100) == Step::none);
    CHECK(fp::knockdownViewStep(third, true, 1, false, false, 0, 1000) == Step::none);
    CHECK(fp::knockdownViewStep(third, true, 1, false, false, 0, 2000) == Step::none);

    // F6 during the knockdown is the player's choice: no switch back, and no switch again.
    fp::KnockdownView manual;
    CHECK(fp::knockdownViewStep(manual, true, 0, knocked, false, 4, 100) == Step::toThird);
    CHECK(fp::knockdownViewStep(manual, true, 0, knocked, false, 5, 200) == Step::none); // F6 back to first
    CHECK(fp::knockdownViewStep(manual, true, 0, knocked, false, 5, 300) == Step::none);
    CHECK(fp::knockdownViewStep(manual, true, 0, false, false, 5, 400) == Step::none);
    CHECK(fp::knockdownViewStep(manual, true, 0, false, false, 5, 2000) == Step::none);
    CHECK(fp::knockdownViewStep(manual, true, 0, knocked, false, 5, 3000) == Step::toThird); // a new knockdown

    // Death never switches to third person.
    fp::KnockdownView death;
    CHECK(fp::knockdownViewStep(death, true, 0, knocked, dead, 0, 100) == Step::none);
    CHECK(fp::knockdownViewStep(death, true, 0, knocked, dead, 0, 200) == Step::none);
    // A death after the switch goes back to first person at once (the next life must not start in third),
    // and only once.
    fp::KnockdownView fell;
    CHECK(fp::knockdownViewStep(fell, true, 0, knocked, false, 0, 100) == Step::toThird);
    CHECK(fp::knockdownViewStep(fell, true, 1, knocked, dead, 0, 200) == Step::toFirst);
    CHECK(fp::knockdownViewStep(fell, true, 0, knocked, dead, 0, 300) == Step::none);
    CHECK(fp::knockdownViewStep(fell, true, 0, false, false, 0, 2000) == Step::none);
    // ...but not when the player chose third person with F6 during the knockdown.
    fp::KnockdownView chose;
    CHECK(fp::knockdownViewStep(chose, true, 0, knocked, false, 0, 100) == Step::toThird);
    CHECK(fp::knockdownViewStep(chose, true, 0, knocked, false, 1, 150) == Step::none); // F6 to first
    CHECK(fp::knockdownViewStep(chose, true, 1, knocked, false, 2, 180) == Step::none); // F6 to third
    CHECK(fp::knockdownViewStep(chose, true, 1, knocked, dead, 2, 200) == Step::none);
    return true;
}

bool camera_motion_jump_push() {
    // Spread over physics steps: one step per new step, several when a poll is late, never past the budget.
    auto p = fp::JumpPush::start(0x1000, 0x2000, 100, 50.f);
    CHECK(p.active && p.left == fp::jumpPushSteps);
    p.left -= 1; // the first step is pushed at the press
    CHECK(fp::jumpPushStep(p, true, 0x1000, 0x2000, 100, 50.f, 0, false) == 0); // no new step yet
    CHECK(fp::jumpPushStep(p, true, 0x1000, 0x2000, 101, 51.f, 0, false) == 1);
    CHECK(fp::jumpPushStep(p, true, 0x1000, 0x2000, 103, 53.f, 0x10004, false) == 2); // a standing action is no reaction
    // The game's own state tests: down (& 0xA00) and the reactions it does not interrupt (& 0xAC0).
    for (unsigned s : {0x200u, 0x300u, 0x600u, 0x800u, 0xA00u}) CHECK(fp::downState(s) && fp::reactionState(s));
    for (unsigned s : {0u, 1u, 2u, 8u, 0x20u, 0x100u, 0x8000u, 0x10000u, 0x10004u}) CHECK(!fp::downState(s) && !fp::reactionState(s));
    CHECK(!fp::downState(0xC0) && fp::reactionState(0xC0)); // the standing stagger
    CHECK(fp::deadState(0x800) && fp::deadState(0xA00) && !fp::deadState(0x200) && !fp::deadState(0x300));
    int total = 4; // the press, then 1 and 2
    for (unsigned s = 104; p.active; ++s) total += fp::jumpPushStep(p, true, 0x1000, 0x2000, s, 55.f, 0, false);
    CHECK(total == fp::jumpPushSteps);
    // The rise cap ends it early.
    auto high = fp::JumpPush::start(0x1000, 0x2000, 0, 0.f);
    CHECK(fp::jumpPushStep(high, true, 0x1000, 0x2000, 1, 5.f, 0, false) == 1);
    CHECK(fp::jumpPushStep(high, true, 0x1000, 0x2000, 2, fp::jumpRiseCapCm, 0, false) == 0 && !high.active);
    CHECK(near_equal(high.risen, fp::jumpRiseCapCm));
    // Each cancel: not owned, another character or scene, death, a knockdown, a long gap, a bad height.
    struct Case { bool owned; uintptr_t root, scene; unsigned step; float y; unsigned state; bool dead; };
    const Case cancels[] = {{false, 0x1000, 0x2000, 1, 0.f, 0, false}, {true, 0x1001, 0x2000, 1, 0.f, 0, false},
                            {true, 0x1000, 0x2001, 1, 0.f, 0, false},  {true, 0x1000, 0x2000, 1, 0.f, 0, true},
                            {true, 0x1000, 0x2000, 1, 0.f, 0x80, false}, {true, 0x1000, 0x2000, 1, 0.f, 0x800, false},
                            {true, 0x1000, 0x2000, 1 + fp::jumpStepGap, 0.f, 0, false},
                            {true, 0x1000, 0x2000, 1, NAN, 0, false}};
    for (const Case& c : cancels) {
        auto q = fp::JumpPush::start(0x1000, 0x2000, 0, 0.f);
        CHECK(fp::jumpPushStep(q, c.owned, c.root, c.scene, c.step, c.y, c.state, c.dead) == 0);
        CHECK(!q.active);
    }
    CHECK(!fp::JumpPush::start(0x1000, 0x2000, 0, NAN).active);
    return true;
}

bool hook_health_menu_immunity_and_stalled_poll_input() {
    fp::HookHealth health{};
    uint32_t hits[fp::healthHooks] = {5, 5, 5, 5, 5, 5};

    // Menu / out-of-play ticks must never fail pollInput even across many seconds.
    CHECK(step_health(health, false, 1000, hits) == fp::HealthVerdict::watching);
    CHECK(step_health(health, false, 20000, hits) == fp::HealthVerdict::watching);
    CHECK(!health.decided);

    // Entering live play starts the observation window; stalled pollInput fails past healthWindowMs.
    CHECK(step_health(health, true, 21000, hits) == fp::HealthVerdict::watching);
    for (unsigned i = 1; i < fp::healthHooks; ++i) hits[i] += 60; // pollInput (index 0) stays stalled
    CHECK(step_health(health, true, 21000 + fp::healthWindowMs - 1, hits) == fp::HealthVerdict::watching);
    CHECK(step_health(health, true, 21000 + fp::healthWindowMs, hits) == fp::HealthVerdict::failed);
    CHECK(health.decided && health.silent == int(fp::HealthHook::pollInput));

    // When every hook advances during live play, healthStep passes.
    fp::HookHealth healthy{};
    uint32_t liveHits[fp::healthHooks] = {1, 1, 1, 1, 1, 1};
    CHECK(step_health(healthy, true, 500, liveHits) == fp::HealthVerdict::watching);
    for (unsigned i = 0; i < fp::healthHooks; ++i) liveHits[i] += 10;
    CHECK(step_health(healthy, true, 500 + fp::healthWindowMs, liveHits) == fp::HealthVerdict::passed);
    CHECK(healthy.decided && healthy.silent == -1);
    return true;
}

bool pointer_set_and_status_json_formatting() {
    fp::PointerSet set;
    set.reset(64);
    for (uintptr_t i = 1; i <= 32; ++i) {
        const uintptr_t ptr = 0x140000000ull + i * 16;
        CHECK(set.insert(ptr));
        CHECK(!set.insert(ptr));
    }
    set.reset(64);
    CHECK(set.insert(0x140000010ull));

    const auto tempPath = std::filesystem::temp_directory_path() / "excamera_status_json_unit_test.json";
    FILE* file = nullptr;
    CHECK(fopen_s(&file, tempPath.string().c_str(), "wb") == 0 && file != nullptr);
    {
        fp::JsonOut json(file);
        const char unterminated[4] = {'A', 'B', 'C', 'D'};
        const float coords[2] = {1.5f, -2.25f};
        json.num("signed", -42)
            .unum("unsigned", 99)
            .fixed("rate", 3.5, 2)
            .boolean("ok", true)
            .null("empty")
            .hex("addr", 0x12ABull)
            .str("escaped", "a\"b\\c\n")
            .str("bounded", unterminated, 3)
            .reals("coords", coords, 2)
            .object("nested")
            .boolean("inner", false)
            .finish();
    }
    std::fclose(file);

    std::ifstream in(tempPath, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    in.close();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    const std::string out = buffer.str();
    CHECK(!out.empty() && out.front() == '{' && out.back() == '\n');
    CHECK(out.find("\"signed\":-42") != std::string::npos);
    CHECK(out.find("\"unsigned\":99") != std::string::npos);
    CHECK(out.find("\"rate\":3.50") != std::string::npos);
    CHECK(out.find("\"ok\":true") != std::string::npos);
    CHECK(out.find("\"empty\":null") != std::string::npos);
    CHECK(out.find("\"addr\":\"0x12ab\"") != std::string::npos);
    CHECK(out.find("\"escaped\":\"a\\\"b\\\\c\\u000a\"") != std::string::npos);
    CHECK(out.find("\"bounded\":\"ABC\"") != std::string::npos);
    CHECK(out.find("\"coords\":[1.5,-2.25]") != std::string::npos);
    CHECK(out.find("\"nested\":{\"inner\":false}}") != std::string::npos);
    return true;
}

bool xr_math_standalone_types_and_transforms() {
    XrVector2f thumbstick{0.25f, -0.75f};
    XrTime sampleTime = XrTime(123456789);
    CHECK(near_equal(thumbstick.x, 0.25f) && near_equal(thumbstick.y, -0.75f));
    CHECK(sampleTime == XrTime(123456789));

    const XrQuaternionf qYaw90 = fp::xr::yawQuaternion(kPi * 0.5f);
    const XrVector3f rotated = fp::xr::rotate(qYaw90, {0.f, 0.f, -1.f});
    CHECK(near_equal(rotated.x, -1.f, 1e-4f) && near_equal(rotated.y, 0.f, 1e-4f) && near_equal(rotated.z, 0.f, 1e-4f));

    float extractedYaw = 0.f;
    CHECK(fp::xr::headingYaw(qYaw90, extractedYaw));
    CHECK(near_equal(extractedYaw, kPi * 0.5f, 1e-4f));

    const XrPosef pose{qYaw90, {1.5f, 1.7f, -0.5f}};
    const XrPosef roundTrip = fp::xr::compose(pose, fp::xr::inverse(pose));
    CHECK(fp::xr::valid(roundTrip));
    CHECK(near_equal(roundTrip.position.x, 0.f, 1e-4f));
    CHECK(near_equal(roundTrip.position.y, 0.f, 1e-4f));
    CHECK(near_equal(roundTrip.position.z, 0.f, 1e-4f));
    CHECK(near_equal(std::fabs(roundTrip.orientation.w), 1.f, 1e-4f));

    const XrQuaternionf align = fp::xr::directionRotation({0.f, 0.f, -1.f}, {1.f, 0.f, 0.f});
    const XrVector3f aligned = fp::xr::rotate(align, {0.f, 0.f, -1.f});
    CHECK(near_equal(aligned.x, 1.f, 1e-4f) && near_equal(aligned.z, 0.f, 1e-4f));

    const auto mat = fp::xr::poseMatrix(pose);
    CHECK(near_equal(mat[12], 1.5f) && near_equal(mat[13], 1.7f) && near_equal(mat[14], -0.5f) && near_equal(mat[15], 1.f));

    const XrFovf symFov{-0.7f, 0.7f, 0.6f, -0.6f};
    const auto infProj = fp::xr::projection(symFov, 1.f, 0.f);
    CHECK(near_equal(infProj[10], -1.f) && near_equal(infProj[14], -2.f));

    fp::xr::HeadCamera cam;
    CHECK(cam.recenter({{0.f, 0.f, 0.f, 1.f}, {1.f, 1.6f, 2.f}}));
    const XrPosef eyeWorld = cam.eye(
        {{0.f, 0.f, 0.f, 1.f}, {100.f, 170.f, 200.f}},
        {{0.f, 0.f, 0.f, 1.f}, {1.2f, 1.6f, 1.9f}},
        100.f
    );
    CHECK(near_equal(eyeWorld.position.x, 120.f, 1e-3f));
    CHECK(near_equal(eyeWorld.position.y, 170.f, 1e-3f));
    CHECK(near_equal(eyeWorld.position.z, 190.f, 1e-3f));
    return true;
}

bool all_signatures_and_native_contracts_parse() {
    fp::Pattern pat;
    for (const auto& sig : fp::sig::signatures) {
        CHECK(sig.pattern != nullptr);
        CHECK(fp::parsePattern(sig.pattern, pat));
        CHECK(!pat.bytes.empty());
        CHECK(pat.anchor < pat.bytes.size());
        CHECK(pat.mask[pat.anchor] == 0xFF);
    }
    size_t largePatterns = 0;
    for (const auto& owner : fp::contract::owners) {
        CHECK(owner.pattern != nullptr);
        CHECK(fp::parsePattern(owner.pattern, pat));
        CHECK(!pat.bytes.empty());
        CHECK(pat.anchor < pat.bytes.size());
        CHECK(pat.mask[pat.anchor] == 0xFF);
        if (pat.bytes.size() > fp::PatternBytes::inlineCapacity) ++largePatterns;
    }
    for (const auto& field : fp::contract::fields) {
        CHECK(field.pattern != nullptr);
        CHECK(fp::parsePattern(field.pattern, pat));
        CHECK(!pat.bytes.empty());
        if (pat.bytes.size() > fp::PatternBytes::inlineCapacity) ++largePatterns;
    }
    CHECK(largePatterns > 0); // verifies multi-kilobyte contracts (up to 4956 bytes) exercise SBO overflow
    char* exeEnv = nullptr;
    size_t exeLen = 0;
    if (_dupenv_s(&exeEnv, &exeLen, "EXCAMERA_TEST_GAME_EXE") == 0 && exeEnv && *exeEnv) {
        const std::filesystem::path exePath(exeEnv);
        std::free(exeEnv);
        std::ifstream in(exePath, std::ios::binary | std::ios::ate);
        CHECK(in.good());
        const auto sz = static_cast<size_t>(in.tellg());
        in.seekg(0);
        std::vector<uint8_t> raw(sz), mapped;
        CHECK(bool(in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz))));
        fp::ImageView view;
        CHECK(fp::mapPeFile(raw.data(), raw.size(), mapped, view));
        fp::Layout layout;
        fp::ResolveReport resolveRep;
        CHECK(fp::resolveLayout(view, layout, resolveRep));
        fp::ContractReport contractRep;
        CHECK(fp::validateNativeContracts(view, layout, contractRep));
    } else if (exeEnv) {
        std::free(exeEnv);
    }
    return true;
}

bool fpc_rtti_vmt_and_shifted_slot_introspection() {
    std::vector<uint8_t> image(0x3000, 0);
    const uint64_t imageBase = 0x140000000ull;
    fp::ImageView view{image.data(), image.size(), imageBase, 0x1000u, 0x2000u};

    auto put64 = [&](size_t rva, uint64_t val) { std::memcpy(image.data() + rva, &val, sizeof val); };
    auto puti64 = [&](size_t rva, int64_t val) { std::memcpy(image.data() + rva, &val, sizeof val); };

    // Pascal ShortString "\x07T3DNode" in .rdata at 0x2100.
    const char name[] = "\x07T3DNode";
    std::memcpy(image.data() + 0x2100, name, 8);

    // Free Pascal x64 VMT at 0x2200.
    puti64(0x2200 + 0x00, 0x1C8);
    puti64(0x2200 + 0x08, -0x1C8);
    put64(0x2200 + 0x10, 0);
    put64(0x2200 + 0x18, imageBase + 0x2100ull);
    put64(0x2200 + 0x60, imageBase + 0x1100ull); // vDestroy in .text
    put64(0x2200 + 0x68, imageBase + 0x1200ull); // virtual slot 1
    put64(0x2200 + 0x70, imageBase + 0x1300ull); // virtual slot 2

    uint32_t instSize = 0;
    CHECK(fp::rtti::readVmtInstanceSize(image.data() + 0x2200, 16, instSize));
    CHECK(instSize == 0x1C8u);

    fp::rtti::VmtInfo vmt{};
    CHECK(fp::rtti::findClassVmt(view, "T3DNode", vmt));
    CHECK(vmt.rva == 0x2200u && vmt.instanceSize == 0x1C8u && vmt.nameRva == 0x2100u);

    uint32_t slotRva = 0;
    CHECK(fp::rtti::findMethodSlot(view, vmt.rva, 0x1200u, slotRva, 0x68u));
    CHECK(slotRva == 0x2268u);

    // Simulate a base-class virtual method insertion at +0x68 shifting 0x1200 -> +0x70.
    put64(0x2200 + 0x68, imageBase + 0x1180ull);
    put64(0x2200 + 0x70, imageBase + 0x1200ull);
    put64(0x2200 + 0x78, imageBase + 0x1300ull);
    CHECK(fp::rtti::findMethodSlot(view, vmt.rva, 0x1200u, slotRva, 0x68u));
    CHECK(slotRva == 0x2270u);

    // Ambiguous duplicate virtual method pointer in two slots fails closed.
    put64(0x2200 + 0x78, imageBase + 0x1200ull);
    CHECK(!fp::rtti::findMethodSlot(view, vmt.rva, 0x1200u, slotRva, 0x68u));
    put64(0x2200 + 0x78, imageBase + 0x1300ull);

    // Ambiguous duplicate class VMT fails closed.
    std::memcpy(image.data() + 0x2500, image.data() + 0x2200, 0x80);
    CHECK(!fp::rtti::findClassVmt(view, "T3DNode", vmt));
    return true;
}

size_t gSyntheticLiveField = 0x2A0;
size_t gSyntheticLiveDerived = 0x2B8;
void resetSyntheticLive() {
    gSyntheticLiveField = 0x2A0;
    gSyntheticLiveDerived = 0x2B8;
}
void commitSyntheticLive() {
    gSyntheticLiveDerived = gSyntheticLiveField + 0x18;
}

bool hde64_dynamic_offset_adaptation_and_conflict_guard() {
    std::vector<uint8_t> image(0x2000, 0x90);
    fp::ImageView view{image.data(), image.size(), 0x140000000ull, 0x1000u, 0x1800u};

    const uint8_t prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
    std::memcpy(image.data() + 0x1100, prologue, sizeof prologue);

    // Shifted disp32: both witnesses access [rcx + 0x2B0] instead of baseline 0x2A0.
    const uint8_t load2B0[] = {0xF3, 0x0F, 0x10, 0x81, 0xB0, 0x02, 0x00, 0x00};
    const uint8_t store2B0[] = {0xF3, 0x0F, 0x11, 0x81, 0xB0, 0x02, 0x00, 0x00};
    std::memcpy(image.data() + 0x1110, load2B0, sizeof load2B0);
    std::memcpy(image.data() + 0x1120, store2B0, sizeof store2B0);

    fp::Layout layout{};
    layout.resolved = true;
    layout.rva[fp::sig::pollInput] = 0x1100u;

    const fp::ContractOwner owners[] = {
        {"synthOwner", fp::sig::pollInput, "48 89 5C 24 08 57 48 83 EC 20", 0},
    };
    const fp::FieldContract fields[] = {
        {"synth::load", 0u, 0x10u, "F3 0F 10 81 A0 02 00 00", nullptr},
        {"synth::store", 0u, 0x20u, "F3 0F 11 81 A0 02 00 00", nullptr},
    };
    const fp::FieldOffsetBinding bindings[] = {
        {0u, 0u, 0x2A0, false, 0x2A0u, &gSyntheticLiveField},
        {1u, 0u, 0x2A0, false, 0x2A0u, &gSyntheticLiveField},
    };

    fp::ContractReport report{};
    CHECK(fp::detail::validateContractTables(view, layout, report, owners, 1, fields, 2, bindings, 2,
                                             &resetSyntheticLive, &commitSyntheticLive));
    CHECK(gSyntheticLiveField == 0x2B0u);
    CHECK(gSyntheticLiveDerived == 0x2C8u);
    CHECK(report.adaptedFields >= 1u);

    // Disagreeing witnesses (0x2B0 vs 0x2C0) must fail closed and reset live offsets to baseline.
    image[0x1120 + 4] = 0xC0;
    CHECK(!fp::detail::validateContractTables(view, layout, report, owners, 1, fields, 2, bindings, 2,
                                              &resetSyntheticLive, &commitSyntheticLive));
    CHECK(report.firstRequiredReason == fp::ContractFailure::changed);
    CHECK(gSyntheticLiveField == 0x2A0u && gSyntheticLiveDerived == 0x2B8u);

    // Opcode mutation (addss 0x58 instead of movss 0x10) must never adapt even with matching disp32.
    image[0x1120 + 4] = 0xB0;
    image[0x1110 + 2] = 0x58;
    CHECK(!fp::detail::validateContractTables(view, layout, report, owners, 1, fields, 2, bindings, 2,
                                              &resetSyntheticLive, &commitSyntheticLive));
    return true;
}

bool call_graph_closure_and_scoped_degradation() {
    std::vector<uint8_t> image(0x3000, 0xCC);
    const uint64_t imageBase = 0x140000000ull;
    fp::ImageView view{image.data(), image.size(), imageBase, 0x1000u, 0x2000u};

    // Owner at 0x1100: sub rsp, 0x28; call 0x1300; add rsp, 0x28; ret
    const uint8_t ownerBytes[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0xF7, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    // Callee at 0x1300
    const uint8_t calleeBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
    std::memcpy(image.data() + 0x1100, ownerBytes, sizeof ownerBytes);
    std::memcpy(image.data() + 0x1300, calleeBytes, sizeof calleeBytes);

    // VMT slot at 0x2200 + 0xD8 holding imageBase + 0x1300.
    const uint64_t slotVa = imageBase + 0x1300ull;
    std::memcpy(image.data() + 0x2200 + 0xD8, &slotVa, sizeof slotVa);

    fp::Layout layout{};
    layout.resolved = true;
    layout.rva[fp::sig::pollInput] = 0x1100u;
    for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
        if (!fp::sig::targetOptionalGroups[t]) continue;
        if (std::strcmp(fp::sig::targetOptionalGroups[t], "nativeSettings") == 0)
            layout.rva[t] = 0x1500u;
        else if (std::strcmp(fp::sig::targetOptionalGroups[t], "heldThrow") == 0)
            layout.rva[t] = 0x1100u;
    }
    layout.rva[fp::sig::heldBodyMetadata] = 0x2200u;

    const fp::ContractOwner owners[] = {
        {"closureOwner", fp::sig::pollInput, "48 83 EC 28 E8 ?? ?? ?? ?? 48 83 C4 28 C3", 0},
    };
    const fp::FieldContract fields[] = {
        {"core::ok", 0u, 0u, "48 83 EC 28", nullptr},
        {"settings::broken", 0u, 0u, "48 83 EC 38", "nativeSettings"},
        {"heldThrow::body", 0u, 0u, "48 83 EC 28 E8 ?? ?? ?? ?? 48 83 C4 28 C3", "heldThrow"},
    };

    // 1. Scoped degradation: broken nativeSettings witness withholds nativeSettings without failing core.
    fp::ContractReport report{};
    CHECK(fp::detail::validateContractTables(view, layout, report, owners, 1, fields, 3));
    CHECK(report.optionalFailures == 1u);
    CHECK(!layout.optionalGroupAvailable("nativeSettings"));
    CHECK(layout.optionalGroupAvailable("heldThrow"));
    CHECK(layout.rva[fp::sig::pollInput] == 0x1100u);

    // 2. Relocation-aware call-graph + VMT closure check against simulated ASLR live memory.
    const fp::ClosureCallee callees[] = {
        {"callee_1300", "40 53 48 83 EC 20 48 8B D9 48 83 C4 20 5B C3"},
    };
    const fp::ClosureBranch branches[] = {
        {"heldThrow", 2u, 5u, 9u, fp::ClosureKind::callee, 0},
    };
    const fp::ClosureVmtSlot vmtSlots[] = {
        {"heldThrow", fp::sig::heldBodyMetadata, 0xD8u, fp::ClosureKind::callee, 0u},
    };

    std::vector<uint8_t> liveRam = image;
    const uintptr_t liveBase = reinterpret_cast<uintptr_t>(liveRam.data());
    const uintptr_t relocatedSlotVa = liveBase + 0x1300u;
    std::memcpy(liveRam.data() + 0x2200 + 0xD8, &relocatedSlotVa, sizeof relocatedSlotVa);

    CHECK(fp::detail::validateClosureTables(view, layout, "heldThrow", owners, 1, fields, 3,
                                            callees, 1, branches, 1, vmtSlots, 1, liveBase));

    // Mutating unrelated .text/.rdata bytes outside the closure still passes!
    image[0x1700] ^= 0xFF;
    liveRam[0x1700] ^= 0xFF;
    CHECK(fp::detail::validateClosureTables(view, layout, "heldThrow", owners, 1, fields, 3,
                                            callees, 1, branches, 1, vmtSlots, 1, liveBase));

    // Retargeting the E8 rel32 call fails closed.
    image[0x1100 + 5] = 0x07;
    liveRam[0x1100 + 5] = 0x07;
    CHECK(!fp::detail::validateClosureTables(view, layout, "heldThrow", owners, 1, fields, 3,
                                             callees, 1, branches, 1, vmtSlots, 1, liveBase));
    image[0x1100 + 5] = 0xF7;
    liveRam[0x1100 + 5] = 0xF7;

    // Retargeting the live VMT slot in RAM fails closed.
    const uintptr_t hookedSlotVa = liveBase + 0x1400u;
    std::memcpy(liveRam.data() + 0x2200 + 0xD8, &hookedSlotVa, sizeof hookedSlotVa);
    CHECK(!fp::detail::validateClosureTables(view, layout, "heldThrow", owners, 1, fields, 3,
                                             callees, 1, branches, 1, vmtSlots, 1, liveBase));
    return true;
}

bool real_binary_resilience_simulations() {
    char* exeEnv = nullptr;
    size_t exeLen = 0;
    if (_dupenv_s(&exeEnv, &exeLen, "EXCAMERA_TEST_GAME_EXE") != 0 || !exeEnv || !*exeEnv) {
        if (exeEnv) std::free(exeEnv);
        return true;
    }
    const std::filesystem::path exePath(exeEnv);
    std::free(exeEnv);

    std::ifstream in(exePath, std::ios::binary | std::ios::ate);
    CHECK(in.good());
    const auto sz = static_cast<size_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> raw(sz), mapped;
    CHECK(bool(in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz))));
    fp::ImageView baseView;
    CHECK(fp::mapPeFile(raw.data(), raw.size(), mapped, baseView));

    fp::Layout baseLayout;
    fp::ResolveReport resolveRep;
    CHECK(fp::resolveLayout(baseView, baseLayout, resolveRep));

    // 1. Free Pascal x64 RTTI matches resolved VMT targets and T3DNode instance size.
    fp::rtti::VmtInfo nodeVmt{}, heldVmt{}, charVmt{}, navVmt{}, playerVmt{};
    CHECK(fp::rtti::findClassVmt(baseView, "T3DNode", nodeVmt));
    CHECK(nodeVmt.rva == baseLayout.rva[fp::sig::nodeVmt] && nodeVmt.instanceSize == 0x1C8u);
    CHECK(fp::rtti::findClassVmt(baseView, "TTetraRBD", heldVmt));
    CHECK(heldVmt.rva == baseLayout.rva[fp::sig::heldBodyMetadata]);
    CHECK(fp::rtti::findClassVmt(baseView, "TMotileDynamics", charVmt));
    CHECK(charVmt.rva == baseLayout.rva[fp::sig::characterMetadata]);
    CHECK(fp::rtti::findClassVmt(baseView, "TNavCameraControl", navVmt));
    CHECK(fp::rtti::findClassVmt(baseView, "TPlayerBehaviour", playerVmt));

    // 2. Hellmode / hotfix simulation: mutating unrelated header/data bytes breaks whole-EXE SHA-256
    // yet all 4 call-graph closure capabilities remain admitted!
    std::vector<uint8_t> hellmodeImg = mapped;
    hellmodeImg[0x40] ^= 0x5Au;
    fp::ImageView hellmodeView = baseView;
    hellmodeView.data = hellmodeImg.data();
    CHECK(fp::validateOptionalClosure(hellmodeView, baseLayout, "attackVariants"));
    CHECK(fp::validateOptionalClosure(hellmodeView, baseLayout, "physicsOwnership"));
    CHECK(fp::validateOptionalClosure(hellmodeView, baseLayout, "heldThrow"));
    CHECK(fp::validateOptionalClosure(hellmodeView, baseLayout, "headwearRender"));

    auto findWitnessRva = [&](const char* witnessName) -> uint32_t {
        for (const auto& f : fp::contract::fields) {
            if (std::strcmp(f.name, witnessName) == 0) {
                const auto& ow = fp::contract::owners[f.owner];
                fp::Pattern pat;
                uint32_t at = 0;
                if (!fp::parsePattern(ow.pattern, pat)) return 0;
                if (fp::findPattern(baseView, pat, baseView.textBegin, baseView.textEnd, &at) != 1) return 0;
                return static_cast<uint32_t>(static_cast<int64_t>(at) + ow.entryOffset) + f.offset;
            }
        }
        return 0;
    };

    // 3. Call-graph tamper detection: retargeting a single attackVariants rel32 fails attackVariants only,
    // and corrupting a TTetraRBD VMT slot fails heldThrow only.
    for (const auto& br : fp::contract::closureBranches) {
        if (std::strcmp(br.group, "attackVariants") == 0) {
            const auto& wf = fp::contract::fields[br.witnessIndex];
            const uint32_t wRva = findWitnessRva(wf.name);
            CHECK(wRva != 0);
            std::vector<uint8_t> tampered = mapped;
            tampered[wRva + br.dispOffset] ^= 0x20u;
            fp::ImageView tamperedView = baseView;
            tamperedView.data = tampered.data();
            CHECK(!fp::validateOptionalClosure(tamperedView, baseLayout, "attackVariants"));
            CHECK(fp::validateOptionalClosure(tamperedView, baseLayout, "headwearRender"));
            break;
        }
    }
    {
        std::vector<uint8_t> vmtTampered = mapped;
        vmtTampered[baseLayout.rva[fp::sig::heldBodyMetadata] + 0xD8u] ^= 0x10u;
        fp::ImageView vmtView = baseView;
        vmtView.data = vmtTampered.data();
        CHECK(!fp::validateOptionalClosure(vmtView, baseLayout, "heldThrow"));
        CHECK(fp::validateOptionalClosure(vmtView, baseLayout, "attackVariants"));
    }

    // 4. Scoped degradation: corrupting inventory::completionCurrentOwner or optionsForm::applied
    // withholds only inventoryReach or nativeSettings while keeping core ExCamera healthy!

    const uint32_t invWitnessRva = findWitnessRva("inventory::completionCurrentOwner");
    CHECK(invWitnessRva != 0);
    {
        std::vector<uint8_t> invMod = mapped;
        invMod[invWitnessRva] ^= 0xFFu;
        fp::ImageView invView = baseView;
        invView.data = invMod.data();
        fp::Layout invLayout = baseLayout;
        fp::ContractReport invRep;
        CHECK(fp::validateNativeContracts(invView, invLayout, invRep));
        CHECK(!invLayout.optionalGroupAvailable("inventoryReach"));
        CHECK(invLayout.optionalGroupAvailable("nativeSettings"));
        CHECK(invLayout.rva[fp::sig::pollInput] != 0);
    }

    const uint32_t optWitnessRva = findWitnessRva("optionsForm::applied");
    CHECK(optWitnessRva != 0);
    {
        std::vector<uint8_t> optMod = mapped;
        optMod[optWitnessRva] ^= 0xFFu;
        fp::ImageView optView = baseView;
        optView.data = optMod.data();
        fp::Layout optLayout = baseLayout;
        fp::ContractReport optRep;
        CHECK(fp::validateNativeContracts(optView, optLayout, optRep));
        CHECK(!optLayout.optionalGroupAvailable("nativeSettings"));
        CHECK(optLayout.optionalGroupAvailable("inventoryReach"));
        CHECK(optLayout.rva[fp::sig::pollInput] != 0);
    }

    // 5. Real-binary hde64 struct-offset adaptation: shift bodyPart::pose 0x2A0 -> 0x2B0.
    const uint32_t poseWitnessRva = findWitnessRva("bodyPart::pose");
    CHECK(poseWitnessRva != 0);
    {
        std::vector<uint8_t> poseMod = mapped;
        CHECK(poseMod[poseWitnessRva + 17] == 0xA0u && poseMod[poseWitnessRva + 18] == 0x02u);
        poseMod[poseWitnessRva + 17] = 0xB0u;
        fp::ImageView poseView = baseView;
        poseView.data = poseMod.data();
        fp::Layout poseLayout = baseLayout;
        fp::ContractReport poseRep;
        CHECK(fp::validateNativeContracts(poseView, poseLayout, poseRep));
        CHECK(poseRep.adaptedFields == 1u);
        CHECK(fp::game::live::bodyPart::pose == 0x2B0u);
        CHECK(fp::game::live::bodyPart::poseForward == 0x2C8u);
        CHECK(fp::game::live::bodyPart::poseOrigin == 0x2D4u);
        fp::game::live::resetLiveOffsets();
        fp::game::live::recomputeDerivedOffsets();
        CHECK(fp::game::live::bodyPart::pose == 0x2A0u);
    }

    return true;
}

bool engine_text_ascii_line() {
    char out[64];
    // The old F12 line: '%', '[', ']' and '|' are kept as they are (the engine formats nothing).
    CHECK(fp::engineAscii(L"Blood: 40% | Hands: [L:Open R:Hilt]", out, sizeof out) == 35);
    CHECK(std::strcmp(out, "Blood: 40% | Hands: [L:Open R:Hilt]") == 0);
    // One line: CR/LF (the engine's line breaks) and tabs become spaces; typographic dashes and quotes ASCII.
    fp::engineAscii(L"a\r\nb\tc \x2014 \x201Cq\x201D \x2019", out, sizeof out);
    CHECK(std::strcmp(out, "a  b c - \"q\" '") == 0);
    // Anything else outside ASCII is '?', and the result always fits and ends.
    fp::engineAscii(L"\x00E9\x4E2D", out, sizeof out);
    CHECK(std::strcmp(out, "??") == 0);
    char small[4];
    CHECK(fp::engineAscii(L"abcdef", small, sizeof small) == 3 && std::strcmp(small, "abc") == 0);
    CHECK(fp::engineAscii(nullptr, small, sizeof small) == 0 && small[0] == 0);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"input ButtonHandoff and RawLook", input_button_handoff_and_raw_look},
        {"input angle wrapping and vrTurningFromText", input_angle_wrapping_and_vr_turning_text},
        {"input combat cues, cursor, and ray helpers", input_combat_cues_cursor_and_ray_helpers},
        {"input F6 cycle policy", input_f6_cycle_policy},
        {"input cursor panels", input_cursor_panels},
        {"input motion refusals", input_motion_refusals},
        {"input game binding keys", input_game_binding_keys},
        {"camera motion FOV, AimBox, NearPlane, and eye stabilization",
         camera_motion_fov_aimbox_nearplane_and_eye_stabilization},
        {"camera motion shoulder slide", camera_motion_shoulder_slide},
        {"camera motion look-follow framing", camera_motion_look_follow_framing},
        {"camera motion knockdown view", camera_motion_knockdown_view},
        {"camera motion jump push", camera_motion_jump_push},
        {"hook health menu immunity and stalled pollInput",
         hook_health_menu_immunity_and_stalled_poll_input},
        {"PointerSet and JsonOut formatting", pointer_set_and_status_json_formatting},
        {"xr/math.h standalone types and transforms", xr_math_standalone_types_and_transforms},
        {"all signatures and native contract patterns parse", all_signatures_and_native_contracts_parse},
        {"Free Pascal x64 RTTI VMT and shifted slot introspection",
         fpc_rtti_vmt_and_shifted_slot_introspection},
        {"hde64 dynamic struct-offset adaptation and conflict guard",
         hde64_dynamic_offset_adaptation_and_conflict_guard},
        {"call-graph closure verification and scoped subsystem degradation",
         call_graph_closure_and_scoped_degradation},
        {"real Exanima.exe resilience simulations", real_binary_resilience_simulations},
        {"engine log text: one ASCII line, nothing escaped", engine_text_ascii_line},
    });
}
