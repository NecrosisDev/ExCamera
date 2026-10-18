// Production camera integration: blocker ownership, held FOV and camera lifetime.
// All native objects are owned byte arrays; native calls are local spies.
// No game process, files, window, hooks, injection, or installed DLL is used.
#ifdef NDEBUG
#error "The camera integration tests require assert()."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool temporalClockEnabled = false;
static LONGLONG temporalClockUs = 0;
static BOOL cameraFixtureCounter(LARGE_INTEGER* value) {
    if (!temporalClockEnabled) return QueryPerformanceCounter(value);
    value->QuadPart = temporalClockUs;
    return TRUE;
}
static ULONGLONG cameraFixtureTick() {
    return temporalClockEnabled ? ULONGLONG(temporalClockUs / 1000) : GetTickCount64();
}
static BOOL fakeClientRect(HWND, RECT* out) {
    *out = RECT{0, 0, 1280, 720};
    return TRUE;
}
#define GetClientRect fakeClientRect
#define QueryPerformanceCounter cameraFixtureCounter
#define GetTickCount64 cameraFixtureTick
#include "first_person.cpp"
#undef GetClientRect
#undef QueryPerformanceCounter
#undef GetTickCount64
#include <cstdio>
#include <cassert>
#include <utility>
#include <vector>
#include "test_head_render_pose.h"

static uintptr_t blocker = 0;
static unsigned pickCalls = 0;
static bool pickFault = false;
static uintptr_t nativeNav(void*, float) { return 123; }
static void nativeProjection(uintptr_t) {}
static void nativeInverse(uintptr_t destination, uintptr_t source) {
    // The verified native adapter expands the affine transform; its historical
    // ExCamera name is not a matrix inversion.
    float affine[12];
    memcpy(affine, (const void*)source, sizeof affine);
    auto out = (float*)destination;
    for (int row = 0; row < 4; ++row) {
        for (int axis = 0; axis < 3; ++axis) out[row * 4 + axis] = affine[row * 3 + axis];
        out[row * 4 + 3] = row == 3 ? 1.f : 0.f;
    }
}
static char nativePick(uintptr_t viewport, float, float, uintptr_t out) {
    assert(viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport));
    if (pickFault) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    ++pickCalls;
    memset(reinterpret_cast<void*>(out), 0, 0x40);
    *reinterpret_cast<uintptr_t*>(out) = blocker;
    *reinterpret_cast<float*>(out + field::pick::distance) = 100.f;
    return 1;
}
static float phasePose[12]{}, phaseOffset = 0.f;
static unsigned phaseCalls = 0;
static bool phaseFault = false;
static bool temporalMotion = false, temporalNearWall = false;
static float temporalCentreSeconds = 0.f;
static float phasedPartRenderSampler(uintptr_t sampler, uintptr_t head, float offset) {
    assert(sampler == gameAt(fp::sig::headRenderSample));
    ++phaseCalls;
    phaseOffset = offset;
    if (phaseFault) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    const float t = temporalCentreSeconds + offset * 0.001f;
    const float yaw = temporalMotion ? 2.f * t : 0.4f * offset;
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float source[12] = {c, 0, -s, 0, 1, 0, s, 0, c,
                              temporalMotion ? 100.f * t : 20.f * offset,
                              temporalMotion ? 160.f : 160.f + 3.f * offset,
                              temporalMotion ? 25.f * t : 7.f * offset};
    memcpy(phasePose, source, sizeof phasePose);
    const uintptr_t mesh = *(uintptr_t*)(head + field::bodyPart::meshNode);
    float* rendered = (float*)(mesh + field::sceneNode::renderTransform);
    for (int row = 0; row < 4; ++row) {
        for (int axis = 0; axis < 3; ++axis) rendered[row * 4 + axis] = phasePose[row * 3 + axis];
        rendered[row * 4 + 3] = row == 3 ? 1.f : 0.f;
    }
    return offset;
}

static bool temporalLevelRay(uintptr_t, const float*, const float*, float length) {
    return temporalNearWall && length >= 1.f;
}

struct TemporalResult {
    CameraFilterState filters;
    std::array<std::array<float, 12>, 3> poses{};
};
static void sameFilterHistory(const CameraFilterState& a, const CameraFilterState& b) {
    auto same = [](float x, float y) { assert(std::fabs(x - y) < 1.e-6f); };
    for (int i = 0; i < 3; ++i) {
        same(a.third.pivot[i], b.third.pivot[i]);
        same(a.third.vel[i], b.third.vel[i]);
        same(a.third.lastEye[i], b.third.lastEye[i]);
        same(a.third.anchor[i], b.third.anchor[i]);
    }
    same(a.third.distance, b.third.distance);
    same(a.third.sideNow, b.third.sideNow);
    same(a.third.pinned, b.third.pinned);
    same(a.third.swapHold, b.third.swapHold);
    same(a.clipFilter.value, b.clipFilter.value);
    same(a.lead.lastYaw, b.lead.lastYaw);
    same(a.lead.rate, b.lead.rate);
    same(a.combat, b.combat);
    assert(a.third.valid == b.third.valid && a.third.haveEye == b.third.haveEye &&
           a.third.down == b.third.down && a.third.knock.down == b.third.knock.down &&
           a.third.knock.clearSince == b.third.knock.clearSince &&
           a.third.swaps == b.third.swaps && a.clipFilter.valid == b.clipFilter.valid && a.lead.valid == b.lead.valid);
}
static CameraFilterState filterHistory() {
    return {thirdPerson, nearPlane, cameraTurnLead, tpCombatK};
}
static void temporalCameraCases(uintptr_t root, uintptr_t head, uintptr_t sector,
                                uintptr_t viewport, uintptr_t view, uintptr_t camera) {
    temporalClockEnabled = temporalMotion = true;
    frequency.QuadPart = 1000000;
    const uintptr_t manager = base + 0x55c000, level = base + 0x55d000;
    *(uintptr_t*)(viewport + field::viewport::sceneManager) = manager;
    *(unsigned*)(manager + field::sceneManager::flags) = 0x2000000;
    *(uintptr_t*)(manager + field::sceneManager::levelGrid) = level;
    levelRay = levelRay2 = temporalLevelRay;
    originalPickRay = nullptr;
    partRenderSampler = phasedPartRenderSampler;
    eyeSetback = 16;
    tpBlockers = fp::blockersPush;
    tpDistance = 150;
    tpShoulder = 50;
    tpHeight = -20;
    tpTight = false;
    tpDown = false;
    tpCombat = true;
    nearClip = 5;
    unsigned char physicsBefore[0x1e0];
    memcpy(physicsBefore, (const void*)(head + field::bodyPart::points), sizeof physicsBefore);

    auto run = [&](int cameraMode, int lag, const std::vector<float>& offsets) {
        if (savedCamera) {
            CameraGuard guard;
            restoreOwnedCamera(view, camera);
        }
        filterRoot = filterHead = 0;
        cameraFrame = {};
        thirdPerson.reset();
        nearPlane.reset();
        cameraTurnLead.reset();
        tpCombatK = 0.f;
        filterTime = {};
        headHold = {};
        holdingNow = 0;
        park = {};
        aim = {};
        viewMode = cameraMode;
        tpLag = lag;
        turnLeadOn = cameraMode == 0;
        TemporalResult result;
        for (int frame = 0; frame < 10; ++frame) {
            // One real presentation boundary; native sample order/cost varies
            // inside it. Leaving a close wall exercises both boom and near ease.
            swapCalls = 100 + frame;
            temporalCentreSeconds = float(frame) * 0.016f;
            const LONGLONG frameClock = 1000000 + LONGLONG(frame) * 16000;
            temporalNearWall = frame == 0;
            *(unsigned char*)(root + field::character::combatStance) = frame >= 3 ? 1 : 0;
            CameraFilterState canonical, prior;
            bool established = false;
            unsigned request = 0;
            for (float offset : offsets) {
                temporalClockUs = frameClock + LONGLONG(request++) * 1500;
                assert(nav((void*)42, offset) == 123);
                assert(phaseOffset == offset && cameraReady && cameraFrame.centreReady);
                if (!established) {
                    canonical = filterHistory();
                    prior = cameraFrame.before;
                    established = true;
                }
                sameFilterHistory(filterHistory(), canonical);
                sameFilterHistory(cameraFrame.before, prior);
                assert(filterTime.QuadPart == frameClock);
                const float* actual = (const float*)(camera + field::camera::pose);
                assert(memcmp(actual, (const void*)(camera + field::camera::pose2), 48) == 0);
                assert(memcmp(actual, lastCamera, 48) == 0);
                const float* expanded = (const float*)(camera + field::camera::inverse);
                for (int row = 0; row < 4; ++row)
                    for (int axis = 0; axis < 3; ++axis)
                        assert(expanded[row * 4 + axis] == actual[row * 3 + axis]);
                float requestedEye[3];
                fp::eyePoint(phasePose, requestedEye, 16.f);
                for (int axis = 0; axis < 3; ++axis) {
                    requestedEye[axis] += *(float*)(sector + field::sector::origin + axis * 4);
                    assert(std::fabs(eyeGlobal[axis] - requestedEye[axis]) < 1.e-4f);
                    if (cameraMode == 0)
                        assert(std::fabs(actual[9 + axis] - requestedEye[axis]) < 1.e-4f);
                }
                if (cameraMode == 1) {
                    const float expectedYaw = 2.f * (temporalCentreSeconds + offset * 0.001f);
                    assert(std::fabs(fp::wrapPi(std::atan2(-actual[6], -actual[8]) - expectedYaw)) < 1.e-5f);
                    const float centreEye[3] = {
                        100.f * temporalCentreSeconds + .5f * std::sin(2.f * temporalCentreSeconds),
                        160.f + fp::eyeAlongAxis2,
                        25.f * temporalCentreSeconds + .5f * std::cos(2.f * temporalCentreSeconds)};
                    for (int axis = 0; axis < 3; ++axis)
                        assert(std::fabs(thirdPerson.lastEye[axis] - centreEye[axis] -
                                         *(float*)(sector + field::sector::origin + axis * 4)) < 1.e-4f);
                }
                if (frame == 9) {
                    const int index = offset < 0.f ? 0 : offset > 0.f ? 2 : 1;
                    memcpy(result.poses[index].data(), actual, 48);
                }
                assert(memcmp(physicsBefore, (const void*)(head + field::bodyPart::points),
                              sizeof physicsBefore) == 0);
            }
        }
        result.filters = filterHistory();
        assert(result.filters.clipFilter.value > .5f && result.filters.clipFilter.value < 3.f);
        if (cameraMode == 1) assert(result.filters.combat > 0.f && result.filters.combat < 1.f);
        if (cameraMode == 0) assert(std::fabs(result.filters.lead.rate) > 1.f);
        return result;
    };

    for (const auto& [cameraMode, lag] : {std::pair{1, 0}, std::pair{1, 40}, std::pair{0, 0}}) {
        const TemporalResult forward = run(cameraMode, lag, {0.f, -4.f, 4.f});
        // Start with a future sample, reverse time, and request duplicates and
        // the centre again after wall time has advanced. The result is identical.
        const TemporalResult reordered = run(cameraMode, lag, {4.f, -4.f, 4.f, 0.f, -4.f, 0.f, 4.f});
        sameFilterHistory(forward.filters, reordered.filters);
        for (int sample = 0; sample < 3; ++sample)
            for (int component = 0; component < 12; ++component)
                assert(std::fabs(forward.poses[sample][component] -
                                 reordered.poses[sample][component]) < 1.e-5f);
        const float yawBefore = std::atan2(-forward.poses[0][6], -forward.poses[0][8]);
        const float yawAfter = std::atan2(-forward.poses[2][6], -forward.poses[2][8]);
        assert(fp::wrapPi(yawAfter - yawBefore) > .01f);
        float displacement = 0.f;
        for (int axis = 9; axis < 12; ++axis)
            displacement += std::fabs(forward.poses[2][axis] - forward.poses[0][axis]);
        assert(displacement > .01f);
        printf("temporal nav: mode=%d lag=%d, reordered/extra requests identical; "
               "sample displacement=%.4f cm; near=%.4f; lead rate=%.4f\n",
               cameraMode, lag, displacement, forward.filters.clipFilter.value, forward.filters.lead.rate);
    }

    // No elapsed history and a long pause have deterministic bounded behavior.
    ++swapCalls;
    temporalClockUs = filterTime.QuadPart + 1000;
    temporalCentreSeconds += .001f;
    assert(nav((void*)42, -4.f) == 123);
    const CameraFilterState tinyStep = filterHistory();
    for (float value : lastCamera) assert(std::isfinite(value));
    temporalClockUs += 1000;
    assert(nav((void*)42, 4.f) == 123);
    sameFilterHistory(tinyStep, filterHistory());
    ++swapCalls;
    temporalClockUs += 5000000;
    temporalCentreSeconds += 5.f;
    assert(nav((void*)42, 4.f) == 123 && cameraFrame.seconds > .5f);
    const CameraFilterState afterPause = filterHistory();
    assert(afterPause.lead.rate == 0.f);
    for (float value : lastCamera) assert(std::isfinite(value));
    temporalClockUs += 1000;
    assert(nav((void*)42, -4.f) == 123);
    sameFilterHistory(afterPause, filterHistory());

    // A native render fault holds the actual last write. Recovery starts a new
    // epoch instead of replaying the earlier presentation's filter snapshot.
    float held[12];
    memcpy(held, lastCamera, sizeof held);
    phaseFault = true;
    temporalClockUs += 1000;
    assert(nav((void*)42, 4.f) == 123 && holdingNow && !cameraFrame.valid);
    assert(memcmp(held, (const void*)(camera + field::camera::pose), sizeof held) == 0);
    phaseFault = false;
    temporalClockUs += 3000;
    assert(nav((void*)42, 4.f) == 123 && !holdingNow && cameraFrame.centreReady);
    assert(cameraFrame.time.QuadPart == temporalClockUs);

    // Exercise the real park/reset adapter without a live game or a window.
    const unsigned beforePark = phaseCalls;
    {
        CameraGuard guard;
        writeStock(view, camera, stockPose, stockFov, stockNear);
        parkCamera(fp::OwnerChange::parkRestore);
    }
    assert(nav((void*)42, 4.f) == 123 && !cameraReady && phaseCalls == beforePark);
    park.active = false; // The fixture supplies the already-authorized rearm edge.
    temporalClockUs += 1000;
    assert(nav((void*)42, 4.f) == 123 && cameraReady);
    assert(!cameraFrame.before.third.valid && !cameraFrame.before.clipFilter.valid &&
           !cameraFrame.before.lead.valid);

    const uintptr_t newRoot = base + 0x568000;
    memcpy((void*)newRoot, (const void*)root, 0x1400);
    *(uintptr_t*)(head + field::bodyPart::character) = newRoot;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = newRoot;
    temporalClockUs += 1000;
    assert(nav((void*)42, 4.f) == 123 && cameraFrame.root == newRoot);
    assert(!cameraFrame.before.third.valid && !cameraFrame.before.clipFilter.valid &&
           !cameraFrame.before.lead.valid);
    *(uintptr_t*)(head + field::bodyPart::character) = root;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;

    puts("temporal nav: tiny/paused time, source fault, park/rearm and actor replacement pass");
    temporalClockEnabled = temporalMotion = temporalNearWall = false;
    levelRay = levelRay2 = nullptr;
    *(uintptr_t*)(viewport + field::viewport::sceneManager) = 0;
    QueryPerformanceFrequency(&frequency);
}

struct Outcome { unsigned detected, retained, published, samples; float heldFov; bool recoveredUpright; };
static Outcome runCase(bool sectorEqualsCurrentScene) {
    std::vector<unsigned char> memory(0x600000);
    base = reinterpret_cast<uintptr_t>(memory.data());
    const uintptr_t root = base + 0x550000, bh = base + 0x552000;
    const uintptr_t view = base + 0x554000, viewport = base + 0x555000;
    const uintptr_t camera = base + 0x556000, head = base + 0x557000;
    const uintptr_t mesh = base + 0x558000, sector = base + 0x559000;
    const uintptr_t scene = sectorEqualsCurrentScene ? sector : base + 0x55a000;
    blocker = base + 0x55b000;
    *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) = root;
    *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) = scene;
    *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::mainViewport)) = viewport;
    *reinterpret_cast<uintptr_t*>(root + field::character::behaviour) = bh;
    *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode) = mesh;
    *reinterpret_cast<uintptr_t*>(root + field::character::headPart) = head;
    *reinterpret_cast<uintptr_t*>(bh + field::behaviour::character) = root;
    *reinterpret_cast<uintptr_t*>(viewport + field::viewport::view) = view;
    *reinterpret_cast<uintptr_t*>(view + field::view::camera) = camera;
    *reinterpret_cast<uintptr_t*>(viewport + field::viewport::pickCamera) = camera;
    *reinterpret_cast<uintptr_t*>(camera + field::camera::controller) = 42;
    *reinterpret_cast<uintptr_t*>(head + field::bodyPart::character) = root;
    *reinterpret_cast<uintptr_t*>(head + field::bodyPart::meshNode) = mesh;
    *reinterpret_cast<uintptr_t*>(mesh + field::sceneNode::bodyPart) = head;
    *reinterpret_cast<uintptr_t*>(mesh + field::sceneNode::sector) = sector;
    fixtureHeadRenderSampling(head);
    auto headPose = reinterpret_cast<float*>(mesh + field::sceneNode::transform);
    headPose[0] = headPose[4] = headPose[8] = 1.f;
    headPose[10] = 160.f;
    *reinterpret_cast<float*>(camera + field::camera::fov) = 75.f;
    *reinterpret_cast<float*>(camera + field::camera::nearPlane) = 100.f;
    fp::basis(0, 0, reinterpret_cast<float*>(camera + field::camera::pose));
    *reinterpret_cast<unsigned*>(blocker + 8) = 0x2000;
    *reinterpret_cast<uintptr_t*>(blocker + field::sceneNode::sector) = sector;
    auto blockerPosition = reinterpret_cast<float*>(blocker + field::sceneNode::transformOrigin);
    blockerPosition[0] = -20.f;
    blockerPosition[1] = 184.60687f;
    blockerPosition[2] = -110.f;
    originalNav = nativeNav;
    originalPickRay = nativePick;
    rebuildProjection = nativeProjection;
    rebuildInverse = nativeInverse;
    hookedWindow = reinterpret_cast<HWND>(1);
    active = 1;
    stopping = 0;
    inputEnabled = 1;
    viewMode = 1;
    tpBlockers = fp::blockersHide;
    tpHeight = 15;
    tpDistance = 250;
    tpShoulder = 40;
    tpLag = 0;
    tpTight = false;
    tpDown = false;
    tpCombat = false;
    selectedFov = 75;
    tpFov = 95;
    eyeSetback = 0;
    savedCamera = savedView = savedController = savedRoot = savedScene = 0;
    filterRoot = filterHead = 0;
    park = {};
    for (auto& r : retired) r = {};
    restorationUnknown = 0;
    pendingRestore = 0;
    aim = {};
    thirdPerson.reset();
    tpHideClear();
    tpHideScene = 0;
    tpHidden = 0;
    samples = 0;
    pickCalls = 0;
    QueryPerformanceFrequency(&frequency);
    filterTime = {};
    // Native pick boundary uses three separate native objects and restores both
    // poses even when the native query faults. No real window/process is touched.
    float prior[24], trial[12];
    memcpy(prior, (void*)(camera + field::camera::pose), 48);
    memcpy(prior + 12, (void*)(camera + field::camera::pose2), 48);
    fp::basis(.2f, .3f, trial);
    trial[9] = 17;
    trial[10] = 180;
    trial[11] = 25;
    for (bool fault : {false, true}) {
        pickFault = fault;
        const float depth = centrePropDepth(camera, trial, root);
        assert(depth == -1.f); // native spy distance is 100, beyond the 16 cm near probe
        assert(!memcmp(prior, (void*)(camera + field::camera::pose), 48));
        assert(!memcmp(prior + 12, (void*)(camera + field::camera::pose2), 48));
    }
    pickFault = false;
    const unsigned beforeMismatch = pickCalls;
    *(uintptr_t*)(viewport + field::viewport::pickCamera) = camera + 0x1000;
    assert(centrePropDepth(camera, trial, root) == -1.f && pickCalls == beforeMismatch);
    *(uintptr_t*)(viewport + field::viewport::pickCamera) = camera;
    const auto result = nav(reinterpret_cast<void*>(42), 0.f);
    Outcome outcome{tpHidden.load(), tpHiddenCount, tpHideTopCount, samples.load(), 0.f, false};
    printf("currentScene %s head sector: native_result=%llu, picks=%u, detected=%u, retained=%u, "
           "published=%u, camera_samples=%u\n", sectorEqualsCurrentScene ? "equals" : "differs from",
           static_cast<unsigned long long>(result), pickCalls, outcome.detected, outcome.retained,
           outcome.published, outcome.samples);
    // A transient bad head holds exactly the prior TP view, including its separate FOV.
    assert(*reinterpret_cast<float*>(camera + field::camera::fov) == 95.f);
    *reinterpret_cast<uintptr_t*>(root + field::character::headPart) = 0;
    assert(nav(reinterpret_cast<void*>(42), 0.f) == 123);
    outcome.heldFov = *reinterpret_cast<float*>(camera + field::camera::fov);
    assert(holdingNow.load() == 1);
    printf("held TP FOV: %.1f (separate first-person setting: %d)\n", outcome.heldFov, selectedFov.load());
    *reinterpret_cast<uintptr_t*>(root + field::character::headPart) = head;
    // A down state must not leak through a first-person visit into a healthy TP view.
    tpDown = true;
    *reinterpret_cast<unsigned*>(root + field::character::stateFlags) = 4;
    assert(nav(reinterpret_cast<void*>(42), 0.f) == 123 && tpDownNow.load());
    viewMode = 0;
    assert(nav(reinterpret_cast<void*>(42), 0.f) == 123);
    *reinterpret_cast<unsigned*>(root + field::character::stateFlags) = 0;
    viewMode = 1;
    assert(nav(reinterpret_cast<void*>(42), 0.f) == 123);
    outcome.recoveredUpright = !tpDownNow.load();
    printf("return to TP after recovery: down=%d\n", int(tpDownNow.load()));
    // Different render offsets can be requested while mesh+40 and the physical
    // state stay unchanged. The view must follow the current sampled matrix.
    viewMode = 0;
    turnLeadOn = false;
    partRenderSampler = phasedPartRenderSampler;
    phaseCalls = 0;
    phaseFault = false;
    const float sectorOrigin[3] = {100.f, 20.f, -50.f};
    memcpy((void*)(sector + field::sector::origin), sectorOrigin, sizeof sectorOrigin);
    unsigned char physicsBefore[0x1e0];
    memcpy(physicsBefore, (const void*)(head + field::bodyPart::points), sizeof physicsBefore);
    float maxEyeError = 0.f, oldPoseError = 0.f;
    for (int setback : {0, 16}) {
        eyeSetback = setback;
        for (float offset : {0.f, 0.25f, 0.5f, 1.f, -0.25f}) {
            const unsigned before = phaseCalls;
            assert(nav(reinterpret_cast<void*>(42), offset) == 123);
            assert(phaseCalls == before + 1 && phaseOffset == offset);
            float eye[3], expected[12], oldEye[3];
            fp::eyePoint(phasePose, eye, float(setback));
            fp::eyePoint(headPose, oldEye, float(setback));
            for (int k = 0; k < 3; ++k) {
                eye[k] += sectorOrigin[k];
                oldEye[k] += sectorOrigin[k];
            }
            assert(fp::headPose(phasePose, eye, expected));
            const float* actual = (const float*)(camera + field::camera::pose);
            float error = 0.f, oldError = 0.f;
            for (int k = 0; k < 12; ++k) assert(std::fabs(actual[k] - expected[k]) < 1.e-4f);
            for (int k = 0; k < 3; ++k) {
                error += (actual[9 + k] - eye[k]) * (actual[9 + k] - eye[k]);
                oldError += (oldEye[k] - eye[k]) * (oldEye[k] - eye[k]);
            }
            maxEyeError = std::fmax(maxEyeError, std::sqrt(error));
            oldPoseError = std::fmax(oldPoseError, std::sqrt(oldError));
            assert(memcmp(physicsBefore, (const void*)(head + field::bodyPart::points),
                          sizeof physicsBefore) == 0);
        }
    }
    assert(oldPoseError > 20.f && maxEyeError < 1.e-4f);
    printf("render phase: samples=%u, max eye mismatch=%.6f cm; old mesh+40 mismatch=%.3f cm\n",
           phaseCalls, maxEyeError, oldPoseError);
    const unsigned beforeFallback = phaseCalls;
    float held[12];
    memcpy(held, lastCamera, sizeof held);
    // An unexpected virtual method is never invoked. Preserve the last good
    // view through the existing bounded hold, then recover on a valid sample.
    const uintptr_t table = *(uintptr_t*)head;
    *(uintptr_t*)(table + partRenderSampleSlot) = base + 0x1000;
    assert(nav(reinterpret_cast<void*>(42), 0.5f) == 123);
    assert(phaseCalls == beforeFallback && holdingNow.load());
    assert(memcmp(held, (const void*)(camera + field::camera::pose), sizeof held) == 0);
    *(uintptr_t*)(table + partRenderSampleSlot) = gameAt(fp::sig::headRenderSample);
    phaseFault = true;
    assert(nav(reinterpret_cast<void*>(42), 0.5f) == 123 && holdingNow.load());
    assert(memcmp(held, (const void*)(camera + field::camera::pose), sizeof held) == 0);
    phaseFault = false;
    assert(nav(reinterpret_cast<void*>(42), 0.5f) == 123 && !holdingNow.load());
    puts("render sample fallback: unknown method not called; native fault holds; valid source resumes");
    if (!sectorEqualsCurrentScene) temporalCameraCases(root, head, sector, viewport, view, camera);
    savedCamera = savedView = savedController = savedRoot = savedScene = 0;
    return outcome;
}

int main() {
    const Outcome separate = runCase(false), alias = runCase(true);
    for (const auto& result : {separate, alias}) {
        assert(result.detected > 0 && result.retained == 1 && result.published == 1 && result.samples == 1);
        assert(result.heldFov == 95.f);
        assert(result.recoveredUpright);
    }
    puts("CAMERA INTEGRATION: production nav keeps blockers, held TP FOV and actor camera lifetime.");
}
