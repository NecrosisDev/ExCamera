// Production GUI/picking adapters with an owned native image and physical mouse.
// Native dispatch below models the reviewed 2FE40 -> 2F7E0 -> 2FCC0 -> 4C800
// chain: click time/position, cumulative drag threshold and registered callbacks.
// It establishes adapter behavior; actual object/hinge simulation needs the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static ULONGLONG fixtureTick = 1000;
static bool fixtureKeys[256]{}, fixtureForeground = true;
static HWND testForeground() { return fixtureForeground ? (HWND)1 : nullptr; }
static DWORD testWindowPid(HWND, DWORD* pid) { *pid = GetCurrentProcessId(); return 1; }
static SHORT WINAPI testKeyState(int key) {
    return key > 0 && key < 256 && fixtureKeys[key] ? SHORT(-32768) : 0;
}
static BOOL WINAPI testClientRect(HWND, RECT* rect) { *rect = {0, 0, 1280, 720}; return TRUE; }
static ULONGLONG WINAPI testTick() { return fixtureTick; }
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#define GetAsyncKeyState testKeyState
#define GetClientRect testClientRect
#define GetTickCount64 testTick
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#undef GetClientRect
#undef GetTickCount64
#include "test_controlled_player.h"

static unsigned checks = 0, failures = 0, presses = 0, drags = 0, doubles = 0, picks = 0;
static unsigned nativePresses = 0, nativePolls = 0, nativeCursors = 0, nativeApplies = 0, nativeQueues = 0;
static unsigned nativeCameraCalls = 0, snapshotCopies = 0;
static uintptr_t actor, behaviorObject, headObject, viewObject, guiObject, dragObject, itemObject, bodyObject;
static float pickedX = 0, pickedY = 0, nativeDistance = 100.f;
static bool throwOnPress = false, changeCoordinates = false;
static bool nativeSawPresentation = false, nativeDoubleOpensUi = false;
static uintptr_t pendingPayload = 0, pendingCallback = 0;
static bool retirePayload = false, payloadRetired = false;
static float heldRayOrigin[3]{}, heldRayDirection[3]{}, heldRayDistance = 1000.f;
static bool heldNativeHit = true, heldQueryThrows = false, nestHeldQuery = false;
static unsigned nativeHeldQueries = 0, heldRayCalls = 0;
static bool changeHeldOwner = false, heldWasMasked = false;
static int levelBehavior = 0;
static unsigned unexpectedLevelRays = 0;
static uintptr_t heldNativeResult = 1;
static float heldNativeHeight = 0.f;
static float queryRayOrigin[3]{}, queryRayDirection[3]{};

static void expect(bool value, const char* label) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
}
static LONG* point(size_t offset) { return (LONG*)(guiObject + offset); }
static unsigned& buttons() { return *(unsigned*)(guiObject + field::gui::buttons); }
static bool at(size_t offset, LONG x, LONG y) { return point(offset)[0] == x && point(offset)[1] == y; }
static void reticle(LONG x = 914, LONG y = 351) {
    reticleX = x; reticleY = y; reticleClear = 1; reticleTick = fixtureTick; reticleFrame = 1;
}
static void nativeCursor(LONG* x, LONG* y) { ++nativeCursors; *x = 100; *y = 200; }
static char nativePick(uintptr_t, float x, float y, uintptr_t out, void*) {
    ++picks; pickedX = x; pickedY = y;
    std::memset((void*)out, 0, 0x40);
    ((float*)(out + field::pick::direction))[2] = 1.f;
    *(float*)(out + field::pick::distance) = nativeDistance;
    return x >= 0 && y >= 0;
}
static char nativeHeldRay(uintptr_t, float x, float y, uintptr_t out) {
    ++heldRayCalls;
    heldWasMasked = (*(unsigned*)(itemObject + 8) & pickSkipBit) != 0;
    pickedX = x; pickedY = y;
    if (x < 0 || y < 0) return 0; // native off-viewport pick leaves its output untouched
    std::memset((void*)out, 0, 0x34);
    std::memcpy((void*)(out + field::pick::origin), heldRayOrigin, sizeof heldRayOrigin);
    std::memcpy((void*)(out + field::pick::direction), heldRayDirection, sizeof heldRayDirection);
    for (int i = 0; i < 3; ++i)
        ((float*)(out + 8))[i] = heldRayOrigin[i] + heldRayDirection[i] * heldRayDistance;
    *(float*)(out + field::pick::distance) = heldRayDistance;
    if (heldQueryThrows) RaiseException(0xe0001235, 0, 0, nullptr);
    if (changeHeldOwner) viewMode = 2;
    if (nestHeldQuery) {
        nestHeldQuery = false;
        float previousDirection[3];
        std::memcpy(previousDirection, heldRayDirection, sizeof previousDirection);
        heldRayDirection[0] = 0.8f; heldRayDirection[1] = -0.6f; heldRayDirection[2] = 0.f;
        float nested[12]{};
        heldQuery(itemObject, viewObject, (uintptr_t)nested, 100.f);
        std::memcpy(heldRayDirection, previousDirection, sizeof previousDirection);
    }
    return heldNativeHit;
}
static bool nativeHoldLevel(uintptr_t, const float* origin, const float* direction, float length) {
    if (origin[0] != 0.f || origin[1] != 160.f || origin[2] != 0.f ||
        direction[0] != 0.f || direction[1] != 0.f || direction[2] != 1.f)
        ++unexpectedLevelRays;
    if (levelBehavior == 2) RaiseException(0xe0001236, 0, 0, nullptr);
    return levelBehavior == 1 && length >= 50.f;
}
static uintptr_t nativeHeldQuery(uintptr_t, uintptr_t viewport, uintptr_t out, float height) {
    ++nativeHeldQueries;
    heldNativeHeight = height;
    if (heldQueryThrows) RaiseException(0xe0001235, 0, 0, nullptr);
    alignas(8) unsigned char ray[0x40]{};
    const char hit = pickRayFor(base + 0x117b18, viewport, // archived native caller, not a runtime dependency
                                float(point(field::gui::pointerX)[0]),
                                float(point(field::gui::pointerX)[1]), (uintptr_t)ray);
    const auto* origin = (const float*)(ray + field::pick::origin);
    const auto* direction = (const float*)(ray + field::pick::direction);
    const auto* picked = (const float*)(ray + 8);
    std::memcpy(queryRayOrigin, origin, sizeof queryRayOrigin);
    std::memcpy(queryRayDirection, direction, sizeof queryRayDirection);
    // The real 117AA0 accepts downward rays, then returns either the hit above
    // the actor or the crossing of the actor's height. 11F610 sends this point
    // directly to 118000. It never calls 1183C0 for a normal world grab.
    if (!(direction[1] < -1.192089982e-7f)) return 0;
    if (hit && height < picked[1])
        std::memcpy((void*)out, picked, sizeof(float) * 3);
    else
        for (int i = 0; i < 3; ++i)
            ((float*)out)[i] = origin[i] + direction[i] * ((height - origin[1]) / direction[1]);
    std::memcpy((void*)(out + 12), direction, sizeof(float) * 3);
    if (heldNativeResult != 1)
        for (int i = 0; i < 12; ++i) ((float*)out)[i] = 500.f + float(i);
    return heldNativeResult;
}
static unsigned worldMessage(unsigned message) {
    alignas(8) unsigned char ray[0x40]{};
    if (message == 0x1001) { ++presses; return 0; }
    if (message == 0x4001) {
        ++drags;
        grabPickFor(gameAt(fp::sig::grabStartPickReturn), 0, float(point(field::gui::pressX)[0]),
                    float(point(field::gui::pressX)[1]), (uintptr_t)ray, nullptr);
        return 0;
    }
    if (message == 0x8001) {
        ++doubles;
        const char hit = grabPickFor(gameAt(fp::sig::doubleClickPickReturn), 0,
                                     float(point(field::gui::pointerX)[0]),
                                     float(point(field::gui::pointerX)[1]), (uintptr_t)ray, nullptr);
        if (hit && nativeDoubleOpensUi) *(int*)gameAt(fp::sig::uiOpenCount) = 1;
        return hit ? 1u : 0u;
    }
    return 0;
}
static void nativePress(uintptr_t gui, unsigned short incoming) {
    ++nativePresses;
    nativeSawPresentation = guiPointerPresentation.active;
    if (throwOnPress) RaiseException(0xe0001234, 0, 0, nullptr);
    if (incoming & 7) std::memcpy(point(0x28), point(field::gui::pointerX), sizeof(LONG) * 2);
    unsigned result = 0;
    const auto last = *(uint32_t*)(gui + 0x58);
    const bool doubleClick = uint32_t(fixtureTick) - last < 400 &&
                             incoming == *(unsigned short*)(gui + 0x54) &&
                             std::abs(point(field::gui::pointerX)[0] - point(field::gui::pressX)[0]) < 2 &&
                             std::abs(point(field::gui::pointerX)[1] - point(field::gui::pressX)[1]) < 2;
    buttons() = 0;
    if (doubleClick) {
        result = worldMessage(0x8000 | incoming);
        if (!(result & ~0x10u)) result = worldMessage(0x1000 | incoming);
    } else {
        *(unsigned short*)(gui + 0x54) = incoming;
        *(uint32_t*)(gui + 0x58) = uint32_t(fixtureTick);
        result = worldMessage(0x1000 | incoming);
    }
    if (result != 0x40) {
        buttons() = incoming;
        std::memcpy(point(field::gui::pressX), point(field::gui::pointerX), sizeof(LONG) * 2);
    }
    if (changeCoordinates) {
        point(field::gui::pointerX)[0] = 71; point(field::gui::pointerX)[1] = 72;
        point(field::gui::pressX)[0] = 81; point(field::gui::pressX)[1] = 82;
    }
}
static char nativePoll(uintptr_t gui, char force) {
    ++nativePolls;
    nativeSawPresentation = guiPointerPresentation.active;
    char moved = 0;
    if (!force) {
        LONG x = 0, y = 0;
        cursorPositionFor(gameAt(fp::sig::uiPollCursorReturn), &x, &y);
        if (at(field::gui::pointerX, x, y)) return 0;
        point(field::gui::pointerX)[0] = x; point(field::gui::pointerX)[1] = y;
        moved = 1;
    }
    if ((buttons() & 7) && !(buttons() & 0x6100)) {
        const int elapsed = int(uint32_t(fixtureTick) - *(uint32_t*)(gui + 0x58));
        const int threshold = (elapsed < 191 ? (200 - elapsed) / 20 : 1) * *(int*)(gui + 0x50);
        if (std::abs(point(field::gui::pointerX)[0] - point(field::gui::pressX)[0]) > threshold ||
            std::abs(point(field::gui::pointerX)[1] - point(field::gui::pressX)[1]) > threshold) {
            buttons() |= 0x100;
            if (!*(uintptr_t*)(gui + field::gui::capture)) worldMessage(0x4000 | (buttons() & 7));
        }
    }
    return moved;
}
static uintptr_t nativeQueue(uintptr_t, uintptr_t callback, uintptr_t payload) {
    ++nativeQueues; pendingPayload = payload; pendingCallback = callback; return 19;
}
static uintptr_t nativeApply(uintptr_t payload) {
    ++nativeApplies;
    const uintptr_t body = *(uintptr_t*)payload;
    // A native-completion spy deliberately writes different final body points.
    // This proves ordering against any writes by the real callback's callees;
    // it does not assert that its internals are a plain array copy.
    auto* now = (float*)(body + field::bodyPart::points);
    auto* before = (float*)(body + field::bodyPart::history);
    auto* older = (float*)(body + field::bodyPart::history2);
    for (int i = 0; i < 12; ++i) { now[i] = 100.f + i; before[i] = -500.f; older[i] = -700.f; }
    if (retirePayload) {
        DWORD previous = 0;
        payloadRetired = VirtualProtect((void*)payload, 4096, PAGE_NOACCESS, &previous) != 0;
    }
    return 23;
}
static void reset(int mode = 0) {
    fixture::resetInput(mode);
    fixtureTick = 1000; fixtureForeground = true;
    for (bool& key : fixtureKeys) key = false;
    std::memset((void*)guiObject, 0, 0x100);
    std::memset((void*)dragObject, 0, 0x180);
    std::memset((void*)bodyObject, 0, 0x200);
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = actor;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = 0x1234;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = guiObject;
    *(uintptr_t*)gameAt(fp::sig::uiView) = viewObject;
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = viewObject;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = dragObject;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(int*)gameAt(fp::sig::pointerOverride) = 0;
    *(uintptr_t*)(guiObject + field::gui::hoverView) = viewObject;
    *(int*)(guiObject + 0x50) = 2;
    *(unsigned char*)(actor + field::character::combatStance) = 0;
    *(unsigned char*)(behaviorObject + field::behaviour::interactionLatch) = 0;
    *(uintptr_t*)(itemObject + field::sceneNode::bodyPart) = bodyObject;
    *(uintptr_t*)(bodyObject + field::bodyPart::meshNode) = itemObject;
    *(uintptr_t*)(actor + field::character::sceneNode) = 0;
    worldPointer = {}; guiPointerPresentation = {}; queuedFlings = {}; flingSequence = 0;
    holdObject = 0; holdGeneration = 0; heldOffsetObject = 0;
    pushSpeed = 0; pushAt = 0; physicsStep = 0.02f;
    for (int i = 0; i < 3; ++i) { eyeGlobal[i] = 0; pushDir[i] = 0; }
    const float zero[3]{}; publishReachHead(zero);
    grabRange = 200; nativeDistance = 100.f;
    savedCamera = filterRoot = filterHead = 0;
    swinging = false; hideSwingCrosshair = true;
    presses = drags = doubles = picks = nativePresses = nativePolls = nativeCursors = nativeQueues = nativeApplies = 0;
    throwOnPress = changeCoordinates = nativeSawPresentation = nativeDoubleOpensUi = false;
    retirePayload = payloadRetired = false; pendingPayload = pendingCallback = 0;
    reticle();
    point(field::gui::pointerX)[0] = point(field::gui::pressX)[0] = reticleX;
    point(field::gui::pointerX)[1] = point(field::gui::pressX)[1] = reticleY;
}
static void release() { buttons() = 0; *(uintptr_t*)(guiObject + field::gui::capture) = 0; }
static void nativeMainMouseTick() {
    // The reviewed 0x263C0 calls 2F7E0(gui, 0) only when the camera callback
    // returns zero. A return of one skips the entire GUI poll, including UI.
    if (!cameraMouse()) guiMousePoll(guiObject, 0);
}
static void step(float dx, float dy, unsigned ms = 20) {
    fixtureTick += ms; reticleTick = fixtureTick; worldPointerMotion(dx, dy); nativeMainMouseTick();
}
static bool pressThrows() {
    __try { guiMousePress(guiObject, 1); }
    __except (GetExceptionCode() == 0xe0001234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
static void gestureTests() {
    for (int mode = 0; mode < 2; ++mode) {
        reset(mode); guiMousePress(guiObject, 1); step(60, 0);
        expect(drags == 1 && picks == 1 && pickedX == 914 && pickedY == 351,
               "FP/TP physical mouse movement starts native world drag at the actual reticle");
        expect(at(field::gui::pointerX, 914, 351) && at(field::gui::pressX, 914, 351) && !guiPointerPresentation.active,
               "native GUI coordinates and nested pointer presentation restore after drag dispatch");
        expect(at(0x28, 914, 351), "the native secondary press snapshot keeps real reticle coordinates");
        step(60, 0);
        expect(drags == 1, "native classified drag is not restarted by later mouse samples");
    }
    reset(); guiMousePress(guiObject, 1);
    for (int i = 0; i < 12; ++i) step(1, 0);
    expect(drags == 1, "slow one-pixel turns accumulate across frames to cross the native drag threshold");
    reset(); guiMousePress(guiObject, 1); reticle(927, 349); step(0, 0, 220);
    expect(drags == 0, "head/reticle jitter alone cannot synthesize a grab");
    release(); fixtureTick += 50; reticle(943, 347); guiMousePress(guiObject, 1);
    expect(doubles == 1 && pickedX == 943 && pickedY == 347,
           "native double-click survives rendered reticle jitter and picks the new rendered point");
    reset(); guiMousePress(guiObject, 1); release(); fixtureTick += 80; reticle();
    worldPointerMotion(2, 0); guiMousePress(guiObject, 1);
    expect(doubles == 0 && presses == 2, "two physical pixels between presses preserve native double-click rejection");
    reset(); guiMousePress(guiObject, 1); release(); fixtureTick += 401; reticle(); guiMousePress(guiObject, 1);
    expect(doubles == 0, "native double-click time window remains authoritative");
    reset(); guiMousePress(guiObject, 1); worldPointerMotion(60, 0); fixtureTick += 20; reticle();
    guiMousePoll(guiObject, 1);
    expect(drags == 0, "forced GUI poll does not invent a physical cursor read");
    guiMousePoll(guiObject, 0);
    expect(drags == 1, "ordinary poll after a forced poll retains unconsumed mouse movement");
    reset(); guiMousePress(guiObject, 1); release(); inputOwner.releaseGameplay();
    fixtureTick += 80; reticle(); worldPointerMotion(1, 0); guiMousePress(guiObject, 1);
    expect(doubles == 0, "a new gameplay owner cannot double-click the previous owner's press");
    reset(); nativeDoubleOpensUi = true; guiMousePress(guiObject, 1); release(); fixtureTick += 80; reticle();
    guiMousePress(guiObject, 1);
    expect(doubles == 1 && *(int*)gameAt(fp::sig::uiOpenCount) == 1 && !guiPointerPresentation.active,
           "a native container callback can open UI and retains its own result");
    reset(); throwOnPress = true;
    expect(pressThrows() && at(field::gui::pointerX, 914, 351) && at(field::gui::pressX, 914, 351) &&
               !guiPointerPresentation.active, "native exceptions restore the temporary pointer and press coordinates");
    reset(); changeCoordinates = true; guiMousePress(guiObject, 1);
    expect(at(field::gui::pointerX, 71, 72) && at(field::gui::pressX, 81, 82),
           "coordinate updates made by native callbacks win over restoration");
    for (unsigned gate = 0; gate < 8; ++gate) {
        reset();
        switch (gate) {
        case 0: viewMode = 2; break;
        case 1: active = 0; break;
        case 2: cameraReady = 0; break;
        case 3: stopping = 1; break;
        case 4: *(int*)gameAt(fp::sig::uiOpenCount) = 1; break;
        case 5: *(uintptr_t*)(guiObject + field::gui::capture) = viewObject; break;
        case 6: fixtureForeground = false; break;
        case 7: *(unsigned char*)(actor + field::character::combatStance) = 1; break;
        }
        guiMousePress(guiObject, 1);
        expect(nativePresses == 1 && !nativeSawPresentation && !worldPointer.havePress,
               "overhead, park, unreadiness, stop, UI, capture, focus and ordinary combat retain native presses");
    }
    reset(); *(unsigned char*)(actor + field::character::combatStance) = 1; fixtureKeys['E'] = true;
    guiMousePress(guiObject, 1); step(60, 0);
    expect(drags == 1 && pickedX == 914, "native Interact in combat can still initiate an intentional world grab");
    for (unsigned free = 0; free < 3; ++free) {
        reset();
        if (free == 0) cursorFree = 1;
        if (free == 1) windowFree = 1;
        if (free == 2) *(int*)gameAt(fp::sig::uiOpenCount) = 1;
        nativeMainMouseTick();
        expect(nativePolls == 1 && nativeCursors == 1 && at(field::gui::pointerX, 100, 200),
               "camera callback lets the native main loop update F8, inventory and other UI pointers");
    }
}
static void beginWorldHold(int mode = 0) {
    reset(mode);
    resetHeldGrab(); holdTick = 0; grabStartDistance = 100.f;
    holdTarget = holdApplied = 0.f;
    heldNativeHit = true; heldQueryThrows = false; nestHeldQuery = false; heldNativeResult = 1;
    nativeHeldQueries = heldRayCalls = 0; heldNativeHeight = -99.f;
    changeHeldOwner = heldWasMasked = false; levelBehavior = 0; unexpectedLevelRays = 0;
    *(uintptr_t*)(viewObject + field::viewport::sceneManager) = 0;
    heldRayDistance = 1000.f;
    heldRayOrigin[0] = heldRayOrigin[2] = 0.f; heldRayOrigin[1] = 160.f;
    heldRayDirection[0] = heldRayDirection[2] = 0.f; heldRayDirection[1] = -1.f;
    eyeGlobal[1] = 160.f;
    *(unsigned*)(dragObject + field::drag::flags) = 0x2000;
    *(uintptr_t*)(dragObject + field::drag::object) = itemObject;
    *(uintptr_t*)(guiObject + field::gui::capture) = viewObject;
    originalHeldQuery = nativeHeldQuery;
    originalPickRay = nativeHeldRay;
}
static bool worldQueryThrows(uintptr_t out) {
    __try { heldQuery(itemObject, viewObject, out, 100.f); }
    __except (GetExceptionCode() == 0xe0001235 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
static void heldQueryTests() {
    alignas(8) float pointAndDirection[12]{};
    const uintptr_t out = (uintptr_t)pointAndDirection;
    beginWorldHold();
    expect(nativeHeldQuery(itemObject, viewObject, out, 100.f) == 1 &&
               pointAndDirection[1] == 100.f && !holdObject.load(),
           "native gameplay query alone raises a low target to the actor plane and never initializes 1183C0 state");
    for (int mode = 0; mode < 2; ++mode) {
        beginWorldHold(mode);
        const unsigned before = holdPlacements.load();
        expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
                   holdObject.load() == itemObject && holdApplied.load() == 100.f &&
                   pointAndDirection[1] == 60.f && nativeHeldQueries == 0 && heldRayCalls == 1 &&
                   holdPlacements.load() == before + 1,
               "FP/TP normal 117AA0 query bootstraps a floor hold without 1183C0 or the actor-height jump");
        expect(pickedX == 914 && pickedY == 351 && !selfPickSet,
               "ordinary held updates use the rendered reticle and restore their pick mask");
        holdTarget = 150.f; fixtureTick += 50; reticleTick = fixtureTick;
        heldQuery(itemObject, viewObject, out, 100.f);
        expect(holdApplied.load() > 100.f && holdApplied.load() < 150.f &&
                   pointAndDirection[1] < 60.f && pushSpeed.load() > 1.f,
               "subsequent native held updates smoothly apply wheel distance and publish outward release speed");
    }
    beginWorldHold(1);
    heldRayOrigin[2] = -300.f; heldRayDirection[1] = 0.f; heldRayDirection[2] = 1.f;
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
               std::fabs(pointAndDirection[2] - 100.f) < .001f,
           "third-person gameplay hold distance starts at the eye, not the camera behind it");
    expect(pointAndDirection[3] == 0.f && pointAndDirection[4] == 0.f && pointAndDirection[5] == 1.f,
           "native placement receives the real horizontal direction for its collision sweep");
    expect(heldWasMasked && !(*(unsigned*)(itemObject + 8) & pickSkipBit),
           "the held object is excluded only during the native view-ray query");
    beginWorldHold();
    heldRayDirection[1] = 0.6f; heldRayDirection[2] = 0.8f;
    for (int i = 6; i < 12; ++i) pointAndDirection[i] = 700.f + float(i);
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
               std::fabs(pointAndDirection[1] - 220.f) < .001f &&
               std::fabs(pointAndDirection[2] - 80.f) < .001f &&
               pointAndDirection[4] == 0.6f && pointAndDirection[5] == 0.8f &&
               nativeHeldQueries == 0 && heldRayCalls == 1,
           "upward holds use only the actual view ray without invoking the overhead query");
    bool tailPreserved = true;
    for (int i = 6; i < 12; ++i) tailPreserved &= pointAndDirection[i] == 700.f + float(i);
    expect(tailPreserved, "ordinary placement changes only its native six-float output");
    beginWorldHold(); nestHeldQuery = true;
    heldRayDirection[1] = 0.f; heldRayDirection[2] = 1.f;
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
               pointAndDirection[0] == 0.f && pointAndDirection[1] == 160.f &&
               pointAndDirection[2] == 100.f && pointAndDirection[3] == 0.f &&
               pointAndDirection[4] == 0.f && pointAndDirection[5] == 1.f &&
               nativeHeldQueries == 0 && heldRayCalls == 2 && !selfPickSet,
           "nested picks preserve the outer point and real direction without shared query state");
    beginWorldHold();
    heldRayDirection[1] = -0.6f; heldRayDirection[2] = 0.8f;
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
               std::fabs(pointAndDirection[1] - 100.f) < .001f &&
               std::fabs(pointAndDirection[2] - 80.f) < .001f &&
               pointAndDirection[4] == -0.6f && pointAndDirection[5] == 0.8f,
           "oblique downward placement retains the real aimed direction");
    expect(nativeHeldQueries == 0 && heldRayCalls == 1,
           "looking downward still uses close-view holding instead of the overhead admission path");
    beginWorldHold(); heldRayDistance = 60.f;
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 && pointAndDirection[1] == 100.f,
           "a real surface nearer than the held distance limits the accepted world point");
    beginWorldHold(); heldNativeHit = false;
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 && pointAndDirection[1] == 60.f,
           "open-air world holding still supplies a native accepted point at the held distance");
    for (int behavior : {1, 2}) {
        beginWorldHold(); heldRayDirection[1] = 0.f; heldRayDirection[2] = 1.f;
        const uintptr_t manager = base + 0x5d4000, level = base + 0x5d6000;
        *(uintptr_t*)(viewObject + field::viewport::sceneManager) = manager;
        *(unsigned*)(manager + field::sceneManager::flags) = 0x2000000;
        *(uintptr_t*)(manager + field::sceneManager::levelGrid) = level;
        levelBehavior = behavior; levelRay = nativeHoldLevel; levelRay2 = nativeHoldLevel;
        std::fill(std::begin(pointAndDirection), std::end(pointAndDirection), 1234.f);
        const uintptr_t result = heldQuery(itemObject, viewObject, out, 100.f);
        expect(behavior == 1 ? result == 1 && pointAndDirection[2] > 30.f && pointAndDirection[2] < 50.f
                             : result == 0 && pointAndDirection[0] == 1234.f,
               "native level collisions limit the hold and query faults reject it without writing an output");
        expect(unexpectedLevelRays == 0 && nativeHeldQueries == 0,
               "all close-view level probes follow the actual horizontal ray");
    }
    beginWorldHold(); changeHeldOwner = true;
    std::fill(std::begin(pointAndDirection), std::end(pointAndDirection), 1234.f);
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 0 && pointAndDirection[0] == 1234.f &&
               !holdObject.load() && !selfPickSet,
           "an ownership change during the native pick cannot publish a close-view placement or hold state");
    for (uintptr_t result : {uintptr_t(0), uintptr_t(0x10), uintptr_t(0x30)}) {
        beginWorldHold(); viewMode = 2; heldNativeResult = result;
        const uintptr_t actual = heldQuery(itemObject, viewObject, out, 100.f);
        bool preserved = true;
        for (int i = 0; i < 12; ++i) preserved &= pointAndDirection[i] == 500.f + float(i);
        expect(actual == result && preserved && nativeHeldQueries == 1,
               "overhead retains native rejection and special placement slots byte for byte");
    }
    for (unsigned gate = 0; gate < 13; ++gate) {
        beginWorldHold();
        uintptr_t viewport = viewObject;
        switch (gate) {
        case 0: viewMode = 2; break;
        case 1: active = 0; break;
        case 2: cameraReady = 0; break;
        case 3: inputEnabled = 0; break;
        case 4: stopping = 1; break;
        case 5: cursorFree = 1; break;
        case 6: windowFree = 1; break;
        case 7: *(int*)gameAt(fp::sig::uiOpenCount) = 1; break;
        case 8: fixtureForeground = false; break;
        case 9: *(unsigned*)(dragObject + field::drag::flags) = 0x4000; break;
        case 10: *(uintptr_t*)(dragObject + field::drag::object) = itemObject + 0x1000; break;
        case 11: viewport += 0x1000; break;
        case 12: *(uintptr_t*)(guiObject + field::gui::capture) = viewObject + 0x1000; break;
        }
        const bool paused = gate == 3 || (gate >= 5 && gate <= 8) || gate == 12;
        const uintptr_t result = heldQuery(itemObject, viewport, out, 100.f);
        expect(paused ? result == 0 && nativeHeldQueries == 0
                      : result == 1 && pointAndDirection[1] == 100.f && heldNativeHeight == 100.f,
               "UI/focus pauses a close-view world hold; overhead, park and unrelated native contexts retain "
               "their query");
    }
    beginWorldHold();
    rmbFree = 1;
    releaseInput();
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 && pointAndDirection[1] == 60.f,
           "RMB free cursor after the real input-release handoff keeps the bounded close-view hold");
    beginWorldHold(); reticleClear = 0;
    const unsigned beforeInvalid = holdPlacements.load();
    expect(heldQuery(itemObject, viewObject, out, 100.f) == 0 && !holdObject.load() &&
               holdPlacements.load() == beforeInvalid,
           "a blocked reticle cannot bootstrap a hold from stale native ray memory");
    beginWorldHold(); heldQueryThrows = true;
    expect(worldQueryThrows(out) && !selfPickSet && !(*(unsigned*)(itemObject + 8) & pickSkipBit),
           "a handled native ray fault restores the held object's pick mask");
}
static int nativeInventoryPlacements = 0;
static int nativeInventoryPlace(uintptr_t object, uintptr_t viewport, float height) {
    ++nativeInventoryPlacements;
    expect(object == itemObject && viewport == viewObject && height == -100000.f &&
               *(unsigned*)(dragObject + field::drag::flags) == 0x1000,
           "inventory transition keeps the original native placement call before its 0x2000 handoff");
    return 7;
}
static void inventoryHoldDistanceTests() {
    alignas(8) float pointAndDirection[12]{};
    const uintptr_t out = (uintptr_t)pointAndDirection;
    for (bool refusedWorldPick : {false, true}) {
        beginWorldHold(); grabStartDistance = 180.f;
        expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 && holdApplied.load() == 180.f,
               "initial ordinary world grab remembers its own 180-unit distance");
        // Ordinary 11B340 submits 116EF0 and clears its native capture. The
        // mod's release adapter must not be relied on to reset a future grab.
        alignas(8) uintptr_t payload[7]{bodyObject};
        queueWorkFor(gameAt(fp::sig::heldReleaseQueueReturn), 5,
                     gameAt(fp::sig::applyHeldRelease), (uintptr_t)payload);
        *(unsigned*)(dragObject + field::drag::flags) = 0;
        *(uintptr_t*)(dragObject + field::drag::object) = 0;
        release();
        applyHeldRelease((uintptr_t)payload);
        if (refusedWorldPick) {
            // A refused native world pick is another real way to have a start
            // distance but no holdObject before moving an inventory item out.
            alignas(8) unsigned char ray[0x40]{};
            heldRayDistance = 900.f;
            nativeHeldRay(viewObject, 1, 1, (uintptr_t)ray);
            expect(!grabPickResult(gameAt(fp::sig::grabStartPickReturn), 1, (uintptr_t)ray) &&
                       !holdObject.load() && grabStartDistance.load() == 900.f,
                   "refused pick leaves no held object before the inventory transition");
        }
        *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
        *(uintptr_t*)(dragObject + field::drag::object) = itemObject;
        originalDragPlace = nativeInventoryPlace;
        expect(dragPlace(itemObject, viewObject, -100000.f) == 7 && holdObject.load() == itemObject &&
                   holdApplied.load() == fp::holdNear * 2.f && grabStartDistance.load() == 0.f,
               "inventory-to-world transition clears an old start distance even when no item was remembered");
        *(unsigned*)(dragObject + field::drag::flags) = 0x2000;
        *(uintptr_t*)(guiObject + field::gui::capture) = viewObject;
        heldRayDistance = 1000.f;
        expect(heldQuery(itemObject, viewObject, out, 100.f) == 1 &&
                   holdApplied.load() == fp::holdNear * 2.f && pushSpeed.load() == 0.f,
               "the next world hold uses its fresh default distance and cannot inherit an earlier item's push");
        alignas(8) unsigned char fresh[0x40]{};
        heldRayDistance = 100.f;
        nativeHeldRay(viewObject, 1, 1, (uintptr_t)fresh);
        expect(grabPickResult(gameAt(fp::sig::grabStartPickReturn), 1, (uintptr_t)fresh) &&
                   grabStartDistance.load() == 100.f,
               "grab-start reset still publishes the new accepted world pick distance afterwards");
    }
    expect(nativeInventoryPlacements == 2, "each inventory transition calls native placement exactly once");
}
static void armRelease(uintptr_t payload) {
    *(uintptr_t*)payload = bodyObject;
    *(unsigned*)(dragObject + field::drag::flags) = 0x2000;
    *(uintptr_t*)(dragObject + field::drag::object) = itemObject;
    holdObject = itemObject; holdGeneration = 7;
    pushSpeed = 100.f; pushDir[2] = 1.f; pushAt = fixtureTick;
}
static bool unchangedHistory() { return *(float*)(bodyObject + field::bodyPart::history) == -500.f; }
static void queuedReleaseTests(uintptr_t payload) {
    reset(); armRelease(payload); retirePayload = true;
    const unsigned beforeFlings = flings.load();
    expect(queueWorkFor(gameAt(fp::sig::heldReleaseQueueReturn), 5, gameAt(fp::sig::applyHeldRelease), payload) == 19 &&
               nativeQueues == 1 && pendingPayload == payload && pendingCallback == gameAt(fp::sig::applyHeldRelease),
           "ordinary release queues the exact native callback and payload without the dragDrop path");
    pushSpeed = 900.f; pushDir[2] = -1.f; physicsStep = 0.05f; holdObject = 0;
    const uintptr_t result = applyHeldRelease(payload);
    const float* previous = (const float*)(bodyObject + field::bodyPart::history);
    const float* older = (const float*)(bodyObject + field::bodyPart::history2);
    expect(result == 23 && nativeApplies == 1 && flings.load() == beforeFlings + 1 && previous[2] == 100.f && older[2] == 98.f,
           "fling uses release-time velocity and step after native completion, even when push state later changes");
    expect(payloadRetired, "queued release payload can be unreadable after native free without being touched again");
    DWORD ignored = 0; VirtualProtect((void*)payload, 4096, PAGE_READWRITE, &ignored);
    for (unsigned gate = 0; gate < 11; ++gate) {
        reset(); armRelease(payload);
        if (gate == 0) *(unsigned char*)(dragObject + field::drag::invalidDrop) = 1;
        if (gate == 1) pushAt = fixtureTick - fp::flingFreshMs - 1;
        queueWorkFor(gameAt(fp::sig::heldReleaseQueueReturn), 5, gameAt(fp::sig::applyHeldRelease), payload);
        switch (gate) {
        case 2: viewMode = 2; break;
        case 3: stopping = 1; break;
        case 4: inputOwner.releaseGameplay(); break;
        case 5: *(uintptr_t*)gameAt(fp::sig::playerRoot) = 0; break;
        case 6: *(uintptr_t*)gameAt(fp::sig::currentScene) = 0x5678; break;
        case 7: ++holdGeneration; break;
        case 8: *(uintptr_t*)(itemObject + field::sceneNode::bodyPart) = 0; break;
        case 9: *(uintptr_t*)(bodyObject + field::bodyPart::meshNode) = 0; break;
        case 10: ++holdGeneration; holdObject = 0; break; // native regrab exists before placement republishes it
        }
        applyHeldRelease(payload);
        expect(unchangedHistory(), "invalid/stale releases, handoffs, regrabs and changed native identities cannot add a fling");
    }
    reset(); armRelease(payload);
    queueWorkFor(0, 5, gameAt(fp::sig::applyHeldRelease), payload); applyHeldRelease(payload);
    expect(unchangedHistory(), "another queue caller cannot acquire held-release velocity metadata");
    reset(); armRelease(payload);
    queueWorkFor(gameAt(fp::sig::heldReleaseQueueReturn), 5, gameAt(fp::sig::applyHeldRelease), payload);
    ++holdGeneration; holdObject = itemObject + 0x1000;
    *(uintptr_t*)(dragObject + field::drag::object) = holdObject.load();
    applyHeldRelease(payload);
    expect(((float*)(bodyObject + field::bodyPart::history))[2] == 100.f,
           "grabbing a different object cannot cancel an earlier object's pending fling");
}
static float inventoryPlaced[3]{};
static int nativeInventoryRayPlace(uintptr_t, uintptr_t viewport, float h) {
    alignas(8) unsigned char ray[0x40]{};
    const char hit = pickRayFor(base + 0x118440, viewport, float(point(field::gui::pointerX)[0]),
                                float(point(field::gui::pointerX)[1]), (uintptr_t)ray);
    const float* d = (float*)(ray + field::pick::direction);
    const float* o = (float*)(ray + field::pick::origin);
    if (hit && ((float*)(ray + 8))[1] >= h) {
        std::memcpy(inventoryPlaced, ray + 8, 12);
        return 1;
    }
    if (d[1] > -1.192089982e-7f) return 0;
    const float t = (h - o[1]) / d[1];
    for (int i = 0; i < 3; ++i) inventoryPlaced[i] = o[i] + d[i] * t;
    return 1;
}
static void inventoryRayTests() {
    for (int mode : {0, 1})
        for (float pitch : {-0.6f, 0.f, 0.6f}) {
            beginWorldHold(mode);
            *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
            heldNativeHit = false;
            heldRayDirection[1] = pitch;
            heldRayDirection[2] = std::sqrt(1.f - pitch * pitch);
            originalDragPlace = nativeInventoryRayPlace;
            inventoryPlaced[0] = inventoryPlaced[1] = inventoryPlaced[2] = NAN;
            const int placed = dragPlace(itemObject, viewObject, -100000.f);
            float n = 0;
            for (int i = 0; i < 3; ++i)
                n += (inventoryPlaced[i] - heldRayOrigin[i]) * (inventoryPlaced[i] - heldRayOrigin[i]);
            expect(placed && std::isfinite(n) && n < 400.f * 400.f,
                   "inventory-to-world initialization stays on the bounded view ray for downward, level and "
                   "upward open-air picks");
        }
    beginWorldHold();
    *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
    heldNativeHit = false;
    heldRayDirection[1] = 0;
    heldRayDirection[2] = 1;
    *(int*)gameAt(fp::sig::uiOpenCount) = 1;
    originalDragPlace = nativeInventoryRayPlace;
    expect(dragPlace(itemObject, viewObject, -100000.f) == 1,
           "inventory UI uses its actual native pointer for bounded world initialization");
    for (int failure = 0; failure < 3; ++failure) {
        beginWorldHold();
        *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
        if (failure == 0) reticleClear = 0;
        if (failure == 1) changeHeldOwner = true;
        if (failure == 2) {
            const uintptr_t manager = base + 0x5d4000, level = base + 0x5d6000;
            *(uintptr_t*)(viewObject + field::viewport::sceneManager) = manager;
            *(unsigned*)(manager + field::sceneManager::flags) = 0x2000000;
            *(uintptr_t*)(manager + field::sceneManager::levelGrid) = level;
            levelBehavior = 2;
            levelRay = nativeHoldLevel;
            levelRay2 = nativeHoldLevel;
        }
        originalDragPlace = [](uintptr_t, uintptr_t, float) -> int {
            return 77;
        };
        expect(
            dragPlace(itemObject, viewObject, -100000.f) == 0,
            "blocked ray, changed view or unavailable level collision cannot reach native height fallback");
    }
    beginWorldHold();
    *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
    heldNativeHit = false;
    heldRayDirection[1] = 0;
    heldRayDirection[2] = 1;
    originalDragPlace = [](uintptr_t, uintptr_t viewport, float h) -> int {
        alignas(8) unsigned char unrelated[0x40]{};
        expect(!pickRayFor(base + 0x123456, viewport, 100, 200, (uintptr_t)unrelated),
               "a nested unrelated ray cannot consume the prepared placement hit");
        return nativeInventoryRayPlace(itemObject, viewport, h);
    };
    expect(dragPlace(itemObject, viewObject, -100000.f) == 1,
           "the exact placement caller still receives the prepared point after an unrelated pick");
    originalDragPlace = [](uintptr_t, uintptr_t, float) -> int {
        RaiseException(0xe0001238, 0, 0, nullptr);
        return 0;
    };
    bool caught = false;
    __try {
        dragPlace(itemObject, viewObject, -100000.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = true;
    }
    alignas(8) unsigned char after[0x40]{};
    expect(caught && !pickRayFor(base + 0x118440, viewObject, 100, 200, (uintptr_t)after),
           "native placement exception restores the prepared-ray scope and leaves later picks native");
}

static unsigned planeFallbacks = 0;
static void constrainedHoldTests() {
    for (int mode : {0, 1}) {
        beginWorldHold(mode);
        originalPlaneHit = [](float* out, uintptr_t, int, int, const float*) {
            ++planeFallbacks;
            out[0] = out[1] = out[2] = INFINITY;
        };
        planeFallbacks = 0;
        heldRayOrigin[0] = 0;
        heldRayOrigin[1] = 100;
        heldRayOrigin[2] = 0;
        heldRayDirection[0] = 0;
        heldRayDirection[1] = 0;
        heldRayDirection[2] = 1;
        alignas(8) unsigned char hit[0x40]{};
        nativeHeldRay(viewObject, 914, 351, (uintptr_t)hit);
        *(float*)(hit + field::pick::distance) = 150;
        grabPickResult(gameAt(fp::sig::grabStartPickReturn), 1, (uintptr_t)hit);
        *(unsigned*)(dragObject + 8) = 0x4000;
        float* pivot = (float*)(dragObject + field::drag::pivot);
        pivot[0] = 0;
        pivot[1] = 50;
        pivot[2] = 0;
        doorDistance = 5; // first update projects onto the pivot
        float out[3]{};
        const float floor[4] = {0, 1, 0, 50}, wall[4] = {1, 0, 0, 30};
        planeHitFor(gameAt(fp::sig::doorPlaneReturn), out, viewObject, 10, 20, floor);
        expect(planeFallbacks == 0 && std::isfinite(out[0] + out[1] + out[2]),
               "first close-view door update at the pivot never calls the unbounded overhead intersection");
        expect(pickedX == 914 && pickedY == 351,
               "frame-driven constrained hold follows the reticle outside GUI presentation");
        *(unsigned*)(dragObject + 8) = 0x10000;
        doorDistance = 120;
        planeHitFor(gameAt(fp::sig::hingePlaneReturnB), out, viewObject, 10, 20, wall);
        float saved[3];
        std::memcpy(saved, out, 12);
        planeHitFor(gameAt(fp::sig::hingePlaneReturnC), out, viewObject, 10, 20, floor);
        heldRayDirection[0] = 1;
        heldRayDirection[2] = 0; // normal to B, valid along C
        planeHitFor(gameAt(fp::sig::hingePlaneReturnB), out, viewObject, 10, 20, wall);
        expect(!std::memcmp(out, saved, 12), "a hinge singularity reuses only the point for its own plane");
        *(unsigned*)(dragObject + 8) = 0x20000;
        heldRayDirection[0] = 0;
        heldRayDirection[1] = 0.6f;
        heldRayDirection[2] = 0.8f;
        planeHitFor(base + 0x11bbd4, out, viewObject, 10, 20, floor);
        expect(planeFallbacks == 0 && std::isfinite(out[1]) && out[1] != floor[3],
               "physical constraint dragging follows the actual upward ray instead of a captured overhead "
               "plane");
        viewMode = 2;
        planeHitFor(gameAt(fp::sig::doorPlaneReturn), out, viewObject, 10, 20, floor);
        expect(planeFallbacks == 1, "overhead constrained dragging keeps its native intersection");
    }
}

static unsigned constrainedCalls = 0, constrainedReleases = 0;
static bool constrainedThrow = false;
static unsigned long long nativeConstrained(uintptr_t, uintptr_t, int message) {
    if (message == 0x80) {
        ++constrainedReleases;
        return 0x80;
    }
    ++constrainedCalls;
    expect(at(field::gui::pointerX, 914, 351), "native constraint callback receives the current reticle");
    if (constrainedThrow) RaiseException(0xe0001237, 0, 0, nullptr);
    return 3;
}
static void constrainedOwnershipTests() {
    for (int mode : {0, 1}) {
        beginWorldHold(mode);
        *(unsigned*)(dragObject + 8) = 0x10000;
        *(uintptr_t*)(dragObject + 0x18) = viewObject;
        const float primary[4] = {1, 0, 0, 30}, secondary[4] = {0, 1, 0, 50};
        std::memcpy((void*)(dragObject + 0xe8), primary, 16);
        std::memcpy((void*)(dragObject + 0xf8), secondary, 16);
        heldRayOrigin[1] = 100;
        heldRayDirection[1] = 0;
        heldRayDirection[2] = 1;
        doorDistance = 120;
        lastDoorObject = 0;
        point(field::gui::pointerX)[0] = 10;
        point(field::gui::pointerX)[1] = 20;
        originalHingeHandler = nativeConstrained;
        constrainedCalls = constrainedReleases = 0;
        constrainedThrow = false;
        hingeHandler(dragObject, guiObject, 0);
        expect(constrainedCalls == 1 && at(field::gui::pointerX, 10, 20),
               "constraint callback borrows and restores native GUI coordinates");
        *(uintptr_t*)gameAt(fp::sig::focusedViewport) = viewObject;
        navThread = 0;
        heldDoorStep();
        expect(constrainedCalls == 2 && at(field::gui::pointerX, 10, 20),
               "frame-driven native constraint update follows the reticle and restores GUI coordinates");
        for (int stop = 0; stop < 5; ++stop) {
            cursorFree = stop == 0;
            windowFree = stop == 1;
            fixtureForeground = stop != 2;
            *(int*)gameAt(fp::sig::uiOpenCount) = stop == 3;
            reticleClear = stop == 4 ? 0 : 1;
            hingeHandler(dragObject, guiObject, 0);
        }
        expect(constrainedCalls == 2,
               "F8, UI, focus loss and a blocked reticle cannot drive constrained targets");
        hingeHandler(dragObject, guiObject, 0x80);
        expect(constrainedReleases == 1,
               "native constrained release remains available while gameplay targeting is blocked");
        cursorFree = windowFree = 0;
        fixtureForeground = true;
        *(int*)gameAt(fp::sig::uiOpenCount) = 0;
        reticle();
        constrainedThrow = true;
        bool caught = false;
        __try {
            hingeHandler(dragObject, guiObject, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            caught = true;
        }
        expect(caught && at(field::gui::pointerX, 10, 20),
               "faulting native constraint callbacks restore GUI ownership");
        constrainedThrow = false;
    }
}

static void cursorOwnershipTests() {
    for (unsigned gate = 0; gate < 7; ++gate) {
        reset(); swinging = true; hideSwingCrosshair = true;
        aim.valid = true; aim.root = filterRoot = actor; aim.head = filterHead = headObject;
        switch (gate) {
        case 0: viewMode = 2; break;
        case 1: active = 0; break;
        case 2: cameraReady = 0; break;
        case 3: stopping = 1; break;
        case 4: *(int*)gameAt(fp::sig::uiOpenCount) = 1; break;
        case 5: fixtureForeground = false; break;
        case 6: break; // valid owner but no saved camera
        }
        LONG x = 0, y = 0; cursorPositionFor(gameAt(fp::sig::crosshairCursorReturn), &x, &y);
        expect(x == 100 && y == 200 && nativeCursors == 1,
               "stale swing state cannot hide the native cursor outside an active rendered camera");
    }
}
// Native 118000/1183C0 place the centre at query - 600*direction + sweep*direction.
// This fixture exercises that downstream math (the old query-only spy did not),
// a player-head sphere, a finite wall and native accepted-pose rollback. It is
// an offline geometric reproduction, not execution of the game's solver.
static uintptr_t placementSelf = 0;
static bool placementWall = false, placementFault = false, placementInventory = false;
static float sweepStart[3]{}, placedCentre[3]{};
static unsigned physicalPlacements = 0;
static float placementCollision(uintptr_t, const float* d, uintptr_t, uintptr_t pose) {
    if (placementFault) RaiseException(0xe0001240, 0, 0, nullptr);
    const float* start = (const float*)pose + 9;
    float best = 1000.f;
    if (*(unsigned*)(placementSelf + 0xc) & 0x200000) {
        const float v[3] = {start[0], start[1] - 160.f, start[2]};
        const float b = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
        const float c = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - 15.f * 15.f;
        const float disc = b * b - c;
        if (c <= 0.f)
            best = 0.f;
        else if (disc >= 0.f && -b - std::sqrt(disc) >= 0.f)
            best = -b - std::sqrt(disc);
    }
    // Front face of a brick stops 5 units before a wall at z=80.
    if (placementWall && d[2] > 0.f) {
        const float wall = (75.f - start[2]) / d[2];
        if (wall >= 0.f) best = std::fmin(best, wall);
    }
    return best;
}
static bool nativePhysicalPlace(uintptr_t, uintptr_t viewport, uintptr_t query) {
    ++physicalPlacements;
    const float* q = (const float*)query;
    float scale = -600.f;
#if FP_REVISION >= 15
    scale = heldPlacementStart(base + (placementInventory ? 0x118527 : 0x118073), q, q + 3, scale);
    expect(heldPlacementStart(base + (placementInventory ? 0x1187ce : 0x1182c2), q, q + 3, -600.f) == scale,
           "recursive held-body setup uses the same bounded start as its root");
    expect(heldPlacementStart(base + 0x123456, q, q + 3, -600.f) == -600.f,
           "unrelated native vector calls retain their scalar");
#endif
    for (int i = 0; i < 3; ++i) sweepStart[i] = q[i] + q[i + 3] * scale;
    float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, sweepStart[0], sweepStart[1], sweepStart[2]};
    float travel = 0.f;
#if FP_REVISION >= 15
    travel = heldSweepFor(base + (placementInventory ? 0x11885c : 0x11834b), bodyObject, q + 3, viewport,
                          (uintptr_t)pose);
#else
    travel = placementCollision(bodyObject, q + 3, viewport, (uintptr_t)pose);
#endif
    for (int i = 0; i < 3; ++i) placedCentre[i] = sweepStart[i] + q[i + 3] * travel;
    return true;
}
static bool placeWorld(uintptr_t query) {
#if FP_REVISION >= 15
    return heldPlace(itemObject, viewObject, query);
#else
    return nativePhysicalPlace(itemObject, viewObject, query);
#endif
}
#if FP_REVISION >= 15
static bool placementThrows(uintptr_t query) {
    __try {
        placeWorld(query);
    } __except (GetExceptionCode() == 0xe0001240 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
#endif
static void physicalPlacementTests() {
    placementSelf = base + 0x5e0000;
    for (int mode : {0, 1})
        for (float pitch : {-0.8f, 0.f, 0.6f}) {
            beginWorldHold(mode);
            *(uintptr_t*)(actor + field::character::sceneNode) = placementSelf;
            *(int*)(placementSelf + field::sceneNode::lastChild) = -1;
            *(unsigned*)(placementSelf + 0xc) = 0x200040;
            heldRayDirection[1] = pitch;
            heldRayDirection[2] = std::sqrt(1.f - pitch * pitch);
            if (mode) heldRayOrigin[2] = -100.f; // real third-person parallax
            grabStartDistance = 180.f;
            heldNativeHit = false;
            placementWall = placementFault = placementInventory = false;
#if FP_REVISION >= 15
            originalHeldPlace = nativePhysicalPlace;
            originalHeldSweep = placementCollision;
#endif
            float q[12]{};
            expect(heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f) == 1 && placeWorld((uintptr_t)q),
                   "world query reaches native physical placement in both close views");
            bool equal = true;
            for (int i = 0; i < 3; ++i) equal &= std::fabs(placedCentre[i] - q[i]) < 0.002f;
            expect(equal, "brick stays at the requested cursor distance instead of snapping to the player's "
                          "head or overshooting");
            expect(*(unsigned*)(placementSelf + 0xc) == 0x200040,
                   "player collision-query flags are restored with unrelated bits intact");
            const float firstZ = placedCentre[2];
            holdTarget = 240.f;
            fixtureTick += 1000;
            reticle();
            heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f);
            placeWorld((uintptr_t)q);
            expect(placedCentre[2] > firstZ && std::fabs(placedCentre[2] - q[2]) < 0.002f,
                   "outward distance adjustment moves the physical item along the cursor ray");
            holdTarget = 80.f;
            fixtureTick += 1000;
            reticle();
            heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f);
            placeWorld((uintptr_t)q);
            expect(placedCentre[2] < firstZ && std::fabs(placedCentre[2] - q[2]) < 0.002f,
                   "inward distance adjustment brings the physical item closer");
        }
    beginWorldHold();
    *(uintptr_t*)(actor + field::character::sceneNode) = placementSelf;
    heldRayDirection[1] = 0.f;
    heldRayDirection[2] = 1.f;
    heldRayOrigin[2] = -1200.f;
    heldNativeHit = false;
    placementWall = false;
    float distant[12]{};
    heldQuery(itemObject, viewObject, (uintptr_t)distant, 100.f);
    placeWorld((uintptr_t)distant);
    expect(std::fabs(placedCentre[2] - distant[2]) < 0.002f,
           "maximum-distance third-person camera holds beyond the native 1000-unit sweep horizon");
    placementWall = true;
    heldQuery(itemObject, viewObject, (uintptr_t)distant, 100.f);
    placeWorld((uintptr_t)distant);
    expect(std::fabs(placedCentre[2] - 75.f) < 0.002f,
           "a native wall collision in a later sweep segment still clamps the physical item");
    heldRayOrigin[2] = 0.f;
    heldNativeHit = false;
    placementWall = true;
    float q[12]{};
    heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f);
    placeWorld((uintptr_t)q);
    expect(std::fabs(placedCentre[2] - 75.f) < 0.002f,
           "native shape collision stops the brick before a wall closer than the requested point");
#if FP_REVISION >= 15
    unsigned before = physicalPlacements;
    expect(!placeWorld((uintptr_t)q) && physicalPlacements == before,
           "a placement proposal is consumed once and cannot replay");
    heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f);
    ++holdGeneration;
    expect(!placeWorld((uintptr_t)q) && physicalPlacements == before,
           "a new grab invalidates the previous placement proposal");
    heldQuery(itemObject, viewObject, (uintptr_t)q, 100.f);
    placementFault = true;
    expect(placementThrows((uintptr_t)q) && !boundedPlacement.object &&
               *(unsigned*)(placementSelf + 0xc) == 0x200040,
           "native sweep exception restores the player mask and placement scope");
    placementFault = false;
    placementInventory = true;
    placementWall = false;
    *(unsigned*)(dragObject + field::drag::flags) = 0x1000;
    originalDragPlace = [](uintptr_t object, uintptr_t viewport, float) -> int {
        unsigned char ray[0x40]{};
        float query[6]{};
        pickRayFor(base + 0x118440, viewport, 1, 1, (uintptr_t)ray);
        memcpy(query, ray + 8, 12);
        memcpy(query + 3, ray + field::pick::direction, 12);
        return nativePhysicalPlace(object, viewport, (uintptr_t)query) ? 1 : 0;
    };
    expect(dragPlace(itemObject, viewObject, -100000.f) == 1 &&
               std::fabs(placedCentre[2] - holdApplied.load()) < 0.002f && !boundedPlacement.object,
           "inventory materialization uses the same bounded native physical placement");
    placementInventory = false;
#endif
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fixture::NativeImage storage(0x600000);
    if (!storage) return 1;
    actor = storage.object(0x5c0000); behaviorObject = storage.object(0x5c2000);
    headObject = storage.object(0x5c4000); viewObject = storage.object(0x5c6000);
    guiObject = storage.object(0x5c8000); dragObject = storage.object(0x5ca000);
    itemObject = storage.object(0x5cc000); bodyObject = storage.object(0x5ce000);
    const fixture::ControlledPlayer player{actor, behaviorObject, headObject, viewObject}; player.connect();
    fixture::bindings({'W', 'A', 'S', 'D', 'C', 'U', VK_MENU, 0, 0, 'E'});
    keyState = testKeyState; originalCursorPosition = nativeCursor; originalGrabPick = nativePick;
    originalGuiMousePress = nativePress; originalGuiMousePoll = nativePoll;
    originalQueueWork = nativeQueue; originalApplyHeldRelease = nativeApply;
    originalCameraMouse = []() -> char { ++nativeCameraCalls; return 0; };
    copyCameraSnapshots = []() { ++snapshotCopies; };
    reset(); expect(closeViewContext() && gameplayOwnsPointer(), "owned fixture enters the actual gameplay pointer adapter");
    gestureTests();
    cursorOwnershipTests();
    heldQueryTests();
    physicalPlacementTests();
    inventoryHoldDistanceTests();
    constrainedHoldTests();
    inventoryRayTests();
    constrainedOwnershipTests();
    const uintptr_t payload = (uintptr_t)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    expect(payload != 0, "owned queued-release payload allocated");
    if (payload) { queuedReleaseTests(payload); VirtualFree((void*)payload, 0, MEM_RELEASE); }
    std::printf("World interaction: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
