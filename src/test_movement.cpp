// Production combat-step ownership with synthetic native memory and call
// boundaries. This establishes request construction/restoration, not physical
// stopping distance, collision outcomes or live control feel.
#ifdef NDEBUG
#error "The tests check behavior; build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool movementForeground = true;
static HWND movementWindow() {
    return movementForeground ? (HWND)1 : nullptr;
}
static DWORD movementWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow movementWindow
#define GetWindowThreadProcessId movementWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId

static unsigned checks = 0, failures = 0, nativeCalls = 0;
static uintptr_t fixtureRoot = 0, fixtureBehavior = 0, fixtureHead = 0, fixtureView = 0;
static unsigned observedPending = 0, observedKeys = 0, observedHover = 0;
static float observedFacing[3]{};
static int nativeAction = 0;
static const float originalFacing[3] = {0.25f, 0.9f, -0.35f};
static const float replacementFacing[3] = {-0.6f, 0.2f, 0.3f};

static void check(bool ok, const char* why) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL: %s\n", why);
    }
}
static bool sameFacing(const float* expected) {
    return !memcmp((void*)(fixtureRoot + field::character::facing), expected, 12);
}
static bool sawLook(float yaw) {
    return std::isfinite(observedFacing[0]) && std::isfinite(observedFacing[2]) && observedFacing[1] == 0 &&
           std::fabs(observedFacing[0] - std::sin(yaw)) < 1e-6f &&
           std::fabs(observedFacing[2] - std::cos(yaw)) < 1e-6f &&
           std::fabs(observedFacing[0] * observedFacing[0] + observedFacing[2] * observedFacing[2] - 1.f) <
               1e-6f;
}
static void nativeMovement(uintptr_t bh) {
    ++nativeCalls;
    const uintptr_t root = *(uintptr_t*)(bh + field::behaviour::character);
    observedPending = *(unsigned*)(bh + field::behaviour::pendingMove);
    observedKeys = *(unsigned*)gameAt(fp::sig::keyWord);
    observedHover = *(unsigned*)(bh + field::behaviour::hoverState);
    memcpy(observedFacing, (void*)(root + field::character::facing), 12);
    if (nativeAction == 1) RaiseException(0xE0424353, 0, 0, nullptr);
    if (nativeAction == 2) {
        memcpy((void*)(root + field::character::facing), replacementFacing, 12);
        *(unsigned*)(bh + field::behaviour::hoverState) = 7;
    }
    if (nativeAction == 3) stopping = 1;
    if (nativeAction == 4) *(uintptr_t*)gameAt(fp::sig::playerRoot) = root + 0x10000;
}
static bool stepFaults(uintptr_t bh) {
    __try {
        step(bh);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static void resetMovement() {
    memset((void*)fixtureRoot, 0, 0x1400);
    memset((void*)fixtureBehavior, 0, 0xA00);
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = fixtureRoot;
    *(uintptr_t*)(fixtureRoot + field::character::behaviour) = fixtureBehavior;
    *(uintptr_t*)(fixtureRoot + field::character::headPart) = fixtureHead;
    *(uintptr_t*)(fixtureBehavior + field::behaviour::character) = fixtureRoot;
    *(uintptr_t*)(fixtureBehavior + field::behaviour::view) = fixtureView;
    *(uintptr_t*)gameAt(fp::sig::uiView) = fixtureView;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = 0;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(unsigned char*)(fixtureRoot + field::character::combatStance) = 1;
    *(unsigned*)(fixtureBehavior + field::behaviour::hoverState) = 4;
    *(unsigned*)(fixtureBehavior + field::behaviour::pendingMove) = 4;
    *(unsigned*)gameAt(fp::sig::keyWord) = 1;
    memset((void*)gameAt(fp::sig::analogStick), 0, 12);
    memcpy((void*)(fixtureRoot + field::character::facing), originalFacing, 12);
    const float gaze[3] = {0.8f, -0.6f, 0}, movement[3] = {0, 0, -1};
    memcpy((void*)(fixtureRoot + field::character::gaze), gaze, 12);
    memcpy((void*)(fixtureRoot + field::character::movementFacing), movement, 12);
    *(float*)(fixtureRoot + field::character::bodyHeading) = -0.2f;
    // Native execution/queue sentinels. These are archive fields, deliberately
    // not new production layout targets introduced by this test.
    *(unsigned*)(fixtureRoot + 0xF70) = 733;
    *(float*)(fixtureRoot + 0xFD0) = 110.f;
    *(float*)(fixtureRoot + 0xFD4) = 90.f;
    *(unsigned*)(fixtureRoot + 0xFDC) = 2;
    *(int*)(fixtureRoot + 0xFEC) = 1;
    *(unsigned char*)(fixtureRoot + 0x12EE) = 1;
    *(unsigned char*)(fixtureRoot + 0x12EF) = 1;
    const float target[3] = {30.f, 40.f, 50.f}, correction[3] = {-12.f, 0.f, 28.f};
    memcpy((void*)(fixtureRoot + field::character::travelTarget), target, 12);
    memcpy((void*)(fixtureRoot + 0xFE0), correction, 12);
    *(unsigned*)(fixtureBehavior + 0x194) = 9;
    *(unsigned*)(fixtureBehavior + 0x198) = 2;
    *(unsigned*)(fixtureBehavior + 0x8FC) = 1;
    *(unsigned*)(fixtureBehavior + 0x900) = 222;
    *(unsigned*)(fixtureBehavior + 0x904) = 333;
    *(unsigned*)(fixtureBehavior + 0x908) = 444;
    float* pose = (float*)(fixtureHead + field::bodyPart::pose);
    memset(pose, 0, 48);
    fp::basis(0, 0, pose);
    active = cameraReady = inputEnabled = 1;
    stopping = cursorFree = rmbFree = windowFree = 0;
    viewMode = 0;
    windowsFreeze = true;
    movementForeground = true;
    nativeAction = 0;
    aim = {};
    aim.root = fixtureRoot;
    aim.head = fixtureHead;
    aim.valid = true;
    aim.yaw = 0.7f;
    aim.pitch = 1.4f;
    aim.rayYaw = -2.f;
    aim.rayValid = true;
    aim.point[0] = -400.f;
    aim.point[1] = 700.f;
    aim.point[2] = -30.f;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    fixtureRoot = base + 0x5C0000;
    fixtureBehavior = base + 0x5C2000;
    fixtureHead = base + 0x5C4000;
    fixtureView = base + 0x5C6000;
    originalStep = nativeMovement;
    unsigned char savedActor[0x1400], savedHistory[0x18];

    // Expected native opposition cancellation, listed independently of the
    // production implementation: F+B and L+R each remove only their own axis.
    const unsigned effective[16] = {0, 1, 2, 3, 4, 0, 6, 2, 8, 9, 0, 1, 12, 8, 4, 0};
    for (int mode : {0, 1}) {
        for (unsigned mask = 0; mask < 16; ++mask) {
            resetMovement();
            viewMode = mode;
            *(unsigned*)gameAt(fp::sig::keyWord) = mask | 0xB0;
            memcpy(savedActor, (void*)fixtureRoot, sizeof savedActor);
            memcpy(savedHistory, (void*)(fixtureBehavior + 0x8FC), sizeof savedHistory);
            const unsigned before = nativeCalls;
            step(fixtureBehavior);
            check(nativeCalls == before + 1 && observedPending == (effective[mask] ? 4u : 0u),
                  "all 16 masks: effective-neutral cancellation before exactly one native call");
            check(observedKeys == (mask | 0xB0) && *(unsigned*)gameAt(fp::sig::keyWord) == (mask | 0xB0),
                  "step adapter preserves native key word and non-movement actions");
            check(effective[mask] ? sawLook(aim.yaw) : !memcmp(observedFacing, originalFacing, 12),
                  "only a non-neutral digital request receives planar look yaw");
            check(!memcmp(savedActor, (void*)fixtureRoot, sizeof savedActor) &&
                      !memcmp(savedHistory, (void*)(fixtureBehavior + 0x8FC), sizeof savedHistory),
                  "request construction preserves actor physics and hold/repeat timestamps");
            check(observedHover == 0 && *(unsigned*)(fixtureBehavior + field::behaviour::hoverState) == 4,
                  "owned hover borrow is presented and restored");
        }
    }

    for (float yaw : {0.f, 1.5707963268f, -1.5707963268f, 3.1415926536f}) {
        resetMovement();
        viewMode = 1;
        aim.yaw = yaw;
        aim.rayYaw = yaw + 1.f;
        float eye[3], vertical[3];
        fp::eyePoint((float*)(fixtureHead + field::bodyPart::pose), eye, float(eyeSetback.load()));
        memcpy(aim.point, eye, 12);
        aim.point[1] += 200.f;
        gazeToPoint(aim, vertical);
        check(vertical[0] == 0 && vertical[1] == 1 && vertical[2] == 0,
              "production TP gaze can be vertical with a finite eye-to-target vector");
        // C1CF0 copies this gaze into facing before invoking the step owner.
        memcpy((void*)(fixtureRoot + field::character::facing), vertical, 12);
        step(fixtureBehavior);
        check(sawLook(yaw), "vertical TP gaze reaches the native step as finite unit planar look yaw");
        check(sameFacing(vertical), "vertical native facing is restored exactly after target construction");
    }

    for (float magnitude : {0.09f, 0.15f, 0.7f}) {
        resetMovement();
        *(float*)gameAt(fp::sig::analogStick) = magnitude;
        step(fixtureBehavior);
        check(magnitude < 0.1f ? sawLook(aim.yaw) : !memcmp(observedFacing, originalFacing, 12),
              "digital facing respects native step analog threshold, including magnitude 0.15");
        check(sameFacing(originalFacing) && *(float*)gameAt(fp::sig::analogStick) == magnitude,
              "analog source and final native facing remain intact");
    }
    resetMovement();
    *(unsigned*)gameAt(fp::sig::keyWord) = 0;
    *(unsigned*)(fixtureBehavior + field::behaviour::pendingMove) = 0x8000;
    step(fixtureBehavior);
    check(observedPending == 0x8000 && !memcmp(observedFacing, originalFacing, 12),
          "ordinary released controller request retains native buffering and direction");
    resetMovement();
    *(unsigned*)gameAt(fp::sig::keyWord) = 0;
    *(unsigned*)(fixtureBehavior + field::behaviour::pendingMove) = 0x8001;
    step(fixtureBehavior);
    check(observedPending == 0x8001, "unknown mixed request code is not treated as owned digital memory");
    for (float bad : {NAN, INFINITY}) {
        resetMovement();
        *(float*)gameAt(fp::sig::analogStick) = bad;
        step(fixtureBehavior);
        check(!memcmp(observedFacing, originalFacing, 12), "invalid native analog input stays native");
        resetMovement();
        aim.yaw = bad;
        step(fixtureBehavior);
        check(!memcmp(observedFacing, originalFacing, 12) && sameFacing(originalFacing),
              "invalid mod yaw cannot publish a non-finite facing");
    }

    // Actual ownership predicates, with no live Aim, across pointer, UI and
    // focus handoff. Only direction memory is cleared; the physical queue is
    // deliberately still native and the hover borrow is not made.
    for (int handoff = 0; handoff < 5; ++handoff) {
        for (unsigned pending : {4u, 0x8000u}) {
            resetMovement();
            viewMode = handoff & 1;
            aim.valid = false;
            *(unsigned*)gameAt(fp::sig::keyWord) = 0;
            *(unsigned*)(fixtureBehavior + field::behaviour::pendingMove) = pending;
            if (handoff == 0) cursorFree = 1;
            if (handoff == 1) rmbFree = 1;
            if (handoff == 2) windowFree = 1;
            if (handoff == 3) movementForeground = false;
            if (handoff == 4) *(int*)gameAt(fp::sig::uiOpenCount) = 1;
            memcpy(savedActor, (void*)fixtureRoot, sizeof savedActor);
            step(fixtureBehavior);
            check(observedPending == 0,
                  "frozen current-player input cancels digital/controller direction memory");
            check(observedHover == 4 && !memcmp(observedFacing, originalFacing, 12) &&
                      !memcmp(savedActor, (void*)fixtureRoot, sizeof savedActor),
                  "frozen step preserves hover, facing and accepted/correction physics without Aim");
        }
    }

    for (int unowned = 0; unowned < 7; ++unowned) {
        resetMovement();
        if (unowned == 0) viewMode = 2;
        if (unowned == 1) active = 0;
        if (unowned == 2) stopping = 1;
        if (unowned == 3) cameraReady = 0;
        if (unowned == 4) *(uintptr_t*)gameAt(fp::sig::playerRoot) = fixtureRoot + 0x10000;
        if (unowned == 5) aim.valid = false;
        if (unowned == 6) *(unsigned*)(fixtureRoot + field::character::lifeFlags) = 0x100;
        *(unsigned*)gameAt(fp::sig::keyWord) = 0;
        const unsigned before = nativeCalls;
        step(fixtureBehavior);
        check(nativeCalls == before + 1 && observedPending == 4 && observedHover == 4 &&
                  !memcmp(observedFacing, originalFacing, 12),
              "overhead/inactive/stopping/unready/foreign/invalid/dead contexts remain native");
    }
    resetMovement();
    *(unsigned char*)(fixtureRoot + field::character::combatStance) = 0;
    step(fixtureBehavior);
    check(!memcmp(observedFacing, originalFacing, 12),
          "unexpected relaxed step does not receive combat facing");

    resetMovement();
    nativeAction = 1;
    check(stepFaults(fixtureBehavior), "native step exception propagates");
    check(sawLook(aim.yaw) && sameFacing(originalFacing) &&
              *(unsigned*)(fixtureBehavior + field::behaviour::hoverState) == 4,
          "native exception restores exact facing and hover borrows");
    resetMovement();
    nativeAction = 2;
    step(fixtureBehavior);
    check(sameFacing(replacementFacing) && *(unsigned*)(fixtureBehavior + field::behaviour::hoverState) == 7,
          "native replacement facing and hover win over restoration");
    for (int action : {3, 4}) {
        resetMovement();
        nativeAction = action;
        step(fixtureBehavior);
        check(sameFacing(originalFacing) && *(unsigned*)(fixtureBehavior + field::behaviour::hoverState) == 4,
              "stop/player change inside native step restores old owned fields");
    }

    printf("Combat movement ownership: %u checks, %u failures, %u native calls.\n", checks, failures,
           nativeCalls);
    return failures ? 1 : 0;
}
