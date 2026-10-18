// Production fade adapter with the reviewed native marking/decay branches modeled
// in an owned process. This is not a rendered Exanima frame or GPU acceptance.
#ifdef NDEBUG
#error "The test requires assertions."
#endif
#include "first_person.cpp"
#include <thread>

static unsigned checks = 0, failures = 0, nativeUpdates = 0, nativeBoundsCalls = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) ++failures;
    printf("%s %s\n", value ? "PASS" : "FAIL", message);
}
struct MeshFade {
    unsigned flags = 0x89, query = 0x20000000, fadeFlags = 0x41, passes = 1;
    float amount = .75f, maximum = .75f, materialAlpha = .65f;
    MeshFade* parent = nullptr;
    MeshFade* child = nullptr;
};
static MeshFade pillar, parentMesh, childMesh, unrelated;
static uintptr_t mainController, otherController;
static bool throwNative = false, nested = false, stopDuringQuery = false;
static bool nestedWasNative = false, unrelatedWasNative = false;
static float origin[3] = {1, 2, 3}, segment[3] = {4, 5, 6}, bounds[4] = {7, 8, 9, 10};
static bool nativeBounds(const float* a, const float* b, const float* c) {
    ++nativeBoundsCalls;
    check(a == origin && b == segment && c == bounds,
          "forwarded native bounds query keeps all three original arguments");
    return true; // This fixture's native orbit intersects the pillar.
}
static bool queryAt(uintptr_t caller) {
#if FP_REVISION >= 16
    return fadeBoundsFor(caller, origin, segment, bounds);
#else
    (void)caller;
    return nativeBounds(origin, segment, bounds);
#endif
}
static void updateFade(uintptr_t controller);
static void nativeFade(uintptr_t controller) {
    ++nativeUpdates;
    if (controller == otherController) {
        nestedWasNative = queryAt(base + 0x90a3a);
        return;
    }
    unrelatedWasNative = queryAt(base + 0x1175ea); // shared math used outside the fade owner
    if (nested) {
        nested = false;
        updateFade(otherController);
    }
    if (throwNative) RaiseException(0xe0001250, 0, 0, nullptr);
    if (stopDuringQuery) stopping = 1;
    // Eligibility, the two broad phases and the clear branch in 90710.
    for (MeshFade* mesh : {&pillar, &unrelated}) {
        if ((mesh->flags & 0x89) != 0x89 || !(mesh->query & 0x20000000)) continue;
        const bool vertical = (mesh->query & 0x70000000) == 0x60000000;
        const bool hit = queryAt(base + (vertical ? 0x90928 : 0x90a3a));
        if (hit) {
            mesh->fadeFlags |= 1;
            if (mesh->parent) mesh->parent->fadeFlags |= 1;
            if (vertical && mesh->child) mesh->child->fadeFlags = mesh->parent->fadeFlags | 1;
        } else if (mesh->fadeFlags & 1) {
            mesh->fadeFlags &= ~1u;
            if (mesh->parent) mesh->parent->fadeFlags &= ~1u;
            if (vertical && mesh->child) mesh->child->fadeFlags = mesh->parent->fadeFlags & ~1u;
        }
    }
}
static void updateFade(uintptr_t controller) {
#if FP_REVISION >= 16
    cameraFade(controller);
#else
    nativeFade(controller);
#endif
}
static void nativeSector(MeshFade& mesh) {
    // The 58CD0 branch changes the fade amount and chooses the opaque/fade pass.
    // Fixed fixture dt: real native delta time and rasterization remain untested.
    mesh.passes = 1;
    if (mesh.fadeFlags & 1)
        mesh.amount = std::fmin(mesh.maximum, mesh.amount + .125f);
    else
        mesh.amount = std::fmax(0.f, mesh.amount - .125f);
    if (mesh.amount > 0.f) mesh.passes = mesh.amount == mesh.maximum ? 0x40000000 : 0x20000000;
}
static void arm(int mode = 0) {
    mainController = base + 0x500000;
    otherController = base + 0x501000;
    const uintptr_t viewport = base + 0x502000;
    savedView = base + 0x503000;
    savedCamera = base + 0x504000;
    savedRoot = base + 0x505000;
    savedScene = base + 0x506000;
    savedController = mainController;
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = viewport;
    *(uintptr_t*)(viewport + field::viewport::view) = savedView;
    *(uintptr_t*)(savedView + field::view::camera) = savedCamera;
    *(uintptr_t*)(savedCamera + field::camera::controller) = mainController;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = savedRoot;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = savedScene;
    viewMode = mode;
    active = cameraReady = pendingRestore = 1;
    stopping = restorationUnknown = 0;
    park.active = false;
    navThread = LONG(GetCurrentThreadId());
    pillar = {};
    parentMesh = {};
    childMesh = {};
    unrelated = {};
    pillar.parent = &parentMesh;
    pillar.child = &childMesh;
    unrelated.query = 0; // unrelated transparency is not a camera-fade candidate
    throwNative = nested = stopDuringQuery = false;
    nestedWasNative = unrelatedWasNative = false;
#if FP_REVISION >= 16
    originalCameraFade = nativeFade;
    originalFadeBounds = nativeBounds;
#endif
}
static bool caughtFadeFault() {
    __try {
        updateFade(mainController);
    } __except (GetExceptionCode() == 0xe0001250 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
int main() {
    std::vector<unsigned char> memory(0x600000);
    base = (uintptr_t)memory.data();
    for (int mode : {0, 1})
        for (unsigned type : {0x20000000u, 0x30000000u, 0x60000000u}) {
            arm(mode);
            pillar.query = type;
            const unsigned before = nativeUpdates;
            updateFade(mainController);
            check(nativeUpdates == before + 1, "native fade owner still runs once in close views");
            check(!(pillar.fadeFlags & 1) && !(parentMesh.fadeFlags & 1),
                  "both close views clear native pillar and propagated parent fade requests");
            if (type == 0x60000000)
                check(!(childMesh.fadeFlags & 1),
                      "native vertical-fade cleanup clears propagated child request");
            check(unrelatedWasNative && unrelated.fadeFlags == 0x41 && unrelated.amount == .75f &&
                      pillar.materialAlpha == .65f && pillar.maximum == .75f,
                  "unrelated bounds queries, transparency, material alpha and fade limit remain untouched");
            for (int i = 0; i < 8; ++i) {
                updateFade(mainController);
                nativeSector(pillar);
            }
            check(pillar.amount == 0.f && pillar.passes == 1,
                  "already translucent pillar recovers through native decay into the opaque pass");
            viewMode = 2;
            updateFade(mainController);
            nativeSector(pillar);
            check((pillar.fadeFlags & 1) && pillar.amount > 0.f && pillar.passes != 1,
                  "switch to overhead immediately restores native obstruction fading");
            viewMode = mode;
            updateFade(mainController);
            check(!(pillar.fadeFlags & 1), "switch back to a close view clears the overhead request again");
        }
    for (int gate = 0; gate < 13; ++gate) {
        arm();
        switch (gate) {
        case 0:
            viewMode = 2;
            break;
        case 1:
            active = 0;
            break;
        case 2:
            stopping = 1;
            break;
        case 3:
            cameraReady = 0;
            break;
        case 4:
            park.active = true;
            break;
        case 5:
            restorationUnknown = 1;
            break;
        case 6:
            *(uintptr_t*)gameAt(fp::sig::playerRoot) = savedRoot + 1;
            break;
        case 7:
            *(uintptr_t*)gameAt(fp::sig::currentScene) = savedScene + 1;
            break;
        case 8:
            *(uintptr_t*)(savedView + field::view::camera) = 0;
            break;
        case 9:
            *(uintptr_t*)(savedCamera + field::camera::controller) = otherController;
            break;
        case 10:
            savedController = otherController;
            break;
        case 11:
            navThread = 0;
            break;
        case 12:
            *(uintptr_t*)gameAt(fp::sig::mainViewport) = 1;
            break; // read fault
        }
        updateFade(mainController);
        check(pillar.fadeFlags == 0x41,
              "overhead, disabled or mismatched camera retains native fade behavior");
    }
    arm();
    inputEnabled = 0;
    cursorFree = windowFree = 1;
    *(int*)gameAt(fp::sig::uiOpenCount) = 1;
    updateFade(mainController);
    check(!(pillar.fadeFlags & 1),
          "F8, inventory and focus release do not restore overhead fading to a close-view camera");
    arm();
    nested = true;
    updateFade(mainController);
    check(nestedWasNative && !(pillar.fadeFlags & 1),
          "nested other-camera update uses native policy and restores the outer close-view scope");
    arm();
    stopDuringQuery = true;
    updateFade(mainController);
    check(pillar.fadeFlags == 0x41,
          "stop during native traversal immediately restores native obstruction tests");
    arm();
    throwNative = true;
    check(caughtFadeFault(), "native fade exception propagates to its caller");
    check(queryAt(base + 0x90a3a), "native exception restores scope for later calls");
#if FP_REVISION >= 16
    check(!clearCameraObstructions, "exception leaves no borrowed fade policy");
    arm();
    AcquireSRWLockExclusive(&cameraLock);
    updateFade(mainController);
    ReleaseSRWLockExclusive(&cameraLock);
    check(pillar.fadeFlags == 0x41, "unexpected call under camera lock remains native without deadlocking");
    arm();
    clearCameraObstructions = true;
    bool otherThreadHit = false;
    std::thread other([&] { otherThreadHit = queryAt(base + 0x90a3a); });
    other.join();
    clearCameraObstructions = false;
    check(otherThreadHit, "close-view scope cannot leak to another thread's query");
#endif
    printf("Native fade: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
