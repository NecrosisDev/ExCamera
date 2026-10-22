// Shared state: the resolved game layout, settings, the status file, notices, faults, input release, drag queries.
// Part of first_person.cpp (one translation unit): included there, in order.
#include "../input_ownership.h"
// Threads. Each hook runs on whichever game thread calls the function it replaces. By role (several roles
// may share one OS thread; the status file's nav_thread, swap_thread and sector_thread say which):
// - render thread: swap (once a frame), cursorPosition, the picks, sectorUpdate, the view features;
// - camera: nav (the replaced sampler; navThread), the game's frame logic;
// - physics: poseBuilder, partSettle;
// - input: input (polling), behavior and its cursorAim/explore/combatStep (the player's behaviour tick);
// - window thread: windowProc (keys, mouse, focus);
// - this mod's worker: runSession (activation, F7/F6, notices, the status file, the hook health check);
// - native GUI maintenance: native settings create/update/retirement on the game GUI owner.
// Values one thread writes and others read are atomics or Interlocked, or sit behind a lock below; the
// rest is used by one thread (most of it says which in its comment).
// Reviewed nested acquisitions (SRW, never recursive):
// cameraLock -> aimLock; aimLock -> rawLock, worldPointerLock
// cameraLock -> reticleLock, hideLock, tpHideLock, reachHeadLock, crouchLock, heldThrowLock, rawLock
// cameraLock or settingsLock -> TrackingUdp's sample lock (copy-only leaf).
// worldPointerLock and handNamesLock are leaf publication locks: no nested locks or native calls.
// reachHeadLock, crouchLock and guardLock only copy publications; no other lock or native call is entered under them.
// cursorHoldLock serializes pointer-hold edges; it may take reticleLock,
// heldThrowLock and rawLock through offset/input retirement, never cameraLock.
// heldThrowLock owns gesture/ticket cancellation and the final staged history delta. Native queue,
// release, integration and destruction calls run after releasing it; it takes no other mod lock.
// settingsLock serializes UI actions, FOV keys and persistence acknowledgements. Apart from the
// tracking sample copy, it does not nest these locks; raw registration and waits occur after release.
// cueWglLock serializes core WGL binding/deletion and scoped deletion borrows; it may take cueLock.
// Same-thread WGL nesting uses cueWglDepth rather than recursively acquiring the SRW lock.
// cueLock owns resources and retirement; it never takes game-state locks or cueWglLock.
// Native WGL calls run after releasing cueLock. cueGlDepth rejects WGL reentry from our GL work.
// cameraLock (CameraGuard) guards camera publication, saved stock values and restoration, including
// the reviewed native sampler/math calls. Retirement polling releases owner locks before sleeping:
// awaitRestore uses TryAcquire for its snapshot, and pose/cue waits run outside cameraLock.
// The session worker drains native hook suspension debt before cancellation or callback-retirement waits.
static uintptr_t base;
// Where each game target is in this game build (game_layout.h). Until activation resolves it, the
// source build's addresses, which the offline suites' fake memory is laid out by.
static fp::Layout layout;
// File-image admission finishes before hooks are created. This report is immutable during a session.
static fp::ContractReport nativeContracts;
static bool manualDefenseFieldMismatch = false;
static uintptr_t gameAt(fp::sig::Target t) {
    return base + layout.rva[t];
}
// Calls into the hooks every game tick reaches (fp::HookHealth): the live check that they work.
static std::atomic<uint32_t> healthHits[fp::healthHooks]{};
// Only the worker decides the verdict. Counters remain enabled through menus,
// paused clocks and interrupted windows, and reopen before any hook is re-armed.
static std::atomic<bool> healthRecording{true};
static void healthHit(fp::HealthHook h) {
    if (healthRecording.load(std::memory_order_relaxed))
        healthHits[static_cast<unsigned>(h)].fetch_add(1, std::memory_order_relaxed);
}
static HMODULE module;
using Nav = uintptr_t (*)(void*, float);
static Nav originalNav;
static uintptr_t nav(void*, float);
static volatile LONG stopping = 0, restored = 0;
static volatile LONG pendingRestore = 0;
// Admitted viewport presentations finish their sampled projections before hook removal.
static volatile LONG renderScopesInFlight = 0;
static volatile LONG active = 0;
static volatile LONG protectionFailure = 0;
static uintptr_t savedCamera = 0, savedView = 0, savedController = 0;
// Gameplay identity when the saved frame was committed: controlled root and scene.
static uintptr_t savedRoot = 0, savedScene = 0;
// Parked: first person released the camera because gameplay was
// left (title screen, death-screen EXIT). Hooks stay armed; nav re-attaches once
// parkStep allows. Guarded by cameraLock.
// live parks too (no-menu swaps, bad head frames); repeated live parks
// in one gameplay back off (fp::recordLivePark) instead of stopping.
struct Park {
    bool active = false, unverified = false, left = false, rearmPending = false;
    uintptr_t root = 0, scene = 0;
    unsigned stable = 0;
    fp::LiveParks recent{};
    ULONGLONG backoffUntil = 0;
};
static Park park;
// A released camera that may still be alive: restored if it is ever the current
// camera again with our values still in it (fov we wrote, near 1). cameraLock.
// root/scene: the gameplay it was released in; left latches once that gameplay
// is gone (a stop while it is still current cannot claim restoration).
// several records, so a second release
// in the same gameplay cannot silently drop the first; when all are taken by
// unresolved records of the current gameplay, restoration becomes unknown.
struct Retired {
    uintptr_t camera = 0, root = 0, scene = 0;
    bool left = false;
    float pose[12]{}, fov = 0, nearPlane = 0, writtenFov = 0, writtenNear = 1;
};
static constexpr unsigned retiredSlots = 4;
static Retired retired[retiredSlots];
static std::atomic<unsigned> parks{0}, rearms{0}, reclaims{0}, parkBackoffs{0};
// Bad head frames: nav thread, cameraLock. Mirrored for status.
static fp::HeadHold headHold;
static std::atomic<unsigned> heldFrames{0}, holds{0}, maxHold{0}, holdParks{0}, holdingNow{0};
static void publishHold() {
    heldFrames = headHold.heldFrames;
    holds = headHold.holds;
    maxHold = headHold.maxHold;
    holdParks = headHold.parks;
    holdingNow = headHold.run;
}
// F7 hold-to-stop: tick of the first key-down / key-up seen by the
// window procedure; the worker decides (fp::stopKeyStep). F6 re-arms after a clean stop.
static std::atomic<ULONGLONG> stopPress{0}, stopRelease{0};
static std::atomic<unsigned> stopTaps{0}, stopRearms{0};
static std::atomic<ULONGLONG> rearmedAt{0}; // tick of the last F6 re-arm (fp::rearmF6Accepted)
static std::atomic<int> parkKind{0};        // fp::OwnerChange of the last park, 0 none.
static volatile LONG parkedNow = 0;
// Restoration uncertainty is sticky: pointer equality later is not lifetime proof.
static volatile LONG restorationUnknown = 0, cameraReady = 0;
static void releaseInput();
static bool gameplayOwnsPointer();
static float stockNear = 100;
static std::atomic<unsigned> samples{0};
static float lastHead[12]{}, lastCamera[12]{};
// Head origin in the camera's (global) frame, paired with lastCamera: the
// start of the native line of sight (0x246610 uses the head part).
static float lastHeadGlobal[3]{};
// Grab reach uses the head origin (not the eye setback). Picking may run apart from nav and cannot
// recursively take cameraLock, so publish a coherent copy with a separate short-lived lock.
static SRWLOCK reachHeadLock = SRWLOCK_INIT;
static float reachHeadGlobal[3]{};
static void publishReachHead(const float* head) {
    AcquireSRWLockExclusive(&reachHeadLock);
    memcpy(reachHeadGlobal, head, sizeof reachHeadGlobal);
    ReleaseSRWLockExclusive(&reachHeadLock);
}
static void readReachHead(float* head) {
    AcquireSRWLockShared(&reachHeadLock);
    memcpy(head, reachHeadGlobal, sizeof reachHeadGlobal);
    ReleaseSRWLockShared(&reachHeadLock);
}
static SRWLOCK aimLock = SRWLOCK_INIT;
struct Aim {
    uintptr_t root = 0, head = 0;
    fp::InputStamp attentionStamp;
    float point[3]{}, origin[3]{}, anchor[3]{}, travel[3]{}, yaw = 0, pitch = 0, targetYaw = 0,
                                                             targetPitch = 0;
    float rayYaw = 0;
    bool rayValid = false;
    fp::SwingGesture swing;
    unsigned swingGeneration = 0, swingPolicy = 0;
    int swingView = -1;
    bool swingCombat = false;
    unsigned revision = 0;
    float entryYaw = 0.f; // aimLock: one spawn seed shared with the input owner
    bool entryPending = false;
    bool valid = false, travelValid = false;
};
static Aim aim;
// Optional external viewing input; never written into Aim or native physics.
static fp::TrackingUdp trackingReceiver;
static std::atomic<bool> trackingEnabled{false};
static std::atomic<int> trackingRecenterKey{0};
static std::atomic<unsigned> trackingReset{0};
static std::atomic<ULONGLONG> trackingAcceptAfter{0};
static fp::TrackingView trackingView; // cameraLock
static bool trackingPresented = false; // cameraLock: includes loss/disable fade-out
static fp::TrackingSample readTrackingSample() { return trackingReceiver.snapshot(); }
static fp::TrackingSample (*trackingSnapshot)() = readTrackingSample;
static volatile LONG mouseCaptured = 0;
static volatile LONG cursorFree = 0;
// Native Controls > Cursor Look frees the cursor while held in close-view exploration.
// Keep the legacy publication name: world grabbing already distinguishes this
// deliberate pointer from Settings/window handoffs. The obsolete RMB option stays off.
static volatile LONG rmbFree = 0;
static std::atomic<int> rmbMode{0}; // obsolete RMB free-cursor mode is fixed off
// The inventory, an item's description, a container or a corpse's loot, or the arena's end-of-match menu
// (NEXT FIGHT) on screen frees the cursor and holds look and movement, in first and third person, as F8
// does ([View] FreezeForWindows, F8 Controls). Set once a frame on the game's thread (windowFreeStep).
static volatile LONG windowFree = 0;
static std::atomic<bool> windowsFreeze{true};
// Crouch: the input thread polls the native duck key, the render thread publishes
// a request, and poseBuilder admits it only for the same eligible character.
static std::atomic<int> crouchMode{2};                      // 0 off, 1 hold, 2 toggle
static std::atomic<bool> duckKey{false}, crouchHeld{false}; // held is a status mirror
static std::atomic<unsigned> crouchTicks{0};
static constexpr unsigned duckKeyBit = 0x10;
struct CrouchRequest {
    uintptr_t root = 0, head = 0, scene = 0;
    int mode = 0;
    bool held = false;
};
static SRWLOCK crouchLock = SRWLOCK_INIT;
static CrouchRequest crouchRequest;
static bool crouchAppliesTo(uintptr_t root);
static bool cursorFreeNow() {
    return InterlockedCompareExchange(&cursorFree, 0, 0) || InterlockedCompareExchange(&rmbFree, 0, 0) ||
           InterlockedCompareExchange(&windowFree, 0, 0);
}
static volatile LONG inputEnabled = 0;
static constexpr unsigned movementBits = 15u; // forward, left, back, right in the game's key word (0x49CDC0)
static unsigned nativeBinding(size_t index) {
    if (!base || index >= field::bindings::count) return 0;
    const size_t offset = index * field::bindings::stride;
    const uintptr_t entry = gameAt(fp::sig::movementKeys) + offset;
    // Native interaction reads a WORD; the other reviewed slots read DWORDs.
    return offset == field::bindings::interact ? *reinterpret_cast<const uint16_t*>(entry) : *reinterpret_cast<const uint32_t*>(entry);
}
static void movementKeys(unsigned* keys) {
    const unsigned fallback[4] = {'W', 'A', 'S', 'D'};
    for (unsigned i = 0; i < 4; ++i)
        keys[i] = base ? nativeBinding(i) : fallback[i];
}
static unsigned keyMask(WPARAM key) {
    unsigned keys[4];
    movementKeys(keys);
    return fp::movementKey(static_cast<unsigned>(key), keys);
}
// All actions polled by 0xAA080, including crouch/run/alternate attack.
// A mod-owned lean/rotate binding must not also trigger its native action.
static unsigned actionMask(WPARAM key) {
    if (!base) return keyMask(key);
    const unsigned bits[7] = {1, 2, 4, 8, 0x10, 0x20, 0x80};
    unsigned mask = 0;
    for (unsigned i = 0; i < 7; ++i) {
        const unsigned bound = nativeBinding(i);
        if (bound && key == bound) mask |= bits[i];
    }
    return mask;
}
// The movement keys physically held now (the tests replace the keyboard).
static SHORT(WINAPI* keyState)(int) = GetAsyncKeyState;
static unsigned physicalMovement() {
    unsigned keys[4];
    movementKeys(keys);
    unsigned held = 0;
    for (unsigned k : keys)
        if (k > 0 && k < 256 && (keyState(static_cast<int>(k)) & 0x8000)) held |= fp::movementKey(k, keys);
    return held;
}
using InputOwner = fp::InputOwnership<field::bindings::count>;
static InputOwner inputOwner;
static std::atomic<float> sensitivity{
    0.0025f}; // [Input] SensitivityPercent; the F8 settings page changes it live
// [View] SwingCrosshair=hide|show (the crosshair during an accepted swing).
static std::atomic<bool> hideSwingCrosshair{false};
// [View] IdleCrosshair=rmb|always: out of combat the crosshair shows only while right mouse frees the cursor.
static std::atomic<bool> idleCrosshairAlways{false};
// [Input] AimMarker=1 shows the crosshair at the aim while the head catches up; FlickCatchup 0..100 (fp::flickGain).
static std::atomic<bool> aimMarker{true};
static std::atomic<int> flickCatchup{50};
// which view F6 shows while the mod owns the camera: 0 first person, 1 third person (F6 cycles
// first -> third -> the game's overhead). [ThirdPerson] Distance / Shoulder / Height in first-person.ini.
static std::atomic<int> viewMode{0}, tpDistance{250}, tpShoulder{40}, tpHeight{15};
// [VR] rows: eye separation in mm (0: the headset's own), game units per metre, render mode.
static std::atomic<uintptr_t> nativeSettingsFaultAt{0};
static std::atomic<unsigned> nativeSettingsDispatches{0}, nativeSettingsSteps{0}, nativeSettingsRefused{0};
static std::atomic<bool> nativeSettingsWidened{false}; // the Settings form carries the five-tab width
// root x, y, width, height; Graphics, Controls, Controller widths; frame scale in hundredths; UI scale.
static std::array<std::atomic<int>, 9> nativeSettingsFrames{};
static std::atomic<int> vrIpdMm{0}, vrUnitsPerMetre{100}, vrRenderMode{0}, vrHandsDrawn{0}, vrPosture{0}, vrHeadset{0}, vrTurning{1}, vrEffects{0}, vrGripAngle{0}, vrHapticPercent{100}, vrPanelWidthCm{120}, vrPanelDistanceCm{150}, vrComfort{0}, vrBodyHandsOnly{0}, vrTitleMenu{1}, vrPointer{1}, vrRecenterMemory{1}, vrWristMenu{1}, vrHaftGrip{1}, vrHeightCm{0}, vrGrabTurn{0}, vrFrameTime{0}, vrEyeScalePercent{100}, vrHandMesh{1}, vrTrackedGrab{1}, vrGripSlideCm{0}, vrKick{0}, vrFingerBones{0}, vrJump{1}, vrWeaponWeight{1}, vrTeleport{1}, vrHandPanel{1}, vrPouch{1};
static std::atomic<int> drawDistanceM{60}, objectLightCap{4}; // the draw cutoff (m, 0 off); lights per mesh (6: the game's)
static std::atomic<int> fingerPoses{1}; // the held-item finger poses through the bones, every build
static std::atomic<int> fingerPosesOthers{1}; // the same for every character the world lists
// Always on, no row: the title in the headset, the pointer, the remembered recenter, the wrist menu, the haft grip, the
// game's hand mesh and the closing grab; the frame-time line stays off. Each keeps its switch for the emulation ini.
// The last recentred origin in the headset's room-fixed space: position, then quaternion x y z w. Written on
// recenter, read at start, kept in the settings ini beside the rows.
static float vrRecenterOrigin[7]{0, 0, 0, 0, 0, 0, 1};
static std::atomic<bool> vrRecenterOriginValid{false};
static bool saveSetting(const wchar_t* section, const wchar_t* key, const wchar_t* value);
// Hands only without hands would be nothing: that mode draws the mod hands as if the hands row were on.
inline bool trackedHandsWanted() { return vrHandsDrawn.load(std::memory_order_relaxed) || vrBodyHandsOnly.load(std::memory_order_relaxed); }
// Requested modes persist independently. The complete optional hook group must
// be admitted before either can become effective.
static std::atomic<int> fpManualGuard{0}, tpManualGuard{0};
static std::atomic<bool> manualDefenseReady{false};
struct GuardRequest {
    fp::InputStamp stamp;
    bool held = false;
};
// Native input publishes; native actor callbacks read. No native call under this lock.
static SRWLOCK guardLock = SRWLOCK_INIT;
static GuardRequest guardRequest;
static std::atomic<unsigned> guardDenied{0}, guardWithdrawals{0};
static bool ordinaryCombat(uintptr_t root, uintptr_t bh);
static bool manualCombat(uintptr_t root, uintptr_t bh);
static void combatInput(uintptr_t root, uintptr_t bh, bool owned, const InputOwner::Sample& sample);
static std::atomic<bool> tpTight{true};
// Combat framing (closer and centred while in combat mode), the third-person FOV (0 = as first person) and
// the third -> overhead blend (viewMode 2 = blending out, ends in the game's view).
static std::atomic<bool> tpCombat{true};
static std::atomic<int> tpFov{0};
// camera lag 0..100, hiding what stands between the
// camera and the player instead of pulling in (walls still pull in), and the knockdown follow.
static std::atomic<int> tpLag{40};
static std::atomic<bool> tpDown{true};
static std::atomic<unsigned> tpHidden{0};
static std::atomic<ULONGLONG> tpBlendStart{0};
static float tpCombatK = 0.f; // swap shoulder / closer framing in tight spaces
// A TP hit belongs to the exact input geometry that the camera sampled. The
// input tick revision advances even without mouse motion, so it cannot serve
// as this cache key. sequence advances only when geometry or ownership changes.
// All request/sample members and the cached point are protected by aimLock.
struct TpAimRequest {
    uintptr_t root = 0, head = 0, sector = 0, worldScene = 0;
    uint32_t generation = 0;
    unsigned policy = 0;
    uint64_t sequence = 0;
    float yaw = 0, pitch = 0, x = 0, y = 0, fov = 0;
    int width = 0, height = 0;
    bool free = false, valid = false;
};
static bool sameTpAimGeometry(const TpAimRequest& a, const TpAimRequest& b) {
    return a.valid && b.valid && a.root == b.root && a.head == b.head && a.sector == b.sector &&
           a.worldScene == b.worldScene && a.generation == b.generation && a.policy == b.policy &&
           a.yaw == b.yaw && a.pitch == b.pitch && a.x == b.x && a.y == b.y && a.fov == b.fov &&
           a.width == b.width && a.height == b.height && a.free == b.free;
}
static bool sameTpAimRequest(const TpAimRequest& a, const TpAimRequest& b) {
    return a.sequence == b.sequence && sameTpAimGeometry(a, b);
}
static TpAimRequest tpAimRequest, tpAimSample;
static float tpAimLocal[3]{};
static bool tpAimSet = false;
static fp::ThirdPerson thirdPerson;          // cameraLock
static std::atomic<unsigned> tpPropPulls{0}; // frames a prop pulled the third-person camera in
// [ThirdPerson] Blockers (fp::blockersHide / Push / Smart): what the camera does about props and people between it
// and the player. [ThirdPerson] CombatAim=free: in combat the crosshair moves inside a box, the view turns at its edge.
static std::atomic<int> tpBlockers{fp::blockersSmart};
static std::atomic<bool> tpCombatAimFree{true};
// Third-person diagnostics for the status file: the eye's height above the character's reference position, and
// what the camera decided last frame (fp::ThirdPerson).
// [Camera] EyeSetback (fp::eyePoint): cm the viewpoint and the aim's origin sit behind the eyes. 0: the eye mount.
static std::atomic<int> eyeSetback{0};
// The physics head's points in its own frame (status: where its middle is and how far its points reach), cm.
static std::atomic<float> headMid2{0}, headMid3{0}, headReach{0};
// Coalesced status demand, consumed only after the native callback's current
// head/scene/pose checks. No game pointer or readability result crosses samples.
static std::atomic<bool> headDiagnosticRequested{true};
static std::atomic<float> tpDrop{0}, tpCeilingRoom{-1}, tpLostHeight{0}, tpPivotPulled{0}, tpAllowed{0};
static std::atomic<bool> tpDownNow{false};
// The free combat crosshair: its offset from the window centre (px; aim thread writes, render thread reads) and
// whether it is in use.
static fp::AimBox tpAimBox; // aim thread
static std::atomic<float> tpAimX{0}, tpAimY{0};
static std::atomic<int> fpDeadzoneX{25}, fpDeadzoneY{25}, tpDeadzoneX{25}, tpDeadzoneY{25};
static std::atomic<int> aimEdgeSoftness{fp::defaultAimEdgeSoftness};
static std::atomic<bool> aimAutoRecenter{true};
// Message thread queues a one-shot click; the game input tick consumes it.
static std::atomic<unsigned> recenterClicks{0};
static std::atomic<int> fpSwingSelection{1}, tpSwingSelection{1}; // 1 mouse direction, 0 native facing
static void releaseInteractionInput();
static std::atomic<bool> tpAimFree{false};
static std::atomic<int> headSteadyRoll{100}, headSteadyPitch{100},
    headSteadyYaw{
        100};
static std::atomic<int> headSteady{
    100};
static std::atomic<bool> swinging{false}; // an ordinary attack the game accepted is under way (publishCue)
static std::atomic<unsigned> swingHides{0};
static std::atomic<unsigned> headRejects
    [8]{}; // why head frames were refused: no head, not the root's, mesh, non-finite, scale, scene, target, pose
static std::atomic<bool> rawWanted{
    false};                      // [Input] RawMouse; the worker registers / removes raw input to match
static HWND rawTarget = nullptr; // the window raw input is registered for (worker thread)
static std::atomic<bool> invertY{false};
static std::atomic<int> aimIndicator{1};
// [Input] RawMouse=1 opts into relative WM_INPUT counts (no pointer acceleration).
// Default off: the cursor-delta path is the accepted feel,
// and raw counts changed effective turn rate enough to upset head stabilization.
static bool rawMouse = false;
// [Input] AttackSideKey=<virtual-key code>. Optional held binding that switches
// the ordinary attack side for presses made while it is held. 0 = unassigned.
static int attackSideKey = 0;
static std::atomic<unsigned> sideFlips{0};
// [View] AttackCue=1: the wind-up arc around the combat crosshair. CueSize/CueOpacity are percentages;
// CueThrust=1 draws thrusts below the crosshair.
static std::atomic<bool> attackCue{true}, cueThrust{true};
static std::atomic<bool> bladeDrag{true}, stabSink{true}; // blade.inl: the drawn edge cuts; a thrust sinks into a soft target
static std::atomic<int> bladeGainPercent{100};           // the gain on the draw velocity handed to the game's impact
static std::atomic<int> cueSize{100}, cueOpacity{100};
// Published on the behaviour tick (publishCue), read by the frame thread only through these.
static std::atomic<int> cueArcSide{0}, cueArcPhase{0};
static std::atomic<float> cueArcFill{0};
static std::atomic<unsigned long long> cueArcFade{0}, cueArcPublished{0};
static std::atomic<int> cueState{0};  // fp::OriginCue, published by behavior.
static std::atomic<int> cueOrigin{0}; // fp::CueSide before roll, from the swinging weapon's hand.
// Last hand resolution (diagnostic): slot, hand, expected hand, hand side, grip class.
static std::atomic<int> handSlot{-1}, handIndex{-1}, handExpected{-1}, handSideSeen{0}, handGrip{-1},
    handCount{-1};
// Hand mesh names are published together after native reads and sanitization finish.
struct HandNamesStatus {
    char names[2][16]{};
};
static SRWLOCK handNamesLock = SRWLOCK_INIT;
static HandNamesStatus handNamesStatus;
static void publishHandNames(const HandNamesStatus& names) {
    AcquireSRWLockExclusive(&handNamesLock);
    handNamesStatus = names;
    ReleaseSRWLockExclusive(&handNamesLock);
}
static HandNamesStatus readHandNamesStatus() {
    AcquireSRWLockShared(&handNamesLock);
    const auto names = handNamesStatus;
    ReleaseSRWLockShared(&handNamesLock);
    return names;
}
static std::atomic<unsigned> cueDraws{0}, uiReticleReads{0};
// Diagnostic: does the pick ray's camera (viewport+0x1C0, read by 0x8F460 for
// near/pose) equal the camera first person writes? -1 unknown, 0 no, 1 yes.
static std::atomic<int> pickCamera{-1};
static std::atomic<int> cueGl{0}; // 0 untried, 1 drawing, -1 unavailable on this context.
// Last on-screen reticle the native gameplay cursor was drawn at (client px).
// Leaf lock: never held while calling anything else.
static SRWLOCK reticleLock = SRWLOCK_INIT;
static LONG reticleX = 0, reticleY = 0, reticleFrame = -1;
static ULONGLONG reticleTick = 0;
// Pick clearance for that reticle: 1 clear, 0 level geometry between the head
// and the pick origin, -1 not determined (no level collision, no camera).
static int reticleClear = -1;
static std::atomic<unsigned> pickBlocked{0}, pickChecks{0}, clearanceQueries{0};
static std::atomic<int> lastClearance{-1};
// Native level ray queries (RVA 0x1430F0, 0x1431C0), as 0x2462F0 calls them:
// (level grid, global origin, unit direction, length) -> hit. Read-only.
using LevelRay = bool (*)(uintptr_t, const float*, const float*, float);
static LevelRay levelRay, levelRay2;
static volatile LONG rawRegistered = 0;
static fp::RawLook rawLook; // Guarded by rawLock.
static SRWLOCK rawLock = SRWLOCK_INIT;
// One managed subclass installation. Callback-local copies survive retirement/replacement.
// Never hold this lock across a window API or a native procedure call.
struct WindowInstallation {
    HWND window = nullptr;
    WNDPROC previous = nullptr;
    uint64_t generation = 0;
    bool retired = false;
};
static SRWLOCK windowInstallationLock = SRWLOCK_INIT;
static WindowInstallation windowInstallation;
static HWND hookedWindow;
static float stockPose[12]{}, stockFov = 30;
static std::atomic<int> selectedFov{75};
// UI actions and FOV keys publish under one settings lock. Disk I/O never owns it. This lock
// is never nested with camera/input locks or held while waiting for raw input.
static SRWLOCK settingsLock = SRWLOCK_INIT;
struct SettingsGuard {
    SettingsGuard() { AcquireSRWLockExclusive(&settingsLock); }
    ~SettingsGuard() { ReleaseSRWLockExclusive(&settingsLock); }
    SettingsGuard(const SettingsGuard&) = delete;
    SettingsGuard& operator=(const SettingsGuard&) = delete;
};
static void queueFovSaveLocked(); // mod/settings.inl; caller owns settingsLock
static void adjustFov(int delta, bool reset = false) {
    SettingsGuard guard;
    const int value = reset ? 75 : fp::clampFov(selectedFov.load() + delta);
    if (value != selectedFov.load()) {
        selectedFov = value;
        queueFovSaveLocked();
    }
}
static std::atomic<int> nearClip{
    fp::nearClipDefaultTenths};  // [Camera] NearClip: the dynamic near plane's floor, tenths of a cm
static float renderedFov = 75;   // Protected by cameraLock, paired with lastCamera.
static float renderedNear = 1.f; // the near plane first person wrote (cameraLock)
// the view's heading as last drawn (render thread), so an aim that (re)starts begins where
// the view looks instead of snapping to the body's heading and level pitch.
static std::atomic<uintptr_t> viewRoot{0};
static std::atomic<float> viewYaw{0}, viewPitch{0};
static wchar_t settingsPath[MAX_PATH]{};
// Settings live in one per-user file that outlives release folders (userSettingsDir: Documents\ExCamera);
// the release's own first-person.ini holds the defaults and is never written. When the user file does not
// exist yet it starts as a copy of the most recently edited release ini (the releases before the per-user
// file sit in sibling ExanimaFirstPerson-* folders; edited = written after its folder's DLL, so a fresh
// build's defaults never win over the player's), else of this release's. moduleIni: the ini
// beside this DLL; userDir: empty keeps the settings beside the DLL.
static std::wstring userSettingsFile(const std::wstring& moduleIni, const std::wstring& userDir) {
    if (userDir.empty()) return moduleIni;
    const std::wstring user = userDir + L"\\first-person.ini";
    if (GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return user;
    SHCreateDirectoryExW(nullptr, userDir.c_str(), nullptr);
    std::wstring source = moduleIni;
    const size_t folderEnd = moduleIni.find_last_of(L'\\'),
                 releasesEnd = folderEnd == std::wstring::npos ? folderEnd
                                                               : moduleIni.find_last_of(L'\\', folderEnd - 1);
    if (releasesEnd != std::wstring::npos) {
        const std::wstring releases = moduleIni.substr(0, releasesEnd);
        ULONGLONG newest = 0;
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileW((releases + L"\\ExanimaFirstPerson-*").c_str(), &f);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;
                const std::wstring folder = releases + L"\\" + f.cFileName;
                WIN32_FILE_ATTRIBUTE_DATA ini{}, dll{};
                if (!GetFileAttributesExW((folder + L"\\first-person.ini").c_str(), GetFileExInfoStandard,
                                          &ini) ||
                    !GetFileAttributesExW((folder + L"\\ExanimaFirstPerson.dll").c_str(),
                                          GetFileExInfoStandard, &dll))
                    continue;
                const ULONGLONG edited =
                    (static_cast<ULONGLONG>(ini.ftLastWriteTime.dwHighDateTime) << 32) | ini.ftLastWriteTime.dwLowDateTime;
                const ULONGLONG built =
                    (static_cast<ULONGLONG>(dll.ftLastWriteTime.dwHighDateTime) << 32) | dll.ftLastWriteTime.dwLowDateTime;
                constexpr ULONGLONG packingSlack =
                    10ull * 10000000ull; // 10 s in file-time units: the build writes both
                if (edited > built + packingSlack && edited > newest) {
                    newest = edited;
                    source = folder + L"\\first-person.ini";
                }
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
    }
    if (CopyFileW(source.c_str(), user.c_str(), TRUE) ||
        GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES)
        return user;
    // An unpackaged build has no default ini. Start an empty per-user file so
    // compiled defaults and future edits still outlive that build folder.
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD missing = GetLastError();
        if (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND) {
            HANDLE file = CreateFileW(user.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                return user;
            }
        }
    }
    return moduleIni;
}
// The per-user settings folder: Documents\ExCamera. Releases before the ExCamera name kept settings in
// Documents\ExaniMate; the file is copied over once (copied, not moved: the ExaniMate editor still uses that
// folder).
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
static std::wstring userDocumentsDir() {
    PWSTR folder = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &folder);
    const std::wstring path = SUCCEEDED(result) && folder ? folder : L"";
    if (folder) CoTaskMemFree(folder);
    return path;
}
static std::wstring userSettingsDir(const std::wstring& profile, const std::wstring& documents = {}) {
    const std::wstring parent = documents.empty() ? profile + L"\\Documents" : documents;
    const std::wstring dir = parent + L"\\ExCamera", file = dir + L"\\first-person.ini",
                       legacy = profile + L"\\Documents\\ExCamera\\first-person.ini";
    std::wstring old = parent + L"\\ExaniMate\\first-person.ini";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES)
        old = profile + L"\\Documents\\ExaniMate\\first-person.ini";
    // Previous builds used USERPROFILE\Documents even when Windows had moved
    // Documents. Preserve that existing ExCamera file before the older brand.
    if (file != legacy && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        CopyFileW(legacy.c_str(), file.c_str(), TRUE);
    }
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        CopyFileW(old.c_str(), file.c_str(), TRUE);
    }
    return dir;
}
static std::atomic<int> persistedFov{75}; // status reads without taking the settings lock
static bool settingsSavePending(); // mod/settings.inl; coherent status for every saved row
static void persistSettingsRows(unsigned budget); // mod/settings.inl; session worker only
static void flushSettings(); // same owner, one full pass after native Apply retires
static void saveFov() { persistSettingsRows(4); }
static unsigned inputClock = ~0u;
static wchar_t statusPath[MAX_PATH]{}, statusTempPath[MAX_PATH]{};
static FILE* openStatus() {
    FILE* f = nullptr;
    if (statusTempPath[0]) _wfopen_s(&f, statusTempPath, L"w");
    return f;
}
static void publishStatus(FILE* f) {
    if (!f) return;
    const bool ok = ferror(f) == 0;
    const bool closed = fclose(f) == 0;
    if (ok && closed)
        MoveFileExW(statusTempPath, statusPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
// Fault notice: the window title is invisible in fullscreen. A topmost,
// click-through, non-activating banner over the game window states the fault.
// Owned and pumped by the worker thread only; it never takes focus and hides
// while another application is in front. Code stays mapped until process exit.
static HWND noticeWindow;
static const wchar_t* noticeText = L"";
static ULONGLONG noticeShownMs = 0, noticeLastTick = 0, noticeStarted = 0;
static bool noticeVisible = false;
static constexpr ULONGLONG noticeLifetimeMs = 120000;
static ULONGLONG noticeVisibleMs = 10000; // Per notice: faults 10 s, F7 hints shorter.
static std::atomic<unsigned> notices{0};
static LRESULT CALLBACK noticeProc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HBRUSH fill = CreateSolidBrush(RGB(120, 18, 18));
        FillRect(dc, &r, fill);
        DeleteObject(fill);
        HBRUSH edge = CreateSolidBrush(RGB(235, 200, 120));
        FrameRect(dc, &r, edge);
        DeleteObject(edge);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 246, 230));
        RECT text = r;
        InflateRect(&text, -r.bottom / 4, -r.bottom / 10);
        // One line when it fits, otherwise wrapped in a smaller font, centred either way.
        for (int height = r.bottom * 9 / 20;; height = height * 3 / 4) {
            HFONT font = CreateFontW(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                     L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, font);
            RECT need = text;
            DrawTextW(dc, noticeText, -1, &need, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            const bool fits = need.bottom - need.top <= text.bottom - text.top || height <= r.bottom / 5;
            if (fits) {
                RECT at = text;
                at.top += (text.bottom - text.top - (need.bottom - need.top)) / 2;
                DrawTextW(dc, noticeText, -1, &at, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
            SelectObject(dc, old);
            DeleteObject(font);
            if (fits) break;
        }
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, message, wp, lp);
}
static BOOL CALLBACK findWindow(HWND w, LPARAM out);
static void showNotice(const wchar_t* text, ULONGLONG visibleMs = 10000) {
    if (noticeWindow) {
        DestroyWindow(noticeWindow);
        noticeWindow = nullptr;
        noticeVisible = false;
    }
    // An early activation fault can precede the first window enumeration.
    if (!hookedWindow || !IsWindow(hookedWindow)) {
        hookedWindow = nullptr;
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&hookedWindow));
    }
    if (!hookedWindow) return;
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = noticeProc;
        wc.hInstance = module;
        wc.lpszClassName = L"ExCameraNotice";
        wc.hCursor = nullptr;
        cls = RegisterClassW(&wc);
        if (!cls) return;
    }
    RECT g;
    if (!GetWindowRect(hookedWindow, &g)) return;
    const int gw = g.right - g.left, gh = g.bottom - g.top;
    if (gw < 200 || gh < 100) return;
    const int width = gw * 4 / 5 < 1200 ? gw * 4 / 5 : 1200, height = gh / 14 < 40 ? 40 : gh / 14;
    noticeText = text;
    noticeVisibleMs = visibleMs;
    noticeWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                       WS_EX_TOOLWINDOW,
                                   L"ExCameraNotice", text, WS_POPUP, g.left + (gw - width) / 2,
                                   g.top + gh / 9, width, height, nullptr, nullptr, module, nullptr);
    if (!noticeWindow) return;
    SetLayeredWindowAttributes(noticeWindow, 0, 235, LWA_ALPHA);
    noticeVisible = false;
    noticeShownMs = 0;
    noticeStarted = noticeLastTick = GetTickCount64();
    ++notices;
}
// Returns false once the notice is gone. Visible time counts only while the
// game is frontmost, so a fault during alt-tab is still seen on return.
static bool pumpNotice() {
    if (!noticeWindow) return false;
    const ULONGLONG now = GetTickCount64();
    if (noticeVisible) noticeShownMs += now - noticeLastTick;
    noticeLastTick = now;
    const bool front = hookedWindow && GetForegroundWindow() == hookedWindow && !IsIconic(hookedWindow);
    if (noticeShownMs >= noticeVisibleMs || now - noticeStarted >= noticeLifetimeMs ||
        !IsWindow(hookedWindow)) {
        DestroyWindow(noticeWindow);
        noticeWindow = nullptr;
        noticeVisible = false;
    } else if (front != noticeVisible) {
        ShowWindow(noticeWindow, front ? SW_SHOWNOACTIVATE : SW_HIDE);
        noticeVisible = front;
        if (front)
            SetWindowPos(noticeWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return noticeWindow != nullptr;
}
// The fault's specifics (which target, which hook) for first-person-status.json; empty when there are none.
static char faultDetail[512]{};
#include "activation.inl"
// notice: shown instead of the generic fault text; must outlive the notice (a literal or a static buffer).
static void reportFault(const char* reason, const char* hooks = "unknown",
                        const char* restoration = "unknown", const wchar_t* notice = nullptr) {
    viewMode = 0;
    InterlockedExchange(&cameraReady, 0);
    InterlockedExchange(&stopping, 1);
    InterlockedExchange(&active, 0);
    releaseInput();
    setActivationPhase(fp::ActivationPhase::failed);
    const bool verified = strcmp(restoration, "restored") == 0;
    const bool removed = strcmp(hooks, "removed") == 0 || strcmp(hooks, "not_installed") == 0;
    if (FILE* f = openStatus()) {
        fp::JsonOut j(f);
        writeActivationStatus(j);
        if (removed)
            j.boolean("installed", false);
        else if (strcmp(hooks, "retained") == 0)
            j.boolean("installed", true);
        else
            j.null("installed");
        j.boolean("active", false)
            .str("state", reason)
            .str("hooks", hooks)
            .str("restoration", restoration)
            .boolean("restored", verified)
            .boolean("restart_required", true)
            .str("detail", faultDetail);
        j.finish();
        publishStatus(f);
    }
    // Immutable text remains valid even if the receiving window times out.
    const wchar_t* message = fp::faultMessage(verified, removed, strcmp(restoration, "not_required") == 0);
    if (hookedWindow) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(hookedWindow, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(message), SMTO_ABORTIFHUNG | SMTO_BLOCK, 200,
                            &ignored);
    }
    showNotice(notice ? notice : message + fp::faultTitlePrefix);
}
static std::atomic<unsigned> controlCalls{0}, pointCalls{0}, relaxedSteps{0};
static volatile LONG navThread = 0, swapThread = 0;
static HANDLE instanceMutex;
static SRWLOCK cameraLock = SRWLOCK_INIT;
// Worker-only snapshot: release the owner lock before formatting or writing status.
static float readRenderedNearStatus() {
    AcquireSRWLockShared(&cameraLock);
    const float value = renderedNear;
    ReleaseSRWLockShared(&cameraLock);
    return value;
}
static fp::NearPlane nearPlane; // dynamic near plane (cameraLock)
static constexpr float cameraNearMaximum = 3.f;
static uintptr_t filterRoot = 0, filterHead = 0;
static LARGE_INTEGER filterTime{}, frequency{};
struct CameraFilterState {
    fp::ThirdPerson third;
    fp::NearPlane clipFilter;
    float combat = 0.f;
};
// Native rendering can request the same presentation's camera at several time
// offsets, in either order. All requests start from this immutable prior-centre
// state; only a centre-time result becomes the next presentation's history.
// cameraLock owns this record and the persistent filters above.
struct CameraFrameHistory {
    bool valid = false, centreReady = false;
    LONG serial = 0;
    uintptr_t view = 0, camera = 0, controller = 0, root = 0, head = 0, scene = 0;
    int mode = 0;
    LARGE_INTEGER time{};
    ULONGLONG tick = 0;
    float seconds = 1.f;
    float effectiveFov = 75.f; // One projection for clearance and every sample in this epoch.
    CameraFilterState before;
    float trackingOffset[12]{};
    bool tracked = false;
    bool eyeLookValid = false;
    float eyeYaw = 0.f, eyePitch = 0.f;
};
static CameraFrameHistory cameraFrame;
// cameraLock: an arena spawn may reuse every native pointer. Keep a world-space
// body anchor as well as identities; temporal head samples are not spawn motion.
struct CameraEntryState {
    uintptr_t root=0, head=0, scene=0;
    float body[3]{}, yaw=0.f;
    bool valid=false, menuSeen=false, requested=false, pending=false;
};
static CameraEntryState cameraEntry;
static std::atomic<unsigned> walkTargets{0}, combatSteps{0}, unexpectedRelaxedSteps{0};
using TravelTarget = void (*)(float*, const float*, const float*, float);
static TravelTarget originalTravelTarget;
using WalkGait = void (*)(uintptr_t, const float*);
static WalkGait originalWalkGait;
static thread_local uintptr_t exploringRoot = 0;
static thread_local float exploringDirection[3]{};
struct CameraGuard {
    CameraGuard() { fp::profile::invoke<fp::profile::Hook::cameraLock>(AcquireSRWLockExclusive, &cameraLock); }
    ~CameraGuard() { ReleaseSRWLockExclusive(&cameraLock); }
};
// While look owns the mouse, the OS cursor is
// confined to the game's client area, so a fast flick in windowed mode or on two
// monitors cannot click another window. Released with input.
static volatile LONG cursorClipped = 0;
static void releaseInput() {
    releaseInteractionInput();
    if (InterlockedExchange(&cursorClipped, 0)) ClipCursor(nullptr);
    if (InterlockedExchange(&inputEnabled, 0)) {
        inputOwner.releaseGameplay();
        trackingAcceptAfter = GetTickCount64();
        ++trackingReset;
    } else
        inputOwner.clearTransient();
    AcquireSRWLockExclusive(&rawLock);
    rawLook.reset();
    ReleaseSRWLockExclusive(&rawLock);
    InterlockedExchange(&mouseCaptured, 0);
    recenterClicks.store(0);
}
static volatile LONG swapCalls = 0, nativeCursorUpdates = 0;
using Behavior = void (*)(uintptr_t);
using Point = void (*)(uintptr_t, float*);
using Swap = BOOL(WINAPI*)(HDC);
using Pose = void (*)(uintptr_t, float, unsigned char);
static Pose originalPose;
static Behavior originalStep;
static Behavior originalDefenseReactive, originalDefenseAttackUpdate, defenseCleanup;
static uint64_t (*originalDefenseMotionSelect)(uintptr_t, uintptr_t);
static void (*originalInput)();
static Behavior originalBehavior, originalCursor, originalExplore;
static Point originalPoint;
static Swap originalSwap;
using Projection = void (*)(uintptr_t);
// Legacy name: native 1CEB0 expands the 12-float affine pose into a 4x4 matrix; it does not invert it.
using Inverse = void (*)(uintptr_t, uintptr_t);
static Projection rebuildProjection;
static Inverse rebuildInverse;
static bool awaitRestore(DWORD timeout = 2000) {
    // Closing the publication gate lasts until a clean stop is re-armed (F6 after
    // F7, runSession). The same lock guards nav/swap publication and this
    // quiescence observation.
    InterlockedExchange(&stopping, 1);
    InterlockedExchange(&active, 0);
    releaseInput();
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (TryAcquireSRWLockExclusive(&cameraLock)) {
            // Stopping while a released camera of the current gameplay is unresolved
            // (never current again, so never reclaimed) cannot be called restored.
            for (const Retired& r : retired)
                if (r.camera && !r.left && *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) == r.root &&
                    *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) == r.scene)
                    InterlockedExchange(&restorationUnknown, 1);
            const bool unknown = InterlockedCompareExchange(&restorationUnknown, 0, 0) != 0;
            const bool ready = !unknown && !savedCamera && !InterlockedCompareExchange(&pendingRestore, 0, 0) &&
                               !InterlockedCompareExchange(&renderScopesInFlight, 0, 0) && stereoDrained();
            if (ready) InterlockedExchange(&cameraReady, 0);
            ReleaseSRWLockExclusive(&cameraLock);
            if (unknown) return false;
            if (ready) return true;
        }
        if (GetTickCount64() - started >= timeout) return false;
        Sleep(1); // Never hold a lock needed by the restoration callback while waiting.
    }
}
using CursorPosition = void (*)(LONG*, LONG*);
static CursorPosition originalCursorPosition;
// drags. The game's drag manager (0x4EFDD0) has flag 0x2000 at +8
// while something is dragged, the dragged object at +0x10; its mesh node at object +0x160.
// Doors and hinged things (0x4000/0x10000) are held in place; a locked door's rattle (0x80000) is the game's.
static std::atomic<float> doorDistance{
    0}; // a held door or lid: its point's distance along the pointer ray; 0 = none
static std::atomic<float> doorCameraOffset{
    0}; // how much farther the camera is than the eye along that ray (third person)
static uintptr_t heldDoorDrag();
static uintptr_t lastDoorObject = 0;
static float doorGrabPoint[3]{};
static bool doorGrabPointValid = false;
static void releaseHeldDoor();
static std::atomic<unsigned> doorHolds{0}, holdNotches{0}, holdPlacements{0}, warpsSkipped{0},
    grabsRefused{0};
static std::atomic<int> grabRange{300}; // [Input] GrabRange, cm from the eye (fp::clampGrabRange)
static std::atomic<float> holdTarget{0}, holdApplied{0};
// Holding an object (held_object.h). The eye the hold distance is measured from (global; the camera thread
// publishes it with the view), the grab's distance from it, the rotate key ([Input] RotateHeldKey, default
// right mouse), whether it is turning the object now, and the mouse movement it has not applied yet (the aim
// thread adds, the placing thread takes).
static std::atomic<float> eyeGlobal[3]{};
static std::atomic<float> grabStartDistance{0};
static std::atomic<bool> hoverOutOfRange{false};
static std::atomic<ULONGLONG> hoverRangeAt{0};
static std::atomic<int> rotateHeldKey{0x02};
static std::atomic<int> kickKey{0}, jumpKey{0}; // motions.inl; unbound until the player picks a key
static std::atomic<bool> motionKickRequested{false}, motionJumpRequested{false}; // a key press claimed by the window handler, taken on the game thread
// Preserve legacy INI preferences only. Explicit leaning is disabled; these
// values never claim input or change torso pose.
static std::atomic<int> leanKey{0x12};
// Heading correction modifies the native head's achieved pose; no synthetic camera turn lead.
static std::atomic<bool> holdHeadingOn{true};
static std::atomic<float> headingTurn{0};
static std::atomic<bool> leanKeysMode{false};
static std::atomic<int> leanLeftKey{0}, leanRightKey{0};
// Overhead controls kept from the game while the mod owns the camera: the wheel's zoom (status).
static std::atomic<unsigned> overheadInputsHeld{0};
static std::atomic<bool> heldRotating{false};
static std::atomic<float> heldTurnX{0}, heldTurnY{0};
static std::atomic<unsigned> heldTurns{0};
static void clearHeldThrow(bool cancelQueued = true);
static void releaseInteractionInput() {
    clearHeldThrow();
    hoverOutOfRange = false;
    hoverRangeAt = 0;
    heldRotating = false;
    heldTurnX = heldTurnY = 0.f;
}
// Pose cadence is a legacy diagnostic only, never release physics timing.
static std::atomic<unsigned> playerSteps{0}, flings{0};
static std::atomic<float> physicsStep{0};
static std::atomic<uintptr_t> holdObject{0};
// A scene-node address can be grabbed repeatedly. Placement caches belong to a grab, not an address;
// the grab-start hook invalidates them even if no idle placement ran between release and re-grab.
// Metadata-only epoch: view/focus/session boundaries cannot revive an inventory preview verdict.
static std::atomic<ULONGLONG> inventoryOwnerEpoch{0};
static std::atomic<ULONGLONG> holdGeneration{0};
static void resetHeldGrab() {
    // Cancel under the final throw-commit lock before changing the grab generation.
    clearHeldThrow();
    holdObject = 0;
    grabStartDistance = 0.f;
    heldTurnX = heldTurnY = 0.f;
    ++holdGeneration;
}
static ULONGLONG holdTick = 0;
static std::atomic<unsigned> dragFlagsStatus{0}; // diagnostic mirror; formatted only by the worker
static bool readable(uintptr_t address, size_t size);
static const char* nodeName(uintptr_t node);
static bool verifiedBuild = false; // the build this release was tested on, by its hash (session.inl sets it)
static int dragKind(uintptr_t* object = nullptr);
static int dragKind(uintptr_t* object) {
    // 0x11F610 (the player's input step) reads the manager's flags: 0x2000 an object drag (queried by
    // 0x117AA0 and placed by 0x118000), 0x4000 and 0x10000 a hinged drag released beyond reach (the object's
    // position at +0x128), 0x80000 a hand grab (cursor state 8). Live: a closed door
    // dragged by its face reads 0x4000, a hand grab beside an open door 0x80000.
    const uintptr_t drag = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::dragManager));
    if (!readable(drag, 0x20)) return 0;
    const unsigned flags = *reinterpret_cast<unsigned*>(drag + 8);
    const uintptr_t o = *reinterpret_cast<uintptr_t*>(drag + field::drag::object);
    if (object) *object = o;
    dragFlagsStatus.store(flags, std::memory_order_relaxed);
    if (flags & 0x2000) return readable(o, 0x168) ? 1 : 0;
    if (flags & (0x4000 | 0x10000 | 0x20000 | 0x80000)) return 2;
    return 0;
}
static void normalize(float* v);
static bool playerContext(uintptr_t behavior);
// Preserve the old free-pointer exploration safety: close-view gameplay only,
// no Settings/native window, combat, special native RMB state or unsafe drag.
// The binding comes from reviewed native slot 8 (Controls > Cursor Look).
static bool rmbEligible() {
    if (viewMode.load() >= 2 || InterlockedCompareExchange(&cursorFree, 0, 0) ||
        InterlockedCompareExchange(&windowFree, 0, 0)) return false;
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot));
    if (!readable(root, 0x1300)) return false;
    const uintptr_t behavior = *reinterpret_cast<uintptr_t*>(root + field::character::behaviour);
    if (!readable(behavior, 0x8a0) || !playerContext(behavior)) return false;
    if (*reinterpret_cast<unsigned char*>(root + field::character::combatStance) ||
        *reinterpret_cast<unsigned*>(root + field::character::stateFlags) == 0x20)
        return false;
    uintptr_t object = 0;
    int kind = 0;
    __try {
        kind = dragKind(&object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (kind == 1)
        return rotateHeldKey.load() != static_cast<int>(nativeBinding(8)); // held-item rotation has priority
    if (kind == 2)
        return doorDistance.load() > 0 &&
               heldDoorDrag(); // a door or lid held in place, never a locked door's rattle
    return kind == 0;
}
// Serialize the window edge and native poll retirement; no camera/aim lock is held.
static SRWLOCK cursorHoldLock = SRWLOCK_INIT;
static fp::RightMouseFree rmbState;
static int cursorHoldKey = 0;
static fp::InputStamp cursorHoldOwner;
static fp::InputStamp cursorHoldStamp() {
    fp::InputStamp s;
    s.root = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot));
    s.head = s.root ? *reinterpret_cast<uintptr_t*>(s.root + field::character::headPart) : 0;
    s.scene = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene));
    s.mode = viewMode.load();
    s.generation = inputOwner.generation();
    s.policyRevision = inputOwner.policyRevision();
    return s;
}
// Something grabbed with the cursor freed keeps its place on screen when Cursor Look is let go: the
// pointer stays as far from the crosshair as the cursor was, for as long as that same thing is held, and
// freeing the cursor again puts it back there.
static HWND gameWindow();
static std::atomic<uintptr_t> heldOffsetObject{0};
static std::atomic<LONG> heldOffsetX{0}, heldOffsetY{0};
static uintptr_t heldThing() {
    uintptr_t o = 0;
    __try {
        if (!dragKind(&o)) o = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o = 0;
    }
    return o;
}
static void keepHeldOffset(bool nowFree) {
    const uintptr_t held = heldThing();
    HWND w = gameWindow();
    if (!held || !w) {
        heldOffsetObject = 0;
        return;
    }
    AcquireSRWLockShared(&reticleLock);
    const LONG rx = reticleX, ry = reticleY;
    ReleaseSRWLockShared(&reticleLock);
    if (!nowFree) {
        POINT c{};
        if (GetCursorPos(&c) && ScreenToClient(w, &c)) {
            heldOffsetX = c.x - rx;
            heldOffsetY = c.y - ry;
            heldOffsetObject = held;
        }
        return;
    }
    if (heldOffsetObject.load() == held) {
        POINT c{rx + heldOffsetX.load(), ry + heldOffsetY.load()};
        if (ClientToScreen(w, &c)) SetCursorPos(c.x, c.y);
    }
    heldOffsetObject = 0;
}
// Caller owns cursorHoldLock. Native Cursor Look uses the held-key high bit,
// not a toggle. releaseInput resets raw deltas/capture, never the camera pose.
static bool cursorHoldEvent(fp::RmbEvent e, bool eligible, int key = 0) {
    bool changed = false;
    const bool eaten = fp::rmbStep(rmbState, 1, e, eligible, changed);
    if (changed) {
        keepHeldOffset(rmbState.on);
        InterlockedExchange(&rmbFree, rmbState.on ? 1 : 0);
        releaseInput();
        if (rmbState.on) {
            cursorHoldKey = key;
            cursorHoldOwner = cursorHoldStamp();
        }
    }
    return eaten;
}
static void rmbEvent(fp::RmbEvent e, bool eligible, bool* swallow = nullptr) {
    AcquireSRWLockExclusive(&cursorHoldLock);
    bool eaten = false;
    __try { eaten = cursorHoldEvent(e, eligible); }
    __finally { ReleaseSRWLockExclusive(&cursorHoldLock); }
    if (swallow) *swallow = eaten;
}
static bool cursorLookMessage(int key, bool down, bool repeat) {
    AcquireSRWLockExclusive(&cursorHoldLock);
    bool eaten = false;
    __try {
        __try {
            if (!down && key == cursorHoldKey) {
                eaten = cursorHoldEvent(fp::RmbEvent::up, false);
            } else if (down && !repeat && key == static_cast<int>(nativeBinding(8))) {
                // Reserved shortcuts and other mod binding owners get first refusal.
                if (key != VK_F6 && key != VK_F7 && key != VK_F8 && key != VK_F9 &&
                    key != VK_F10 && key != VK_F11 && key != VK_ESCAPE)
                    eaten = cursorHoldEvent(fp::RmbEvent::down, rmbEligible(), key);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cursorHoldEvent(fp::RmbEvent::lost, false);
        }
    } __finally { ReleaseSRWLockExclusive(&cursorHoldLock); }
    return eaten;
}
static void cursorLookPoll() {
    // No held state to retire: retain the previous input hook's cheap idle gate.
    if (!InterlockedCompareExchange(&rmbFree, 0, 0)) return;
    AcquireSRWLockExclusive(&cursorHoldLock);
    __try {
        __try {
            if (rmbState.on) {
                const fp::InputStamp now = cursorHoldStamp();
                if (!rmbEligible() || cursorHoldKey != static_cast<int>(nativeBinding(8)) ||
                    !(keyState(cursorHoldKey) & 0x8000) ||
                    now.root != cursorHoldOwner.root || now.head != cursorHoldOwner.head ||
                    now.scene != cursorHoldOwner.scene || now.mode != cursorHoldOwner.mode ||
                    now.generation != cursorHoldOwner.generation ||
                    now.policyRevision != cursorHoldOwner.policyRevision)
                    cursorHoldEvent(fp::RmbEvent::lost, false);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cursorHoldEvent(fp::RmbEvent::lost, false);
        }
    } __finally { ReleaseSRWLockExclusive(&cursorHoldLock); }
}

// Native settings are serviced only by their native GUI maintenance callback.
static void nativeSettingsMaintenance(uintptr_t);
static void (*originalGuiMaintenance)(uintptr_t)=nullptr;
static bool awaitNativeSettings(DWORD timeout=2000);
static void nativeSettingsBeforeStop();
static void writeNativeCheckboxStatus(fp::JsonOut&);
