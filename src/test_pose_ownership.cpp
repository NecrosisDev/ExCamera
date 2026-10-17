// Actual pose/aim adapters with owned fake memory. No game process or solver.
#ifdef NDEBUG
#error "The tests check behavior; build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HWND testForeground() { return (HWND)1; }
static DWORD testWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId

static unsigned checks = 0, failures = 0;
static void check(bool ok, const char* why) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL: %s\n", why);
    }
}
static uintptr_t testRoot = 0, testChest = 0, testLower = 0;
static int nativeMode = 0, nativeCalls = 0;
static float seenQuat[2][4]{};
static unsigned seenFlags[2]{};
static uintptr_t seenContext = 0;
static HANDLE poseEntered = nullptr, poseContinue = nullptr;
static void nativePose(uintptr_t root, float, unsigned char) {
    ++nativeCalls;
    const uintptr_t parts[2] = {testChest, testLower};
    for (int i = 0; i < 2; ++i) {
        memcpy(seenQuat[i], (void*)(parts[i] + field::bodyPart::extraRotation), 16);
        seenFlags[i] = *(unsigned*)(parts[i] + field::bodyPart::flags);
    }
    seenContext = *(uintptr_t*)(root + field::character::contextualTarget);
    if (nativeMode == 1) InterlockedExchange(&stopping, 1);
    if (nativeMode == 2) {
        const float changed[4] = {0, 0, 0.3f, 0.9539392f};
        memcpy((void*)(testChest + field::bodyPart::extraRotation), changed, 16);
        *(unsigned*)(testChest + field::bodyPart::flags) |= 0x20000;
    }
    if (nativeMode == 3) *(uintptr_t*)gameAt(fp::sig::playerRoot) = root + 0x20000;
    if (nativeMode == 4) RaiseException(0xE0424242, 0, 0, nullptr);
    if (nativeMode == 5) *(unsigned*)(testChest + field::bodyPart::flags) |= 0x20000;
    if (nativeMode == 6) {
        SetEvent(poseEntered);
        WaitForSingleObject(poseContinue, 5000);
    }
}
static bool poseFaults(uintptr_t root) {
    __try {
        poseBuilder(root, 0, 0);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static void nativePoint(uintptr_t, float* out) { out[0] = 7; out[1] = 8; out[2] = 9; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    testRoot = base + 0x550000;
    const uintptr_t head = base + 0x552000, behaviorAddress = base + 0x554000,
                    view = base + 0x556000, pelvis = base + 0x558000;
    testChest = base + 0x55a000;
    testLower = base + 0x55c000;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = testRoot;
    *(uintptr_t*)(testRoot + field::character::headPart) = head;
    *(uintptr_t*)(testRoot + field::character::chestPart) = testChest;
    *(uintptr_t*)(testRoot + field::character::behaviour) = behaviorAddress;
    *(uintptr_t*)(behaviorAddress + field::behaviour::character) = testRoot;
    *(uintptr_t*)(behaviorAddress + field::behaviour::view) = view;
    *(unsigned char*)(testRoot + field::character::physicsLive) = 1;
    *(float*)(testRoot + field::character::balance) = 1;
    *(float*)(testRoot + field::character::balanceMax) = 1;
    const float rows[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (uintptr_t p : {head, testChest, testLower, pelvis}) {
        memcpy((void*)(p + field::bodyPart::pose), rows, 48);
        *(uintptr_t*)(p + field::bodyPart::character) = testRoot;
        *(uintptr_t*)(p + field::bodyPart::rootPart) = pelvis;
    }
    *(uintptr_t*)(testChest + field::bodyPart::parentPart) = testLower;
    *(uintptr_t*)(testLower + field::bodyPart::parentPart) = pelvis;
    *(float*)(testRoot + field::character::gaze + 4) = -0.7f;
    *(float*)(testRoot + field::character::gaze + 8) = 0.7141428f;
    originalPose = nativePose;
    originalPoint = nativePoint;
    headSteady = 0;
    spineFollow = 60;
    active = cameraReady = 1;
    stopping = 0;
    aim = {};
    const float original[4] = {0.11f, -0.07f, 0.03f, 0.9905049f};
    auto reset = [&](unsigned flag = 0x81) {
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = testRoot;
        *(unsigned*)(testRoot + field::character::stateFlags) = 0;
        viewMode = 0;
        stopping = 0;
        nativeMode = 0;
        for (uintptr_t p : {testChest, testLower}) {
            memcpy((void*)(p + field::bodyPart::extraRotation), original, 16);
            *(unsigned*)(p + field::bodyPart::flags) = flag;
        }
    };
    auto restoredParts = [&](unsigned flags = 0x81) {
        bool same = true;
        for (uintptr_t p : {testChest, testLower})
            same = same && !memcmp((void*)(p + field::bodyPart::extraRotation), original, 16) &&
                   *(unsigned*)(p + field::bodyPart::flags) == flags;
        return same;
    };
    for (int mode : {0, 1}) {
        reset();
        viewMode = mode;
        poseBuilder(testRoot, 0, 0);
        check((seenFlags[0] & 0x4000) && seenQuat[0][0] > 0 && seenQuat[1][0] > 0 &&
                  seenQuat[1][0] < seenQuat[0][0], "native pose consumes split spine bend in both close views");
        check(restoredParts(), "native quaternion bytes and original flag restored after pose returns");
    }
    reset(0x4081);
    poseBuilder(testRoot, 0, 0);
    check(std::fabs(seenQuat[0][2]) > 0.001f, "pre-existing native extra rotation is composed, not erased");
    check(restoredParts(0x4081), "pre-existing enabled rotation survives scoped borrow exactly");
    reset();
    nativeMode = 1;
    poseBuilder(testRoot, 0, 0);
    check(stopping && restoredParts(), "stop inside native build cannot strand a spine write");
    reset();
    nativeMode = 3;
    poseBuilder(testRoot, 0, 0);
    check(restoredParts(), "controlled-player switch inside build restores still-owned old parts");
    reset();
    nativeMode = 4;
    check(poseFaults(testRoot), "native pose exception propagates to the caller");
    check(restoredParts(), "native exception restores both spine borrows");
    reset();
    nativeMode = 5;
    poseBuilder(testRoot, 0, 0);
    check(!memcmp((void*)(testChest + field::bodyPart::extraRotation), original, 16) &&
              *(unsigned*)(testChest + field::bodyPart::flags) == 0x20081,
          "restoration preserves unrelated flag bits written by native build");
    reset();
    nativeMode = 2;
    poseBuilder(testRoot, 0, 0);
    check(*(float*)(testChest + field::bodyPart::extraRotation + 8) == 0.3f &&
              (*(unsigned*)(testChest + field::bodyPart::flags) & 0x20000),
          "native replacement quaternion is not overwritten on return");
    for (int state : {0, 1, 2}) {
        reset();
        if (state == 0) stopping = 1;
        if (state == 1) viewMode = 2;
        if (state == 2) *(unsigned*)(testRoot + field::character::stateFlags) = 0x200;
        poseBuilder(testRoot, 0, 0);
        check(!(seenFlags[0] & 0x4000) && restoredParts(), "stopped, overhead and knockdown builds stay native");
    }
    reset(0x4081);
    *(float*)(testChest + field::bodyPart::extraRotation) = NAN;
    poseBuilder(testRoot, 0, 0);
    check(std::isnan(seenQuat[0][0]) &&
              std::isnan(*(float*)(testChest + field::bodyPart::extraRotation)),
          "invalid native extra rotation remains native instead of becoming mod-owned identity");

    reset();
    aim.root = testRoot;
    aim.head = head;
    aim.valid = true;
    aim.pitch = -0.7f;
    aim.point[0] = 123;
    const uintptr_t context = testRoot + 0x400;
    *(uintptr_t*)(testRoot + field::character::contextualTarget) = context;
    nativeMode = 4;
    check(poseFaults(testRoot) && seenContext == 0, "owned aim temporarily removes contextual target");
    check(*(uintptr_t*)(testRoot + field::character::contextualTarget) == context && restoredParts(),
          "pose fault also restores contextual target");
    nativeMode = 0;
    Aim observed;
    for (int mode : {0, 1}) {
        viewMode = mode;
        cursorFree = 1;
        check(heldAim(behaviorAddress, observed), "free-pointer aim remains held in close views");
    }
    for (int handoff : {0, 1, 2}) {
        viewMode = 2;
        cursorFree = handoff == 0;
        *(int*)gameAt(fp::sig::uiOpenCount) = handoff == 1 ? 1 : 0;
        *(uintptr_t*)gameAt(fp::sig::uiView) = handoff == 2 ? view + 1 : 0;
        check(!heldAim(behaviorAddress, observed), "overhead blend cannot reclaim held aim through UI/free-pointer path");
        float out[3]{};
        point(behaviorAddress, out);
        check(out[0] == 7 && out[1] == 8 && out[2] == 9, "overhead point adapter calls original through every handoff");
    }
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(uintptr_t*)gameAt(fp::sig::uiView) = 0;
    viewMode = 0;
    cursorFree = 1;
    active = 0;
    check(!heldAim(behaviorAddress, observed), "settled inactive overhead cannot reclaim held aim");

#ifndef EXCAMERA_PRE_REVIEW
    // Hold the real native-build call open on another owned thread. Teardown must
    // time out while it borrows fields, then succeed after its finally restores.
    active = 1;
    cursorFree = 0;
    aim.valid = false;
    reset();
    poseEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    poseContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    nativeMode = 6;
    HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { poseBuilder(testRoot, 0, 0); return 0; },
                                 nullptr, 0, nullptr);
    check(thread && WaitForSingleObject(poseEntered, 2000) == WAIT_OBJECT_0, "fixture reaches in-flight native pose");
    InterlockedExchange(&stopping, 1);
    check(!awaitPoseBuilders(20), "teardown cannot report completion while borrowed physics fields are in flight");
    SetEvent(poseContinue);
    check(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0 && awaitPoseBuilders(20) && restoredParts(),
          "teardown completes after in-flight pose restores exact native fields");
    CloseHandle(thread);
    CloseHandle(poseEntered);
    CloseHandle(poseContinue);
#endif
    printf("Pose/aim ownership: %u checks, %u failures (%d native pose calls).\n", checks, failures, nativeCalls);
    return failures ? 1 : 0;
}
