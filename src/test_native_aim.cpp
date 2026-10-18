// Production native target and cursor owners with distinct viewport/view/camera objects.
// Synthetic native boundaries check ownership, not game physics or rendered acceptance.
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

static unsigned checks = 0, failures = 0, calls = 0, picks = 0, planes = 0, nesting = 0;
static uintptr_t actor, bh, head, view, camera, sector, aimOther, otherMesh, otherSector, mesh;
static bool selected = true, haveHit = false, selfHit = false;
static int rayX = 0, rayY = 0;
static void stockCursor(LONG* x, LONG* y) {
    *x = 31;
    *y = 47;
}
static float distanceRatio = 0, targetLocal[3] = {0, 130, 300}, hitLocal[3] = {0, -200, 400};
static int action = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}
static bool aimNear(const float* a, const float* b, float eps = .01f) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(a[i]) || fabsf(a[i] - b[i]) > eps) return false;
    return true;
}
static void jump(uintptr_t at, const void* fn) {
    unsigned char code[12] = {0x48, 0xb8};
    memcpy(code + 2, &fn, 8);
    code[10] = 0xff;
    code[11] = 0xe0;
    DWORD old = 0;
    VirtualProtect((void*)at, 32, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, code, 12);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 12);
}
static void unproject(uintptr_t, float x, float y, float* ray) {
    rayX = int(x);
    rayY = int(y);
    ray[0] = ray[1] = ray[2] = 0;
    ray[3] = (x - 400) / 300;
    ray[4] = (300 - y) / 300;
    ray[5] = -1;
}
static uintptr_t owningRoot(uintptr_t p) {
    return p;
}
static void inverse(float* out, const float* in, const float* pose) {
    float d[3] = {in[0] - pose[9], in[1] - pose[10], in[2] - pose[11]};
    for (int i = 0; i < 3; ++i) out[i] = d[0] * pose[3 * i] + d[1] * pose[3 * i + 1] + d[2] * pose[3 * i + 2];
}
static void rayWorld(uintptr_t viewport, int x, int y, float* o, float* d) {
    float ray[6];
    unproject(viewport, float(x), float(y), ray);
    const float* p = (const float*)(camera + field::camera::pose);
    float n = 0;
    for (int i = 0; i < 3; ++i) {
        o[i] = p[9 + i];
        d[i] = p[i] * ray[3] + p[3 + i] * ray[4] + p[6 + i] * ray[5];
        n += d[i] * d[i];
    }
    n = sqrtf(n);
    for (int i = 0; i < 3; ++i) d[i] /= n;
}
static void intersect(float* out, uintptr_t viewport, int x, int y, const float* plane) {
    ++planes;
    float o[3], d[3];
    rayWorld(viewport, x, y, o, d);
    float top = plane[3], bot = 0;
    for (int i = 0; i < 3; ++i) {
        top -= plane[i] * o[i];
        bot += plane[i] * d[i];
    }
    const float t = top / bot;
    for (int i = 0; i < 3; ++i) out[i] = o[i] + d[i] * t;
    *(float*)gameAt(fp::sig::dragHitDistance) = t;
}
static char worldPick(uintptr_t v, float x, float y, uintptr_t out) {
    ++picks;
    memset((void*)out, 0, 0x38);
    float o[3], d[3];
    rayWorld(v, int(x), int(y), o, d);
    memcpy((void*)(out + 0x20), o, 12);
    memcpy((void*)(out + 0x14), d, 12);
    float p[3];
    const float* offset = (float*)(sector + field::sector::origin);
    for (int i = 0; i < 3; ++i) p[i] = hitLocal[i] + offset[i];
    memcpy((void*)(out + 8), p, 12);
    *(float*)(out + 0x2c) = 400;
    *(uintptr_t*)out = selfHit ? mesh : otherMesh;
    return haveHit ? 1 : 0;
}
static void provider(uintptr_t behavior, float* out) {
    ++calls;
    if (behavior != bh) {
        out[0] = 11;
        out[1] = 12;
        out[2] = 13;
        return;
    }
    // AE2E0 frame at the plane call, using exact Ghidra-reviewed offsets.
    alignas(16) unsigned char frame[0xc0]{};
    uintptr_t rbp = (uintptr_t)frame + sizeof frame;
    *(uintptr_t*)(rbp - 8) = behavior;
    *(float**)(rbp - 0x10) = out;
    *(unsigned char*)(rbp - 0xc0) = selected ? 0 : 1;
    *(float*)(rbp - 0xb4) = 10;
    *(float*)(rbp - 0xac) = 100 * distanceRatio * distanceRatio;
    float pointInCamera[3], chosen[3];
    const float* pose = (float*)(camera + field::camera::pose);
#if FP_REVISION >= 21
    aimWorldToLocalFor(base + 0xae47b, pointInCamera, targetLocal, pose);
#else
    inverse(pointInCamera, targetLocal, pose);
#endif
    for (int i = 0; i < 3; ++i)
        chosen[i] = pose[9 + i] + pointInCamera[0] * pose[i] + pointInCamera[1] * pose[3 + i] +
                    pointInCamera[2] * pose[6 + i];
    memcpy((void*)(rbp - 0x8c), chosen, 12);
    float* plane = (float*)(rbp - 0x80);
    plane[0] = 0;
    plane[1] = 1;
    plane[2] = 0;
    plane[3] = 130;
    if (action == 1 && !nesting) {
        ++nesting;
        float nested[3];
        point(bh, nested);
        --nesting;
    }
    if (action == 2 && !nesting) {
        ++nesting;
        float nested[3];
        point(aimOther, nested);
        --nesting;
        check(nested[0] == 11, "nested aimOther provider result retained");
    }
    if (action == 3) {
        *(unsigned char*)(base + 0x5b4460) = 1;
        RaiseException(0xe0420071, 0, 0, nullptr);
    }
    if (action == 4) viewMode = 2;
    if (action == 5) ++aim.revision;
    if (action == 6) *(int*)(base + 0x49ce50) = 777;
    if (action == 7) *(uintptr_t*)(rbp - 8) = 0; // mismatched native frame must not supply depth
    if (action == 8) { // all other callers of shared projection/plane helpers remain native
#if FP_REVISION >= 21
        float transformed[3], expected[3];
        inverse(expected, targetLocal, pose);
        aimWorldToLocalFor(base + 0x12345, transformed, targetLocal, pose);
        check(aimNear(transformed, expected), "unrelated projection caller is untouched inside aim scope");
        float foreign[4] = {0, 0, 1, pose[11] + 200}, nativePoint[3], adaptedPoint[3];
        intersect(nativePoint, view, 400, 300, foreign);
        planeHitFor(base + 0x12345, adaptedPoint, view, 400, 300, foreign);
        check(aimNear(nativePoint, adaptedPoint), "unrelated plane caller is untouched inside aim scope");
#endif
    }
    if (action == 9) *(uintptr_t*)(actor + field::character::headPart) = head + 0x1000;
    if (action == 10) *(uintptr_t*)gameAt(fp::sig::currentScene) = base + 0x860000;
    if (action == 11) inputOwner.policyChanged();
    if (action == 12) *(uintptr_t*)gameAt(fp::sig::playerRoot) = aimOther;
    if (action == 15) *(uintptr_t*)(view + field::viewport::view) = savedView + 0x1000;
    if (action == 16) *(uintptr_t*)(savedView + field::view::camera) = camera + 0x1000;
    if (action == 13) *(float*)(rbp - 0xb4) = std::numeric_limits<float>::quiet_NaN();
    if (action == 14) *(float*)(rbp - 0xac) = std::numeric_limits<float>::infinity();

    float result[3];
    planeHitFor(base + 0xae73e, result, view, *(int*)(base + 0x49ce50), *(int*)(base + 0x49ce60), plane);
    if (!selected) {
        alignas(8) unsigned char pick[0x40]{};
        *(unsigned char*)(base + 0x5b4460) = 1;
        const char hit = pickRayFor(base + 0xae7b6, view, float(*(int*)(base + 0x49ce50)),
                                    float(*(int*)(base + 0x49ce60)), (uintptr_t)pick);
        // Native overhead height gate deliberately rejects low/high points.
        if (hit && !selfHit && hitLocal[1] > 130 && hitLocal[1] < 370) memcpy(result, hitLocal, 12);
        *(unsigned char*)(base + 0x5b4460) = 0;
    }
    memcpy(out, result, 12);
    memcpy((void*)(behavior + field::behaviour::aimPoint), result, 12);
}
static void reset(int mode = 0, float pitch = 0, float distance = 0) {
    memset((void*)(base + 0x600000), 0, 0x300000);
    fixture::resetInput(mode);
    foreground = true;
    fixture::ControlledPlayer{actor, bh, head, view}.connect();
    *(uintptr_t*)(actor + field::character::sceneNode) = mesh;
    *(uintptr_t*)(mesh + field::sceneNode::sector) = sector;
    *(uintptr_t*)(aimOther + field::character::sceneNode) = otherMesh;
    *(uintptr_t*)(otherMesh + field::sceneNode::sector) = otherSector;
    *(uintptr_t*)(actor + 0xe38) = aimOther;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = base + 0x850000;
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = view;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = 0;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = 0;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(int*)gameAt(fp::sig::pointerOverride) = 0;
    *(uintptr_t*)(view + field::viewport::pickCamera) = camera;
    *(int*)(view + 0x19c) = 800;
    *(int*)(view + 0x1a0) = 600;
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    *(unsigned*)(actor + field::character::motion) = 7;
    const float offset[3] = {10000, 500, -20000};
    memcpy((void*)(sector + field::sector::origin), offset, 12);
    memcpy((void*)(otherSector + field::sector::origin), offset, 12);
    float* p = (float*)(camera + field::camera::pose);
    fp::basis(0, pitch, p);
    p[9] = offset[0];
    p[10] = offset[1] + 170;
    p[11] = offset[2] - distance;
    savedCamera = camera;
    savedView = base + 0x6a0000;
    *(uintptr_t*)(view + field::viewport::view) = savedView;
    *(uintptr_t*)(savedView + field::view::camera) = camera;
    savedRoot = actor;
    savedScene = base + 0x850000;
    park = {};
    restorationUnknown = 0;
    renderedFov = 90;
    aim.valid = true;
    aim.root = actor;
    aim.head = head;
    aim.revision = 1;
    aim.origin[0] = 0;
    aim.origin[1] = 170;
    aim.origin[2] = 0;
    for (int i = 0; i < 3; ++i) aim.point[i] = aim.origin[i] - p[6 + i] * 1000;
    aim.attentionStamp = {
        actor, head, savedScene, mode, inputOwner.generation(), inputOwner.policyRevision()};
    tpAimFree = false;
    tpAimX = tpAimY = 0;
    *(int*)(base + 0x49ce50) = 123;
    *(int*)(base + 0x49ce60) = 234;
    *(unsigned char*)(base + 0x5b4460) = 0;
    originalPoint = (Point)(base + 0xae2e0);
    originalPlaneHit = intersect;
    originalPickRay = worldPick;
#if FP_REVISION >= 21
    originalAimWorldToLocal = inverse;
    check(nativeAimScope == nullptr && nativeAimCameraDepth == 0, "scope starts clear");
#endif
    selected = true;
    haveHit = false;
    selfHit = false;
    distanceRatio = 0;
    targetLocal[0] = 0;
    targetLocal[1] = 130;
    targetLocal[2] = 300;
    hitLocal[0] = 0;
    hitLocal[1] = -200;
    hitLocal[2] = 400;
    calls = picks = planes = nesting = 0;
    action = 0;
}
static bool invokeFault() {
    __try {
        float out[3];
        point(bh, out);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static void aimRestored(const float* before) {
    check(*(int*)(base + 0x49ce50) == 123 && *(int*)(base + 0x49ce60) == 234,
          "native cursor coordinates aimRestored");
    check(*(unsigned char*)(base + 0x5b4460) == 0, "native pick flag aimRestored");
    check(!memcmp(before, (void*)(camera + field::camera::pose), 48), "physical camera bytes unchanged");
#if FP_REVISION >= 21
    check(nativeAimScope == nullptr && nativeAimCameraDepth == 0, "scope and lock depth aimRestored");
#endif
    check(*(unsigned*)(actor + field::character::motion) == 7, "accepted native attack unchanged");
    check(TryAcquireSRWLockExclusive(&cameraLock) != 0, "camera lock released");
    ReleaseSRWLockExclusive(&cameraLock);
}
int main() {
    fixture::NativeImage image(0x900000);
    if (!image) return 2;
    actor = base + 0x600000;
    bh = base + 0x610000;
    head = base + 0x620000;
    mesh = base + 0x630000;
    view = base + 0x640000;
    camera = base + 0x650000;
    sector = base + 0x660000;
    aimOther = base + 0x670000;
    otherMesh = base + 0x680000;
    otherSector = base + 0x690000;
    jump(base + 0xae2e0, (void*)provider);
    jump(base + 0x8f460, (void*)unproject);
    jump(base + 0x4fcb0, (void*)owningRoot);
    for (int mode : {0, 1}) {
        for (float pitch : {-.75f, -.01f, 0.f, .01f, .75f}) {
            reset(mode, pitch);
            float before[12];
            memcpy(before, (void*)(camera + field::camera::pose), 48);
            float out[3];
            point(bh, out);
            check(calls == 1 && planes == 1 && picks == 0,
                  "native selected-target path executes once without extra pick");
            float o[3], d[3];
            rayWorld(view, 400, 300, o, d);
            float depth = 0;
            const float* off = (float*)(sector + field::sector::origin);
            for (int i = 0; i < 3; ++i) depth += (targetLocal[i] + off[i] - o[i]) * d[i];
            float expected[3];
            for (int i = 0; i < 3; ++i) expected[i] = o[i] + d[i] * depth - off[i];
            check(aimNear(out, expected),
                  "native selected depth follows pitched cursor ray with sector conversion");
            check(fabsf(*(float*)gameAt(fp::sig::dragHitDistance) - depth) < .02f,
                  "native plane distance publication retained");
            check(aimNear(out, (float*)(bh + field::behaviour::aimPoint)),
                  "native target cache agrees with caller");
            aimRestored(before);
        }
        for (float ratio : {0.f, 1.f, 1.5f, 1.99f}) {
            reset(mode);
            distanceRatio = ratio;
            float out[3];
            point(bh, out);
            const float weight = std::clamp(2 - ratio, 0.f, 1.f);
            check(fabsf(out[2] - (300 * weight + 1000 * (1 - weight))) < .02f,
                  "native assistance inner/outer blend retained in depth");
        }
        reset(mode);
        *(float*)(otherSector + field::sector::origin + 8) += 200;
        float out[3];
        point(bh, out);
        check(fabsf(out[2] - 500) < .01f, "opponent in another sector projects into correct world depth");
        for (float height : {-200.f, 1000.f}) {
            reset(mode);
            selected = false;
            haveHit = true;
            hitLocal[1] = height;
            point(bh, out);
            check(aimNear(out, hitLocal), "existing world pick accepts below/above overhead band");
            check(picks == 1 && calls == 1, "world fallback uses exactly native query count");
        }
        reset(mode);
        selected = false;
        haveHit = false;
        point(bh, out);
        const float aimFar[3] = {0, 170, 1000};
        check(aimNear(out, aimFar), "empty horizon has finite forward fallback");
        reset(mode);
        selected = false;
        haveHit = true;
        selfHit = true;
        point(bh, out);
        check(aimNear(out, aimFar), "self hit rejected");
        reset(mode);
        selected = false;
        haveHit = true;
        hitLocal[2] = -400;
        point(bh, out);
        check(aimNear(out, aimFar), "backward native hit rejected");
        reset(mode);
        selected = false;
        haveHit = true;
        hitLocal[0] = std::numeric_limits<float>::quiet_NaN();
        point(bh, out);
        check(aimNear(out, aimFar), "invalid native hit rejected");
        for (int which : {1, 2, 3, 4, 5, 9, 10, 11, 12, 15, 16}) {
            reset(mode);
            action = which;
            float before[12];
            memcpy(before, (void*)(camera + field::camera::pose), 48);
            if (which == 3)
                check(invokeFault(), "native exception propagates");
            else
                point(bh, out);
            aimRestored(before);
            if (which == 4 || which == 5 || which >= 9)
                check(aimNear(out, aim.point), "mid-call owner/input change discards adapted target");
        }
        for (int which : {7, 13, 14}) {
            reset(mode);
            action = which;
            point(bh, out);
            check(aimNear(out, aim.point), "invalid frame or selection values use finite forward fallback");
        }
        reset(mode);
        selected = false;
        *(uintptr_t*)(actor + 0xe38) = 0;
        point(bh, out);
        check(calls == 1 && picks == 1 && aimNear(out, aim.point),
              "no opponent retains native world-pick branch");
        reset(mode);
        targetLocal[2] = -300;
        point(bh, out);
        check(aimNear(out, aim.point), "behind-camera target cannot create a backward plane");
        reset(mode);
        action = 8;
        point(bh, out);
        reset(mode);
        originalPoint = provider;
        point(bh, out);
        check(calls == 0, "foreign point-slot predecessor is not adapted by native frame ABI");
        reset(mode);
        *(unsigned char*)(bh + field::behaviour::pressBlocked) = 1;
        *(uintptr_t*)(actor + field::character::actionCallback) = 1;
        *(unsigned*)(actor + field::character::stateFlags) = 0x200;
        point(bh, out);
        check(calls == 1, "native targeting continues through accepted action and movement states");
        reset(mode);
        action = 6;
        point(bh, out);
        check(*(int*)(base + 0x49ce50) == 777, "later cursor owner write preserved");
        reset(mode);
        *(int*)gameAt(fp::sig::uiOpenCount) = 1;
        point(bh, out);
        check(calls == 0 && aimNear(out, aim.point), "UI holds aim without native cursor borrowing");
        reset(mode);
        ++aim.attentionStamp.generation;
        point(bh, out);
        check(calls == 0 && aimNear(out, aim.point), "stale input generation uses handoff target");
        reset(mode);
        aim.attentionStamp.policyRevision++;
        point(bh, out);
        check(calls == 0, "stale policy is excluded");
        reset(mode);
        savedCamera = 0;
        point(bh, out);
        check(calls == 0, "unpublished camera is excluded");
        reset(mode);
        AcquireSRWLockExclusive(&cameraLock);
        point(bh, out);
        ReleaseSRWLockExclusive(&cameraLock);
        check(calls == 0, "camera owner is never waited on");
    }
    for (int mode : {0, 1}) {
        for (int mismatch : {0, 1}) {
            reset(mode);
            if (mismatch)
                *(uintptr_t*)(savedView + field::view::camera) = camera + 0x1000;
            else
                *(uintptr_t*)(view + field::viewport::view) = savedView + 0x1000;
            float out[3];
            point(bh, out);
            check(!calls && aimNear(out, aim.point),
                  "different rendering view/camera excludes native borrow");
        }
        for (float offset : {-60.f, 0.f, 60.f}) {
            reset(mode, .3f);
            filterRoot = actor;
            filterHead = head;
            memcpy(lastCamera, (void*)(camera + field::camera::pose), 48);
            originalCursorPosition = stockCursor;
            hideSwingCrosshair = false;
            aimMarker = false;
            swinging = false;
            if (mode == 0)
                aim.point[0] += offset;
            else {
                tpAimFree = true;
                tpAimX = offset;
                tpAimY = -20;
            }
            float out[3];
            point(bh, out);
            const int nativeX = rayX, nativeY = rayY;
            LONG x = -1, y = -1;
            cursorPositionFor(gameAt(fp::sig::crosshairCursorReturn), &x, &y);
            check(calls == 1 && x == nativeX && y == nativeY,
                  "native target and rendered combat crosshair share FP/TP screen coordinates");
            check(reticleX == x && reticleY == y, "interaction reticle agrees with combat screen position");
        }
    }
    reset(1, 0, 1200);
    selected = false;
    float out[3];
    point(bh, out);
    check(fabsf(out[2] - 1000) < .01f, "distant TP fallback remains ahead of player");
    reset(1);
    tpAimFree = true;
    tpAimX = 90;
    tpAimY = -60;
    selected = false;
    point(bh, out);
    check(out[0] < -200 && out[1] > 300 && out[2] > 500, "TP off-center reticle uses native ray");
    reset(2);
    point(bh, out);
    check(calls == 1 && planes == 1, "overhead retains original target provider");
    float overheadExpected[3], overheadPlane[4] = {0, 1, 0, 130};
    intersect(overheadExpected, view, 123, 234, overheadPlane);
    check(aimNear(out, overheadExpected), "overhead result matches original horizontal-plane provider");
    check(image.close(), "fixture memory released");
    printf("Native aim checks: %u; failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
