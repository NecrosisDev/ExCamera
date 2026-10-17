// Calls the production grab, placement, rotation and release owners in an owned process.
// Time and native game calls are fixtures; no game process or gameplay claim.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static ULONGLONG fixtureTick = 1000;
static ULONGLONG fixtureNow() { return fixtureTick; }
#define GetTickCount64 fixtureNow
#include "first_person.cpp"
#undef GetTickCount64
#include <vector>

static int checks = 0, failures = 0;
static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) ++failures;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
}
static float fixturePickDistance = 1000.f;
static int nativeDrops = 0, nativeDoorReleases = 0;
static bool failPlacement = false;
static void ray(uintptr_t out, float distance) {
    std::memset((void*)out, 0, 0x34);
    ((float*)(out + field::pick::direction))[2] = 1.f;
    ((float*)(out + 8))[2] = distance;
    *(float*)(out + field::pick::distance) = distance;
}
static char nativeRay(uintptr_t, float, float, uintptr_t out) {
    ray(out, fixturePickDistance);
    return 1;
}
static int nativePlace(uintptr_t, uintptr_t camera, float) {
    if (failPlacement) RaiseException(0xe0420001, 0, 0, nullptr);
    alignas(8) unsigned char out[0x40]{};
    pickRay(camera, 0, 0, (uintptr_t)out);
    // The native placement may preserve the existing shape while translating it. Individual tests
    // install a new native shape to model release, settling and a subsequent grab.
    return 1;
}
static void nativeDrop(uintptr_t drag) {
    ++nativeDrops;
    const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
    const uintptr_t body = object ? *(uintptr_t*)(object + field::sceneNode::bodyPart) : 0;
    if (body) {
        std::memcpy((void*)(body + field::bodyPart::points), (void*)(drag + field::drag::targetPoints), 48);
        std::memcpy((void*)(body + field::bodyPart::history), (void*)(body + field::bodyPart::points), 48);
        std::memcpy((void*)(body + field::bodyPart::history2), (void*)(body + field::bodyPart::points), 48);
    }
    *(unsigned*)(drag + 8) = 0;
    *(uintptr_t*)(drag + field::drag::object) = 0;
}
static unsigned long long nativeDoor(uintptr_t, uintptr_t, int message) {
    if (message == 0x80) ++nativeDoorReleases;
    return 1;
}
static void place(uintptr_t object) { dragPlace(object, 0, -100000.f); }
static bool placementFaultCaught(uintptr_t object) {
    __try {
        place(object);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    const uintptr_t drag = base + 0x5b0000, object = base + 0x5b1000, body = base + 0x5b2000;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = drag;
    *(uintptr_t*)(object + field::sceneNode::bodyPart) = body;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = base + 0x5b3000;
    originalPickRay = nativeRay;
    originalDragPlace = nativePlace;
    originalDragDrop = nativeDrop;
    originalDoorHandler = nativeDoor;
    originalHingeHandler = nativeDoor;
    active = 1;
    stopping = 0;
    inputEnabled = 1;
    cameraReady = 1;
    viewMode = 0;
    grabRange = 240;
    viewYaw = viewPitch = 0.f;
    for (int i = 0; i < 3; ++i) eyeGlobal[i] = lastHeadGlobal[i] = 0.f;
    auto begin = [&](float distance) {
        *(unsigned*)(drag + 8) = 0x2000;
        *(uintptr_t*)(drag + field::drag::object) = object;
        alignas(8) unsigned char out[0x40]{};
        ray((uintptr_t)out, distance);
        return grabPickResult(gameAt(fp::sig::grabStartPickReturn), 1, (uintptr_t)out);
    };
    auto* pts = (float(*)[3])(drag + field::drag::targetPoints);
    const float original[4][3] = {{10, 0, 0}, {0, 10, 0}, {0, 0, 10}, {-5, -5, -5}};

    // The F6 blend deliberately retains active/inputEnabled/cameraReady. The selected view still wins.
    viewMode = 2;
    const unsigned refused = grabsRefused.load();
    check(begin(900.f) == 1 && grabsRefused.load() == refused,
          "overhead blend: a far native grab remains accepted");
    alignas(8) unsigned char hover[0x40]{};
    ray((uintptr_t)hover, 900.f);
    hoverOutOfRange = false;
    check(grabPickResult(gameAt(fp::sig::hoverPickReturn), 1, (uintptr_t)hover) == 1 &&
              !hoverOutOfRange.load(),
          "overhead blend: native hover result and crosshair range state are preserved");
    active = 0;
    check(begin(900.f) == 1, "settled overhead: native grab remains accepted");
    active = 1;
    for (int mode : {0, 1}) {
        viewMode = mode;
        check(begin(900.f) == 0 && begin(100.f) == 1,
              mode ? "third person still enforces grab reach" : "first person still enforces grab reach");
    }
    viewMode = 0;

    begin(100.f);
    std::memcpy(pts, original, 48);
    heldTurnX = 100.f;
    place(object);
    check(std::memcmp(pts, original, 48) != 0, "a held mouse turn rotates the native target points");
    float firstTurn[4][3];
    std::memcpy(firstTurn, pts, 48);
    fixtureTick += 16;
    place(object);
    bool stable = true;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i) stable &= std::fabs(firstTurn[k][i] - pts[k][i]) < 1e-3f;
    check(stable, "the same grab preserves one rotation without compounding it");
    pushSpeed = 0.f;
    dragDrop(drag);
    fixtureTick += 16;
    // The released body settled to a different native pose. It remains the same scene-node address.
    begin(100.f);
    std::memcpy(pts, original, 48);
    place(object);
    check(std::memcmp(pts, original, 48) == 0,
          "same-item re-grab starts from its current native pose with no old rotation");
    heldTurnX = 25.f;
    pushSpeed = 500.f;
    pushAt = fixtureTick;
    begin(100.f);
    check(heldTurnX.load() == 0.f && pushSpeed.load() == 0.f && pushAt.load() == 0,
          "new grab clears queued old mouse movement and old fling velocity");

    // No wheel movement in either grab: distance changes only because the same object was re-grabbed.
    std::memcpy(pts, original, 48);
    fixtureTick += 16;
    begin(100.f);
    place(object);
    fixtureTick += 16;
    begin(200.f);
    place(object);
    check(pushSpeed.load() == 0.f,
          "quick same-item re-grab at a different distance does not create a push");
    physicsStep = 1.f / 60.f;
    const unsigned flung = flings.load();
    dragDrop(drag);
    check(flings.load() == flung &&
              !std::memcmp((void*)(body + field::bodyPart::points),
                           (void*)(body + field::bodyPart::history), 48),
          "release after that re-grab adds no phantom fling");
    begin(100.f);
    place(object);
    fixtureTick += 16;
    holdApplied = holdTarget = 130.f;
    place(object);
    check(pushSpeed.load() > 1.f, "real outward movement still produces a push");

    // Switching to overhead before the next placement must not use the remembered close-view fling.
    for (int mode : {2, 0}) {
        viewMode = mode;
        stopping = mode == 0 ? 1 : 0;
        *(unsigned*)(drag + 8) = 0x2000;
        *(uintptr_t*)(drag + field::drag::object) = object;
        holdObject = object;
        pushSpeed = 300.f;
        pushAt = fixtureTick;
        pushDir[0] = pushDir[1] = 0.f;
        pushDir[2] = 1.f;
        const unsigned before = flings.load();
        dragDrop(drag);
        check(flings.load() == before &&
                  !std::memcmp((void*)(body + field::bodyPart::points),
                               (void*)(body + field::bodyPart::history), 48),
              mode ? "overhead blend: release leaves the native body history unchanged"
                   : "stopping: release leaves the native body history unchanged");
        *(unsigned*)(drag + 8) = 0x4000;
        *(uintptr_t*)(drag + field::drag::object) = object;
        const int doorBefore = nativeDoorReleases, dropBefore = nativeDrops;
        dragDrop(drag);
        check(nativeDoorReleases == doorBefore && nativeDrops == dropBefore + 1,
              mode ? "overhead blend: door release uses the native drop owner"
                   : "stopping: door release uses the native drop owner");
    }
    viewMode = 0;
    stopping = 0;
    begin(100.f);
    std::memcpy(pts, original, 48);
    heldTurnX = 60.f;
    place(object);
    viewMode = 2;
    place(object);
    std::memcpy(pts, original, 48);
    viewMode = 0;
    place(object);
    check(std::memcmp(pts, original, 48) == 0,
          "return from overhead uses the held object's current native pose");
    failPlacement = true;
    const bool caught = placementFaultCaught(object);
    check(caught && placingThread == 0,
          "handled native placement fault restores the placement-pick scope");
    failPlacement = false;
    // Hide-list publication: native work runs outside the lock, the scene travels with the list,
    // and a handled mesh fault releases the read lock before clearing the list exclusively.
    const uintptr_t sector = base + 0x5b4000, mesh = base + 0x5b5000, list = base + 0x5b6000;
    *(int*)(sector + field::sector::lastMesh) = 0;
    *(uintptr_t*)(sector + field::sector::meshes) = list;
    *(uintptr_t*)list = mesh;
    *(unsigned*)(mesh + field::sceneNode::passMask) = 0x10000001;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = 1;
    originalSectorUpdate = [](uintptr_t) -> uintptr_t { return 9; };
    hideAdd(mesh);
    hideAdd(mesh);
    hidePublish();
    check(hideMeshCount == 1 && sectorUpdate(sector) == 9 &&
              *(unsigned*)(mesh + field::sceneNode::passMask) == fp::hiddenPassMask(0x10000001),
          "published hide list deduplicates and masks the current scene");
    *(uintptr_t*)gameAt(fp::sig::currentScene) = 2;
    *(unsigned*)(mesh + field::sceneNode::passMask) = 0x10000001;
    sectorUpdate(sector);
    check(*(unsigned*)(mesh + field::sceneNode::passMask) == 0x10000001,
          "a hidden mesh from another scene remains native");
    hideAdd(mesh);
    hidePublish();
    *(uintptr_t*)(sector + field::sector::meshes) = 0;
    const uintptr_t faultResult = sectorUpdate(sector);
    const bool unlocked = TryAcquireSRWLockExclusive(&hideLock) != 0;
    if (unlocked) ReleaseSRWLockExclusive(&hideLock);
    check(faultResult == 9 && viewFault.load() == 1 && hideMeshCount == 0 && unlocked,
          "handled sector fault clears the publication and releases the hide lock");
    std::printf("HELD LIFECYCLE: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
