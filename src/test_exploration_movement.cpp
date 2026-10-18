// Offline owner regressions. The production hooks run against this process's
// allocated fixture memory and native-call spies; no game is opened or injected.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HWND fixtureForeground() {
    return (HWND)1;
}
static DWORD fixtureWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return GetCurrentThreadId();
}
#define GetForegroundWindow fixtureForeground
#define GetWindowThreadProcessId fixtureWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#include <vector>

static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
}
static bool sameVector(const float* a, const float* b) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > 1e-5f) return false;
    return true;
}
static int analogCalls = 0;
static void nativeAnalogScale(float* out, const float* axis, float amount) {
    ++analogCalls;
    for (int i = 0; i < 3; ++i) out[i] = axis[i] * amount;
}
static void nativeAnalogAdd(float* out, const float* axis, float amount) {
    ++analogCalls;
    for (int i = 0; i < 3; ++i) out[i] += axis[i] * amount;
}
enum class ExploreCall { ordinary, fault, replaceInput, replaceFlags, replaceAnalog, replaceOwner, nested };
static ExploreCall exploreCall = ExploreCall::ordinary;
static int exploreCalls = 0, exploreDepth = 0;
static uintptr_t replacementRoot = 0;
static unsigned seenKeys = 0;
static unsigned char seenIntent = 0;
static float seenAnalog[3]{}, observedAxis[3]{}, observedDuck = 0.f;
static bool nestedScopeRestored = false;
static void nativeExploreFixture(uintptr_t bh) {
    ++exploreCalls;
    auto keys = (unsigned*)gameAt(fp::sig::keyWord);
    auto analog = (float*)gameAt(fp::sig::analogStick);
    auto intent = (unsigned char*)(bh + field::behaviour::moveIntent);
    seenKeys = *keys;
    seenIntent = *intent;
    memcpy(seenAnalog, analog, sizeof seenAnalog);
    switch (exploreCall) {
    case ExploreCall::fault:
        RaiseException(0xe0420010, 0, 0, nullptr);
        break;
    case ExploreCall::replaceInput:
        *keys = 0xa8;
        analog[0] = 0.6f;
        analog[1] = 0.f;
        analog[2] = -0.8f;
        *intent = 1;
        break;
    case ExploreCall::replaceFlags:
        *keys = 0x20; // native changes nonmovement flags while the movement borrow is still zero
        break;
    case ExploreCall::replaceAnalog:
        analog[0] = 0.8f;
        analog[1] = 0.f;
        analog[2] = 0.6f;
        *intent = 1;
        break;
    case ExploreCall::replaceOwner:
        *(uintptr_t*)(bh + field::behaviour::character) = replacementRoot;
        *intent = 0;
        break;
    case ExploreCall::nested:
        if (!exploreDepth) {
            const uintptr_t outerRoot = exploringRoot;
            float outerDirection[3];
            memcpy(outerDirection, exploringDirection, sizeof outerDirection);
            ++exploreDepth;
            *keys = 8;
            explore(bh);
            --exploreDepth;
            nestedScopeRestored =
                exploringRoot == outerRoot && sameVector(exploringDirection, outerDirection);
        }
        break;
    default:
        break;
    }
}
static void nativeGaitFixture(uintptr_t, const float* axis) {
    memcpy(observedAxis, axis, sizeof observedAxis);
}
static void nativePoseFixture(uintptr_t root, float, unsigned char) {
    observedDuck = *(float*)(root + field::character::duckAmount);
}
static bool exploreFault(uintptr_t bh) {
    __try {
        explore(bh);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static bool ceiling = false, rayFault = false;
static unsigned ceilingQueries = 0;
static bool nativeCeilingFixture(uintptr_t, const float*, const float* up, float length) {
    ++ceilingQueries;
    check(up[0] == 0.f && up[1] == 1.f && up[2] == 0.f && length == crouchHeadroom,
          "headroom owner retains its vertical level query");
    if (rayFault) RaiseException(0xe0420011, 0, 0, nullptr);
    return ceiling;
}
static void clearCrouch(uintptr_t root) {
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;
    *(unsigned char*)(root + field::character::combatStance) = 0;
    *(unsigned*)(root + field::character::stateFlags) = 0;
    *(unsigned*)(root + field::character::lifeFlags) = 0;
    *(unsigned char*)gameAt(fp::sig::dialogueActive) = 0;
    active = 1;
    stopping = 0;
    viewMode = 0;
    windowFree = 0;
    crouchMode = 0;
    duckKey = false;
    crouchFrame();
    crouchMode = 2;
}
static void armCrouch(uintptr_t root) {
    clearCrouch(root);
    duckKey = true;
    crouchFrame();
    duckKey = false;
    crouchFrame();
}
static bool poseKeepsNativeDuck(uintptr_t root) {
    *(float*)(root + field::character::duckAmount) = 0.25f;
    poseBuilderOwned(root, 0.f, 0);
    return observedDuck == 0.25f;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x800000);
    base = (uintptr_t)storage.data();
    const uintptr_t root = base + 0x610000, bh = base + 0x620000, head = base + 0x630000,
                    replacement = base + 0x640000, otherHead = base + 0x650000, scene = base + 0x660000,
                    otherScene = base + 0x670000;
    replacementRoot = replacement;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = scene;
    *(uintptr_t*)(root + field::character::behaviour) = bh;
    *(uintptr_t*)(root + field::character::headPart) = head;
    *(uintptr_t*)(replacement + field::character::headPart) = otherHead;
    *(uintptr_t*)(bh + field::behaviour::character) = root;
    active = 1;
    stopping = 0;
    cameraReady = 1;
    inputEnabled = 1;
    cursorFree = rmbFree = windowFree = 0;
    viewMode = 0;
    aim = {};
    aim.root = root;
    aim.head = head;
    aim.valid = true;
    originalExplore = nativeExploreFixture;
    originalWalkGait = nativeGaitFixture;
    originalPose = nativePoseFixture;
    auto nativeAnalog = (float*)gameAt(fp::sig::analogStick);
    auto nativeKeys = (unsigned*)gameAt(fp::sig::keyWord);
    auto moveIntent = (unsigned char*)(bh + field::behaviour::moveIntent);
    auto walking = (unsigned char*)(root + field::character::walking);
    const float nativeDirection[3] = {-1.f, 0.f, 0.f};
    const float forward[3] = {0.f, 0.f, 1.f};
    const float zero[3]{};

    originalAnalogScale = nativeAnalogScale;
    originalAnalogAdd = nativeAnalogAdd;
    for (int mode : {0, 1}) {
        viewMode = mode;
        for (float yaw : {0.f, 1.57079632679f, -0.7f}) {
            aim.yaw = yaw;
            for (int provider : {0, 1}) {
                const uintptr_t rightReturn =
                    gameAt(provider ? fp::sig::xinputRightReturn : fp::sig::controllerRightReturn);
                const uintptr_t forwardReturn =
                    gameAt(provider ? fp::sig::xinputForwardReturn : fp::sig::controllerForwardReturn);
                for (float pitch : {-1.f, 0.f, 1.f}) {
                    float pose[12];
                    fp::basis(yaw, pitch, pose);
                    pose[4] = 0.f;
                    const float expected[3] = {-std::cos(yaw) * 0.3f + std::sin(yaw) * 0.4f, 0.f,
                                               std::sin(yaw) * 0.3f + std::cos(yaw) * 0.4f};
                    const int calls = analogCalls;
                    analogVectorFor(rightReturn, false, nativeAnalog, pose, 0.3f);
                    analogVectorFor(forwardReturn, true, nativeAnalog, pose + 3, 0.4f);
                    check(sameVector(nativeAnalog, expected) && analogCalls == calls + 2,
                          "both native providers retain stick magnitude and yaw at down/level/up pitch");
                }
                float other[3];
                analogVectorFor(rightReturn, false, other, nativeDirection, 1.f);
                check(sameVector(other, nativeDirection), "nonmovement destination remains native");
                analogVectorFor(0, false, nativeAnalog, nativeDirection, 1.f);
                check(sameVector(nativeAnalog, nativeDirection), "unrelated math caller remains native");
                for (int gate = 0; gate < 4; ++gate) {
                    viewMode = gate == 0 ? 2 : mode;
                    cursorFree = gate == 1;
                    windowFree = gate == 2;
                    if (gate == 3) aim.root = replacement;
                    analogVectorFor(rightReturn, false, nativeAnalog, nativeDirection, 1.f);
                    check(sameVector(nativeAnalog, nativeDirection),
                          "overhead/UI/stale controller owner remains native");
                    aim.root = root;
                    cursorFree = windowFree = 0;
                    viewMode = mode;
                }
            }
        }
    }
    aim.yaw = 0.f;

    for (int view : {0, 1}) {
        viewMode = view;
        *walking = 1;
        *nativeKeys = 1;
        memcpy(nativeAnalog, zero, sizeof zero);
        explore(bh); // establish the last digital publication through its actual owner
        check(aim.travelValid && sameVector(aim.travel, forward),
              "exploration publishes a camera-relative digital direction");
        *nativeKeys = 0;
        memcpy(nativeAnalog, nativeDirection, sizeof nativeDirection);
        explore(bh);
        check(sameVector(seenAnalog, nativeDirection) && sameVector(nativeAnalog, nativeDirection),
              "native analog vector survives the keyboard-to-controller handoff");
        walkGait(root, nativeDirection);
        check(sameVector(observedAxis, nativeDirection),
              view ? "TP gait relinquishes the old digital direction to the controller"
                   : "FP gait relinquishes the old digital direction to the controller");

        // Neutral releases retain the previous axis only for a native walk that
        // is still braking. They must not create another directional request.
        *nativeKeys = 1;
        memcpy(nativeAnalog, zero, sizeof zero);
        explore(bh);
        *nativeKeys = 0;
        explore(bh);
        check(aim.travelValid && seenIntent == 0 && sameVector(seenAnalog, zero),
              "neutral input asks native exploration to brake without erasing its gait axis");
        walkGait(root, nativeDirection);
        check(sameVector(observedAxis, forward), "the existing digital gait survives native braking");
        *walking = 0;
        explore(bh);
        walkGait(root, nativeDirection);
        check(!aim.travelValid && sameVector(observedAxis, nativeDirection),
              "completed native braking releases the digital gait publication");
    }

    // Native backward gait already supplies a negative thrust. The adapter
    // must preserve that sign contract and leave gait flags owned by native code.
    viewMode = 0;
    *walking = 1;
    *nativeKeys = 4;
    memcpy(nativeAnalog, zero, sizeof zero);
    *(unsigned*)(root + field::character::gaitFlags) = 0x212;
    explore(bh);
    walkGait(root, nativeDirection);
    check(sameVector(observedAxis, forward) && *(unsigned*)(root + field::character::gaitFlags) == 0x212,
          "backward digital intent retains native negative-thrust and brake-before-reverse flags");
    *(unsigned char*)(root + field::character::combatStance) = 1;
    walkGait(root, nativeDirection);
    check(sameVector(observedAxis, nativeDirection), "combat gait keeps the native axis");
    *(unsigned char*)(root + field::character::combatStance) = 0;
    *(unsigned*)(root + field::character::gaitFlags) = 0;
    const uintptr_t previousPlayer = base + 0x6e0000;
    *(uintptr_t*)(previousPlayer + field::character::behaviour) = bh;
    *(unsigned char*)(previousPlayer + field::character::walking) = 1;
    walkGait(previousPlayer, nativeDirection);
    check(sameVector(observedAxis, nativeDirection),
          "a reused behavior pointer cannot apply today's player gait to an older character");

    const float oldAnalog[3] = {0.25f, 0.f, 0.4f};
    const float oldDirection[3] = {0.2f, 0.f, 0.7f};
    const uintptr_t oldExploring = base + 0x680000;
    *nativeKeys = 0x41;
    memcpy(nativeAnalog, oldAnalog, sizeof oldAnalog);
    *moveIntent = 1;
    exploringRoot = oldExploring;
    memcpy(exploringDirection, oldDirection, sizeof oldDirection);
    exploreCall = ExploreCall::fault;
    check(exploreFault(bh), "native exploration exceptions reach the caller");
    check(*nativeKeys == 0x41 && *moveIntent == 1 && sameVector(nativeAnalog, oldAnalog) &&
              exploringRoot == oldExploring && sameVector(exploringDirection, oldDirection),
          "exception unwinding restores still-borrowed input and the prior travel scope");

    exploreCall = ExploreCall::replaceFlags;
    explore(bh);
    check(*nativeKeys == 0x21, "borrow restoration preserves native nonmovement flag changes");
    *nativeKeys = 0x41;
    *moveIntent = 0;
    exploreCall = ExploreCall::replaceInput;
    explore(bh);
    const float replacementAnalog[3] = {0.6f, 0.f, -0.8f};
    check(*nativeKeys == 0xa8 && *moveIntent == 1 && sameVector(nativeAnalog, replacementAnalog),
          "native replacement movement, analog and move-intent publications survive return");
    *nativeKeys = 0;
    memcpy(nativeAnalog, nativeDirection, sizeof nativeDirection);
    exploreCall = ExploreCall::replaceAnalog;
    explore(bh);
    const float changedAnalog[3] = {0.8f, 0.f, 0.6f};
    check(sameVector(nativeAnalog, changedAnalog),
          "an unborrowed controller publication keeps native updates made during exploration");
    *nativeKeys = 1;
    *moveIntent = 1;
    exploreCall = ExploreCall::replaceOwner;
    explore(bh);
    check(*moveIntent == 0, "restoration does not write the old player's intent into a new behavior owner");
    *(uintptr_t*)(bh + field::behaviour::character) = root;
    *nativeKeys = 1;
    exploreCall = ExploreCall::nested;
    explore(bh);
    check(nestedScopeRestored && exploringRoot == oldExploring &&
              sameVector(exploringDirection, oldDirection),
          "nested exploration restores each caller's own travel-target scope");
    exploringRoot = 0;
    exploreCall = ExploreCall::ordinary;

    // Overhead, stopped and invalid-Aim owners delegate the original publications.
    for (int boundary : {0, 1, 2}) {
        viewMode = boundary == 0 ? 2 : 0;
        stopping = boundary == 1 ? 1 : 0;
        aim.valid = boundary != 2;
        *nativeKeys = 0x81;
        *moveIntent = 1;
        memcpy(nativeAnalog, oldAnalog, sizeof oldAnalog);
        const int called = exploreCalls;
        explore(bh);
        check(exploreCalls == called + 1 && seenKeys == 0x81 && seenIntent == 1 &&
                  sameVector(seenAnalog, oldAnalog),
              "unowned exploration reaches native code without substituted input");
    }
    stopping = 0;
    viewMode = 0;
    aim.valid = false; // isolate crouch admission from unrelated head/look features

    armCrouch(root);
    check(crouchHeld.load(), "the render owner preserves ordinary exploration crouch toggling");
    *(float*)(root + field::character::duckAmount) = 0.25f;
    poseBuilderOwned(root, 0.f, 0);
    check(observedDuck == 1.f, "an eligible request reaches the native physical pose builder");
    viewMode = 1;
    crouchFrame();
    *(float*)(root + field::character::duckAmount) = 0.25f;
    poseBuilderOwned(root, 0.f, 0);
    check(crouchHeld.load() && observedDuck == 1.f, "FP and TP retain one crouch across a view switch");

    for (unsigned state : {0u, 0x200u, 0x800u, 0x40000u}) {
        armCrouch(root);
        *(unsigned*)(root + field::character::stateFlags) = state;
        *(unsigned char*)(root + field::character::combatStance) = state == 0 ? 1 : 0;
        check(poseKeepsNativeDuck(root),
              state == 0 ? "combat entry between frames withdraws the exploration crouch request"
                         : "native recovery state between frames withdraws the exploration crouch request");
    }
    armCrouch(root);
    *(unsigned*)(root + field::character::lifeFlags) = 0x100;
    check(poseKeepsNativeDuck(root), "death between frames leaves duck amount to the native pose owner");
    armCrouch(root);
    crouchMode = 0;
    check(poseKeepsNativeDuck(root), "switching crouch off is effective before the next render frame");
    armCrouch(root);
    crouchMode = 1;
    check(poseKeepsNativeDuck(root), "a changed crouch mode waits for its own render request");
    armCrouch(root);
    stopping = 1;
    check(poseKeepsNativeDuck(root), "stopping withdraws the physical crouch request immediately");
    armCrouch(root);
    viewMode = 2;
    check(poseKeepsNativeDuck(root), "overhead handoff leaves physical crouch with native code");

    const uintptr_t choices = base + 0x690000;
    armCrouch(root);
    *(uintptr_t*)gameAt(fp::sig::dialogueChoices) = choices;
    *(unsigned char*)(choices + field::form::visible) = 1;
    *(unsigned char*)gameAt(fp::sig::dialogueActive) = 1;
    check(poseKeepsNativeDuck(root), "dialogue entry between frames withdraws exploration crouch");
    *(unsigned char*)gameAt(fp::sig::dialogueActive) = 0;

    armCrouch(root);
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = replacement;
    check(poseKeepsNativeDuck(replacement), "an old crouch request cannot enter a replacement player's pose");
    crouchFrame();
    check(!crouchHeld.load(), "a crouch toggle does not migrate into a replacement player");
    armCrouch(root);
    *(uintptr_t*)(root + field::character::headPart) = otherHead;
    check(poseKeepsNativeDuck(root), "a replaced head invalidates an unpublished physical crouch request");
    crouchFrame();
    check(!crouchHeld.load(), "a replaced head also resets the render crouch toggle");
    *(uintptr_t*)(root + field::character::headPart) = head;
    armCrouch(root);
    *(uintptr_t*)gameAt(fp::sig::currentScene) = otherScene;
    check(poseKeepsNativeDuck(root), "a scene change invalidates an unpublished physical crouch request");
    crouchFrame();
    check(!crouchHeld.load(), "a scene change resets the render crouch toggle");
    *(uintptr_t*)gameAt(fp::sig::currentScene) = scene;

    for (int mode : {1, 2}) {
        clearCrouch(root);
        crouchMode = mode;
        duckKey = true;
        crouchFrame();
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = replacement;
        crouchFrame();
        crouchFrame();
        check(!crouchHeld.load(), "a duck press held across player replacement cannot rearm the new player");
        duckKey = false;
        crouchFrame();
        duckKey = true;
        crouchFrame();
        check(crouchHeld.load(), "a fresh duck press after release can crouch the replacement player");
    }
    clearCrouch(root);
    duckKey = true;
    crouchFrame();
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = 0;
    crouchFrame();
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;
    crouchFrame();
    check(!crouchHeld.load(), "a held press cannot rearm crouch after an observed no-player interval");

    // Use the real level-grid/headroom owner, replacing only its native rays.
    const uintptr_t mesh = base + 0x6a0000, viewport = base + 0x6b0000, manager = base + 0x6c0000,
                    level = base + 0x6d0000;
    *(uintptr_t*)(head + field::bodyPart::meshNode) = mesh;
    *(uintptr_t*)(mesh + field::sceneNode::sector) = scene;
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = viewport;
    *(uintptr_t*)(viewport + field::viewport::sceneManager) = manager;
    *(unsigned*)(manager + field::sceneManager::flags) = 0x2000000;
    *(uintptr_t*)(manager + field::sceneManager::levelGrid) = level;
    levelRay = levelRay2 = nativeCeilingFixture;
    ceiling = true;
    armCrouch(root);
    duckKey = true; // second toggle asks to stand, but a ceiling retains the achieved crouch
    crouchFrame();
    duckKey = false;
    crouchFrame();
    check(crouchHeld.load() && ceilingQueries > 0,
          "standing waits for the existing native level headroom gate");
    ceiling = false;
    crouchFrame();
    check(!crouchHeld.load(), "standing resumes when the level headroom query becomes clear");
    armCrouch(root);
    duckKey = true;
    rayFault = true;
    crouchFrame();
    check(!crouchHeld.load(), "an unknown level-ray result retains the existing room-to-stand policy");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
