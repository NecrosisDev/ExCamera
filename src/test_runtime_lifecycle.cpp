// Actual adapter functions, owned fake native memory, controlled interleavings.
// Never opens Exanima, loads a save, injects, or sends gameplay input.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <vector>
#include <cstdio>
#include <limits>
#include <atomic>
static HWND fakeForeground() {
    return (HWND)1;
}
static DWORD fakeWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
static HANDLE sampleEntered = nullptr, sampleContinue = nullptr;
static std::atomic<bool> holdCounter{false};
static std::atomic<bool> counterTimeout{false}, holdProjection{false};
static BOOL fakeCounter(LARGE_INTEGER* counter) {
    if (holdCounter.load()) {
        SetEvent(sampleEntered);
        if (WaitForSingleObject(sampleContinue, 5000) != WAIT_OBJECT_0) counterTimeout = true;
    }
    return QueryPerformanceCounter(counter);
}
#define GetForegroundWindow fakeForeground
#define GetWindowThreadProcessId fakeWindowPid
#define QueryPerformanceCounter fakeCounter
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef QueryPerformanceCounter
static int checks = 0, failures = 0, projections = 0, inverses = 0;
static float projectedFov = 0;
static bool inspectNative = false, nativeSawStock = false, movingNative = false;
static void check(bool value, const char* name) {
    ++checks;
    if (!value) {
        ++failures;
        printf("FAIL: %s\n", name);
    }
}
static uintptr_t fakeNav(void*, float) {
    if (movingNative) {
        auto v = *(uintptr_t*)(base + 0x4efc90);
        v = *(uintptr_t*)(v + 0x1a8);
        auto c = *(uintptr_t*)(v + 0x50);
        *(float*)(c + 0x1c8) = 39;
        *(float*)(c + 0x40 + 9 * 4) = 17;
    }

    if (inspectNative) {
        auto v = *(uintptr_t*)(base + 0x4efc90);
        v = *(uintptr_t*)(v + 0x1a8);
        auto c = *(uintptr_t*)(v + 0x50);
        nativeSawStock = *(float*)(c + 0x1c8) == 37 && projectedFov == 37 && *(float*)(c + 0x1cc) == 100;
    }
    return 123;
}
static void fakeProjection(uintptr_t view) {
    ++projections;
    auto c = *(uintptr_t*)(view + 0x50);
    projectedFov = *(float*)(c + 0x1c8);
    if (holdProjection && projectedFov == 75) {
        holdProjection = false;
        SetEvent(sampleEntered);
        if (WaitForSingleObject(sampleContinue, 5000) != WAIT_OBJECT_0) counterTimeout = true;
    }
}
static void fakeInverse(uintptr_t, uintptr_t) {
    ++inverses;
}
static BOOL WINAPI fakeSwap(HDC) {
    return TRUE;
}
struct Fixture {
    std::vector<unsigned char> data;
    uintptr_t root, bh, view, viewport, camera, head, mesh, scene;
    float initial[12]{};
    Fixture() : data(0x600000) {
        base = (uintptr_t)data.data();
        root = base + 0x550000;
        bh = base + 0x552000;
        view = base + 0x554000;
        viewport = base + 0x555000;
        camera = base + 0x556000;
        head = base + 0x557000;
        mesh = base + 0x558000;
        scene = base + 0x559000;
        *(uintptr_t*)(base + 0x4f0830) = root;
        *(uintptr_t*)(root + 0xd40) = bh;
        *(uintptr_t*)(bh + 0x28) = root;
        *(uintptr_t*)(bh + 0x898) = view;
        *(uintptr_t*)(base + 0x4efc90) = viewport;
        *(uintptr_t*)(viewport + 0x1a8) = view;
        *(uintptr_t*)(view + 0x50) = camera;
        *(uintptr_t*)(camera + 0x160) = 42;
        *(uintptr_t*)(root + 0x11e0) = head;
        *(uintptr_t*)(head + 0x580) = root;
        *(uintptr_t*)(head + 0x18) = mesh;
        *(uintptr_t*)(mesh + 0x160) = head;
        *(uintptr_t*)(mesh + 0x130) = scene;
        auto h = (float*)(mesh + 0x40);
        h[0] = h[4] = h[8] = 1;
        fp::basis(.7f, .3f, initial);
        initial[9] = 1;
        initial[10] = 2;
        initial[11] = 3;
        memcpy((void*)(camera + 0x40), initial, 48);
        *(float*)(camera + 0x1c8) = 37;
        *(float*)(camera + 0x1cc) = 100;
        originalNav = fakeNav;
        rebuildProjection = fakeProjection;
        rebuildInverse = fakeInverse;
        originalSwap = fakeSwap;
        stopping = 0;
        active = 1;
        inputEnabled = 1;
        cursorFree = 0;
        savedCamera = 0;
        pendingRestore = 0;
        filterRoot = filterHead = 0;
        QueryPerformanceFrequency(&frequency);
        filterTime = {};
        samples = 0;
        aim = {};
        mouseCaptured = 0;
        projections = inverses = 0;
        projectedFov = 0;
        selectedFov = 75;
        inspectNative = nativeSawStock = movingNative = false;
        holdCounter = false;
        counterTimeout = false;
        holdProjection = false;
#ifdef FP_LIFECYCLE_V2
        savedView = savedController = 0;
        cameraReady = 0;
        restorationUnknown = 0;
        savedRoot = savedScene = 0;
        park = Park{};
        for (Retired& r : retired) r = Retired{};
        parks = 0;
        rearms = 0;
        reclaims = 0;
        parkKind = 0;
        parkedNow = 0;
        headHold = fp::HeadHold{};
        publishHold();
        parkBackoffs = 0;
        stopPress = 0;
        stopRelease = 0;
        stopTaps = 0;
        *(uintptr_t*)(base + 0x49b300) = 0;
        *(int*)(base + 0x499860) = 0;
#endif
    }
    bool stock() const {
        return memcmp((void*)(camera + 0x40), initial, 48) == 0 && *(float*)(camera + 0x1c8) == 37 &&
               *(float*)(camera + 0x1cc) == 100;
    }
};
int main() {
    {
        Fixture f;
        aim = {};
        aim.valid = true;
        aim.root = f.root;
        aim.head = f.head;
        aim.yaw = aim.targetYaw = 0.05f;
        holdHeadingOn = true;
        turnLeadOn = false;
        nav((void*)42, 0);
        const float* c = (const float*)(f.camera + 0x40);
        check(std::fabs(c[6]) < 1e-5f && std::fabs(c[8] + 1.f) < 1e-5f,
              "small aim error never rotates the first-person camera independently of its head");
    }
    {
        Fixture f;
        check(nav((void*)42, 0) == 123, "native return preserved");
        check(savedCamera == f.camera && pendingRestore && *(float*)(f.camera + 0x1c8) == 75,
              "first FP frame committed");
        inspectNative = true;
        nav((void*)42, 0);
        inspectNative = false;
        check(nativeSawStock, "stationary native sampler sees consistent stock matrices/fields");
        check(samples == 2 && savedCamera == f.camera, "normal subsequent frame retains FP");
        active = 0;
        nav((void*)42, 0);
        check(f.stock() && projectedFov == 37 && !savedCamera && !pendingRestore, "F6 restoration complete");
    }
    {
        // A source that never comes back: hold, then the park returns the NEW stock output.
        Fixture f;
        nav((void*)42, 0);
        movingNative = true;
        *(uintptr_t*)(f.root + 0x11e0) = 0;
        for (unsigned i = 0; i <= fp::headHoldFrames; ++i) nav((void*)42, 0);
        movingNative = false;
        check(*(float*)(f.camera + 0x1c8) == 39 && projectedFov == 39 &&
                  *(float*)(f.camera + 0x40 + 9 * 4) == 17,
              "rejected source preserves/rebuilds NEW stock output of a moving native sampler");
    }
    {
        // the camera's rotation is the head's. A mouse aim far
        // from where the head faces (a fast turn the head has not followed yet) must not turn the camera.
        Fixture f;
        AcquireSRWLockExclusive(&aimLock);
        aim = {};
        aim.valid = true;
        aim.root = f.root;
        aim.head = f.head;
        aim.yaw = aim.targetYaw = 2.f;
        aim.pitch = aim.targetPitch = 0.8f;
        ReleaseSRWLockExclusive(&aimLock);
        nav((void*)42, 0);
        nav((void*)42, 0);
        const float* c =
            (const float*)(f.camera +
                           0x40); // the head faces +Z (identity): the camera's back row is (0,0,-1)
        check(savedCamera == f.camera && std::fabs(c[6]) < 1e-4f && std::fabs(c[7]) < 1e-4f &&
                  std::fabs(c[8] + 1) < 1e-4f,
              "the camera faces where the head faces, whatever the mouse aim says");
    }
    {
        // the camera is exactly the head, every frame. The head turns 20 degrees, tilts and rolls in one
        // frame while the mouse moves and the aim points 30 degrees elsewhere: the camera's axes are the
        // head's (fp::headPose) and its position the head's eye mount, within 1e-4. Hard check: any camera
        // smoothing, levelling, offset or mouse input of its own fails the build.
        Fixture f;
        AcquireSRWLockExclusive(&aimLock);
        aim = {};
        aim.valid = true;
        aim.root = f.root;
        aim.head = f.head;
        aim.yaw = aim.targetYaw = 0.5236f;
        aim.pitch = aim.targetPitch = .3f;
        ReleaseSRWLockExclusive(&aimLock);
        nav((void*)42, 0);
        nav((void*)42, 0);
        float* h = (float*)(f.mesh + 0x40);
        {
            float yaw[9], tilt[9], roll[9], m1[9], m[9];
            const float a = 0.34906585f, p = 0.2f, r = 0.3f; // 20 degrees yaw, ~11 pitch, ~17 roll
            const float Y[9] = {std::cos(a), 0, -std::sin(a), 0, 1, 0, std::sin(a), 0, std::cos(a)};
            memcpy(yaw, Y, 36);
            const float P[9] = {1, 0, 0, 0, std::cos(p), -std::sin(p), 0, std::sin(p), std::cos(p)};
            memcpy(tilt, P, 36);
            const float R[9] = {std::cos(r), std::sin(r), 0, -std::sin(r), std::cos(r), 0, 0, 0, 1};
            memcpy(roll, R, 36);
            auto mul = [](const float* x, const float* y, float* o) {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        o[i * 3 + j] = 0;
                        for (int k = 0; k < 3; ++k) o[i * 3 + j] += x[i * 3 + k] * y[k * 3 + j];
                    }
            };
            mul(roll, tilt, m1);
            mul(m1, yaw, m);
            memcpy(h, m, 36);
            h[9] = 40;
            h[10] = 150;
            h[11] = -20;
        }
        for (int frame = 0; frame < 3; ++frame) {
            AcquireSRWLockExclusive(&aimLock);
            aim.targetYaw += 0.2f;
            aim.yaw += 0.2f;
            ReleaseSRWLockExclusive(&aimLock); // the mouse keeps moving
            nav((void*)42, 0);
            float eye[3], want[12];
            const float* sceneOffset = (const float*)(f.scene + 0x64);
            for (int i = 0; i < 3; ++i)
                eye[i] =
                    h[9 + i] + h[3 + i] * fp::eyeAlongAxis2 + h[6 + i] * fp::eyeAlongAxis3 + sceneOffset[i];
            fp::headPose(h, eye, want);
            const float* c = (const float*)(f.camera + 0x40);
            float worst = 0;
            for (int i = 0; i < 12; ++i) worst = (std::max)(worst, std::fabs(c[i] - want[i]));
            check(savedCamera == f.camera && worst < 1e-4f,
                  "the camera is exactly the head (axes and eye) after a fast turn, tilt and roll, mouse "
                  "active, aim 30 degrees off");
            float back[3] = {-h[6], -h[7], -h[8]};
            fp::unit(back);
            float d = 0;
            for (int i = 0; i < 3; ++i) d += c[6 + i] * back[i];
            check(d > 0.99999f, "the camera faces where the head faces");
            // independent of headPose: right = up x back, from the head's own rows
            float upv[3] = {h[3], h[4], h[5]};
            fp::unit(upv);
            const float right[3] = {upv[1] * back[2] - upv[2] * back[1], upv[2] * back[0] - upv[0] * back[2],
                                    upv[0] * back[1] - upv[1] * back[0]};
            const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            check(std::fabs(c[0] - right[0] / rl) < 1e-4f && std::fabs(c[1] - right[1] / rl) < 1e-4f &&
                      std::fabs(c[2] - right[2] / rl) < 1e-4f,
                  "the camera's right axis is up x back of the head (handedness)");
        }
    }
    {
        // third person: a pose of its own behind the aim; back in first
        // person the camera is exactly the head again on the very first frame (no third-person state left).
        Fixture f;
        AcquireSRWLockExclusive(&aimLock);
        aim = {};
        aim.valid = true;
        aim.root = f.root;
        aim.head = f.head;
        aim.yaw = aim.targetYaw = 0.4f;
        aim.pitch = aim.targetPitch = 0.1f;
        ReleaseSRWLockExclusive(&aimLock);
        nav((void*)42, 0);
        viewMode = 1;
        nav((void*)42, 0);
        nav((void*)42, 0);
        const float* c = (const float*)(f.camera + 0x40);
        const float* h = (const float*)(f.mesh + 0x40);
        float eye[3];
        for (int i = 0; i < 3; ++i)
            eye[i] = h[9 + i] + h[3 + i] * fp::eyeAlongAxis2 + h[6 + i] * fp::eyeAlongAxis3 +
                     *(float*)(f.scene + 0x64 + i * 4);
        float d2 = 0;
        for (int i = 0; i < 3; ++i) {
            const float d = c[9 + i] - eye[i];
            d2 += d * d;
        }
        bool finite = true;
        for (int i = 0; i < 12; ++i)
            if (!std::isfinite(c[i])) finite = false;
        const float fwdZ = -c[8], fwdX = -c[6];
        check(savedCamera == f.camera && finite && std::sqrt(d2) > 200.f &&
                  std::fabs(std::atan2(fwdX, fwdZ) - 0.4f) < 1e-3f,
              "third person: the camera stands behind the eye along the mouse aim");
        viewMode = 0;
        nav((void*)42, 0);
        float want[12];
        check(fp::headPose(h, eye, want), "head pose made");
        float worst = 0;
        for (int i = 0; i < 12; ++i) worst = (std::max)(worst, std::fabs(c[i] - want[i]));
        check(worst < 1e-4f && !thirdPerson.valid, "back in first person the camera is exactly the head on "
                                                   "the first frame, no third-person state left");
    }
    for (int defect = 0; defect < 10; ++defect) {
        Fixture f;
        nav((void*)42, 0);
        switch (defect) {
        case 0:
            *(uintptr_t*)(f.root + 0x11e0) = 0;
            break;
        case 1:
            *(uintptr_t*)(f.head + 0x580) = 0;
            break;
        case 2:
            *(uintptr_t*)(f.head + 0x18) = 0;
            break;
        case 3:
            *(uintptr_t*)(f.mesh + 0x160) = 0;
            break;
        case 4:
            *(float*)(f.mesh + 0x40) = std::numeric_limits<float>::quiet_NaN();
            break;
        case 5:
            *(float*)(f.mesh + 0x40) = 0;
            break;
        case 6:
            *(uintptr_t*)(f.mesh + 0x130) = 0;
            break;
        case 7:
            *(float*)(f.scene + 0x64) = std::numeric_limits<float>::quiet_NaN();
            break;
        case 8:
            *(float*)(f.root + 0xab4) = std::numeric_limits<float>::quiet_NaN();
            break;
        case 9: {
            float* hm = (float*)(f.mesh + 0x40);
            hm[3] = 0;
            hm[4] = 0;
            hm[5] = 1;
            hm[6] = 0;
            hm[7] = 0;
            hm[8] = 1;
            break;
        } // up parallel to forward
        }
#ifdef FP_LIFECYCLE_V2
        // a bad head frame during live play holds the last good view.
        float held[12];
        memcpy(held, lastCamera, 48);
        nav((void*)42, 0);
        check(!stopping && active && cameraReady && controls(f.bh) && savedCamera == f.camera &&
                  pendingRestore,
              "bad live head frame holds: FP and input kept");
        check(memcmp((void*)(f.camera + 0x40), held, 48) == 0 && *(float*)(f.camera + 0x1c8) == 75 &&
                  projectedFov == 75,
              "held frame republishes the last good pose");
        check(heldFrames == 1 && holds == 1 && holdingNow == 1, "hold counted");
        for (unsigned i = 1; i < fp::headHoldFrames; ++i) nav((void*)42, 0);
        check(
            memcmp((void*)(f.camera + 0x40), held, 48) == 0 && *(float*)(f.camera + 0x1c8) == 75,
            "cut straight back: the whole hold shows the last good view, no slide toward the game's camera");
        check(!stopping && savedCamera == f.camera && heldFrames == fp::headHoldFrames &&
                  maxHold == fp::headHoldFrames,
              "hold lasts headHoldFrames");
        nav((void*)42, 0);
        check(f.stock() && projectedFov == 37, "exhausted hold restores fields AND derived projection");
        check(active && !stopping && !inputEnabled && !controls(f.bh) && parkedNow && holdParks == 1,
              "exhausted hold parks: no stop, input released");
        check(!pendingRestore && !savedCamera && !restorationUnknown,
              "parked hold has no dangling restoration debt");
        check(awaitRestore(0), "F7 while a hold is parked is a clean stop");
#endif
    }
#ifdef FP_LIFECYCLE_V2
    {
        // One bad frame, then a sane head: the view resumes with no park.
        Fixture f;
        nav((void*)42, 0);
        nav((void*)42, 0);
        *(float*)(f.mesh + 0x40) = std::numeric_limits<float>::quiet_NaN();
        nav((void*)42, 0);
        check(holdingNow == 1 && savedCamera == f.camera, "single bad frame held");
        *(float*)(f.mesh + 0x40) = 1;
        nav((void*)42, 0);
        check(holdingNow == 0 && holds == 1 && !parks && samples == 3 && savedCamera == f.camera && !stopping,
              "sane head resumes after a hold");
    }
    {
        // A head that stays bad: park, re-arm only once it reads sane again.
        Fixture f;
        nav((void*)42, 0);
        nav((void*)42, 0);
        *(uintptr_t*)(f.root + 0x11e0) = 0;
        for (unsigned i = 0; i <= fp::headHoldFrames; ++i) nav((void*)42, 0);
        check(parkedNow && parks == 1 && parkKind == int(fp::OwnerChange::parkRestore) && f.stock(),
              "persistent bad head parks with stock");
        for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        nav((void*)42, 0);
        check(!savedCamera && parkedNow && !stopping && parks == 1,
              "re-armed but waits: head still invalid, no new park");
        *(uintptr_t*)(f.root + 0x11e0) = f.head;
        nav((void*)42, 0);
        check(savedCamera == f.camera && rearms == 1 && !parkedNow && !stopping,
              "re-attaches when the head reads sane");
        check(*(float*)(f.camera + 0x1c8) == 75 && projectedFov == 75,
              "back from a hold park (getting up) first person cuts straight back (no ease-in)");
        {
            const float* h = (const float*)(f.mesh + 0x40);
            float eye[3], want[12];
            for (int i = 0; i < 3; ++i)
                eye[i] = h[9 + i] + h[3 + i] * fp::eyeAlongAxis2 + h[6 + i] * fp::eyeAlongAxis3 +
                         *(float*)(f.scene + 0x64 + i * 4);
            const bool made = fp::headPose(h, eye, want);
            float worst = 0;
            for (int i = 0; i < 12; ++i)
                worst = (std::max)(worst, std::fabs(((const float*)(f.camera + 0x40))[i] - want[i]));
            check(made && worst < 1e-4f,
                  "the first frame after re-arm is exactly the head (no ease-in of pose)");
        }
    }
    {
        // A hold while a menu opens or the scene changes is the park.
        Fixture f;
        nav((void*)42, 0);
        nav((void*)42, 0);
        *(uintptr_t*)(f.root + 0x11e0) = 0;
        nav((void*)42, 0);
        check(holdingNow == 1, "held");
        *(int*)(base + 0x499860) = 1;
        nav((void*)42, 0);
        check(parkedNow && holdingNow == 0 && f.stock() && !stopping, "menu during a hold parks");
    }
#endif
    {
        Fixture f;
        *(uintptr_t*)(f.root + 0x11e0) = 0;
        nav((void*)42, 0);
        check(active && !stopping && !savedCamera && !inputEnabled,
              "initial source not ready waits without owning input or declaring failure");
        *(uintptr_t*)(f.root + 0x11e0) = f.head;
        nav((void*)42, 0);
        check(savedCamera == f.camera && !stopping, "initial source can become ready");
        *(uintptr_t*)(base + 0x4f0830) = 0;
        nav((void*)42, 0);
        check(active && !stopping && f.stock() && !savedCamera && !inputEnabled,
              "actorless loading restores stock without permanent fault");
    }
    {
        Fixture f;
        nav((void*)42, 0);
        nav((void*)99, 0);
        check(savedCamera == f.camera && *(float*)(f.camera + 0x1c8) == 75,
              "unrelated controller callback cannot restore active FP camera");
        stopping = 1;
        swap(nullptr);
        check(f.stock() && projectedFov == 37 && !pendingRestore && awaitRestore(0),
              "swap restoration on stop is acknowledged");
    }
    for (int changed = 0; changed < 4; ++changed) {
        Fixture f;
        nav((void*)42, 0);
        if (changed == 0) {
            *(uintptr_t*)(f.view + 0x50) = base + 0x55b000;
        }
        if (changed == 1) {
            auto other = base + 0x55b000;
            *(uintptr_t*)(other + 0x50) = f.camera;
            *(uintptr_t*)(f.viewport + 0x1a8) = other;
        }
        if (changed == 2) {
            *(uintptr_t*)(f.camera + 0x160) = 43;
        }
        if (changed == 3) {
            *(uintptr_t*)(base + 0x4efc90) = 0;
        }
        stopping = 1;
        swap(nullptr);
#ifdef FP_LIFECYCLE_V2
        if (changed == 1 || changed == 2) {
            // Our camera is still the current one: restoration is provable, no fault.
            check(f.stock() && !savedCamera && !pendingRestore && !restorationUnknown,
                  "current camera restored on owner change");
            check(parks == 1 && parkKind == int(fp::OwnerChange::parkRestore) && awaitRestore(0),
                  "restored owner change parks and allows a clean stop");
            continue;
        }
#endif
        check(*(float*)(f.camera + 0x1c8) == 75, "unknown owner is never blindly written");
#ifdef FP_LIFECYCLE_V2
        // the swap parked (release); the stop still cannot claim restoration.
        check(!awaitRestore(10) && parks == 1 && parkKind == int(fp::OwnerChange::parkRelease) &&
                  retired[retiredSlots - 1].camera == f.camera,
              "unknown restoration cannot pass stop gate");
        check(restorationUnknown, "owner change explicitly records unknown outcome");
#else
        check(!awaitRestore(10) && pendingRestore, "unknown restoration cannot pass stop gate");
#endif
        // Reappearing addresses cannot erase the missing lifetime proof.
        *(uintptr_t*)(base + 0x4efc90) = f.viewport;
        *(uintptr_t*)(f.viewport + 0x1a8) = f.view;
        *(uintptr_t*)(f.view + 0x50) = f.camera;
        *(uintptr_t*)(f.camera + 0x160) = 42;
        swap(nullptr);
        check(!awaitRestore(0) && *(float*)(f.camera + 0x1c8) == 75, "ownership uncertainty remains sticky");
    }
    {
        Fixture f;
        nav((void*)42, 0);
        auto start = GetTickCount64();
        check(!awaitRestore(40) && GetTickCount64() - start < 1000 && pendingRestore,
              "missing callbacks time out without erasing pending restoration");
        nav((void*)42, 0);
        check(awaitRestore(0) && f.stock(),
              "late native callback can still restore while hooks remain mapped");
    }
    sampleEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sampleContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!sampleEntered || !sampleContinue) {
        puts("FAIL: cannot create test synchronization events");
        return 2;
    }
    for (int scenario = 0; scenario < 64; ++scenario) {
        Fixture f;
        if (scenario % 2) nav((void*)42, 0);
        ResetEvent(sampleEntered);
        ResetEvent(sampleContinue);
        holdCounter = true;
        std::thread callback([] { nav((void*)42, 0); });
        auto arrived = WaitForSingleObject(sampleEntered, 5000);
        if (arrived != WAIT_OBJECT_0) {
            SetEvent(sampleContinue);
            callback.join();
            check(false, "controlled callback reached publication boundary");
            break;
        }
        InterlockedExchange(&stopping, 1);
        check(!awaitRestore(5), "stop cannot accept quiescence while camera callback is in flight");
        SetEvent(sampleContinue);
        callback.join();
        holdCounter = false;
        check(!counterTimeout && awaitRestore(50),
              "stop accepted only after callback exits publication region");
        check(f.stock() && !pendingRestore && !savedCamera, "no late FP publication after stopping");
    }
    // Stop after the final check, while the FP projection is being committed.
    for (int frame = 0; frame < 2; ++frame) {
        Fixture f;
        if (frame) nav((void*)42, 0);
        ResetEvent(sampleEntered);
        ResetEvent(sampleContinue);
        holdProjection = true;
        std::thread callback([] { nav((void*)42, 0); });
        if (WaitForSingleObject(sampleEntered, 5000) != WAIT_OBJECT_0) {
            SetEvent(sampleContinue);
            callback.join();
            check(false, "projection publication reached");
            break;
        }
        InterlockedExchange(&stopping, 1);
        check(!awaitRestore(5), "mid-commit stop cannot acknowledge quiescence");
        SetEvent(sampleContinue);
        callback.join();
        check(!counterTimeout && pendingRestore && savedCamera == f.camera,
              "late commit retains its restoration debt");
        nav((void*)42, 0);
        check(awaitRestore(0) && f.stock(), "committed frame restored before accepting shutdown");
    }
    // F6 is reversible and distinct from permanently stopping the module.
    {
        Fixture f;
        nav((void*)42, 0);
        ResetEvent(sampleEntered);
        ResetEvent(sampleContinue);
        holdCounter = true;
        std::thread callback([] { nav((void*)42, 0); });
        if (WaitForSingleObject(sampleEntered, 5000) != WAIT_OBJECT_0) {
            SetEvent(sampleContinue);
            callback.join();
            check(false, "F6 transition reached");
        } else {
            InterlockedExchange(&active, 0);
            SetEvent(sampleContinue);
            callback.join();
            holdCounter = false;
            check(!counterTimeout && f.stock() && !savedCamera && !stopping,
                  "F6 off during a callback cannot publish a late FP frame");
            active = 1;
            nav((void*)42, 0);
            check(savedCamera == f.camera && !stopping, "F6 may reactivate after a voluntary transition");
        }
    }
    CloseHandle(sampleEntered);
    CloseHandle(sampleContinue);
    {
        Fixture f;
        check(awaitRestore(0), "zero-debt stop accepted");
        nav((void*)42, 0);
        check(f.stock() && !savedCamera && !pendingRestore,
              "permanently closed gate rejects later first frame");
    }
#ifdef FP_LIFECYCLE_V2
    {
        Fixture f;
        check(!controls(f.bh), "first-person input cannot precede a complete camera frame");
        nav((void*)42, 0);
        check(controls(f.bh) && cameraReady, "complete camera publication opens input eligibility");
        active = 0;
        nav((void*)42, 0);
        check(!cameraReady && !controls(f.bh), "restoring stock closes view/input eligibility");
    }
    {
        Fixture f;
        wchar_t dir[MAX_PATH]{}, file[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"fpt", 0, file)) {
            check(false, "status fixture creation");
        } else {
            wcscpy_s(statusPath, file);
            swprintf_s(statusTempPath, L"%s.tmp", file);
            hookedWindow = nullptr;
            auto statusContains = [&](const char* text) {
                FILE* in = nullptr;
                _wfopen_s(&in, statusPath, L"rb");
                if (!in) return false;
                char buf[2048]{};
                fread(buf, 1, sizeof(buf) - 1, in);
                fclose(in);
                return strstr(buf, text) != nullptr;
            };
            reportFault("restore_timeout", "retained", "pending");
            check(statusContains("\"installed\":true") && statusContains("\"restoration\":\"pending\""),
                  "pending restoration reports retained hooks, not successful removal");
            reportFault("stop_rollback_failed", "unknown", "restored");
            check(statusContains("\"installed\":null") && statusContains("\"restored\":true"),
                  "rollback uncertainty distinguished from restored camera");
            reportFault("hook_not_firing", "removed", "restored");
            check(statusContains("\"installed\":false") && statusContains("\"state\":\"hook_not_firing\""),
                  "a stop with hooks removed reports confirmed restoration/removal");
            DeleteFileW(statusPath);
            DeleteFileW(statusTempPath);
            statusPath[0] = statusTempPath[0] = 0;
        }
    }
#endif
#ifdef FP_LIFECYCLE_V2
    // leaving gameplay (death-screen EXIT, ESC EXIT to title) parks
    // first person without a fault and re-attaches when gameplay returns.
    {
        Fixture f;
        const uintptr_t title = base + 0x55b000;
        *(float*)(title + 0x1c8) = 30;
        *(float*)(title + 0x1cc) = 100;
        *(uintptr_t*)(title + 0x160) = 42;
        nav((void*)42, 0);
        *(uintptr_t*)(f.view + 0x50) = title;
        *(uintptr_t*)(base + 0x4f0830) = 0;
        *(int*)(base + 0x499860) = 1; // Title screen.
        swap(nullptr);
        check(active && !stopping && !restorationUnknown, "title return is not a fault");
        check(!savedCamera && !pendingRestore && parkedNow && parks == 1 &&
                  parkKind == int(fp::OwnerChange::parkRelease),
              "title return parks and releases the record");
        check(*(float*)(f.camera + 0x1c8) == 75, "released camera is not written while not current");
        check(*(float*)(title + 0x1c8) == 30, "title camera untouched");
        nav((void*)42, 0);
        swap(nullptr);
        check(!savedCamera && *(float*)(title + 0x1c8) == 30, "parked: no attachment on the title screen");
        *(uintptr_t*)(base + 0x4f0830) = f.root;
        nav((void*)42, 0);
        swap(nullptr);
        check(!savedCamera, "parked: menu still open blocks attachment");
        *(int*)(base + 0x499860) = 0;
        *(uintptr_t*)(base + 0x49b300) = 0x1234;
        swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == title && *(float*)(title + 0x1c8) == 75 && rearms == 1 && !parkedNow,
              "new gameplay re-attaches like a fresh F6");
        check(stockFov == 30, "stock captured from the new camera");
        // The old camera comes back later carrying our values: reclaimed to stock first.
        *(uintptr_t*)(f.view + 0x50) = f.camera;
        *(uintptr_t*)(base + 0x49b300) = 0x5678;
        *(uintptr_t*)(base + 0x4f0830) = 0;
        *(int*)(base + 0x499860) = 1;
        swap(nullptr);
        check(parks == 2 && !restorationUnknown && reclaims == 1 && f.stock(),
              "retired camera reclaimed when current again");
        check(*(float*)(title + 0x1c8) == 75, "second release leaves the not-current camera alone");
        *(uintptr_t*)(base + 0x4f0830) = f.root;
        *(int*)(base + 0x499860) = 0;
        swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == f.camera && stockFov == 37 && rearms == 2, "re-attached with true stock values");
        active = 0;
        nav((void*)42, 0);
        check(f.stock(), "F6 after re-attachment restores the true stock");
    }
    {
        // Death screen: camera swapped while the same gameplay shows a UI. Decision
        // deferred; the camera returns with our values, is reclaimed, and FP re-arms.
        Fixture f;
        const uintptr_t other = base + 0x55b000;
        *(float*)(other + 0x1c8) = 30;
        nav((void*)42, 0);
        *(int*)(base + 0x499860) = 1;
        *(uintptr_t*)(f.view + 0x50) = other;
        swap(nullptr);
        check(!stopping && !restorationUnknown && parkedNow && park.unverified,
              "UI swap parks provisionally");
        *(uintptr_t*)(f.view + 0x50) = f.camera;
        *(int*)(base + 0x499860) = 0;
        swap(nullptr);
        check(f.stock() && reclaims == 1 && park.active, "camera returning after the menu is reclaimed");
        for (unsigned i = 1; i < fp::parkGraceFrames; ++i) swap(nullptr);
        check(!park.active && !stopping, "same gameplay re-arms only after the grace frames");
        nav((void*)42, 0);
        check(savedCamera == f.camera && rearms == 1 && !stopping, "same gameplay re-arms after reclaim");
    }
    {
        // Same gameplay, menu closed, camera never returns (arena round end, knockout):
        // re-arms on the new camera; only a stop while the old one is
        // unresolved reports restoration unknown.
        Fixture f;
        const uintptr_t other = base + 0x55b000;
        *(float*)(other + 0x1c8) = 30;
        *(float*)(other + 0x1cc) = 100;
        *(uintptr_t*)(other + 0x160) = 42;
        nav((void*)42, 0);
        *(int*)(base + 0x499860) = 1;
        *(uintptr_t*)(f.view + 0x50) = other;
        swap(nullptr);
        *(int*)(base + 0x499860) = 0;
        for (unsigned i = 1; i < fp::parkGraceFrames; ++i) swap(nullptr);
        check(!stopping && !restorationUnknown && park.active, "grace frames before deciding");
        swap(nullptr);
        check(!stopping && !restorationUnknown && !park.active,
              "unresolved live swap re-arms instead of stopping");
        nav((void*)42, 0);
        check(savedCamera == other && *(float*)(other + 0x1c8) == 75 && stockFov == 30 && rearms == 1,
              "re-attached to the new camera");
        check(*(float*)(f.camera + 0x1c8) == 75, "unreachable camera never written");
        check(!awaitRestore(0) && restorationUnknown, "stop while the old camera is unresolved stays honest");
    }
    {
        // No menu at all: a live swap parks straight away and re-arms after the grace frames.
        Fixture f;
        const uintptr_t other = base + 0x55b000;
        *(float*)(other + 0x1c8) = 30;
        *(float*)(other + 0x1cc) = 100;
        *(uintptr_t*)(other + 0x160) = 42;
        nav((void*)42, 0);
        *(uintptr_t*)(f.view + 0x50) = other;
        swap(nullptr);
        check(!stopping && parkedNow && parkKind == int(fp::OwnerChange::parkRelease),
              "no-menu swap parks, no fault");
        for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == other && rearms == 1 && !parkedNow, "no-menu swap re-arms");
        // The old camera comes back current: reclaimed, then FP moves back to it.
        *(uintptr_t*)(f.view + 0x50) = f.camera;
        swap(nullptr);
        check(reclaims == 1 && f.stock() && *(float*)(other + 0x1c8) == 75,
              "old camera reclaimed when it returns");
        for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == f.camera && stockFov == 37, "back on the original camera with true stock");
        *(uintptr_t*)(f.view + 0x50) = other;
        *(uintptr_t*)(base + 0x4f0830) = 0;
        swap(nullptr); // Leave gameplay.
        check(awaitRestore(0) && *(float*)(other + 0x1c8) == 30 && reclaims == 2,
              "stop after that gameplay is gone is clean");
    }
    {
        // F7 while a release is unresolved cannot claim restoration.
        Fixture f;
        nav((void*)42, 0);
        *(int*)(base + 0x499860) = 1;
        *(uintptr_t*)(f.view + 0x50) = base + 0x55b000;
        swap(nullptr);
        check(!awaitRestore(0) && restorationUnknown, "stop during unresolved release reports unknown");
    }
    {
        // F7 after leaving gameplay is an ordinary stop.
        Fixture f;
        nav((void*)42, 0);
        *(uintptr_t*)(f.view + 0x50) = base + 0x55b000;
        *(uintptr_t*)(base + 0x4f0830) = 0;
        swap(nullptr);
        check(awaitRestore(0) && !restorationUnknown, "stop after leaving gameplay is clean");
    }
    {
        // Source lost on the death screen (UI open): stock restored, parked, no fault.
        Fixture f;
        nav((void*)42, 0);
        nav((void*)42, 0);
        *(int*)(base + 0x499860) = 1;
        *(uintptr_t*)(f.root + 0x11e0) = 0;
        nav((void*)42, 0);
        check(f.stock() && !stopping && parkedNow && parkKind == int(fp::OwnerChange::parkRestore),
              "death-screen source loss parks");
        *(uintptr_t*)(f.root + 0x11e0) = f.head;
        *(int*)(base + 0x499860) = 0;
        for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == f.camera && rearms == 1, "re-arms when the source is back and the UI is closed");
    }
    {
        // Live play, our camera kept but its controller flapping: backs
        // off (stock restored, parked) instead of stopping.
        Fixture f;
        for (unsigned round = 0; round < fp::liveParkLimit; ++round) {
            nav((void*)42, 0);
            *(uintptr_t*)(f.camera + 0x160) = 43;
            swap(nullptr);
            *(uintptr_t*)(f.camera + 0x160) = 42;
            check(!stopping && parkedNow && f.stock(), "live owner flap parks with stock restored");
            nav((void*)42, 0);
            check(!savedCamera, "no re-attach inside the grace frames");
            for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        }
        check(parkBackoffs == 1 && park.active && !stopping, "third quick live park backs off, no stop");
        nav((void*)42, 0);
        check(!savedCamera && f.stock(), "no re-attach during the back-off");
        park.backoffUntil = 0; // The back-off time has passed.
        for (unsigned i = 0; i < fp::parkGraceFrames; ++i) swap(nullptr);
        nav((void*)42, 0);
        check(savedCamera == f.camera && !stopping, "re-attaches after the back-off");
        stopping = 1;
        swap(nullptr);
        check(awaitRestore(0) && f.stock(), "stop after flaps is clean");
    }
    {
        // F7: a tap is ignored, a one-second hold stops.
        Fixture f;
        nav((void*)42, 0);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExCameraLifecycleGame";
        RegisterClassW(&wc);
        HWND game = CreateWindowExW(0, wc.lpszClassName, L"game", WS_POPUP, 100, 100, 1280, 720, nullptr,
                                    nullptr, wc.hInstance, nullptr);
        hookedWindow = game;
        module = GetModuleHandleW(nullptr);
        const ULONGLONG now = GetTickCount64();
        stopPress = now - 200;
        stopRelease = now - 100;
        stopKeyTick();
        check(!stopping && stopTaps == 1 && !stopPress.load(), "F7 tap ignored and counted");
        stopRelease = 0;
        stopPress = now - 200;
        stopKeyTick();
        check(!stopping && stopPress.load(), "F7 held under a second keeps running");
        stopPress = now - fp::stopHoldMs - 10;
        stopKeyTick();
        check(stopping && !stopPress.load(), "F7 held a second stops");
        swap(nullptr);
        check(awaitRestore(0) && f.stock(), "F7 hold stop restores stock");
        check(fp::rearmAllowed(true, true, restorationUnknown != 0), "clean F7 stop is re-armable");
        check(notices >= 2, "F7 hint notices shown");
        if (noticeWindow) {
            DestroyWindow(noticeWindow);
            noticeWindow = nullptr;
        }
        hookedWindow = nullptr;
        if (game) DestroyWindow(game);
    }
#endif
    printf("LIFECYCLE: %d checks, %d failures; 67 controlled publication/F6/stop interleavings. Owned "
           "fixtures only.\n",
           checks, failures);
    return failures ? 1 : 0;
}
