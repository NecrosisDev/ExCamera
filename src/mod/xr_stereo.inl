// Optional XR rendering below camera sampling, sector updates and shadow preparation.
// Geometry stereo replays the lower world draw once per eye; reconstruction draws the
// head once and lets the host synthesise both eyes from colour plus depth.
#include "../xr/backend.h"
#include "../xr/native_contracts.h"
#include "../xr/transaction.h"
#include <memory>
static void trackedReleaseTick();
using TrackedItemRelease = uintptr_t (*)(uintptr_t, uintptr_t, int);
static TrackedItemRelease trackedItemRelease = nullptr;
static fp::xr::Backend stereoBackend{};
static fp::xr::StereoFrame stereoFrame{};
static bool stereoRequested = false, stereoPrepared = false, stereoDrawn = false, stereoTitle = false; // stereoTitle: the prepared frame is the title screen
static uint64_t stereoRetryAt = 0; // after the runtime is lost: no new host before this tick
static constexpr uint64_t stereoRetryMs = 5000, stereoRetryMaxMs = 60000; // the pause doubles per failed return, up to a minute
static uint64_t stereoRetryDelayMs = stereoRetryMs;
static void trackedWristPresent(); // the wrist menu lives with the drive, presented from here on the render thread
static void trackedBeamPresent();  // the pointer beam likewise
static std::atomic<unsigned> stereoRuntimeLosses{0};
static bool stereoOriginSavePending = false; // a recenter happened; write the new origin once the next frame has it
static uintptr_t stereoView = 0, stereoCamera = 0, stereoRoot = 0, stereoScene = 0, stereoController = 0, stereoVolume = 0;
static DWORD stereoThread = 0;
static std::atomic<bool> stereoLive{false};
static std::unique_ptr<fp::xr::Transaction> stereoUnion;
static std::atomic<unsigned> stereoFrames{0}, stereoEyes{0}, stereoRejected{0}, stereoFaults{0};
static std::atomic<const char*> stereoReason{"disabled"};
// Haptics: the strongest and longest request per hand since the last present, as amplitude in
// thousandths with the duration in milliseconds above it. Impacts repeat per physics step, so a
// hand is not pulsed again for the same reason within a short hold.
static std::atomic<unsigned> trackedHapticPending[2]{}, trackedHapticPulses{0};
// Mirror mode: the native world draw is the left eye and only the right eye is replayed, two
// world draws instead of three; the desktop shows the left eye. Geometry mode only.
static std::atomic<int> vrMirrorLeft{0};
static std::atomic<unsigned> trackedEffectPasses{0};
// Comfort: the edges darken while the body turns smoothly or walks on the stick, in thousandths set
// by the input tick; the ring is drawn on each eye image before it leaves for the headset.
static std::atomic<int> trackedTurnRateMilli{0}, trackedWalkRateMilli{0};
// A snap turn blinks: the whole view dims and clears again over this long, so the jump in heading is never seen.
static std::atomic<uint64_t> trackedSnapBlinkAt{0};
static constexpr uint64_t snapBlinkMs = 160;
static std::atomic<unsigned> trackedComfortDraws{0};
static void (*trackedComfortObserver)() = nullptr; // test seam
static float trackedComfortStrength() {
    const int mode = vrComfort.load(std::memory_order_relaxed);
    if (!mode) return 0;
    const float turn = static_cast<float>(trackedTurnRateMilli.load(std::memory_order_relaxed)) * .001f;
    const float walk = mode >= 2 ? static_cast<float>(trackedWalkRateMilli.load(std::memory_order_relaxed)) * .0007f : 0.f;
    const uint64_t blinkAt = trackedSnapBlinkAt.load(std::memory_order_relaxed), now = GetTickCount64();
    if (blinkAt && now >= blinkAt && now - blinkAt < snapBlinkMs) { // above 1: the ring at full plus the whole view dimmed by the excess
        const float t = static_cast<float>(now - blinkAt) / static_cast<float>(snapBlinkMs);
        return 1.f + (t < .4f ? 1.f : 1.f - (t - .4f) / .6f);
    }
    return (std::max)(turn, walk);
}
static void trackedComfortEye() {
    const float strength = trackedComfortStrength();
    if (!(strength > 0) || !stereoBackend.comfort) return;
    if (stereoBackend.comfort(strength)) ++trackedComfortDraws;
    if (trackedComfortObserver) trackedComfortObserver();
}
static void (*trackedEffectsObserver)(bool desktopShowsIt) = nullptr; // test seam: the image changed under the probe
// The desktop's own AO and SSR, run on an eye image while it is still in the native target. When
// that same image becomes the desktop frame (mirror, reconstruction), the desktop pass is told.
static void trackedEffectsEye(bool desktopShowsIt) {
    if (!vrEffects.load(std::memory_order_relaxed) || !stereoView) return;
    ViewRequest eye;
    eye.eye = true;
    glGetIntegerv(glc::drawFramebufferBinding, &eye.eyeFramebuffer);
    eye.eyeWidth = *(int*)(stereoView + 0x160);
    eye.eyeHeight = *(int*)(stereoView + 0x164);
    presentViewEffects(stereoView, &eye); // the view still holds the eye's matrices
    if (desktopShowsIt) viewEffectsHandledFrame = InterlockedCompareExchange(&swapCalls, 0, 0);
    ++trackedEffectPasses;
    if (trackedEffectsObserver) trackedEffectsObserver(desktopShowsIt);
}
static void trackedHapticRequest(unsigned side, float amplitude, unsigned milliseconds) {
    if (side > 1 || !(amplitude > 0)) return;
    const unsigned packed = (std::min)(static_cast<unsigned>(amplitude * 1000.f + .5f), 1000u) | ((std::min)(milliseconds, 1000u) << 16);
    unsigned seen = trackedHapticPending[side].load();
    while (true) {
        const unsigned merged = (std::max)(seen & 0xFFFF, packed & 0xFFFF) | (std::max)(seen >> 16, packed >> 16) << 16;
        if (trackedHapticPending[side].compare_exchange_weak(seen, merged)) return;
    }
}
static void trackedHapticFlush() {
    for (unsigned side = 0; side < 2; ++side) {
        const unsigned packed = trackedHapticPending[side].exchange(0);
        if (!packed || !stereoBackend.haptic) continue;
        try { if (stereoBackend.haptic(side, static_cast<float>(packed & 0xFFFF) * .001f, static_cast<uint64_t>(packed >> 16) * 1000000ull)) ++trackedHapticPulses; }
        catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_haptic_failed"; }
    }
}
static float stereoUnitsPerMetre = 100;
static char stereoFaultText[128]{}; // the last exception text, for the status JSON and probes
static void stereoNoteFault(const std::exception& e) { strncpy_s(stereoFaultText, e.what(), _TRUNCATE); }
static Behavior originalStereoDraw = nullptr, originalStereoFrustum = nullptr;
static Behavior stereoRebuildPlanes = nullptr, stereoFlushBindings = nullptr;
static thread_local bool stereoReplaying = false;
static bool stereoHooks[2]{};
static bool stereoCallbacksAdmitted(uintptr_t view, uintptr_t renderer);
static bool stereoRenderContext() {
    return hookedWindow && GetWindowThreadProcessId(hookedWindow, nullptr) == GetCurrentThreadId() &&
           wglGetCurrentContext() && WindowFromDC(wglGetCurrentDC()) == hookedWindow;
}
static bool stereoOwner(uintptr_t view) {
    uintptr_t currentView = 0, camera = 0;
    currentCamera(currentView, camera);
    return stereoRenderContext() && view && currentView == view && sameOwner(view, camera) && viewMode.load() == 0 && !park.active &&
           active && !stopping && cameraReady && !uiOpen() && !identityChanged() && savedRoot && savedScene;
}
// The title screen: no player yet, the menu open over the title level. Its own camera anchors the
// eyes and the menu stands in the level; the first-person owner takes over once a player exists.
static bool stereoTitleOwner(uintptr_t view) {
    uintptr_t currentView = 0, camera = 0;
    currentCamera(currentView, camera);
    return vrTitleMenu.load(std::memory_order_relaxed) && stereoRenderContext() && view && currentView == view && camera &&
           readable(camera, field::camera::controller + 8) && !*reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) &&
           *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) && !savedRoot && !stopping && !park.active;
}
static bool stereoTitleWanted() { return vrTitleMenu.load(std::memory_order_relaxed) && !*reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)); }
static bool stereoIdentity() {
    if (GetCurrentThreadId() != stereoThread) return false;
    if (stereoTitle)
        return stereoScene == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) && stereoTitleOwner(stereoView);
    return stereoView == savedView && stereoCamera == savedCamera &&
           stereoRoot == savedRoot && stereoScene == savedScene && stereoOwner(stereoView);
}
static bool stereoLifetime() {
    uintptr_t view = 0, camera = 0;
    currentCamera(view, camera);
    return GetCurrentThreadId() == stereoThread && view == stereoView && camera == stereoCamera &&
           readable(camera, 0x168) && *reinterpret_cast<uintptr_t*>(camera + field::camera::controller) == stereoController &&
           *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) == stereoRoot && *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) == stereoScene &&
           readable(view + 0x140, 8) && *reinterpret_cast<uintptr_t*>(view + 0x140) == stereoVolume;
}
static bool stereoCall(Behavior fn, uintptr_t object) {
    __try { fn(object); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool stereoReconstructing() { return stereoBackend.mode && fp::xr::reconstructs(stereoBackend.mode()); }
static void stereoVolumeSnapshot(fp::xr::Transaction& undo, uintptr_t view) {
    if (!readable(view + 0x140, 8)) throw std::runtime_error("missing native frustum");
    const auto volume = *reinterpret_cast<uintptr_t*>(view + 0x140);
    if (!readable(volume, 0x18) || *reinterpret_cast<int*>(volume + 8) != 3) throw std::runtime_error("unsupported native frustum layout");
    const auto records = *reinterpret_cast<uintptr_t*>(volume + 0x10);
    undo.capture(records, 4 * 0x20);
    for (unsigned i = 0; i < 4; ++i) undo.capture(*reinterpret_cast<uintptr_t*>(records + i * 0x20 + 0x18), 48);
    undo.capture(view + 0x190, 48);
}
static void stereoWriteVolume(uintptr_t view, const fp::xr::Corners& corners) {
    const auto volume = *reinterpret_cast<uintptr_t*>(view + 0x140), records = *reinterpret_cast<uintptr_t*>(volume + 0x10);
    // The reviewed four-plane native template omits the near and far faces.
    constexpr unsigned faces[4][4] = {{1, 0, 4, 5}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}};
    for (unsigned i = 0; i < 4; ++i) {
        auto points = reinterpret_cast<float*>(*reinterpret_cast<uintptr_t*>(records + i * 0x20 + 0x18));
        for (unsigned p = 0; p < 4; ++p) memcpy(points + p * 3, corners[faces[i][p]].data(), 12);
    }
    memcpy(reinterpret_cast<void*>(view + 0x190), corners.data() + 4, 48);
    if (!stereoCall(stereoRebuildPlanes, volume)) throw std::runtime_error("native frustum plane rebuild fault");
}
static void stereoEndFrame() {
    if (!stereoPrepared) return;
    if (stereoUnion) {
        if (!stereoLifetime()) stereoUnion->abandon();
        stereoUnion.reset();
    }
    const bool accepted = stereoDrawn;
    stereoPrepared = false;
    stereoDrawn = false;
    try { if (stereoBackend.finish) stereoBackend.finish(accepted); }
    catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_submission_failed"; }
    stereoTitle = false;
}
static bool stereoPassesAdmitted(uintptr_t view) {
    return *reinterpret_cast<int*>(view + 0x1F4) == 0 && *(unsigned char*)(view + 0x206) == 0 &&
           *(unsigned char*)(view + 0x203) == 0 && *(unsigned char*)(view + 0x204) == 0;
}
static void stereoApplyEye(const fp::xr::Eye& eye, const std::vector<struct StereoMesh>& meshes, const std::vector<uintptr_t>& sectors);
static void stereoFrustum(uintptr_t view) {
    originalStereoFrustum(view);
    if (!stereoRequested || !stereoBackend.begin || !stereoRenderContext() || !TryAcquireSRWLockExclusive(&cameraLock)) return;
    try {
        if (stereoPrepared && (!stereoIdentity() || stereoFrame.serial != static_cast<uint64_t>(static_cast<LONG>(swapCalls)))) stereoEndFrame();
        const bool owner = stereoOwner(view), title = !owner && stereoTitleOwner(view);
        if (!owner && !title) { ReleaseSRWLockExclusive(&cameraLock); return; }
        if (!stereoPrepared) {
            if (!readable(view + 0x138, 8) || !stereoCallbacksAdmitted(view, *reinterpret_cast<uintptr_t*>(view + 0x138)) || !stereoPassesAdmitted(view)) {
                ++stereoRejected;
                stereoReason = "unsupported_pass_or_mutable_callback";
                ReleaseSRWLockExclusive(&cameraLock);
                return;
            }
            uintptr_t camera = savedCamera;
            if (title) { uintptr_t currentView = 0; currentCamera(currentView, camera); } // the game's own title camera
            // The anchor keeps the sampled physical eye position and the camera's yaw only;
            // animated pitch/roll must not multiply the headset's own.
            fp::xr::NativePose anchor{};
            const float* physical = reinterpret_cast<const float*>(camera + field::camera::pose);
            fp::basis(std::atan2(-physical[6], -physical[8]), 0, anchor.data());
            memcpy(anchor.data() + 9, physical + 9, 12);
            // The posture's height nudge lifts (or lowers) the anchor by a set amount: a chair puts a seated head low,
            // and a standing player taller or shorter than the character sees the world from the character's eyes
            // only after this; leaning and looking stay tracked either way.
            if (!title) anchor.data()[10] += static_cast<float>(vrHeightCm.load(std::memory_order_relaxed)) * .01f * static_cast<float>(vrUnitsPerMetre.load());
            const float clipNear = *reinterpret_cast<float*>(camera + field::camera::nearPlane);
            const float clipFar = *reinterpret_cast<float*>(camera + field::camera::farPlane);
            stereoLive = true;
            stereoUnitsPerMetre = static_cast<float>(vrUnitsPerMetre.load()); // one scale per frame, for eyes and depth alike
            if (stereoBackend.configure)
                stereoBackend.configure({static_cast<float>(vrIpdMm.load()) * .001f, stereoUnitsPerMetre, static_cast<float>(vrGripAngle.load()),
                                         static_cast<float>(vrHapticPercent.load()) * .01f, static_cast<float>(vrPanelWidthCm.load()) * .01f, static_cast<float>(vrPanelDistanceCm.load()) * .01f,
                                         vrFrameTime.load(std::memory_order_relaxed) != 0, vrTrackedGrab.load(std::memory_order_relaxed) != 0, vrHandPanel.load(std::memory_order_relaxed) != 0},
                                        vrRenderMode.load() == 2 ? fp::xr::RenderMode::centerReconstructionFilled : vrRenderMode.load() == 1 ? fp::xr::RenderMode::centerReconstruction : fp::xr::RenderMode::geometryStereo);
            static unsigned seenReset = trackingReset.load(); // the mod's one recenter action, shared with the UDP tracker
            if (const unsigned reset = trackingReset.load(); reset != seenReset) {
                seenReset = reset;
                if (stereoBackend.recenter) stereoBackend.recenter();
                stereoOriginSavePending = true;
            }
            if (stereoRetryAt) { // the runtime went away; a new host is tried only after the pause
                if (GetTickCount64() < stereoRetryAt) { ReleaseSRWLockExclusive(&cameraLock); return; }
                stereoRetryAt = 0;
            }
            const bool freshHost = !stereoLive.load();
            if (!stereoBackend.begin(static_cast<uint64_t>(static_cast<LONG>(swapCalls)), anchor, clipNear, clipFar, stereoFrame)) {
                stereoReason = "tracking_or_session_unavailable";
                ReleaseSRWLockExclusive(&cameraLock);
                return;
            }
            if (freshHost) stereoRetryDelayMs = stereoRetryMs; // the runtime is back: the next loss waits the short pause again
            if (vrRecenterMemory.load(std::memory_order_relaxed)) { // the remembered origin comes back on a new host; a recenter writes a new one
                if (freshHost && vrRecenterOriginValid.load() && stereoBackend.restoreOrigin) {
                    const float* o = vrRecenterOrigin;
                    stereoBackend.restoreOrigin({{o[3], o[4], o[5], o[6]}, {o[0], o[1], o[2]}});
                }
                if (stereoOriginSavePending && stereoBackend.originInStage) {
                    XrPosef origin{};
                    if (stereoBackend.originInStage(origin)) {
                        const float o[7] = {origin.position.x, origin.position.y, origin.position.z, origin.orientation.x, origin.orientation.y, origin.orientation.z, origin.orientation.w};
                        memcpy(vrRecenterOrigin, o, sizeof o);
                        vrRecenterOriginValid = true;
                        wchar_t text[160];
                        swprintf_s(text, L"%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", o[0], o[1], o[2], o[3], o[4], o[5], o[6]);
                        saveSetting(L"VR", L"RecenterOrigin", text);
                    }
                }
            }
            stereoOriginSavePending = false;
            stereoThread = GetCurrentThreadId();
            stereoView = view;
            stereoCamera = camera;
            stereoRoot = title ? 0 : savedRoot;
            stereoScene = title ? *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) : savedScene;
            stereoController = title ? *reinterpret_cast<uintptr_t*>(camera + field::camera::controller) : savedController;
            stereoVolume = *reinterpret_cast<uintptr_t*>(view + 0x140);
            stereoPrepared = true;
            stereoDrawn = false;
            stereoTitle = title;
            if (!title) { // no body to drive before a player exists
                trackedDrivePublish();
                trackedBodyStereo(stereoFrame);
            }
            stereoUnion.reset(new fp::xr::Transaction(readable)); // the overloaded readable needs the target type
            stereoVolumeSnapshot(*stereoUnion, view);
        }
        stereoWriteVolume(view, stereoReconstructing() ? fp::xr::corners(stereoFrame.center, stereoFrame.nearPlane, stereoFrame.farPlane) : fp::xr::unionCorners(stereoFrame));
        stereoReason = "prepared";
    } catch (const std::exception& e) {
        ++stereoFaults;
        stereoNoteFault(e);
        stereoReason = "frustum_preparation_failed";
        stereoEndFrame();
    }
    ReleaseSRWLockExclusive(&cameraLock);
}
// Only the already-suppressed cover passes may remain registered: any other pass
// callback or an active particle/fog owner could mutate state during a replay.
static bool stereoCallbacksAdmitted(uintptr_t view, uintptr_t renderer) {
    if (!readable(renderer, 0x170) || *reinterpret_cast<uintptr_t*>(renderer + 0x158) || *reinterpret_cast<uintptr_t*>(view + 0x1C8)) return false;
    const bool coversSuppressed = coversHiddenInFirstPerson.load(std::memory_order_relaxed) && supersampleRenderScope.owned;
    const auto depthCallback = *reinterpret_cast<uintptr_t*>(base + 0x49B260);
    if (depthCallback && (depthCallback != gameAt(fp::sig::coverDepth) || !coversSuppressed)) return false;
    const auto environment = *reinterpret_cast<uintptr_t*>(renderer + 0x118);
    if (!readable(environment, 0x330) || *reinterpret_cast<uintptr_t*>(environment + 0x2D8)) return false;
    for (size_t offset : {0x18u, 0x28u, 0x38u, 0x48u, 0x58u, 0x68u}) {
        const int count = *reinterpret_cast<int*>(renderer + offset);
        const auto records = *reinterpret_cast<uintptr_t*>(renderer + offset + 8);
        if (count < 0 || count > 64 || (count && !readable(records, static_cast<size_t>(count) * 24))) return false;
        for (int i = 0; i < count; ++i)
            if (*reinterpret_cast<uintptr_t*>(records + static_cast<size_t>(i) * 24) != gameAt(fp::sig::coverPass) || !coversSuppressed) return false;
    }
    return true;
}
struct StereoMesh { uintptr_t address; uint32_t mask; std::array<float, 17> bounds; fp::xr::Point origin; };
static std::vector<StereoMesh> stereoMeshes(fp::xr::Transaction& undo, std::vector<uintptr_t>& sectorsOut) {
    std::vector<StereoMesh> out;
    if (!readable(stereoScene + 0x3C4, 12)) throw std::runtime_error("missing native sector array");
    const int last = *reinterpret_cast<int*>(stereoScene + 0x3C4);
    const auto sectors = *reinterpret_cast<uintptr_t*>(stereoScene + 0x3C8);
    if (last < 0 || last > 4095 || !readable(sectors, static_cast<size_t>(last + 1) * 8)) throw std::runtime_error("invalid native sector count");
    for (int i = 0; i <= last; ++i) {
        const auto sector = *reinterpret_cast<uintptr_t*>(sectors + static_cast<size_t>(i) * 8);
        if (!readable(sector, 0x3A1)) throw std::runtime_error("invalid native sector");
        undo.capture(sector + 0x3A0, 1);
        sectorsOut.push_back(sector);
        const int meshLast = *reinterpret_cast<int*>(sector + field::sector::lastMesh);
        const auto meshes = *reinterpret_cast<uintptr_t*>(sector + field::sector::meshes);
        if (meshLast < -1 || meshLast > 65535 || out.size() + static_cast<size_t>(meshLast + 1) > 65536 ||
            (meshLast >= 0 && !readable(meshes, static_cast<size_t>(meshLast + 1) * 8))) throw std::runtime_error("invalid native mesh count");
        for (int j = 0; j <= meshLast; ++j) {
            const auto mesh = *reinterpret_cast<uintptr_t*>(meshes + static_cast<size_t>(j) * 8);
            if (!readable(mesh, 0xE4)) throw std::runtime_error("invalid native mesh");
            StereoMesh value{};
            value.address = mesh;
            value.mask = *reinterpret_cast<uint32_t*>(mesh + field::sceneNode::passMask);
            memcpy(value.bounds.data(), reinterpret_cast<void*>(mesh + 0x20), sizeof(value.bounds));
            memcpy(value.origin.data(), reinterpret_cast<void*>(sector + field::sector::origin), 12);
            undo.capture(mesh + field::sceneNode::passMask, 4);
            out.push_back(value);
        }
    }
    return out;
}
static void stereoApplyEye(const fp::xr::Eye& eye, const std::vector<StereoMesh>& meshes, const std::vector<uintptr_t>& sectors) {
    memcpy(reinterpret_cast<void*>(stereoCamera + field::camera::pose), eye.pose.data(), 48);
    memcpy(reinterpret_cast<void*>(stereoCamera + field::camera::pose2), eye.pose.data(), 48);
    rebuildInverse(stereoCamera + field::camera::inverse, stereoCamera + field::camera::pose);
    const auto view = fp::xr::viewMatrix(eye.pose), combined = fp::xr::product(eye.projection, view);
    memcpy(reinterpret_cast<void*>(stereoView + 0x60), view.data(), 64);
    memcpy(reinterpret_cast<void*>(stereoView + 0xA0), eye.projection.data(), 64);
    memcpy(reinterpret_cast<void*>(stereoView + 0xE0), eye.projection.data(), 64);
    // The lower draw owner consumes these published matrices and does not resample nav.
    memcpy(reinterpret_cast<void*>(base + 0x49AD10), eye.projection.data(), 64);
    memcpy(reinterpret_cast<void*>(base + 0x49AE10), view.data(), 64);
    memcpy(reinterpret_cast<void*>(base + 0x49AF10), combined.data(), 64);
    *reinterpret_cast<int*>(base + 0x49ACB0) = 0;
    memcpy(reinterpret_cast<void*>(stereoScene + 0x1C38), eye.pose.data() + 9, 12);
    memcpy(reinterpret_cast<void*>(stereoScene + 0x1C44), view.data() + 8, 12);
    stereoWriteVolume(stereoView, fp::xr::corners(eye, stereoFrame.nearPlane, stereoFrame.farPlane));
    // Sector rectangles were projected for the desktop camera; the eye frustum and GPU
    // clipping replace that screen-space optimisation for the replay.
    for (auto sector : sectors) *(unsigned char*)(sector + 0x3A0) = 0;
    const auto nativeTarget = *reinterpret_cast<uintptr_t*>(base + 0x49A4A0);
    *(unsigned char*)(nativeTarget + 0x1CC) = 0;
    const auto frustum = fp::xr::planes(fp::xr::corners(eye, stereoFrame.nearPlane, stereoFrame.farPlane));
    for (const auto& mesh : meshes) {
        uint32_t mask = mesh.mask & ~0x10000000u;
        if (!fp::xr::visible(frustum, mesh.bounds.data(), mesh.origin)) mask &= ~0x01000001u;
        *reinterpret_cast<uint32_t*>(mesh.address + field::sceneNode::passMask) = mask;
    }
}
static void stereoCaptureState(fp::xr::Transaction& undo, uintptr_t renderer) {
    undo.capture(stereoCamera + 0x40, 0xA8);
    undo.capture(stereoView + 0x60, 0xC0);
    stereoVolumeSnapshot(undo, stereoView);
    undo.capture(stereoScene + 0x1C38, 24);
    undo.capture(stereoScene + 0x1DAC, 4);
    undo.capture(base + 0x49ACB0, 0xC10);
    undo.capture(base + 0x2AE3F0, 0x58);
    undo.capture(base + 0x2EEFC0, 4);
    undo.capture(base + 0x5B7480, 0x6018);
    const auto target = *reinterpret_cast<uintptr_t*>(base + 0x49A4A0);
    undo.capture(target + 0x1BC, 0x11);
    const auto manager = *reinterpret_cast<uintptr_t*>(base + 0x5B1E10);
    undo.capture(manager + 0x18, 4);
    undo.capture(renderer + 8, 4);
    const auto environment = *reinterpret_cast<uintptr_t*>(renderer + 0x118);
    for (auto range : {std::pair<size_t, size_t>{0x3C, 8}, {0x9C, 12}, {0xB8, 12}, {0xD4, 16}, {0x208, 28}, {0x23C, 4}, {0x290, 8}})
        undo.capture(environment + range.first, range.second);
}
// After a replay the captured bytes go back only into an owner that still exists; a scene or
// view replaced by the native draw must not receive them. Material matrix bindings are then
// re-uploaded rather than trusted by pointer identity.
static bool stereoSettle(fp::xr::Transaction& undo, uintptr_t manager) {
    if (!stereoLifetime()) {
        undo.abandon();
        stereoReason = "native_stereo_owner_changed";
        return false;
    }
    undo.restore();
    return stereoCall(stereoFlushBindings, manager);
}
static void stereoReplay(uintptr_t renderer, float exposureSeed, bool leftDrawnNatively = false) {
    struct Scope { Scope() { stereoReplaying = true; } ~Scope() { stereoReplaying = false; } } scope;
    fp::xr::Transaction undo(readable);
    stereoCaptureState(undo, renderer);
    const auto manager = *reinterpret_cast<uintptr_t*>(base + 0x5B1E10);
    const auto environment = *reinterpret_cast<uintptr_t*>(renderer + 0x118);
    std::vector<uintptr_t> sectors;
    const auto meshes = stereoMeshes(undo, sectors);
    // Eye scale: below 100 % the eye replays draw into a smaller corner of the native target, so the game shades
    // fewer pixels per eye; the view's size fields carry it and the undo puts them back. Only a full replay can
    // shrink (the mirror path shows the native draw at full size), and only in geometry mode (reconstruction
    // reads the whole target).
    const int percent = leftDrawnNatively || stereoReconstructing() ? 100 : (std::max)(50, (std::min)(100, vrEyeScalePercent.load(std::memory_order_relaxed)));
    const int fullWidth = *reinterpret_cast<int*>(stereoView + 0x160), fullHeight = *reinterpret_cast<int*>(stereoView + 0x164);
    const int width = (std::max)(32, fullWidth * percent / 100), height = (std::max)(32, fullHeight * percent / 100);
    if (percent < 100) undo.capture(stereoView + 0x160, 8);
    // The whole target is backed up whatever the eye scale, so a pass that ignores the view size still leaves the
    // desktop whole; only the delivered rectangle shrinks.
    if (!stereoBackend.saveTarget || !stereoBackend.saveTarget(fullWidth, fullHeight)) throw std::runtime_error("native target backup unavailable");
    if (percent < 100 && stereoBackend.eyeRect) stereoBackend.eyeRect(width, height);
    try {
        if (leftDrawnNatively) { // the backup just taken is the left eye: deliver it, then replay the right only
            if (!stereoBackend.copyEye || !stereoBackend.copyEye(0)) throw std::runtime_error("XR eye copy failed");
            ++stereoEyes;
        }
        for (unsigned eye = leftDrawnNatively ? 1u : 0u; eye < 2; ++eye) {
            if (!stereoIdentity()) throw std::runtime_error("native stereo owner changed");
            if (!stereoBackend.prepareEye || !stereoBackend.prepareEye(eye)) throw std::runtime_error("native GL state restoration failed");
            undo.restore();
            if (percent < 100) { *reinterpret_cast<int*>(stereoView + 0x160) = width; *reinterpret_cast<int*>(stereoView + 0x164) = height; }
            stereoApplyEye(stereoFrame.eyes[eye], meshes, sectors);
            // 7BC80 advances exposure while deriving the eye's environment values.
            *reinterpret_cast<float*>(environment + 0x40) = exposureSeed;
            viewEffectsEyeDraw = true; // this eye's silhouette replay draws the held-item mask in its own view
            const bool drawn = stereoCall(originalStereoDraw, renderer);
            if (!drawn) { viewEffectsEyeDraw = false; throw std::runtime_error("native eye draw fault"); }
            trackedEffectsEye(false);
            viewEffectsEyeDraw = false;
            trackedComfortEye();
            if (!stereoBackend.copyEye || !stereoBackend.copyEye(eye)) throw std::runtime_error("XR eye copy failed");
            ++stereoEyes;
        }
        stereoBackend.restoreTarget();
    } catch (...) {
        bool targetRestored = true;
        try { stereoBackend.restoreTarget(); } catch (...) { targetRestored = false; }
        if (!stereoSettle(undo, manager) || !targetRestored) throw std::runtime_error("native replay restoration failed");
        throw;
    }
    if (!stereoSettle(undo, manager)) throw std::runtime_error("native shader cache invalidation failed");
}
// Reconstruction: the desktop draw already happened from the desktop camera. Draw the world
// once more from the head with both eyes' frustum, copy colour and depth, and let the host
// synthesise both eyes. Native state is restored exactly as after a geometry replay.
static void stereoReconstruct(uintptr_t renderer, float exposureSeed) {
    struct Scope { Scope() { stereoReplaying = true; } ~Scope() { stereoReplaying = false; } } scope;
    fp::xr::Transaction undo(readable);
    stereoCaptureState(undo, renderer);
    const auto manager = *reinterpret_cast<uintptr_t*>(base + 0x5B1E10);
    const auto environment = *reinterpret_cast<uintptr_t*>(renderer + 0x118);
    std::vector<uintptr_t> sectors;
    const auto meshes = stereoMeshes(undo, sectors);
    const int width = *reinterpret_cast<int*>(stereoView + 0x160), height = *reinterpret_cast<int*>(stereoView + 0x164);
    if (!stereoBackend.saveTarget || !stereoBackend.saveTarget(width, height)) throw std::runtime_error("native target backup unavailable");
    try {
        if (!stereoBackend.prepareEye || !stereoBackend.prepareEye(0)) throw std::runtime_error("native GL state restoration failed");
        stereoApplyEye(stereoFrame.center, meshes, sectors);
        *reinterpret_cast<float*>(environment + 0x40) = exposureSeed;
        if (!stereoCall(originalStereoDraw, renderer)) throw std::runtime_error("native centre draw fault");
        trackedEffectsEye(true); // both eyes are warped from this image, which is also the desktop's
        trackedComfortEye();      // sets the strength; the host rings the warped eyes
        fp::xr::FrameContract contract{};
        contract.width = width;
        contract.height = height;
        contract.subrect = {0, 0, width, height};
        contract.projection = stereoFrame.center.projection;
        contract.view = stereoFrame.center.pose;
        contract.unitsPerMetre = stereoUnitsPerMetre;
        contract.clipNear = stereoFrame.nearPlane;
        contract.clipFar = stereoFrame.farPlane;
        contract.serial = stereoFrame.serial;
        contract.sceneGeneration = stereoScene;
        contract.contextGeneration = reinterpret_cast<uintptr_t>(wglGetCurrentContext());
        GLfloat range[2]{};
        glGetFloatv(0x0B70, range);
        contract.depth.rangeNear = range[0];
        contract.depth.rangeFar = range[1];
        if (!stereoBackend.reconstruct || !stereoBackend.reconstruct(contract, stereoFrame)) throw std::runtime_error("reconstruction unavailable");
        stereoEyes += 2;
        stereoBackend.restoreTarget();
    } catch (...) {
        bool targetRestored = true;
        try { stereoBackend.restoreTarget(); } catch (...) { targetRestored = false; }
        undo.restore();
        const bool bindings = stereoCall(stereoFlushBindings, manager);
        if (!targetRestored || !bindings) throw std::runtime_error("native reconstruction restoration failed");
        throw;
    }
    if (!stereoSettle(undo, manager)) throw std::runtime_error("native shader cache invalidation failed");
}
static void stereoWorldDraw(uintptr_t renderer) {
    float exposureSeed = 0;
    bool seedValid = false;
    if (stereoRenderContext() && stereoPrepared && !stereoReplaying && readable(renderer + 0x118, 8)) {
        const auto environment = *reinterpret_cast<uintptr_t*>(renderer + 0x118);
        if (readable(environment + 0x40, 4)) { exposureSeed = *reinterpret_cast<float*>(environment + 0x40); seedValid = true; }
    }
    // Mirror: the native draw itself renders the left eye, under the camera lock like a replay, and
    // the native matrices come back before anything else reads them.
    bool mirrored = false;
    std::unique_ptr<fp::xr::Transaction> mirrorUndo;
    if (seedValid && vrMirrorLeft.load(std::memory_order_relaxed) && !stereoReconstructing() && !stereoDrawn && GetCurrentThreadId() == stereoThread &&
        TryAcquireSRWLockExclusive(&cameraLock)) {
        try {
            if (stereoIdentity() && *reinterpret_cast<uintptr_t*>(base + 0x49C9E0) == stereoView && readable(renderer, 0x170) && *reinterpret_cast<uintptr_t*>(renderer + 0x10) == stereoScene &&
                *reinterpret_cast<int*>(base + 0x49ACB0) == 0 && stereoPassesAdmitted(stereoView) && stereoCallbacksAdmitted(stereoView, renderer)) {
                mirrorUndo.reset(new fp::xr::Transaction(readable));
                stereoCaptureState(*mirrorUndo, renderer);
                stereoApplyEye(stereoFrame.eyes[0], {}, {});
                mirrored = true;
            }
        } catch (const std::exception& e) { mirrorUndo.reset(); stereoNoteFault(e); ++stereoFaults; }
        if (!mirrored) ReleaseSRWLockExclusive(&cameraLock);
    }
    originalStereoDraw(renderer);
    if (mirrored) {
        bool nativeBack = true;
        try { trackedEffectsEye(true); trackedComfortEye(); } catch (const std::exception& e) { stereoNoteFault(e); ++stereoFaults; } // the left eye, with its matrices still in the view
        try { mirrorUndo->restore(); } catch (const std::exception& e) { nativeBack = false; stereoNoteFault(e); ++stereoFaults; }
        mirrorUndo.reset();
        if (!nativeBack || !stereoRenderContext() || !stereoPrepared) { stereoReason = "mirror_restore_failed"; ReleaseSRWLockExclusive(&cameraLock); return; }
        try {
            stereoReplay(renderer, exposureSeed, true);
            stereoDrawn = true;
            ++stereoFrames;
            stereoReason = "emulated_stereo_mirror";
        } catch (const std::exception& e) {
            ++stereoFaults;
            stereoNoteFault(e);
            stereoReason = "XR_replay_failed";
            stereoLifetime();
        }
        ReleaseSRWLockExclusive(&cameraLock);
        return;
    }
    if (!stereoRenderContext() || !stereoPrepared || stereoDrawn || stereoReplaying || GetCurrentThreadId() != stereoThread ||
        !TryAcquireSRWLockExclusive(&cameraLock)) return;
    try {
        if (!stereoIdentity() || *reinterpret_cast<uintptr_t*>(base + 0x49C9E0) != stereoView || !readable(renderer, 0x170) || *reinterpret_cast<uintptr_t*>(renderer + 0x10) != stereoScene) {
            stereoReason = "foreign_or_stale_world_draw";
            ++stereoRejected;
        } else if (!seedValid || *reinterpret_cast<int*>(base + 0x49ACB0) != 0 || !stereoPassesAdmitted(stereoView) || !stereoCallbacksAdmitted(stereoView, renderer)) {
            stereoReason = "unsupported_pass_or_mutable_callback";
            ++stereoRejected;
        } else {
            if (stereoReconstructing()) stereoReconstruct(renderer, exposureSeed);
            else stereoReplay(renderer, exposureSeed);
            stereoDrawn = true;
            ++stereoFrames;
            stereoReason = stereoReconstructing() ? "emulated_reconstruction" : "emulated_stereo";
        }
    } catch (const std::exception& e) {
        ++stereoFaults;
        stereoNoteFault(e);
        stereoReason = "native_eye_replay_failed";
    }
    ReleaseSRWLockExclusive(&cameraLock);
}
// The emulation opt-in is the process environment or an explicit xr-emulation.ini beside the DLL.
// The file publishes its values into this process only, so a game that Steam started (whose
// environment an emulation run cannot set) can still carry them; nothing persists past the process.
static void stereoReadOptIn() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    auto slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"xr-emulation.ini");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    static constexpr const wchar_t* keys[][2] = {
        {L"Directory", L"EXCAMERA_XR_EMULATION_DIR"}, {L"Runtime", L"XR_RUNTIME_JSON"},
        {L"UpperBody", L"EXCAMERA_XR_UPPER_BODY"}, {L"FullBody", L"EXCAMERA_XR_FULL_BODY"},
        {L"Locomotion", L"EXCAMERA_XR_LOCOMOTION"}, {L"Drive", L"EXCAMERA_XR_DRIVE"},
        {L"BodyDriver", L"EXCAMERA_XR_BODY_DRIVER"}, {L"Weapons", L"EXCAMERA_XR_WEAPONS"},
        {L"Evidence", L"EXCAMERA_XR_EVIDENCE_DIR"}, {L"EvidenceFrames", L"EXCAMERA_XR_EVIDENCE_FRAMES"},
        {L"KickBindingVive", L"EXCAMERA_XR_KICK_VIVE"}, {L"KickBindingSimple", L"EXCAMERA_XR_KICK_SIMPLE"}}; // kick slots for the profiles without a stick click; unbound unless set
    for (const auto& key : keys) {
        wchar_t value[1024]{};
        if (GetEnvironmentVariableW(key[1], nullptr, 0)) continue; // the environment wins
        if (GetPrivateProfileStringW(L"Emulation", key[0], L"", value, 1024, path) > 0) SetEnvironmentVariableW(key[1], value);
    }
    // Emulation-run values of the three VR rows, applied as unsaved live values so the player's own
    // first-person.ini is neither read for them nor rewritten by the run.
    const int ipd = GetPrivateProfileIntW(L"Emulation", L"IpdMillimetres", -1, path);
    const int scale = GetPrivateProfileIntW(L"Emulation", L"UnitsPerMetre", -1, path);
    const int mode = GetPrivateProfileIntW(L"Emulation", L"RenderMode", -1, path);
    if (ipd >= 0 && ipd <= 100) vrIpdMm = ipd;
    if (scale >= 50 && scale <= 200) vrUnitsPerMetre = scale;
    if (mode >= 0 && mode <= 2) vrRenderMode = mode;
    const int hands = GetPrivateProfileIntW(L"Emulation", L"Hands", -1, path);
    if (hands == 0 || hands == 1) vrHandsDrawn = hands;
    const int posture = GetPrivateProfileIntW(L"Emulation", L"Posture", -1, path);
    if (posture == 0 || posture == 1) vrPosture = posture;
    const int mirror = GetPrivateProfileIntW(L"Emulation", L"MirrorLeft", -1, path);
    if (mirror == 0 || mirror == 1) vrMirrorLeft = mirror;
    const int effects = GetPrivateProfileIntW(L"Emulation", L"Effects", -1, path);
    if (effects == 0 || effects == 1) vrEffects = effects;
    const int comfort = GetPrivateProfileIntW(L"Emulation", L"Comfort", -1, path);
    if (comfort >= 0 && comfort <= 2) vrComfort = comfort;
    const int body = GetPrivateProfileIntW(L"Emulation", L"Body", -1, path);
    if (body == 0 || body == 1) vrBodyHandsOnly = body;
    const int title = GetPrivateProfileIntW(L"Emulation", L"MainMenu", -1, path);
    if (title == 0 || title == 1) vrTitleMenu = title;
    const int pointer = GetPrivateProfileIntW(L"Emulation", L"Pointer", -1, path);
    if (pointer == 0 || pointer == 1) vrPointer = pointer;
    const int memory = GetPrivateProfileIntW(L"Emulation", L"RecenterMemory", -1, path);
    if (memory == 0 || memory == 1) vrRecenterMemory = memory;
    const int wristMenu = GetPrivateProfileIntW(L"Emulation", L"WristMenu", -1, path);
    if (wristMenu == 0 || wristMenu == 1) vrWristMenu = wristMenu;
    const int haft = GetPrivateProfileIntW(L"Emulation", L"HaftGrip", -1, path);
    if (haft == 0 || haft == 1) vrHaftGrip = haft;
    const int height = GetPrivateProfileIntW(L"Emulation", L"HeightCentimetres", -1000, path);
    if (height >= -50 && height <= 50) vrHeightCm = height;
    const int grabTurn = GetPrivateProfileIntW(L"Emulation", L"GrabTurn", -1, path);
    if (grabTurn == 0 || grabTurn == 1) vrGrabTurn = grabTurn;
    const int frameTime = GetPrivateProfileIntW(L"Emulation", L"FrameTime", -1, path);
    if (frameTime == 0 || frameTime == 1) vrFrameTime = frameTime;
    const int eyeScale = GetPrivateProfileIntW(L"Emulation", L"EyeScalePercent", -1, path);
    if (eyeScale >= 50 && eyeScale <= 100) vrEyeScalePercent = eyeScale;
    const int handMesh = GetPrivateProfileIntW(L"Emulation", L"HandMesh", -1, path);
    if (handMesh == 0 || handMesh == 1) vrHandMesh = handMesh;
    const int trackedGrab = GetPrivateProfileIntW(L"Emulation", L"TrackedGrab", -1, path);
    if (trackedGrab == 0 || trackedGrab == 1) vrTrackedGrab = trackedGrab;
    const int slide = GetPrivateProfileIntW(L"Emulation", L"GripSlideCentimetres", -1000, path);
    if (slide >= -10 && slide <= 10) vrGripSlideCm = slide;
    const int kick = GetPrivateProfileIntW(L"Emulation", L"Kick", -1, path);
    if (kick == 0 || kick == 1) vrKick = kick;
    const int jump = GetPrivateProfileIntW(L"Emulation", L"Jump", -1, path);
    if (jump == 0 || jump == 1) vrJump = jump;
    const int weight = GetPrivateProfileIntW(L"Emulation", L"WeaponWeight", -1, path);
    if (weight == 0 || weight == 1) vrWeaponWeight = weight;
    const int teleport = GetPrivateProfileIntW(L"Emulation", L"Teleport", -1, path);
    if (teleport == 0 || teleport == 1) vrTeleport = teleport;
    const int handPanel = GetPrivateProfileIntW(L"Emulation", L"HandPanel", -1, path);
    if (handPanel == 0 || handPanel == 1) vrHandPanel = handPanel;
    const int pouchRow = GetPrivateProfileIntW(L"Emulation", L"HipPouch", -1, path);
    if (pouchRow == 0 || pouchRow == 1) vrPouch = pouchRow;
    const int bones = GetPrivateProfileIntW(L"Emulation", L"FingerBones", -1, path);
    if (bones == 0 || bones == 1) vrFingerBones = bones;
    const int grip = GetPrivateProfileIntW(L"Emulation", L"GripAngleDegrees", -1000, path);
    if (grip >= -45 && grip <= 45) vrGripAngle = grip;
    const int haptic = GetPrivateProfileIntW(L"Emulation", L"HapticPercent", -1, path);
    if (haptic >= 0 && haptic <= 200) vrHapticPercent = haptic;
    const int panelWidth = GetPrivateProfileIntW(L"Emulation", L"PanelWidthCentimetres", -1, path);
    if (panelWidth >= 60 && panelWidth <= 200) vrPanelWidthCm = panelWidth;
    const int panelDistance = GetPrivateProfileIntW(L"Emulation", L"PanelDistanceCentimetres", -1, path);
    if (panelDistance >= 80 && panelDistance <= 300) vrPanelDistanceCm = panelDistance;
}
// Which runtime, if any, this session asks for: the dev opt-in (environment or the file beside the
// DLL) wins; otherwise the player's headset row selects the real runtime through the loader shipped
// beside the mod. No loader beside the DLL means a reported reason, never a search of the machine.
enum class StereoChoice { none, emulation, hardware, loaderMissing };
static StereoChoice stereoChoose(bool optIn, bool headsetRow, bool loaderBeside) {
    if (optIn) return StereoChoice::emulation;
    if (!headsetRow) return StereoChoice::none;
    return loaderBeside ? StereoChoice::hardware : StereoChoice::loaderMissing;
}
static bool createStereoHooks() {
    stereoReadOptIn();
    wchar_t directory[32768]{};
    const bool optIn = GetEnvironmentVariableW(L"EXCAMERA_XR_EMULATION_DIR", directory, 32768) > 0;
    wchar_t beside[MAX_PATH]{};
    GetModuleFileNameW(module, beside, MAX_PATH);
    if (auto slash = wcsrchr(beside, L'\\')) slash[1] = 0;
    const std::wstring loader = std::wstring(beside) + L"openxr_loader.dll";
    const bool loaderBeside = beside[0] && GetFileAttributesW(loader.c_str()) != INVALID_FILE_ATTRIBUTES;
    const StereoChoice choice = stereoChoose(optIn, vrHeadset.load() != 0, loaderBeside);
    if (choice == StereoChoice::loaderMissing) { stereoReason = "loader_missing_beside_mod"; return true; }
    if (choice == StereoChoice::hardware) {
        SetEnvironmentVariableW(L"EXCAMERA_XR_EMULATION_DIR", beside);
        SetEnvironmentVariableW(L"EXCAMERA_XR_RUNTIME", L"hardware");
        SetEnvironmentVariableW(L"EXCAMERA_XR_UPPER_BODY", L"1");
        SetEnvironmentVariableW(L"EXCAMERA_XR_DRIVE", L"1");
    }
    stereoRequested = choice != StereoChoice::none;
    if (!stereoRequested) return true;
    stereoBackend = fp::xr::emulatedBackend();
    // Admission rests on the whole-function byte contracts of every owner the replay touches;
    // a patched executable with those owners intact is admitted and reported as compatible.
    if (!fp::xr::admitted(base, readable)) { stereoReason = "native_contract_mismatch"; return true; }
    trackedItemRelease = layout.rva[fp::sig::heldItemRelease] ? reinterpret_cast<TrackedItemRelease>(gameAt(fp::sig::heldItemRelease)) : nullptr;
    stereoRebuildPlanes = reinterpret_cast<Behavior>(base + 0x78050);
    stereoFlushBindings = reinterpret_cast<Behavior>(base + 0x1FF3C0);
    if (MH_CreateHook(reinterpret_cast<void*>(base + 0x4F750), reinterpret_cast<void*>(stereoWorldDraw), reinterpret_cast<void**>(&originalStereoDraw)) != MH_OK) return false;
    stereoHooks[0] = true;
    if (MH_CreateHook(reinterpret_cast<void*>(base + 0x9F500), reinterpret_cast<void*>(stereoFrustum), reinterpret_cast<void**>(&originalStereoFrustum)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(base + 0x4F750));
        stereoHooks[0] = false;
        return false;
    }
    stereoHooks[1] = true;
    if (!createTrackedDriveHooks()) return false;
    stereoReason = "waiting_for_owned_view";
    return true;
}
static MH_STATUS disableStereoHooks() {
    MH_STATUS result = disableTrackedDriveHooks();
    const uintptr_t rvas[2] = {0x4F750, 0x9F500};
    for (unsigned i = 0; i < 2; ++i)
        if (stereoHooks[i]) {
            const auto status = nativeDisableHook(reinterpret_cast<void*>(base + rvas[i]));
            awaitNativeHookRecovery();
            if (status != MH_OK && status != MH_ERROR_DISABLED) result = status;
        }
    return result;
}
static bool stereoDrained() { return !stereoLive.load() && trackedPhysicalDrained(); }
static void stereoBeforePresent() {
    if (!stereoRequested || !stereoRenderContext()) return;
    if (!TryAcquireSRWLockExclusive(&cameraLock)) return;
    try { trackedWristPresent(); trackedBeamPresent(); if (stereoBackend.overlay) stereoBackend.overlay(uiOpen(), trackedHandsWanted(), stereoTitleWanted() && !savedRoot); } // the title menu stands in the level even on a frame the headset refused
    catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_overlay_failed"; }
    trackedHapticFlush();
    stereoEndFrame();
    ReleaseSRWLockExclusive(&cameraLock);
    trackedReleaseTick();
    trackedPickupTick();
    try { if (stereoBackend.finish) stereoBackend.finish(false); }
    catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_submission_failed"; }
    if (stereoBackend.lost && stereoBackend.lost()) { // headset or runtime gone: fall back to the desktop, try again in a while
        try { if (!stereoBackend.retire || stereoBackend.retire()) stereoLive = false; }
        catch (const std::exception&) { ++stereoFaults; }
        stereoRetryAt = GetTickCount64() + stereoRetryDelayMs;
        stereoRetryDelayMs = (std::min)(stereoRetryDelayMs * 2, stereoRetryMaxMs);
        ++stereoRuntimeLosses;
        stereoReason = "runtime_lost_retrying";
    }
    if (stopping || (!active && !stereoTitleWanted())) { // the title needs no first-person camera
        try { if (!stereoBackend.retire || stereoBackend.retire()) stereoLive = false; }
        catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_retirement_failed"; }
    }
}
static void stereoLeavingContext() {
    trackedDriveClear();
    if (!stereoRequested || !stereoRenderContext()) return;
    if (!TryAcquireSRWLockExclusive(&cameraLock)) return;
    stereoEndFrame();
    ReleaseSRWLockExclusive(&cameraLock);
    try { if (!stereoBackend.retire || stereoBackend.retire()) stereoLive = false; }
    catch (const std::exception&) { ++stereoFaults; stereoReason = "XR_context_retirement_failed"; }
}

// Controller-held items: desired poses come from the hold; native placement decides.
struct TrackedHoldState {
    fp::xr::HandHold hold;
    uint64_t object = 0, generation = 0, epoch = 0, serial = UINT64_MAX;
    fp::xr::ItemTarget target{};
};
static thread_local TrackedHoldState trackedHold;
// The hip pouch: the player's inventory reached at the left hip, through ScriptRuntime.dll's inventory
// exports (work/vr-rebuild/INVENTORY-EXPORTS.md, version 1), the runtime being the one owner of every
// transfer. Letting go of a held item at the hip puts it away (native release, then the runtime's pickup);
// squeezing an empty hand there takes the last item put away back into that hand (the runtime's drop at
// the hand, then the native grab). Without the exports, or with the row off, nothing changes.
namespace pouch {
struct ItemV1 { uint64_t ticket; uint32_t row, item, state; int16_t x, y; uint32_t flags; char name[32]; };
static_assert(sizeof(ItemV1) == 64, "InventoryItemV1 layout");
using Version = uint32_t (*)();
using List = int (*)(ItemV1*, int);
using Drop = uint32_t (*)(uint64_t, uint32_t, const float*);
using Pickup = uint32_t (*)(uintptr_t);
using State = int (*)(uint32_t, uintptr_t*);
struct Exports { List list = nullptr; Drop drop = nullptr; Pickup pickup = nullptr; State state = nullptr; };
constexpr float reach = .22f;          // metres from the hip point that count as at the hip
constexpr unsigned remembered = 16;    // items put away this session, newest last
}
static pouch::Exports trackedPouchExports{};
static uint64_t trackedPouchProbed = 0;
static std::array<uint32_t, pouch::remembered> trackedPouchItems{};
static unsigned trackedPouchCount = 0;
static uint32_t trackedPouchRequest = 0, trackedPouchRequestItem = 0;
static unsigned trackedPouchRequestSide = 0;
static bool trackedPouchRequestIsDrop = false;
static std::atomic<unsigned> trackedPouchStores{0}, trackedPouchTakes{0}, trackedPouchFaults{0};
static std::array<bool, 2> trackedPouchGrab{}; // a completed take: the next frame grabs with that hand
static std::vector<uint32_t> trackedPouchBefore; // carried item ids before a put-away, to find the one it added
static bool trackedPouchIds(std::vector<uint32_t>& ids) {
    ids.clear();
    std::array<pouch::ItemV1, 128> rows{};
    int count = 0;
    __try { count = trackedPouchExports.list(rows.data(), int(rows.size())); }
    __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; trackedPouchFaults.fetch_add(1, std::memory_order_relaxed); }
    if (count < 0) return false;
    for (int r = 0; r < count && r < int(rows.size()); ++r) ids.push_back(rows[size_t(r)].item);
    return true;
}
static bool trackedPouchReady() {
    if (!vrPouch.load(std::memory_order_relaxed)) return false;
    if (trackedPouchExports.list) return true;
    const uint64_t now = GetTickCount64();
    if (now - trackedPouchProbed < 2000) return false;
    trackedPouchProbed = now;
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) return false;
    const auto version = reinterpret_cast<pouch::Version>(GetProcAddress(runtime, "inventory_exports_version"));
    pouch::Exports e{reinterpret_cast<pouch::List>(GetProcAddress(runtime, "inventory_list")), reinterpret_cast<pouch::Drop>(GetProcAddress(runtime, "inventory_drop_row")),
                     reinterpret_cast<pouch::Pickup>(GetProcAddress(runtime, "inventory_pickup_node")), reinterpret_cast<pouch::State>(GetProcAddress(runtime, "inventory_request_state"))};
    if (!version || !e.list || !e.drop || !e.pickup || !e.state) return false;
    uint32_t v = 0;
    __try { v = version(); } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    if (v != 1) return false;
    trackedPouchExports = e;
    return true;
}
// The left hip in the tracking space: the waist tracker when there is one, else below and left of the head.
static bool trackedPouchHip(const fp::xr::TrackingFrame& frame, XrVector3f& hip) {
    const auto& head = frame.sample.head;
    if (!fp::xr::valid(head)) return false;
    const XrVector3f forward = fp::xr::rotate(head.orientation, {0, 0, -1});
    const float yaw = std::atan2(-forward.x, -forward.z);
    const XrVector3f right = {std::cos(yaw), 0, -std::sin(yaw)};
    if (fp::xr::lowerValid(frame.lower, frame.lower.time)) {
        const XrPosef waist = frame.lower.trackers[0].pose;
        hip = fp::xr::add(waist.position, fp::xr::scale(right, -.17f));
        return true;
    }
    hip = fp::xr::add({head.position.x, head.position.y - .68f, head.position.z}, fp::xr::scale(right, -.18f));
    return true;
}
static bool trackedPouchAtHip(const fp::xr::TrackingFrame& frame, XrVector3f at) {
    XrVector3f hip{};
    if (trackedPouchHip(frame, hip) && fp::xr::length(fp::xr::sub(at, hip)) <= pouch::reach) return true;
    // or over the inventory held in the left hand: the game's own release over a window never inserts
    XrPosef panel{};
    return vrHandPanel.load(std::memory_order_relaxed) && *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0 &&
           fp::xr::handPanelPose(frame.sample, panel) && fp::xr::length(fp::xr::sub(at, panel.position)) <= .25f;
}
static int trackedPouchStateCall(uint32_t request, uintptr_t* node) { // SEH-only frame
    __try { return trackedPouchExports.state(request, node); }
    __except (EXCEPTION_EXECUTE_HANDLER) { trackedPouchFaults.fetch_add(1, std::memory_order_relaxed); return -1; }
}
// The runtime publishes its table only while the exports were called in the last 5 s (r39 review), so the
// pouch keeps it warm every 2 s while on and remembers the last good list for a put-away's "before".
static std::vector<uint32_t> trackedPouchWarm;
static uint64_t trackedPouchWarmAt = 0, trackedPouchWarmGood = 0;
// Polled each frame: the outstanding request finishes, and a completed take arms a grab.
static void trackedPouchPoll() {
    const uint64_t now = GetTickCount64();
    if (trackedPouchReady() && now - trackedPouchWarmAt >= 2000) {
        trackedPouchWarmAt = now;
        if (trackedPouchIds(trackedPouchWarm)) trackedPouchWarmGood = now;
    }
    if (!trackedPouchRequest || !trackedPouchExports.state) return;
    uintptr_t node = 0;
    const int state = trackedPouchStateCall(trackedPouchRequest, &node);
    if (state == 0) return;
    if (state == 1 && trackedPouchRequestIsDrop) {
        trackedPouchTakes.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < trackedPouchCount; ++i) // the item is out again
            if (trackedPouchItems[i] == trackedPouchRequestItem) { for (unsigned k = i + 1; k < trackedPouchCount; ++k) trackedPouchItems[k - 1] = trackedPouchItems[k]; --trackedPouchCount; break; }
        trackedPouchGrab[trackedPouchRequestSide & 1] = true;
    } else if (state == 1) {
        trackedPouchStores.fetch_add(1, std::memory_order_relaxed);
        std::vector<uint32_t> after; // the item the put-away added: one more of it than before
        trackedPouchRequestItem = 0;
        if (trackedPouchIds(after))
            for (uint32_t id : after)
                if (std::count(after.begin(), after.end(), id) > std::count(trackedPouchBefore.begin(), trackedPouchBefore.end(), id)) { trackedPouchRequestItem = id; break; }
        if (trackedPouchCount == pouch::remembered) { for (unsigned k = 1; k < pouch::remembered; ++k) trackedPouchItems[k - 1] = trackedPouchItems[k]; --trackedPouchCount; }
        if (trackedPouchRequestItem) trackedPouchItems[trackedPouchCount++] = trackedPouchRequestItem;
    }
    trackedPouchRequest = 0;
}
// Put away: the node just released natively at the hip or over the open inventory panel.
static void trackedPouchStore(uintptr_t node) {
    if (trackedPouchRequest || !trackedPouchReady()) return;
    if (!trackedPouchIds(trackedPouchBefore)) { // not published this pulse: the warm copy, if recent
        if (!trackedPouchWarmGood || GetTickCount64() - trackedPouchWarmGood > 4000) return;
        trackedPouchBefore = trackedPouchWarm;
    }
    uint32_t id = 0;
    __try { id = trackedPouchExports.pickup(node); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; trackedPouchFaults.fetch_add(1, std::memory_order_relaxed); }
    if (!id) return;
    trackedPouchRequest = id;
    trackedPouchRequestItem = 0;
    trackedPouchRequestIsDrop = false;
}
// Take out: the newest item put away that is still carried, dropped at `position` (player-sector cm) for `side`.
static bool trackedPouchTake(unsigned side, const float* position) {
    if (trackedPouchRequest || !trackedPouchCount || !trackedPouchReady()) return false;
    std::array<pouch::ItemV1, 128> rows{};
    int count = 0;
    __try { count = trackedPouchExports.list(rows.data(), int(rows.size())); }
    __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; trackedPouchFaults.fetch_add(1, std::memory_order_relaxed); }
    if (count <= 0) return false;
    for (unsigned k = trackedPouchCount; k-- > 0;)
        for (int r = 0; r < count && r < int(rows.size()); ++r)
            if (rows[size_t(r)].item == trackedPouchItems[k] && (rows[size_t(r)].flags & 1u)) {
                uint32_t id = 0;
                __try { id = trackedPouchExports.drop(rows[size_t(r)].ticket, rows[size_t(r)].row, position); }
                __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; trackedPouchFaults.fetch_add(1, std::memory_order_relaxed); }
                if (!id) return false;
                trackedPouchRequest = id;
                trackedPouchRequestItem = trackedPouchItems[k];
                trackedPouchRequestSide = side;
                trackedPouchRequestIsDrop = true;
                return true;
            }
    return false;
}
struct TrackedRelease { uintptr_t object = 0, root = 0, scene = 0; uint64_t generation = 0; bool atHip = false; };
static thread_local TrackedRelease trackedRelease;
static bool trackedReleaseCall(uintptr_t drag, uintptr_t gui) {
    __try { trackedItemRelease(drag, gui, 0x80); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void trackedReleaseTick() {
    const auto request = trackedRelease;
    trackedRelease = {};
    if (!request.object || !trackedItemRelease || !stereoRenderContext() || !stereoOwner(savedView)) return;
    const auto drag = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::dragManager));
    if (request.root != savedRoot || request.scene != savedScene || request.generation != holdGeneration.load() ||
        !readable(drag, field::drag::invalidDrop + 1) || *reinterpret_cast<unsigned*>(drag + field::drag::flags) != 0x2000 ||
        *reinterpret_cast<uintptr_t*>(drag + field::drag::object) != request.object) return;
    const auto viewport = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::mainViewport)), gui = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiManager));
    if (gameplayHeldObject(viewport) != request.object || !readable(gui, field::gui::capture + 8) ||
        *reinterpret_cast<uintptr_t*>(drag + field::drag::viewport) != viewport) return;
    if (!trackedReleaseCall(drag, gui)) { trackedItemRelease = nullptr; ++stereoFaults; stereoReason = "tracked_item_release_fault"; return; }
    resetHeldGrab();
    if (request.atHip) trackedPouchStore(request.object); // released at the hip or over the inventory: the runtime puts it away
}
// The accepted material point of the current hold, when native placement has recorded one.
static bool trackedAcceptedPoint(uintptr_t object, uintptr_t viewport, float* point) {
    const auto& accepted = acceptedHeldPlacement;
    if (!heldPlacementOwner(accepted) || accepted.object != object || accepted.viewport != viewport) return false;
    memcpy(point, accepted.values, 12);
    return true;
}
static int trackedHeldTarget(uintptr_t object, uintptr_t viewport, float* point, float* axes) {
    if (!stereoRenderContext()) return 0;
    using namespace fp::xr;
    auto& hold = trackedHold.hold;
    auto& target = trackedHold.target;
    TrackingFrame frame{};
    if (!trackingFrame(frame)) { hold.reset(); target = {}; return 0; }
    if (!stereoRenderContext() || !stereoOwner(savedView) || viewport != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::mainViewport)) ||
        gameplayHeldObject(viewport) != object || !trackedItemRelease) { hold.reset(); target = {}; return 1; }
    if (!frame.sample.focused) { hold.reset(); target = {}; trackedHold.serial = UINT64_MAX; return 1; }
    const auto drag = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::dragManager));
    if (!readable(drag, field::drag::flags + 4) || *reinterpret_cast<unsigned*>(drag + field::drag::flags) != 0x2000 ||
        !readable(object, field::sceneNode::transform + 48) || *reinterpret_cast<uintptr_t*>(object + field::sceneNode::parent)) return 1;
    try {
        auto sample = frame.sample;
        for (unsigned side = 0; side < 2; ++side)
            if (sample.hands[side].tracked) {
                auto& hand = sample.hands[side];
                NativePose achieved{};
                const int physical = trackedBodyHand(side, achieved.data());
                if (physical == 1) hand.tracked = false;
                else hand.grip = nativePoseMetres(physical == 2 ? achieved : nativeEye(frame.anchor, frame.calibration.relative(hand.grip), static_cast<float>(vrUnitsPerMetre.load())));
            }
        const InteractionOwner owner{savedScene, savedRoot, holdGeneration.load()};
        if (trackedHold.object != object || trackedHold.generation != owner.generation || trackedHold.epoch != sample.epoch) {
            hold.reset();
            trackedHold.object = object;
            trackedHold.generation = owner.generation;
            trackedHold.epoch = sample.epoch;
            trackedHold.serial = UINT64_MAX;
            target = {};
        }
        if (trackedHold.serial != frame.serial) {
            trackedHold.serial = frame.serial;
            if (!hold.holding()) {
                hold.step(sample, owner);
                NativePose native{};
                memcpy(native.data(), reinterpret_cast<void*>(object + field::sceneNode::transform), 48);
                if (!sceneNodeWorldOrigin(object, native.data() + 9)) return 1;
                float accepted[3];
                if (trackedAcceptedPoint(object, viewport, accepted)) {
                    const XrVector3f hit{accepted[0] / stereoUnitsPerMetre, accepted[1] / stereoUnitsPerMetre, accepted[2] / stereoUnitsPerMetre};
                    for (unsigned i = 0; i < 2; ++i)
                        if (hold.bind(sample, owner, i, object, nativePoseMetres(native, stereoUnitsPerMetre), hit)) {
                            trackedHapticRequest(i, .5f, 40); // the hand closed on the item
                            break;
                        }
                }
                target = {}; // acquire now; update on the next predicted sample.
            } else target = hold.step(sample, owner);
        }
        if (!target.held) {
            if (target.released && !target.cancelled)
                trackedRelease = {object, savedRoot, savedScene, owner.generation, trackedPouchReady() && trackedPouchAtHip(frame, frame.sample.hands[hold.hand() & 1].grip.position)};
            return 1;
        }
        const auto pose = nativeItemPose(target.pose, stereoUnitsPerMetre);
        const float anchorPoint[3] = {target.anchor.x * stereoUnitsPerMetre, target.anchor.y * stereoUnitsPerMetre, target.anchor.z * stereoUnitsPerMetre};
        memcpy(point, anchorPoint, 12);
        memcpy(axes, pose.data(), 36);
        const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
        float distance = 0;
        for (unsigned i = 0; i < 3; ++i) distance += (point[i] - eye[i]) * (point[i] - eye[i]);
        if (!std::isfinite(distance) || distance > static_cast<float>(grabRange.load()) * static_cast<float>(grabRange.load())) return 1;
        return 2; // native release retains native velocity; no synthetic throw.
    } catch (const std::exception&) { hold.reset(); target = {}; return 1; }
}
