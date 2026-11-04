#include "camera_motion.h"
#include "hook_health.h"
#include "input_policy.h"
#include "native_contracts.h"
#include "pointer_set.h"
#include "status_json.h"
#include "xr/math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

} // namespace

int main() {
    return run_tests({
        {"input ButtonHandoff and RawLook", input_button_handoff_and_raw_look},
        {"input angle wrapping and vrTurningFromText", input_angle_wrapping_and_vr_turning_text},
        {"input combat cues, cursor, and ray helpers", input_combat_cues_cursor_and_ray_helpers},
        {"camera motion FOV, AimBox, NearPlane, and eye stabilization",
         camera_motion_fov_aimbox_nearplane_and_eye_stabilization},
        {"hook health menu immunity and stalled pollInput",
         hook_health_menu_immunity_and_stalled_poll_input},
        {"PointerSet and JsonOut formatting", pointer_set_and_status_json_formatting},
        {"xr/math.h standalone types and transforms", xr_math_standalone_types_and_transforms},
        {"all signatures and native contract patterns parse", all_signatures_and_native_contracts_parse},
    });
}
