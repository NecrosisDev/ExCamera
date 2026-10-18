// Calls the production input, camera snapshot and TP-pick publication owners
// with synthetic native memory and deterministic window/pick boundaries.
// This proves command/sample ownership, not live occlusion or control feel.
#ifdef NDEBUG
#error "The tests check behavior; build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int tpFixtureWidth = 800, tpFixtureHeight = 600;
static POINT tpFixtureCursor{400, 300};
static bool tpFixtureForeground = true;
static HWND tpFixtureWindow() {
    return tpFixtureForeground ? (HWND)1 : nullptr;
}
static DWORD tpFixtureWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
static BOOL tpFixtureClientRect(HWND, RECT* r) {
    *r = {0, 0, tpFixtureWidth, tpFixtureHeight};
    return TRUE;
}
static BOOL tpFixtureCursorPos(POINT* p) {
    *p = tpFixtureCursor;
    return TRUE;
}
static BOOL tpFixtureSetCursor(int x, int y) {
    tpFixtureCursor = {x, y};
    return TRUE;
}
static BOOL tpFixtureClientScreen(HWND, POINT*) {
    return TRUE;
}
static HMONITOR tpFixtureMonitor(POINT, DWORD) {
    return (HMONITOR)1;
}
static int tpFixtureMap(HWND, HWND, POINT*, UINT) {
    return 0;
}
static BOOL tpFixtureClip(const RECT*) {
    return TRUE;
}
#define GetForegroundWindow tpFixtureWindow
#define GetWindowThreadProcessId tpFixtureWindowPid
#define GetClientRect tpFixtureClientRect
#define GetCursorPos tpFixtureCursorPos
#define SetCursorPos tpFixtureSetCursor
#define ClientToScreen tpFixtureClientScreen
#define MonitorFromPoint tpFixtureMonitor
#define MapWindowPoints tpFixtureMap
#define ClipCursor tpFixtureClip
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetClientRect
#undef GetCursorPos
#undef SetCursorPos
#undef ClientToScreen
#undef MonitorFromPoint
#undef MapWindowPoints
#undef ClipCursor

static unsigned tpChecks = 0, tpFailures = 0, tpNativeCalls = 0;
static uintptr_t tpRoot = 0, tpBehavior = 0, tpHead = 0, tpMesh = 0, tpSector = 0, tpWorldScene = 0,
                 tpCamera = 0, tpView = 0;
static int tpDuringPick = 0, tpPickResult = 1;
static float tpPickX = 0, tpPickY = 0;
static float tpLastNativePoint[3]{};
static void checkTp(bool ok, const char* label) {
    ++tpChecks;
    if (!ok) {
        ++tpFailures;
        std::printf("FAIL: %s\n", label);
    }
}
static void tickTp(int dx = 0, int dy = 0) {
    tpFixtureCursor = {tpFixtureWidth / 2 + dx, tpFixtureHeight / 2 + dy};
    *(unsigned*)gameAt(fp::sig::gameClock) += 16;
    updateAim(tpBehavior);
}
static void tpRay(float* direction) {
    float orientation[12]{};
    fp::basis(aim.yaw, aim.pitch, orientation);
    fp::cursorRay(orientation, tpAimFree.load() ? tpAimX.load() : 0.f, tpAimFree.load() ? tpAimY.load() : 0.f,
                  float(tpFixtureHeight), float(tpFov.load() > 0 ? tpFov.load() : selectedFov.load()),
                  direction);
}
static bool tpFreshHeading() {
    float pose[12]{}, direction[3];
    fp::basis(aim.yaw, aim.pitch, pose);
    tpRay(direction);
    return std::fabs(fp::wrapPi(aim.rayYaw - fp::cursorHeading(pose, direction))) < 1e-6f;
}
static bool tpEyeFallback() {
    float direction[3];
    tpRay(direction);
    for (int i = 0; i < 3; ++i)
        if (std::fabs(aim.point[i] - (aim.origin[i] + direction[i] * 1000.f)) > 0.002f) return false;
    return true;
}
static void tpMutate(int which) {
    switch (which) {
    case 1:
        tickTp(65, -25);
        break;
    case 2:
        releaseInput();
        break;
    case 3:
        inputOwner.policyChanged();
        break;
    case 4:
        selectedFov = 95;
        break;
    case 5:
        tpFixtureWidth += 100;
        break;
    case 6:
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = tpRoot + 0x10000;
        break;
    case 7:
        *(uintptr_t*)(tpRoot + field::character::headPart) = tpHead + 0x1000;
        break;
    case 8:
        *(uintptr_t*)gameAt(fp::sig::currentScene) = tpWorldScene + 0x1000;
        break;
    case 9:
        *(uintptr_t*)(tpMesh + field::sceneNode::sector) = tpSector + 0x1000;
        break;
    case 10:
        viewMode = 0;
        break;
    case 11:
        active = 0;
        break;
    case 12:
        stopping = 1;
        break;
    case 13:
        aim.valid = false;
        break;
    case 14:
        tpCombatAimFree = false;
        break;
    case 15:
        tpFixtureForeground = false;
        break;
    case 20:
        tickTp();
        checkTp(tpAimSet && !memcmp(aim.point, tpAimLocal, 12),
                "input during a valid refresh retains matching depth until replacement");
        break;
    case 17:
        *(uintptr_t*)(tpView + field::viewport::pickCamera) = tpCamera + 0x2000;
        break;
    case 18:
        *(uintptr_t*)gameAt(fp::sig::mainViewport) = tpView + 0x2000;
        break;
    case 19:
        *(uintptr_t*)(tpView + 0x1000 + field::view::camera) = tpCamera + 0x2000;
        break;
    case 16:
        // Same geometry again after two real input updates: an old in-flight
        // query must not alias a newer command merely because its floats match.
        tickTp(50, 0);
        tickTp(-50, 0);
        break;
    }
}
static char tpNativePick(uintptr_t viewport, float x, float y, uintptr_t out) {
    checkTp(viewport == tpView, "native pick receives viewport, never camera");
    if (viewport != tpView) return 0;
    const uintptr_t camera = *(uintptr_t*)(viewport + field::viewport::pickCamera);
    ++tpNativeCalls;
    tpPickX = x;
    tpPickY = y;
    const float* pose = (const float*)(camera + field::camera::pose);
    float direction[3];
    fp::cursorRay(pose, x - float(tpFixtureWidth) * 0.5f, y - float(tpFixtureHeight) * 0.5f,
                  float(tpFixtureHeight), *(float*)(camera + field::camera::fov), direction);
    float origin[3];
    for (int i = 0; i < 3; ++i) origin[i] = pose[9 + i] + direction[i] * 3.f;
    memcpy((void*)(out + field::pick::origin), origin, 12);
    memcpy((void*)(out + field::pick::direction), direction, 12);
    *(float*)(out + field::pick::distance) = 650.f;
    for (int i = 0; i < 3; ++i)
        tpLastNativePoint[i] =
            origin[i] + direction[i] * 650.f - *(float*)(tpSector + field::sector::origin + i * 4);
    const int change = tpDuringPick;
    tpDuringPick = 0;
    if (change) tpMutate(change);
    if (tpPickResult == -1) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (tpPickResult == -2) memset((void*)(out + field::pick::direction), 0, 12);
    if (!tpPickResult) *(float*)(out + field::pick::distance) = INFINITY;
    return tpPickResult == 1;
}
static void sampleTp(int beforePick = 0) {
    float yaw = 0, pitch = 0;
#ifdef EXCAMERA_PRE_REVIEW
    // The original camera owner copied just these two fields. The original
    // thirdPersonAim then read the independent box atomics at query time.
    AcquireSRWLockShared(&aimLock);
    if (aim.valid && aim.root == tpRoot && aim.head == tpHead) {
        yaw = aim.yaw;
        pitch = aim.pitch;
    }
    ReleaseSRWLockShared(&aimLock);
#else
    const TpAimRequest request = captureTpAimRequest(tpRoot, tpHead, tpSector, yaw, pitch);
#endif
    float eye[3], pose[12]{};
    fp::eyePoint((const float*)(tpHead + field::bodyPart::pose), eye, float(eyeSetback.load()));
    for (int i = 0; i < 3; ++i) eye[i] += *(float*)(tpSector + field::sector::origin + i * 4);
    auto clear = [](const float*, const float*, float length, bool) {
        return length;
    };
    thirdPerson.update(eye, yaw, pitch, 250.f, 40.f, 15.f, 0.016f, clear, pose, false);
    memcpy((void*)(tpCamera + field::camera::pose), pose, 48);
    memcpy((void*)(tpCamera + field::camera::pose2), pose, 48);
    *(float*)(tpCamera + field::camera::fov) = float(tpFov.load() > 0 ? tpFov.load() : selectedFov.load());
    if (beforePick) tpMutate(beforePick);
#ifdef EXCAMERA_PRE_REVIEW
    thirdPersonAim(tpCamera, pose, eye, tpSector);
#else
    thirdPersonAim(tpCamera, pose, eye, tpSector, request);
#endif
}
static void resetTp() {
    memset((void*)tpRoot, 0, 0x1400);
    memset((void*)tpBehavior, 0, 0xA00);
    memset((void*)tpHead, 0, 0x1000);
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = tpRoot;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = tpWorldScene;
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = tpView;
    *(uintptr_t*)(tpView + field::viewport::pickCamera) = tpCamera;
    *(uintptr_t*)(tpView + field::viewport::view) = tpView + 0x1000;
    *(uintptr_t*)(tpView + 0x1000 + field::view::camera) = tpCamera;
    *(int*)(tpView + field::viewport::width) = 800;
    *(int*)(tpView + field::viewport::height) = 600;
    *(uintptr_t*)gameAt(fp::sig::uiView) = tpView;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = 0;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(uintptr_t*)(tpRoot + field::character::behaviour) = tpBehavior;
    *(uintptr_t*)(tpRoot + field::character::headPart) = tpHead;
    *(unsigned char*)(tpRoot + field::character::combatStance) = 1;
    *(uintptr_t*)(tpBehavior + field::behaviour::character) = tpRoot;
    *(uintptr_t*)(tpBehavior + field::behaviour::view) = tpView;
    *(uintptr_t*)(tpHead + field::bodyPart::meshNode) = tpMesh;
    *(uintptr_t*)(tpHead + field::bodyPart::character) = tpRoot;
    *(uintptr_t*)(tpMesh + field::sceneNode::sector) = tpSector;
    const float facing[3] = {0, 0, 1}, origin[3] = {10000.f, 500.f, -20000.f};
    memcpy((void*)(tpRoot + field::character::facing), facing, 12);
    memcpy((void*)(tpSector + field::sector::origin), origin, 12);
    float* physical = (float*)(tpHead + field::bodyPart::pose);
    fp::basis(0, 0, physical);
    physical[9] = 100.f;
    physical[10] = 170.f;
    physical[11] = 200.f;
    active = cameraReady = inputEnabled = 1;
    stopping = cursorFree = rmbFree = windowFree = mouseCaptured = cursorClipped = rawRegistered = 0;
    viewMode = 1;
    viewRoot = 0;
    hookedWindow = (HWND)1;
    tpFixtureForeground = true;
    tpFixtureWidth = 800;
    tpFixtureHeight = 600;
    tpFixtureCursor = {400, 300};
    tpCombatAimFree = true;
    tpDeadzoneX = tpDeadzoneY = 20;
    tpFov = 0;
    selectedFov = 75;
    eyeSetback = 0;
    sensitivity = 0.0025f;
    heldRotating = leaning = false;
    inputClock = ~0u;
    inputOwner.releaseGameplay();
    aim = {};
    tpAimSet = false;
    tpAimBox = {};
    tpAimX = tpAimY = 0;
    thirdPerson = {};
    thirdPerson.lag = 40.f;
    originalPickRay = tpNativePick;
    tpDuringPick = 0;
    tpPickResult = 1;
    tickTp();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    tpRoot = base + 0x5C0000;
    tpBehavior = base + 0x5C2000;
    tpHead = base + 0x5C4000;
    tpMesh = base + 0x5C5000;
    tpSector = base + 0x5C6000;
    tpWorldScene = base + 0x5C7000;
    tpCamera = base + 0x5C8000;
    tpView = base + 0x5CC000;

    resetTp();
    sampleTp();
    checkTp(tpAimSet, "current camera query publishes a usable TP hit");
    float stationary[3];
    memcpy(stationary, tpAimLocal, 12);
    checkTp(!memcmp(stationary, tpLastNativePoint, 12),
            "native hit is kept in the player's scene-local frame");
    const unsigned firstRevision = aim.revision;
    for (int i = 0; i < 8; ++i) tickTp();
    checkTp(aim.revision == firstRevision + 8 && !memcmp(aim.point, stationary, 12),
            "stationary input reuses a matching hit across advancing input-tick revisions");
    checkTp(tpFreshHeading(), "matching world hit does not replace current input ray heading");

    tickTp(60, -30);
    checkTp(tpFreshHeading(), "new free-box input immediately keeps its own ray heading");
    checkTp(tpEyeFallback() && !tpAimSet, "older TP hit cannot replace a newer input ray point");
    sampleTp();
    checkTp(tpAimSet && tpPickX == 460.f && tpPickY == 270.f,
            "camera pick uses the captured box coordinates for the new command");
    float matched[3];
    memcpy(matched, tpAimLocal, 12);
    tickTp();
    checkTp(!memcmp(aim.point, matched, 12) && tpFreshHeading(),
            "next unchanged sample adopts the newly matched world hit without changing heading");

    for (int setting = 0; setting < 7; ++setting) {
        resetTp();
        tickTp(40, -20);
        sampleTp();
        if (setting == 0) aim.targetYaw += 0.4f;
        if (setting == 1) aim.targetPitch += 0.25f;
        if (setting == 2) selectedFov = 105;
        if (setting == 3) tpFixtureHeight = 700;
        if (setting == 4) tpCombatAimFree = false;
        if (setting == 5) inputOwner.policyChanged();
        if (setting == 6) releaseInput();
        tickTp();
        checkTp(tpFreshHeading() && tpEyeFallback() && !tpAimSet,
                "look/pitch/FOV/size/free-mode/policy/recapture changes invalidate the cached hit");
    }

    for (int change = 1; change <= 19; ++change) {
        resetTp();
        tpDuringPick = change;
        sampleTp();
        checkTp(!tpAimSet, "input or owner change during native pick rejects late publication");
        if (change == 1 || change == 16)
            checkTp(tpFreshHeading() && tpEyeFallback(),
                    "late camera completion preserves the newer input's heading and fallback point");
    }

    resetTp();
    const unsigned before = tpNativeCalls;
    sampleTp(1);
    checkTp(!tpAimSet && tpNativeCalls == before,
            "input after camera snapshot is rejected before querying with mixed pose and box");
    checkTp(tpFreshHeading() && tpEyeFallback(), "rejected old snapshot does not disturb new input");

    resetTp();
    tpCombatAimFree = false;
    tickTp();
    sampleTp();
    checkTp(tpAimSet && tpPickX == 400.f && tpPickY == 300.f,
            "locked TP aim still uses the camera-center native ray");
    tickTp();
    checkTp(!memcmp(aim.point, tpAimLocal, 12) && tpFreshHeading(),
            "locked TP aim accepts its own coherent hit");

    resetTp();
    tickTp(60, -30);
    originalPickRay = nullptr;
    sampleTp();
    tickTp();
    checkTp(!tpAimSet && tpEyeFallback(),
            "missing native pick reuses input owner fallback, no second reconstructed ray");
    resetTp();
    sampleTp();
    tpDuringPick = 20;
    sampleTp();
    checkTp(tpAimSet, "refresh publishes after interleaved unchanged input");
    for (int result : {0, -1, -2}) {
        for (bool free : {false, true}) {
            resetTp();
            tpCombatAimFree = free;
            tickTp(30, -20);
            sampleTp();
            checkTp(tpAimSet, "fresh native depth exists before refresh");
            tpPickResult = result;
            sampleTp();
            tickTp();
            if (result) {
                checkTp(!tpAimSet && tpEyeFallback(), "failed/invalid refresh drops previous depth");
            } else {
                checkTp(tpAimSet, "native miss retains finite forward depth on native near ray");
                const float* pose = (float*)(tpCamera + field::camera::pose);
                float direction[3];
                fp::cursorRay(pose, free ? 30.f : 0.f, free ? -20.f : 0.f, 600.f, 75.f, direction);
                bool finiteForward = true;
                for (int i = 0; i < 3; ++i) {
                    const float expected = pose[9 + i] + direction[i] * 1003.f -
                                           *(float*)(tpSector + field::sector::origin + i * 4);
                    finiteForward = finiteForward && std::isfinite(tpAimLocal[i]) &&
                                    std::fabs(tpAimLocal[i] - expected) < .003f;
                }
                checkTp(finiteForward, "native miss ignores horizontal-plane infinity in either aim mode");
            }
        }
    }
    for (int change : {17, 18, 19}) {
        resetTp();
        sampleTp();
        tpMutate(change);
        sampleTp();
        tickTp();
        checkTp(!tpAimSet && tpEyeFallback(), "camera/viewport handoff cannot reuse old gaze depth");
    }

    std::printf("TP aim freshness: %u checks, %u failures, %u native picks.\n", tpChecks, tpFailures,
                tpNativeCalls);
    return tpFailures ? 1 : 0;
}
