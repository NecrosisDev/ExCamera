// Production attention owner, synthetic actors and the Ghidra BEAC0 score.
// This checks input/field ownership, not Exanima's physics or rendered defense.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool foreground = true;
static HWND testWindow() {
    return foreground ? (HWND)1 : nullptr;
}
static DWORD testPid(HWND w, DWORD* pid) {
    *pid = w ? GetCurrentProcessId() : 0;
    return GetCurrentThreadId();
}
static SHORT WINAPI testKey(int) {
    return 0;
}
static BOOL testRect(HWND, RECT* r) {
    *r = {0, 0, 800, 600};
    return TRUE;
}
static BOOL testCursor(POINT* p) {
    *p = {400, 300};
    return TRUE;
}
static BOOL testSetCursor(int, int) {
    return TRUE;
}
static BOOL testScreen(HWND, POINT*) {
    return TRUE;
}
static HMONITOR testMonitor(POINT, DWORD) {
    return (HMONITOR)1;
}
static int testMap(HWND, HWND, POINT*, UINT) {
    return 0;
}
static BOOL testClip(const RECT*) {
    return TRUE;
}
#define GetForegroundWindow testWindow
#define GetWindowThreadProcessId testPid
#define GetAsyncKeyState testKey
#define GetClientRect testRect
#define GetCursorPos testCursor
#define SetCursorPos testSetCursor
#define ClientToScreen testScreen
#define MonitorFromPoint testMonitor
#define MapWindowPoints testMap
#define ClipCursor testClip
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#undef GetClientRect
#undef GetCursorPos
#undef SetCursorPos
#undef ClientToScreen
#undef MonitorFromPoint
#undef MapWindowPoints
#undef ClipCursor
#include "test_controlled_player.h"
#include <limits>

static unsigned checks = 0, failures = 0, nativeCalls = 0;
static void check(bool value, const char* label) {
    ++checks;
    if (!value) ++failures;
    printf("%s %s\n", value ? "PASS" : "FAIL", label);
}
template <class T> static T& at(uintptr_t p, size_t offset) {
    return *(T*)(p + offset);
}
static bool vectorNear(const float* a, const float* b) {
    for (int i = 0; i < 3; ++i)
        if (fabsf(a[i] - b[i]) > 1e-5f) return false;
    return true;
}
static uintptr_t actor, bh, head, scene, attentionOther, otherBh, otherHead, gui, drag;
static float looked[3] = {0, 0, 1}, aimed[3] = {.6f, 0, .8f}, seenGaze[3];
static float candidates[2][3] = {{0, 0, 150}, {90, 0, 120}};
static bool visible[2] = {true, true};
static int selected = -1, action = 0, depth = 0;
static void invoke(uintptr_t self);
static void nativeAttention(uintptr_t self) {
    ++nativeCalls;
    const uintptr_t root = at<uintptr_t>(self, field::behaviour::character);
    const float* g = (const float*)(root + field::character::gaze);
    memcpy(seenGaze, g, sizeof seenGaze);
    // BEAC0: range <= 500, score (500-distance)*(dot(direction,gaze)+0.5),
    // positive scores only, strictly greater wins. Eligibility/visibility remain
    // inputs to this spy; the production adapter must not override their result.
    float best = 0;
    selected = -1;
    for (int i = 0; i < 2; ++i) {
        const float* d = candidates[i];
        float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!visible[i] || n > 500 || n <= 0) continue;
        float score = (500 - n) * ((d[0] * g[0] + d[1] * g[1] + d[2] * g[2]) / n + .5f);
        if (score > best) {
            best = score;
            selected = i;
        }
    }
    // Only the native spy owns target publication, mirroring BEAC0.
    at<uintptr_t>(root, 0xe38) = selected < 0 ? 0 : attentionOther + selected * 0x100;
    at<uintptr_t>(root, field::character::contextualTarget) = selected < 0 ? 0 : otherBh;
    if (depth) return;
    if (action == 1) RaiseException(0xe0420051, 0, 0, nullptr);
    if (action == 2) {
        const float changed[3] = {1, 0, 0};
        memcpy((void*)(root + field::character::gaze), changed, 12);
    }
    if (action == 3) {
        ++depth;
        invoke(self);
        --depth;
    }
    if (action == 4) {
        ++depth;
        invoke(otherBh);
        --depth;
        check(vectorNear(seenGaze, looked), "nested other actor retains native gaze");
    }
    if (action == 5) {
        viewMode = 2;
        stopping = 1;
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = attentionOther;
    }
}
static void invoke(uintptr_t self) {
#if FP_REVISION >= 17
    combatAttention(self);
#else
    nativeAttention(self);
#endif
}
static bool faults() {
    __try {
        invoke(bh);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static void stamp() {
#if FP_REVISION >= 17
    aim.attentionStamp = {
        actor, head, scene, viewMode.load(), inputOwner.generation(), inputOwner.policyRevision()};
#endif
}
static void reset(int mode = 0) {
    memset((void*)(base + 0x600000), 0, 0x200000);
    fixture::resetInput(mode);
    inputClock = ~0u;
    foreground = true;
    fixture::ControlledPlayer{actor, bh, head, base + 0x760000}.connect();
    fixture::ControlledPlayer{attentionOther, otherBh, otherHead, base + 0x770000}.connect(false);
    *(uintptr_t*)gameAt(fp::sig::currentScene) = scene;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = gui;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = drag;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(unsigned*)gameAt(fp::sig::gameClock) = 100;
    at<unsigned char>(actor, field::character::combatStance) = 1;
    at<unsigned char>(attentionOther, field::character::combatStance) = 1;
    at<int>(actor, field::character::motion) = 7;
    at<float>(actor, field::character::motionProgress) = .37f;
    for (auto root : {actor, attentionOther}) {
        memcpy((void*)(root + field::character::gaze), looked, 12);
        memcpy((void*)(root + field::character::facing), looked, 12);
        memcpy((void*)(root + 0xa08), looked, 12); // native defensive forward, deliberately not adapted
    }
    float* pose = (float*)(head + field::bodyPart::pose);
    for (int i = 0; i < 12; ++i) pose[i] = i == 0 || i == 4 || i == 8 ? 1.f : 0.f;
    aim.valid = true;
    aim.root = actor;
    aim.head = head;
    aim.revision = 1;
    aim.rayValid = true;
    aim.rayYaw = atan2f(aimed[0], aimed[2]);
    aim.origin[0] = 1000;
    aim.origin[1] = 170;
    aim.origin[2] = -400;
    for (int i = 0; i < 3; ++i) aim.point[i] = aim.origin[i] + 1000 * aimed[i];
    stamp();
    selected = -1;
    action = depth = 0;
    nativeCalls = 0;
    visible[0] = visible[1] = true;
    candidates[0][0] = 0;
    candidates[0][1] = 0;
    candidates[0][2] = 150;
    candidates[1][0] = 90;
    candidates[1][1] = 0;
    candidates[1][2] = 120;
#if FP_REVISION >= 17
    originalCombatAttention = nativeAttention;
#endif
}
static void preserved() {
    check(vectorNear((float*)(actor + field::character::gaze), looked), "head-look gaze restored");
    check(vectorNear((float*)(actor + field::character::facing), looked), "body facing untouched");
    check(vectorNear((float*)(actor + 0xa08), looked), "native defensive forward untouched");
    check(at<int>(actor, field::character::motion) == 7 &&
              at<float>(actor, field::character::motionProgress) == .37f,
          "accepted attack and recovery untouched");
}
static void exempt(int mode, int kind) {
    reset(mode);
    switch (kind) {
    case 0:
        active = 0;
        break;
    case 1:
        stopping = 1;
        break;
    case 2:
        cameraReady = 0;
        break;
    case 3:
        inputEnabled = 0;
        break;
    case 4:
        cursorFree = 1;
        break;
    case 5:
        foreground = false;
        break;
    case 6:
        *(int*)gameAt(fp::sig::uiOpenCount) = 1;
        break;
    case 7:
        *(uintptr_t*)gameAt(fp::sig::uiView) = base + 0x780000;
        break;
    case 8:
        at<unsigned char>(actor, field::character::combatStance) = 0;
        break;
    case 9:
        at<unsigned>(actor, field::character::lifeFlags) = 0x100;
        break;
    case 10:
        at<unsigned>(actor, field::character::stateFlags) = 0x20;
        break;
    case 11:
        at<uintptr_t>(actor, field::character::actionCallback) = 1;
        break;
    case 12:
        at<unsigned>(drag, field::drag::flags) = 0x2000;
        break;
    case 13:
        at<unsigned>(gui, field::gui::dragFormat) = 1;
        break;
    case 14:
        at<unsigned char>(bh, field::behaviour::interactionLatch) = 1;
        break;
    case 15:
        at<unsigned char>(bh, field::behaviour::pressBlocked) = 1;
        break;
    case 16:
        aim.valid = false;
        break;
    case 17:
        aim.head = otherHead;
        break;
    case 18:
        *(uintptr_t*)gameAt(fp::sig::currentScene) = scene + 16;
        break;
    case 19:
        viewMode = 1 - mode;
        break;
    case 20:
        inputOwner.releaseGameplay();
        break;
    case 21:
        aim.revision = 0;
        break;
    case 22:
        memcpy(aim.point, aim.origin, sizeof aim.point);
        break;
    case 23:
        aim.point[1] = std::numeric_limits<float>::quiet_NaN();
        break;
    case 24:
        aim.origin[2] = std::numeric_limits<float>::infinity();
        break;
    case 25:
        inputOwner.policyChanged();
        break;
    case 26:
        at<uintptr_t>(actor, field::character::behaviour) = otherBh;
        break;
    }
    invoke(bh);
    check(nativeCalls == 1 && vectorNear(seenGaze, looked) && selected == 0,
          "excluded/stale sample retains native scoring");
    preserved();
}
int main() {
    fixture::NativeImage image(0x800000);
    if (!image) return 2;
    actor = image.object(0x600000);
    bh = image.object(0x610000);
    head = image.object(0x620000);
    attentionOther = image.object(0x630000);
    otherBh = image.object(0x640000);
    otherHead = image.object(0x650000);
    gui = image.object(0x660000);
    drag = image.object(0x670000);
    scene = image.object(0x680000);
    for (int mode = 0; mode < 2; ++mode) {
        for (int manual = 0; manual < 2; ++manual) {
            reset(mode);
            fpManualGuard = tpManualGuard = manual != 0;
            manualDefenseReady = manual != 0;
            float pose[12];
            memcpy(pose, (void*)(head + field::bodyPart::pose), sizeof pose);
            invoke(bh);
            check(nativeCalls == 1 && vectorNear(seenGaze, aimed),
                  "selector receives current aim in both defense modes");
            check(selected == 1 && at<uintptr_t>(actor, 0xe38) == attentionOther + 0x100,
                  "off-centre aimed opponent wins native scoring over head-centred opponent");
            check(!memcmp(pose, (void*)(head + field::bodyPart::pose), sizeof pose),
                  "physical head pose unchanged");
            preserved();
        }
        for (int kind = 0; kind < 27; ++kind) exempt(mode, kind);
        reset(mode);
        aim.point[1] += 750;
        invoke(bh);
        const float pitched[3] = {.48f, .6f, .64f};
        check(vectorNear(seenGaze, pitched),
              "attention preserves vertical aim rather than flattening to body heading");
        preserved();
        reset(mode);
        visible[1] = false;
        invoke(bh);
        check(selected == 0, "aim does not override native visibility/eligibility");
        reset(mode);
        candidates[1][0] *= 5;
        candidates[1][2] *= 5;
        invoke(bh);
        check(selected == 0, "aim does not override native range limit");
        reset(mode);
        visible[0] = visible[1] = false;
        invoke(bh);
        check(selected == -1 && !at<uintptr_t>(actor, 0xe38), "no eligible opponent remains no opponent");
        reset(mode);
        action = 1;
        check(faults(), "native fault propagates");
        preserved();
        reset(mode);
        action = 2;
        invoke(bh);
        const float changed[3] = {1, 0, 0};
        check(vectorNear((float*)(actor + field::character::gaze), changed), "later native gaze write wins");
        for (int nested : {3, 4, 5}) {
            reset(mode);
            action = nested;
            invoke(bh);
            preserved();
            check(nativeCalls == (nested == 5 ? 1u : 2u),
                  "native call count preserved through nesting/handoff");
        }
        reset(mode);
        invoke(otherBh);
        check(vectorNear(seenGaze, looked), "uncontrolled actor remains native");
        preserved();
        reset(mode);
        updateAim(bh);
#if FP_REVISION >= 17
        check(aim.attentionStamp.root == actor && aim.attentionStamp.scene == scene &&
                  aim.attentionStamp.mode == mode && aim.attentionStamp.generation == inputOwner.generation(),
              "production input owner publishes attention identity");
#endif
        invoke(bh);
        check(nativeCalls == 1, "production aim publication reaches native selector");
    }
    reset(2);
    invoke(bh);
    check(vectorNear(seenGaze, looked) && selected == 0, "overhead including active blend remains native");
    preserved();
    check(image.close(), "owned memory released");
    printf("Combat attention checks: %u; failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
