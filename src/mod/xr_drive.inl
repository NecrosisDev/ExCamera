// Tracking is copied on the render owner; native motor inputs are consumed on the pose-builder thread.
#include "../xr/body_driver.h"
#include "../xr/equipped.h"
#include "../xr/locomotion.h"
#include "../xr/physical_contact.h"
namespace xd = fp::xr;
struct TrackedDriveFrame {
    uintptr_t root = 0, scene = 0;
    uint64_t epoch = 0, serial = 0, tick = 0, stream = 0;
    bool bodyDriven = false;
    xd::BodySample bodySample{};
    xd::NativePose renderHead{};
    uint32_t inputGeneration = 0;
    xd::LowerSample lower{};
    XrPosef stageHead{{0, 0, 0, 1}, {}};
    XrQuaternionf stageBasis{0, 0, 0, 1};
    bool roomClear = false;
    xd::MotionTarget motion{};
    uintptr_t motionSector = 0;
    std::array<xd::NativePose, xd::jointCount> poses{};
    std::array<bool, xd::jointCount> valid{};
    std::array<bool, 2> pressed{};
    // Tracked hand joints in native units and space, for the mod-drawn hand; valid only when a
    // tracker (or the controller inference) supplied every joint.
    struct HandJoints {
        std::array<XrVector3f, xd::handJointCount> position{};
        std::array<float, xd::handJointCount> radius{};
        bool valid = false;
    };
    std::array<HandJoints, 2> handJoints{};
};
static SRWLOCK trackedDriveLock = SRWLOCK_INIT;
static TrackedDriveFrame trackedDriveFrame;
static std::atomic<uint64_t> trackedDriveGeneration{0};
static std::atomic<bool> trackedDriveRequested{false}, trackedDriveFault{false}, trackedLocomotionRequested{false}, trackedWeaponsRequested{false}, trackedBodyRequested{false};
// Body driver and room-scale walking default on while body trackers are live: a key left unset (no
// EXCAMERA_XR_BODY_DRIVER / EXCAMERA_XR_LOCOMOTION, no ini key) follows the trackers, "1" forces on, "0" off.
// Without trackers nothing changes. Trackers lost for under half a second keep the switches on.
static bool trackedBodyAuto = false, trackedLocomotionAuto = false;
static uint64_t trackedLowerSeen = 0;
static constexpr uint64_t lowerHoldMs = 500;
static void trackedLowerAuto(bool live) {
    const uint64_t now = GetTickCount64();
    if (live) trackedLowerSeen = now;
    const bool on = trackedLowerSeen && now - trackedLowerSeen < lowerHoldMs;
    if (trackedBodyAuto) trackedBodyRequested = on;
    if (trackedLocomotionAuto) trackedLocomotionRequested = on;
}
static std::atomic<uintptr_t> trackedMotorRoot{0};
static std::atomic<unsigned> trackedMotorTargets{0}, trackedPickups{0};
static void trackedBodyPublish(TrackedDriveFrame&);
// The game never clears its stick slot without a pad, so whatever we wrote stays until we zero it.
// The stab modifier is a word bit we hold the same way and release the same way.
static bool trackedStickWritten = false, trackedStabWritten = false;
static void trackedStabRelease() {
    if (!trackedStabWritten) return;
    trackedStabWritten = false;
    *(unsigned*)gameAt(fp::sig::keyWord) &= ~0x80u;
}
static void trackedStickRelease() {
    trackedWalkRateMilli = 0;
    if (!trackedStickWritten) return;
    trackedStickWritten = false;
    auto analog = reinterpret_cast<float*>(gameAt(fp::sig::analogStick));
    analog[0] = analog[1] = analog[2] = 0;
}
static void trackedDriveClear() {
    trackedStickRelease();
    trackedStabRelease();
    trackedMotorRoot = 0;
    ++trackedDriveGeneration;
    if (!TryAcquireSRWLockExclusive(&trackedDriveLock)) return;
    trackedDriveFrame = {};
    ReleaseSRWLockExclusive(&trackedDriveLock);
}
static void trackedLocomotionFault() {
    if (trackedLocomotionRequested) { trackedDriveFault = true; trackedDriveClear(); }
}
static bool trackedStandClear(uintptr_t root) {
    __try { return levelGrid() && levelRay && levelRay2 && crouchHeadroomClear(root); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool trackedNoHeldItems(uintptr_t root) {
    return !*reinterpret_cast<uintptr_t*>(root + field::character::heldItems) && !*reinterpret_cast<uintptr_t*>(root + field::character::heldItems + 8);
}
// One render-owner planner publishes immutable goals/posture; input and physics never calibrate independently.
static void trackedMotionPublishImpl(TrackedDriveFrame& next) {
    static xd::Locomotion motion;
    if (!next.root || !xd::lowerValid(next.lower, next.lower.time) || !trackedLocomotionRequested || trackedBodyRequested || !manualDefenseHooksCreated || !inputEnabled || cursorFreeNow() ||
        !bodyCalm(next.root, *reinterpret_cast<uintptr_t*>(next.root + field::character::headPart)) || *(unsigned char*)(next.root + field::character::combatStance) ||
        !trackedNoHeldItems(next.root) || *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0) { motion.reset(); return; }
    const auto node = *reinterpret_cast<uintptr_t*>(next.root + field::character::sceneNode);
    const auto sector = readable(node, field::sceneNode::sector + 8) ? *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector) : 0;
    if (!sector) { motion.reset(); return; }
    const xd::MotionOwner owner{next.root, next.scene, next.epoch, next.stream, sector, next.inputGeneration};
    XrVector3f position{};
    memcpy(&position, reinterpret_cast<void*>(next.root + field::character::position), 12);
    next.motion = motion.step(owner, next.lower, next.stageHead, xd::scale(position, 1 / stereoUnitsPerMetre), next.stageBasis, next.roomClear);
    next.motionSector = sector;
}
static void trackedMotionPublish(TrackedDriveFrame& next) {
    __try { trackedMotionPublishImpl(next); }
    __except (EXCEPTION_EXECUTE_HANDLER) { next = {}; trackedDriveFault = true; }
}
// The grip pose a controller hand shows: a hilt for a weapon on that hand (or the haft the other hand
// took), a flat hand under a strapped shield, a loose hold on a carried item, a fist on the squeeze.
static bool trackedHaftHeld(); // the equipped drive, defined with the weapon code below
// The thickness of what a hand holds, in metres: the geometric mean of the two smaller half extents of
// the held part's mesh bounds (a sword's hilt is round-ish; a thick object fills the palm). 0 when unknown.
static float trackedHeldRadius(uintptr_t root, unsigned side) { return heldRadius(root, side); } // xr_bones.inl owns the held-item reading
static xd::HandPose trackedHandPose(uintptr_t root, unsigned side, bool pressed) {
    const xd::HandPose held = heldHandPose(root, side); // flat, hilt, hold, or open with nothing held
    if (held != xd::HandPose::open) return held;
    if (pressed && root && readable(root, 0x1300) && *reinterpret_cast<int*>(root + field::character::weaponCount) > 0 && trackedHaftHeld()) return xd::HandPose::hilt;
    return pressed ? xd::HandPose::fist : xd::HandPose::open;
}
static void trackedDrivePublish() {
    TrackedDriveFrame next{};
    xd::TrackingFrame frame{};
    if (trackedDriveRequested && !trackedDriveFault && stereoIdentity() && gameWindow() && xd::trackingFrame(frame) && frame.sample.focused) {
        try {
            const XrPosef identity{{0, 0, 0, 1}, {}};
            next.inputGeneration = inputOwner.generation();
            next.stream = trackedDriveGeneration.load();
            next.root = savedRoot;
            next.scene = savedScene;
            next.epoch = frame.sample.epoch;
            next.serial = frame.serial;
            next.tick = frame.tick;
            next.bodySample = frame.sample;
            next.poses[xd::head] = xd::nativeEye(frame.anchor, frame.calibration.relative(frame.sample.head), static_cast<float>(vrUnitsPerMetre.load()));
            next.renderHead = next.poses[xd::head];
            next.valid[xd::head] = true;
            const bool lowerLive = xd::lowerValid(frame.lower, frame.sample.time) && !vrPosture.load(std::memory_order_relaxed);
            trackedLowerAuto(lowerLive);
            if (lowerLive) { // seated: the legs stay the game's
                next.lower = frame.lower;
                next.stageHead = frame.sample.head;
                next.roomClear = trackedLocomotionRequested && trackedStandClear(savedRoot);
                next.stageBasis = xd::nativePoseMetres(xd::nativeEye(frame.anchor, frame.calibration.relative(identity), static_cast<float>(vrUnitsPerMetre.load()))).orientation;
                for (unsigned i = 0; i < 3; ++i) {
                    next.poses[xd::waist + i] = xd::nativeEye(frame.anchor, frame.calibration.relative(frame.lower.trackers[i].pose), static_cast<float>(vrUnitsPerMetre.load()));
                    next.valid[xd::waist + i] = true;
                }
            }
            for (unsigned i = 0; i < 2; ++i)
                if (frame.sample.hands[i].tracked) {
                    next.poses[xd::leftHand + i] = xd::nativeEye(frame.anchor, frame.calibration.relative(frame.sample.hands[i].grip), static_cast<float>(vrUnitsPerMetre.load()));
                    next.valid[xd::leftHand + i] = true;
                    next.pressed[i] = frame.sample.hands[i].pressed;
                    // Controller hands take the pose of what they hold; a tracked hand keeps its real fingers.
                    const auto& sampled = frame.sample.hands[i].joints;
                    const xd::HandJoints joints = sampled.tracked ? sampled : xd::inferJoints(frame.sample.hands[i].grip, trackedHandPose(savedRoot, i, frame.sample.hands[i].pressed), i == 1, trackedHeldRadius(savedRoot, i));
                    if ((joints.validMask & xd::allHandJoints) == xd::allHandJoints) {
                        auto& out = next.handJoints[i];
                        const float units = static_cast<float>(vrUnitsPerMetre.load());
                        for (unsigned j = 0; j < xd::handJointCount; ++j) {
                            const auto native = xd::nativeEye(frame.anchor, frame.calibration.relative(joints.pose[j]), units);
                            out.position[j] = {native[9], native[10], native[11]};
                            out.radius[j] = joints.radius[j] * units;
                        }
                        out.valid = true;
                    }
                }
        } catch (const std::exception&) { next = {}; }
    }
    trackedBodyPublish(next);
    trackedMotionPublish(next);
    if (!TryAcquireSRWLockExclusive(&trackedDriveLock)) return;
    trackedDriveFrame = next;
    ReleaseSRWLockExclusive(&trackedDriveLock);
}
static bool trackedDriveRead(uintptr_t root, TrackedDriveFrame& frame) {
    if (!trackedDriveRequested || trackedDriveFault || !active || stopping || !cameraReady || viewMode.load() != 0 ||
        cursorFree || windowFree || !TryAcquireSRWLockShared(&trackedDriveLock)) return false;
    frame = trackedDriveFrame;
    ReleaseSRWLockShared(&trackedDriveLock);
    return frame.inputGeneration == inputOwner.generation() && frame.stream == trackedDriveGeneration.load() && frame.root == root && frame.scene && frame.scene == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) &&
           root == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) && frame.valid[xd::head] && GetTickCount64() - frame.tick <= 100;
}
static bool trackedMotion(uintptr_t root, xd::MotionTarget& out) {
    if (!root || !base || root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) || vrPosture.load(std::memory_order_relaxed)) return false; // seated: no room walking or crouching
    TrackedDriveFrame frame{};
    if (!trackedLocomotionRequested || trackedBodyRequested || !manualDefenseHooksCreated || !inputEnabled || cursorFreeNow() || !trackedDriveRead(root, frame) || !frame.motion.valid || !readable(root, 0x1300) ||
        !bodyCalm(root, *reinterpret_cast<uintptr_t*>(root + field::character::headPart)) || (*(unsigned char*)(root + field::character::combatStance) && !trackedBodyOwns(root)) ||
        !trackedNoHeldItems(root) || *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0) return false;
    const auto node = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
    const auto sector = readable(node, field::sceneNode::sector + 8) ? *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector) : 0;
    if (!sector || sector != frame.motionSector) return false;
    XrVector3f position{};
    memcpy(&position, reinterpret_cast<void*>(root + field::character::position), 12);
    auto error = xd::sub(frame.motion.goal, xd::scale(position, 1 / stereoUnitsPerMetre));
    error.y = 0;
    const float distance = xd::length(error);
    if (!std::isfinite(distance) || distance > 1.5f) return false;
    out = frame.motion;
    out.distance = distance;
    out.moving = out.moving && distance > .025f;
    out.direction = out.moving ? xd::unit(error) : XrVector3f{};
    return true;
}
static bool trackedWalkInput(uintptr_t root, float* direction, float& distance) {
    __try {
        xd::MotionTarget target{};
        if (!trackedMotion(root, target)) return false;
        memcpy(direction, &target.direction, 12);
        distance = target.distance * stereoUnitsPerMetre;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; return false; }
}
// Controller actions feed the game's own input words, the ones keys and a gamepad already fill,
// so native movement, combat and menus see nothing new. Keys and a real stick win over ours.
static std::atomic<bool> trackedAttack{false};
static bool trackedMenuWasDown = false, trackedMenuLong = false;
static uint64_t trackedMenuDownSince = 0;
// Turn requests in micro-radians, taken by the aim update on the input thread, so the body yaw
// turns exactly as a mouse movement would and the headset view follows the body.
static std::atomic<long> trackedTurnPending{0};
static bool trackedTurnArmed = true;
static uint64_t trackedTurnTick = 0;
static float trackedTurnTake() { return static_cast<float>(trackedTurnPending.exchange(0)) * 1e-6f; }
static void trackedTurnStep(float stickX, bool stickActive) {
    const int mode = vrTurning.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    const float dt = trackedTurnTick ? (std::min)(static_cast<float>(now - trackedTurnTick), 100.f) * .001f : 0.f;
    trackedTurnTick = now;
    trackedTurnRateMilli = 0;
    if (!stickActive || mode == 0) { trackedTurnArmed = true; return; }
    if (mode == 3) { // smooth: 90 degrees per second at full deflection, with a dead zone
        if (std::fabs(stickX) > .25f) { trackedTurnPending.fetch_add(static_cast<long>(-stickX * 1.5707963f * dt * 1e6f)); trackedTurnRateMilli = int(std::fabs(stickX) * 1000); }
        return;
    }
    if (trackedTurnArmed && std::fabs(stickX) > .7f) { // snap: one fixed turn per flick, re-armed near centre
        trackedTurnArmed = false;
        trackedTurnPending.fetch_add(static_cast<long>((stickX > 0 ? -1.f : 1.f) * (mode == 2 ? .7853982f : .5235988f) * 1e6f));
        trackedSnapBlinkAt = now; // the comfort blink hides the jump (any comfort setting but off)
    } else if (std::fabs(stickX) < .3f) trackedTurnArmed = true;
}
static bool trackedAttackHeld() { return trackedAttack.load(std::memory_order_relaxed); }
// The VR kick and jump: the stick pressed in, and a physical hop of the headset (standing posture), both
// through the shared motions (motions.inl). No button for the jump, so the mod picks no key.
static void trackedBonesTick(uintptr_t root, bool owned, const TrackedDriveFrame& frame); // xr_bones_vr.inl, after the hand helpers it needs
static std::array<bool, 2> trackedKickWasDown{};
static float trackedJumpHeadY = 0;
static XrTime trackedJumpHeadTime = 0;
static constexpr float jumpHopSpeed = 1.1f; // m/s of upward head speed that counts as a hop
static void trackedJumpTick(uintptr_t root, bool owned, const TrackedDriveFrame& frame) {
    const float y = frame.bodySample.head.position.y;
    const XrTime time = frame.bodySample.time;
    float speed = 0;
    if (trackedJumpHeadTime && time > trackedJumpHeadTime && std::isfinite(y)) {
        const float dt = static_cast<float>(time - trackedJumpHeadTime) * 1e-9f;
        if (dt > .005f && dt < .2f) speed = (y - trackedJumpHeadY) / dt;
    }
    if (std::isfinite(y) && time > trackedJumpHeadTime) { trackedJumpHeadY = y; trackedJumpHeadTime = time; }
    if (speed >= jumpHopSpeed && vrJump.load(std::memory_order_relaxed) && !vrPosture.load(std::memory_order_relaxed) && owned) motionJump(root);
}
static void trackedKickTick(uintptr_t root, bool owned, const TrackedDriveFrame& frame) {
    bool pressed = false;
    for (unsigned side = 0; side < 2; ++side) {
        const bool down = frame.bodySample.hands[side].tracked && frame.bodySample.hands[side].kick;
        pressed = pressed || (down && !trackedKickWasDown[side]);
        trackedKickWasDown[side] = down;
    }
    if (pressed && vrKick.load(std::memory_order_relaxed) && owned && !trackedAttack.load(std::memory_order_relaxed)) motionKick(root); // not while the trigger swings
}
// Grab-to-turn: an empty hand squeezing the air and sweeping sideways around the head turns the body
// the other way, as if the world were dragged. Goes through the same yaw path as the stick turns.
static std::array<bool, 2> trackedGrabTurning{};
static std::array<float, 2> trackedGrabAngle{};
static std::atomic<unsigned> trackedGrabTurns{0};
static void trackedGrabTurn(uintptr_t root, const TrackedDriveFrame& frame) {
    const bool on = vrGrabTurn.load(std::memory_order_relaxed) && readable(root, field::character::heldItems + 16) && *reinterpret_cast<int*>(root + field::character::weaponCount) == 0;
    for (unsigned side = 0; side < 2; ++side) {
        const auto& hand = frame.bodySample.hands[side];
        const bool holding = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8) != 0;
        const bool grab = on && hand.tracked && hand.pressed && !holding;
        if (!grab) { trackedGrabTurning[side] = false; continue; }
        const auto& head = frame.bodySample.head.position;
        const float angle = std::atan2(hand.grip.position.x - head.x, hand.grip.position.z - head.z); // the hand's bearing around the head
        if (trackedGrabTurning[side]) {
            float delta = angle - trackedGrabAngle[side];
            while (delta > 3.1415927f) delta -= 6.2831853f;
            while (delta < -3.1415927f) delta += 6.2831853f;
            if (std::fabs(delta) < 1.5f) { trackedTurnPending.fetch_add(static_cast<long>(delta * 1e6f)); trackedGrabTurns.fetch_add(1, std::memory_order_relaxed); } // a wrap or a tracking jump is not a sweep
        }
        trackedGrabTurning[side] = true;
        trackedGrabAngle[side] = angle;
    }
}
// Controller pointer: the aim ray of a hand meets the menu panel (the head-locked quad, or the one
// standing in the level at the title); the hit drives the game's own cursor and the trigger clicks it.
static std::atomic<unsigned> trackedPointerMoves{0}, trackedPointerClicks{0};
static bool trackedPointerDown = false, trackedPointerRightDown = false;
static uint64_t trackedPointerWheelTick = 0;
static constexpr uint64_t pointerWheelMs = 150; // one notch per this while the stick is held over
static constexpr float pointerWheelStick = .5f;
enum TrackedPointerEvent { pointerMove, pointerLeftDown, pointerLeftUp, pointerRightDown, pointerRightUp, pointerWheelUp, pointerWheelDown };
static void (*trackedPointerObserver)(int x, int y, int event) = nullptr; // test seam
// A beam from the aiming hand to the hit, for the host to draw; written on the input thread, read at present.
struct TrackedBeamState { bool shown = false; XrVector3f from{}, to{}; float u = 0, v = 0; };
static SRWLOCK trackedBeamLock = SRWLOCK_INIT;
static TrackedBeamState trackedBeam;
static void trackedPointerAct(int x, int y, int event) {
    if (trackedPointerObserver) { trackedPointerObserver(x, y, event); return; }
    if (!hookedWindow) return;
    POINT screen{x, y};
    if (!ClientToScreen(hookedWindow, &screen)) return;
    const WPARAM held = (trackedPointerDown ? MK_LBUTTON : 0) | (trackedPointerRightDown ? MK_RBUTTON : 0);
    switch (event) {
    case pointerMove: // the game reads the Windows cursor, as its own warp writes it; the move message carries a drag
        SetCursorPos(screen.x, screen.y);
        PostMessageW(hookedWindow, WM_MOUSEMOVE, held, MAKELPARAM(x, y));
        break;
    case pointerLeftDown: PostMessageW(hookedWindow, WM_LBUTTONDOWN, held | MK_LBUTTON, MAKELPARAM(x, y)); break;
    case pointerLeftUp: PostMessageW(hookedWindow, WM_LBUTTONUP, held & ~WPARAM(MK_LBUTTON), MAKELPARAM(x, y)); break;
    case pointerRightDown: PostMessageW(hookedWindow, WM_RBUTTONDOWN, held | MK_RBUTTON, MAKELPARAM(x, y)); break;
    case pointerRightUp: PostMessageW(hookedWindow, WM_RBUTTONUP, held & ~WPARAM(MK_RBUTTON), MAKELPARAM(x, y)); break;
    case pointerWheelUp: PostMessageW(hookedWindow, WM_MOUSEWHEEL, MAKEWPARAM(held, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y)); break; // the wheel's point is in screen space
    case pointerWheelDown: PostMessageW(hookedWindow, WM_MOUSEWHEEL, MAKEWPARAM(held, WORD(-WHEEL_DELTA)), MAKELPARAM(screen.x, screen.y)); break;
    default: break;
    }
}
// A ray meets a panel only when it points at it from the front; a direction this close to the plane is a miss.
static constexpr float pointerGrazing = 1e-4f;
static bool trackedPointerHit(const xd::TrackingFrame& tracked, bool inLevel, float aspect, float& u, float& v, unsigned& side, XrVector3f& from, XrVector3f& to) {
    const float width = static_cast<float>(vrPanelWidthCm.load(std::memory_order_relaxed)) * .01f;
    const float distance = static_cast<float>(vrPanelDistanceCm.load(std::memory_order_relaxed)) * .01f, height = width * aspect;
    XrPosef panel = fp::xr::compose(inLevel ? tracked.calibration.origin() : tracked.sample.head, {{0, 0, 0, 1}, {0, 0, -distance}});
    float panelWidth = width, panelHeight = height;
    XrPosef held{};
    const bool inHand = !inLevel && vrHandPanel.load(std::memory_order_relaxed) && fp::xr::handPanelPose(tracked.sample, held); // the host's own placement
    if (inHand) { panel = held; panelWidth = fp::xr::handPanelWidth; panelHeight = panelWidth * aspect; }
    for (unsigned s : {1u, 0u}) { // the right hand first
        const auto& hand = tracked.sample.hands[s];
        if (!hand.aimTracked || (inHand && s == 0)) continue; // the hand holding the panel does not point at it
        const XrPosef rel = fp::xr::compose(fp::xr::inverse(panel), hand.aim);
        const XrVector3f dir = fp::xr::rotate(rel.orientation, {0, 0, -1});
        if (!(dir.z < -pointerGrazing) || !(rel.position.z > 0)) continue; // must point at the panel from in front of it
        const float t = -rel.position.z / dir.z;
        const float x = rel.position.x + dir.x * t, y = rel.position.y + dir.y * t;
        u = x / panelWidth + .5f;
        v = .5f - y / panelHeight;
        if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) continue;
        side = s;
        from = hand.aim.position;
        to = fp::xr::add(panel.position, fp::xr::rotate(panel.orientation, {x, y, 0}));
        return true;
    }
    return false;
}
// An item carried out of the inventory: the game's own drag of an inventory item (format 0xFB01) leaving its
// window turns into the item in the world following the cursor, placed where the cursor's ray meets the world
// when the button is let go (ExCamera's inventory preview path owns that native flow). In VR the beam leaving the
// hand panel with an item carried keeps the button held and puts the cursor where the right hand points: the
// point 1.5 m along its aim projected through the game's camera, so the native ray from the eye passes the same
// point. Letting the trigger go places it there, to pick up with a squeeze. Nothing native is replaced.
static std::atomic<unsigned> trackedItemCarries{0};
static bool trackedCarryingItem() {
    const uintptr_t gui = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiManager));
    return readable(gui, field::gui::dragFormat + 4) && *reinterpret_cast<unsigned*>(gui + field::gui::dragFormat) == 0xFB01;
}
static bool trackedCarryCursor(const xd::TrackingFrame& tracked, int width, int height, int& x, int& y) {
    const auto& hand = tracked.sample.hands[1];
    if (!hand.aimTracked || !savedCamera || !readable(savedCamera, field::camera::fov + 4) || width < 2 || height < 2) return false;
    const float units = static_cast<float>(vrUnitsPerMetre.load(std::memory_order_relaxed));
    const XrVector3f ahead = xd::add(hand.aim.position, xd::rotate(hand.aim.orientation, {0, 0, -1.5f}));
    const auto native = xd::nativeEye(tracked.anchor, tracked.calibration.relative({{0, 0, 0, 1}, ahead}), units);
    const float* pose = reinterpret_cast<const float*>(savedCamera + field::camera::pose); // right, up, back, origin
    const float fov = *reinterpret_cast<const float*>(savedCamera + field::camera::fov);
    const float d[3] = {native[9] - pose[9], native[10] - pose[10], native[11] - pose[11]};
    const float lx = d[0] * pose[0] + d[1] * pose[1] + d[2] * pose[2], ly = d[0] * pose[3] + d[1] * pose[4] + d[2] * pose[5], lz = d[0] * pose[6] + d[1] * pose[7] + d[2] * pose[8];
    if (!std::isfinite(fov) || fov < 10 || fov > 170 || !(lz < -1.f)) return false; // in front of the camera
    const float t = std::tan(fov * .5f * 3.14159265f / 180.f), aspect = float(width) / float(height);
    const float nx = lx / (-lz * t * aspect), ny = ly / (-lz * t);
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
    x = int((std::min)(.98f, (std::max)(.02f, nx * .5f + .5f)) * float(width));
    y = int((std::min)(.98f, (std::max)(.02f, .5f - ny * .5f)) * float(height));
    return true;
}
static void trackedBeamPublish(const TrackedBeamState& state) {
    AcquireSRWLockExclusive(&trackedBeamLock);
    trackedBeam = state;
    ReleaseSRWLockExclusive(&trackedBeamLock);
}
static void trackedPointerTick(const xd::TrackingFrame& tracked, bool live) {
    RECT client{};
    float u = 0, v = 0;
    unsigned side = 1;
    XrVector3f from{}, to{};
    const bool hit = vrPointer.load(std::memory_order_relaxed) && live && *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0 && hookedWindow &&
                     GetClientRect(hookedWindow, &client) && client.right > 0 && client.bottom > 0 &&
                     trackedPointerHit(tracked, stereoTitleWanted() && !savedRoot, static_cast<float>(client.bottom) / static_cast<float>(client.right), u, v, side, from, to);
    if (!hit && trackedPointerDown && live && vrHandPanel.load(std::memory_order_relaxed) && *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0 &&
        trackedCarryingItem() && hookedWindow && GetClientRect(hookedWindow, &client) && client.right > 1 && client.bottom > 1) {
        int x = 0, y = 0; // an inventory item carried off the panel: the cursor follows the right hand into the world
        if (trackedCarryCursor(tracked, client.right, client.bottom, x, y)) {
            trackedPointerAct(x, y, pointerMove);
            if (!tracked.sample.hands[1].trigger) { trackedPointerAct(x, y, pointerLeftUp); trackedPointerDown = false; trackedItemCarries.fetch_add(1, std::memory_order_relaxed); }
            trackedBeamPublish({});
            return;
        }
    }
    if (!hit) { // the buttons let go where the beam last was, so a drag ends instead of sticking
        if (trackedPointerDown) { trackedPointerAct(0, 0, pointerLeftUp); trackedPointerDown = false; }
        if (trackedPointerRightDown) { trackedPointerAct(0, 0, pointerRightUp); trackedPointerRightDown = false; }
        trackedBeamPublish({});
        return;
    }
    const int x = static_cast<int>(u * static_cast<float>(client.right)), y = static_cast<int>(v * static_cast<float>(client.bottom));
    trackedPointerAct(x, y, pointerMove);
    trackedPointerMoves.fetch_add(1, std::memory_order_relaxed);
    const auto& hand = tracked.sample.hands[side];
    if (hand.trigger && !trackedPointerDown) { trackedPointerDown = true; trackedPointerAct(x, y, pointerLeftDown); trackedPointerClicks.fetch_add(1, std::memory_order_relaxed); }
    else if (!hand.trigger && trackedPointerDown) { trackedPointerDown = false; trackedPointerAct(x, y, pointerLeftUp); }
    if (hand.pressed && !trackedPointerRightDown) { trackedPointerRightDown = true; trackedPointerAct(x, y, pointerRightDown); } // the squeeze is the right button
    else if (!hand.pressed && trackedPointerRightDown) { trackedPointerRightDown = false; trackedPointerAct(x, y, pointerRightUp); }
    const uint64_t now = GetTickCount64();
    if (hand.stickActive && std::fabs(hand.stick.y) > pointerWheelStick && now - trackedPointerWheelTick >= pointerWheelMs) { // the stick scrolls, a notch at a time
        trackedPointerWheelTick = now;
        trackedPointerAct(x, y, hand.stick.y > 0 ? pointerWheelUp : pointerWheelDown);
    }
    trackedBeamPublish({true, from, to, u, v});
}
// On the render thread: hand this frame's beam to the host.
static void trackedBeamPresent() {
    if (!stereoBackend.beam) return;
    TrackedBeamState state;
    AcquireSRWLockShared(&trackedBeamLock);
    state = trackedBeam;
    ReleaseSRWLockShared(&trackedBeamLock);
    stereoBackend.beam(state.shown, state.from, state.to, state.u, state.v);
}
// Wrist menu: raise the left hand near the head with the palm or its back turned to the face, and a
// small panel floats above that wrist with the toggles a player reaches for in the headset; the right
// hand points at a line and the trigger takes it (and does not attack while it does).
static SRWLOCK trackedWristLock = SRWLOCK_INIT;
static constexpr float wristReach = .7f;      // metres from the left grip to the head, at most
static constexpr float wristFacing = .7f;     // cosine between the palm normal and the line to the head, at least
static constexpr float wristDrop = .45f;      // metres the grip may hang below the head
static constexpr float wristLift = .08f;      // metres the panel floats above the grip
static constexpr float wristLineHeight = xd::wristPanelHeight / xd::wristPanelLines;
struct TrackedWristState { bool shown = false; XrPosef pose{{0, 0, 0, 1}, {}}; int highlight = -1; char lines[xd::wristPanelLines][32]{}; };
static TrackedWristState trackedWrist;
static bool trackedWristDown = false;
static std::atomic<int> trackedWristHighlight{-1};
static std::atomic<unsigned> trackedWristActions{0};
static void trackedWristNarrow(char* out, size_t size, const wchar_t* prefix, const wchar_t* value) {
    size_t n = 0;
    for (const wchar_t* w = prefix; *w && n + 1 < size; ++w) out[n++] = char(*w);
    for (const wchar_t* w = value; w && *w && n + 1 < size; ++w) out[n++] = char(*w);
    out[n] = 0;
}
static void trackedWristAct(int row) {
    switch (row) {
    case 0: ++trackingReset; break;
    case 1: vrPosture = vrPosture.load() ? 0 : 1; saveSetting(L"VR", L"Posture", fp::vrPostureText(vrPosture.load())); break;
    case 2: vrTurning = (vrTurning.load() + 1) % 4; saveSetting(L"VR", L"Turning", fp::vrTurningText(vrTurning.load())); break;
    case 3: vrHandsDrawn = vrHandsDrawn.load() ? 0 : 1; saveSetting(L"VR", L"Hands", fp::vrHandsText(vrHandsDrawn.load())); break;
    case 4: vrBodyHandsOnly = vrBodyHandsOnly.load() ? 0 : 1; saveSetting(L"VR", L"Body", fp::vrBodyText(vrBodyHandsOnly.load())); break;
    case 5: vrComfort = (vrComfort.load() + 1) % 3; saveSetting(L"VR", L"Comfort", fp::vrComfortText(vrComfort.load())); break;
    case 6: { // haptics in quarter steps, wrapping from double back to off
        const int percent = (vrHapticPercent.load() + 50) % 250;
        vrHapticPercent = percent;
        saveSetting(L"VR", L"HapticPercent", std::to_wstring(percent).c_str());
        break;
    }
    default: return;
    }
    trackedWristActions.fetch_add(1, std::memory_order_relaxed);
}
static void trackedWristTick(const xd::TrackingFrame& tracked, bool live) {
    TrackedWristState next{};
    const auto& left = tracked.sample.hands[0];
    const auto& right = tracked.sample.hands[1];
    const auto& head = tracked.sample.head;
    const bool panelInHand = vrHandPanel.load(std::memory_order_relaxed) && *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0; // the left hand holds the menu instead
    if (vrWristMenu.load(std::memory_order_relaxed) && live && left.tracked && !panelInHand) {
        const XrVector3f toHead = {head.position.x - left.grip.position.x, head.position.y - left.grip.position.y, head.position.z - left.grip.position.z};
        const float dist = std::sqrt(toHead.x * toHead.x + toHead.y * toHead.y + toHead.z * toHead.z);
        const XrVector3f palm = fp::xr::rotate(left.grip.orientation, {1, 0, 0}); // the grip's x axis is the palm normal, either way round
        const float facing = dist > 1e-3f ? std::fabs((palm.x * toHead.x + palm.y * toHead.y + palm.z * toHead.z) / dist) : 0.f;
        next.shown = dist < wristReach && facing > wristFacing && left.grip.position.y > head.position.y - wristDrop;
        if (next.shown) {
            next.pose = {fp::xr::yawQuaternion(std::atan2(toHead.x, toHead.z)), {left.grip.position.x, left.grip.position.y + wristLift, left.grip.position.z}}; // above the wrist, facing the head
            if (right.aimTracked) {
                const XrPosef rel = fp::xr::compose(fp::xr::inverse(next.pose), right.aim);
                const XrVector3f dir = fp::xr::rotate(rel.orientation, {0, 0, -1});
                if (dir.z < -pointerGrazing && rel.position.z > 0) {
                    const float t = -rel.position.z / dir.z, x = rel.position.x + dir.x * t, y = rel.position.y + dir.y * t;
                    if (std::fabs(x) <= xd::wristPanelWidth / 2 && std::fabs(y) <= xd::wristPanelHeight / 2)
                        next.highlight = (std::min)(int(xd::wristPanelLines) - 2, (std::max)(0, int((xd::wristPanelHeight / 2 - y) / wristLineHeight)));
                }
            }
            const bool trigger = right.aimTracked && right.trigger;
            if (trigger && !trackedWristDown && next.highlight >= 0) trackedWristAct(next.highlight);
            trackedWristDown = trigger;
        }
    }
    if (!next.shown) trackedWristDown = false;
    trackedWristNarrow(next.lines[0], 32, L"Recenter", nullptr);
    trackedWristNarrow(next.lines[1], 32, L"Posture: ", fp::vrPostureText(vrPosture.load(std::memory_order_relaxed)));
    trackedWristNarrow(next.lines[2], 32, L"Turning: ", fp::vrTurningText(vrTurning.load(std::memory_order_relaxed)));
    trackedWristNarrow(next.lines[3], 32, L"Hands: ", fp::vrHandsText(vrHandsDrawn.load(std::memory_order_relaxed)));
    trackedWristNarrow(next.lines[4], 32, L"Body: ", fp::vrBodyText(vrBodyHandsOnly.load(std::memory_order_relaxed)));
    trackedWristNarrow(next.lines[5], 32, L"Comfort: ", fp::vrComfortText(vrComfort.load(std::memory_order_relaxed)));
    trackedWristNarrow(next.lines[6], 32, L"Haptics: ", (std::to_wstring(vrHapticPercent.load(std::memory_order_relaxed)) + L"%").c_str());
    trackedWristHighlight = next.shown ? next.highlight : -1;
    AcquireSRWLockExclusive(&trackedWristLock);
    trackedWrist = next;
    ReleaseSRWLockExclusive(&trackedWristLock);
}
// On the render thread: hand this frame's wrist panel to the host.
static void trackedWristPresent() {
    if (!stereoBackend.wrist) return;
    TrackedWristState state;
    AcquireSRWLockShared(&trackedWristLock);
    state = trackedWrist;
    ReleaseSRWLockShared(&trackedWristLock);
    const char* lines[xd::wristPanelLines] = {state.lines[0], state.lines[1], state.lines[2], state.lines[3], state.lines[4], state.lines[5], state.lines[6], state.lines[7]};
    stereoBackend.wrist(state.shown, state.pose, lines, xd::wristPanelLines - 1, state.highlight); // seven lines on the eight-line panel
}
// Teleport: the right stick pushed forward aims an arc from the right hand's aim pose, shown by the pointer
// beam to where it meets the floor under the player; letting the stick go moves the character there with the
// game's own character move 170810(character, transform48, 1) (the low-level placement its arrival, knock-out
// and device paths use), keeping the character's own basis and height. Every arc segment is checked against
// the level with the game's level ray, so it never passes a wall; at most 6 m; on the game thread; not while
// dragging, attacking, down or dead. The move resets the character's stamina, which is put back. Row VR: teleport.
static std::atomic<unsigned> trackedTeleports{0};
static std::atomic<bool> trackedTeleportFault{false};
static bool trackedTeleportAiming = false, trackedTeleportValid = false;
static XrVector3f trackedTeleportNative{};
static uint64_t trackedTeleportLast = 0;
static constexpr float teleportArcSpeed = 7.f, teleportGravity = 9.8f, teleportStep = .025f, teleportReach = 6.f; // metres, seconds
static bool trackedTeleportCall(uintptr_t root, const float* transform) { // SEH-only frame
    __try {
        reinterpret_cast<void (*)(uintptr_t, const float*, bool)>(gameAt(fp::sig::characterMove))(root, transform, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedTeleportFault = true; return false; }
}
static bool trackedTeleportAllowed(uintptr_t root) {
    if (!vrTeleport.load(std::memory_order_relaxed) || trackedTeleportFault.load() || !layout.optionalGroupAvailable("teleport") || !root || !readable(root, 0x1300) ||
        root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) || uiOpen() || cursorFreeNow()) return false;
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    const unsigned state = *reinterpret_cast<unsigned*>(root + field::character::stateFlags);
    if ((motion >= attackFamilyFirst && motion <= attackFamilyLast) || motion == kickMotion || motion == jumpMotion) return false;
    if ((state != 0 && state != 8) || (*reinterpret_cast<unsigned*>(root + field::character::lifeFlags) & 0x100)) return false;
    const uintptr_t drag = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::dragManager));
    if (readable(drag, 0x20) && (*reinterpret_cast<const unsigned*>(drag + field::drag::flags) & (0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x80000u))) return false; // holding something through the drag manager
    return true;
}
// The landing point (native, global) of the arc from the right hand, or false: the floor is the plane through the player's feet.
static bool trackedTeleportArc(uintptr_t root, const xd::TrackingFrame& tracked, XrVector3f& landingXr, XrVector3f& landingNative) {
    const auto& hand = tracked.sample.hands[1];
    if (!hand.aimTracked) return false;
    const uintptr_t actor = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
    if (!readable(actor, field::sceneNode::sector + 8)) return false;
    const uintptr_t sector = *reinterpret_cast<uintptr_t*>(actor + field::sceneNode::sector);
    const uintptr_t level = levelGrid();
    if (!readable(sector, field::sector::origin + 12) || !level || !levelRay || !levelRay2) return false;
    const float* origin = reinterpret_cast<const float*>(sector + field::sector::origin);
    const float* feet = reinterpret_cast<const float*>(root + field::character::position);
    const float floorY = feet[1] + origin[1];
    const float units = static_cast<float>(vrUnitsPerMetre.load(std::memory_order_relaxed));
    if (!std::isfinite(floorY) || !(units > 0)) return false;
    auto native = [&](XrVector3f p, XrVector3f& out) {
        const auto pose = xd::nativeEye(tracked.anchor, tracked.calibration.relative({{0, 0, 0, 1}, p}), units);
        out = {pose[9], pose[10], pose[11]};
        return xd::finite(out);
    };
    const XrVector3f start = hand.aim.position, velocity = xd::scale(xd::rotate(hand.aim.orientation, {0, 0, -1}), teleportArcSpeed);
    XrVector3f previous = start, previousNative{};
    if (!native(start, previousNative)) return false;
    for (unsigned i = 1; i <= 80; ++i) {
        const float t = float(i) * teleportStep;
        const XrVector3f p = {start.x + velocity.x * t, start.y + velocity.y * t - .5f * teleportGravity * t * t, start.z + velocity.z * t};
        XrVector3f n{};
        if (!native(p, n)) return false;
        const XrVector3f d = xd::sub(n, previousNative);
        const float len = xd::length(d);
        if (len > 1e-3f) {
            const XrVector3f dir = xd::scale(d, 1 / len);
            const float from[3] = {previousNative.x, previousNative.y, previousNative.z}, along[3] = {dir.x, dir.y, dir.z};
            if (levelClear(level, from, along, len) < len - 1.f) return false; // a wall on the way
        }
        if (n.y <= floorY && previousNative.y > floorY) { // the arc comes down through the floor plane
            const float k = (previousNative.y - floorY) / (previousNative.y - n.y);
            landingNative = xd::add(previousNative, xd::scale(xd::sub(n, previousNative), k));
            landingXr = xd::add(previous, xd::scale(xd::sub(p, previous), k));
            const float dx = landingNative.x - (feet[0] + origin[0]), dz = landingNative.z - (feet[2] + origin[2]);
            return std::sqrt(dx * dx + dz * dz) <= teleportReach * units;
        }
        previous = p;
        previousNative = n;
    }
    return false;
}
static void trackedTeleportTick(uintptr_t root, const xd::TrackingFrame& tracked) {
    const auto& stick = tracked.sample.hands[1];
    const bool forward = stick.stickActive && stick.stick.y > .7f && std::fabs(stick.stick.x) < .5f;
    if (!trackedTeleportAllowed(root)) { trackedTeleportAiming = trackedTeleportValid = false; return; }
    if (forward) {
        XrVector3f xr{}, n{};
        trackedTeleportAiming = true;
        trackedTeleportValid = trackedTeleportArc(root, tracked, xr, n);
        if (trackedTeleportValid) { trackedTeleportNative = n; trackedBeamPublish({true, stick.aim.position, xr, -1, -1}); }
        return;
    }
    if (!trackedTeleportAiming) return;
    const bool go = trackedTeleportValid && std::fabs(stick.stick.y) < .3f;
    if (!go && stick.stickActive && stick.stick.y >= .3f) return; // still easing back: keep aiming
    trackedTeleportAiming = false;
    trackedTeleportValid = false;
    const uint64_t now = GetTickCount64();
    if (!go || now - trackedTeleportLast < 400) return;
    const uintptr_t actor = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
    const uintptr_t sector = readable(actor, field::sceneNode::sector + 8) ? *reinterpret_cast<uintptr_t*>(actor + field::sceneNode::sector) : 0;
    if (!readable(sector, field::sector::origin + 12) || !readable(root + field::character::transform, 48)) return;
    const float* origin = reinterpret_cast<const float*>(sector + field::sector::origin);
    float transform[12];
    memcpy(transform, reinterpret_cast<const void*>(root + field::character::transform), 36); // the character's own basis
    const float* feet = reinterpret_cast<const float*>(root + field::character::position);
    transform[9] = trackedTeleportNative.x - origin[0];
    transform[10] = feet[1];                               // its own height: the landing is on the plane through its feet
    transform[11] = trackedTeleportNative.z - origin[2];
    for (float f : transform)
        if (!std::isfinite(f)) return;
    const float stamina = *reinterpret_cast<float*>(root + field::character::stamina); // the move refills it; teleporting is not a rest
    trackedTeleportLast = now;
    if (trackedTeleportCall(root, transform)) {
        if (std::isfinite(stamina)) *reinterpret_cast<float*>(root + field::character::stamina) = stamina;
        trackedTeleports.fetch_add(1, std::memory_order_relaxed);
    }
}
static void trackedControllerInput(uintptr_t root, uintptr_t bh, bool owned) {
    trackedAttack = false;
    // The menu button must also close what it opened, so it only needs a focused, fresh sample.
    xd::TrackingFrame tracked{};
    const bool live = gameWindow() && xd::trackingFrame(tracked) && tracked.sample.focused;
    const bool menuDown = live && (tracked.sample.hands[0].menu || tracked.sample.hands[1].menu);
    const uint64_t now = GetTickCount64();
    // A tap is the game's own menu key on release; a long hold recenters the play space instead.
    if (menuDown && !trackedMenuWasDown) { trackedMenuDownSince = now; trackedMenuLong = false; }
    if (menuDown && !trackedMenuLong && now - trackedMenuDownSince >= 1500) { trackedMenuLong = true; ++trackingReset; }
    if (!menuDown && trackedMenuWasDown && !trackedMenuLong && now - trackedMenuDownSince < 600 && hookedWindow) {
        PostMessageW(hookedWindow, WM_KEYDOWN, VK_ESCAPE, 1);
        PostMessageW(hookedWindow, WM_KEYUP, VK_ESCAPE, 0xC0000001);
    }
    trackedMenuWasDown = menuDown;
    trackedPointerTick(tracked, live);
    trackedWristTick(tracked, live);
    TrackedDriveFrame frame{};
    if (!owned || !bh || !live || !inputEnabled || !trackedDriveRead(root, frame) || !frame.bodySample.focused) { trackedStickRelease(); trackedStabRelease(); return; }
    const auto& hands = frame.bodySample.hands;
    if (*reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount)) > 0 || cursorFreeNow()) { trackedStickRelease(); trackedStabRelease(); trackedTurnStep(0, false); return; }
    // The hand that does not walk turns: the right stick when the left walks, else nothing turns.
    trackedTeleportTick(root, tracked);
    trackedTurnStep(trackedTeleportAiming ? 0.f : hands[1].stick.x, !trackedTeleportAiming && hands[0].stickActive && hands[1].stickActive);
    trackedGrabTurn(root, frame);
    trackedKickTick(root, owned, frame);
    trackedJumpTick(root, owned, frame);
    trackedBonesTick(root, owned, frame);
    trackedAttack = hands[1].trigger && trackedWristHighlight.load(std::memory_order_relaxed) < 0; // the trigger on a wrist line is a choice, not a swing
    // Left trigger: the game's stab modifier (its Alt word bit) while held; the attack then stabs.
    if (hands[0].trigger) { *(unsigned*)gameAt(fp::sig::keyWord) |= 0x80u; trackedStabWritten = true; }
    else trackedStabRelease();
    const auto& hand = hands[0].stickActive ? hands[0] : hands[1];
    auto analog = reinterpret_cast<float*>(gameAt(fp::sig::analogStick));
    if (!hand.stickActive || (*reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord)) & movementBits) || (!trackedStickWritten && fp::analogActive(analog))) { trackedStickRelease(); return; }
    Aim a;
    if (!getAim(bh, a) || !std::isfinite(a.yaw)) { trackedStickRelease(); return; }
    const float x = hand.stick.x, y = hand.stick.y; // stick axes in the aim's yaw frame, as the gamepad path builds them
    analog[0] = std::sin(a.yaw) * y - std::cos(a.yaw) * x;
    analog[1] = 0;
    analog[2] = std::cos(a.yaw) * y + std::sin(a.yaw) * x;
    trackedStickWritten = true;
    trackedWalkRateMilli = static_cast<int>((std::min)(std::sqrt(x * x + y * y), 1.f) * 1000);
}
static bool trackedDuckInput(uintptr_t root, float& amount) {
    __try {
        xd::MotionTarget target{};
        if (!trackedMotion(root, target)) return false;
        amount = target.duck;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; return false; }
}
struct TrackedMotorEntry {
    uintptr_t part = 0, parent = 0;
    xd::NativePose local{};
    unsigned lowerKind = 0; // 0 upper chain, 1 pelvis, 2 thigh/shin, 3 foot
    XrQuaternionf world{0, 0, 0, 1};
    XrVector3f position{};
    bool positioned = false;
};
struct TrackedWeaponStamp {
    uintptr_t weapon = 0, joints = 0, parent = 0;
    int count = 0;
    unsigned grip = 0, required = 0;
    std::array<uintptr_t, 2> hands{}, rows{};
    std::array<std::array<unsigned char, 0x68>, 2> bytes{};
};
struct TrackedMotorScope {
    uintptr_t root = 0, scene = 0;
    uint64_t tick = 0, stream = 0, epoch = 0;
    uint32_t inputGeneration = 0;
    unsigned count = 0;
    bool bodyDriven = false;
    TrackedWeaponStamp weapon{};
    std::array<TrackedMotorEntry, 16> entries{};
    float keptGaze[3]{}, writtenGaze[3]{};
    bool gazeWritten = false;
};
static thread_local std::array<TrackedMotorScope, 8> trackedMotorScopes;
static thread_local unsigned trackedMotorDepth = 0;
static bool trackedPart(uintptr_t part, uintptr_t root) {
    return readable(part, 0x600) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root;
}
static XrPosef trackedPartPose(uintptr_t part) {
    xd::NativePose pose{};
    memcpy(pose.data(), reinterpret_cast<void*>(part + field::bodyPart::pose), 48);
    const auto node = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!readable(node, field::sceneNode::bodyPart + 8) || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::bodyPart) != part) throw std::runtime_error("motor node ownership");
    const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
    if (!readable(sector, field::sector::origin + 12)) throw std::runtime_error("motor sector");
    for (unsigned i = 0; i < 3; ++i) pose[9 + i] += (reinterpret_cast<float*>(sector + field::sector::origin))[i];
    return xd::nativePoseMetres(pose, stereoUnitsPerMetre);
}
#include "xr_body_driver.inl"
#include "xr_weapon.inl"
static bool trackedHaftHeld() { return trackedEquippedDrive.haft(); }
#include "xr_physical_contact.inl"
static void trackedLowerPlan(uintptr_t root, const TrackedDriveFrame& frame, TrackedMotorScope& scope);
static bool trackedMotorPlan(uintptr_t root, TrackedMotorScope& scope) {
    TrackedDriveFrame frame{};
    auto refuse = [] { trackedEquippedDrive.reset(); return false; };
    if (!trackedDriveRead(root, frame) || !readable(root, 0x1300) || *reinterpret_cast<int*>(gameAt(fp::sig::uiOpenCount))) return refuse();
    const bool physical = frame.bodyDriven && trackedBodyOwns(root);
    if (frame.bodyDriven && (!physical || *(unsigned char*)(root + field::character::defenseActive))) return refuse();
    const bool equipped = !trackedNoHeldItems(root);
    xd::EquippedTarget equippedTarget{};
    TrackedWeaponStamp weaponStamp{};
    if (!equipped) {
        trackedEquippedDrive.reset();
        if (*(unsigned char*)(root + field::character::combatStance) && !physical) return false;
    }
    const auto head = *reinterpret_cast<uintptr_t*>(root + field::character::headPart), chest = *reinterpret_cast<uintptr_t*>(root + field::character::chestPart);
    if (!bodyCalm(root, head) || !trackedPart(chest, root) || !trackedPart(head, root) || *reinterpret_cast<uintptr_t*>(head + field::bodyPart::parentPart) != chest) return refuse();
    if ((*reinterpret_cast<unsigned*>(chest + field::bodyPart::flags) | *reinterpret_cast<unsigned*>(head + field::bodyPart::flags)) & 0x40) return refuse();
    const auto lower = *reinterpret_cast<uintptr_t*>(chest + field::bodyPart::parentPart);
    if (!trackedPart(lower, root)) return refuse();
    const auto chestPose = trackedPartPose(chest), headPose = trackedPartPose(head), lowerPose = trackedPartPose(lower);
    if (equipped && !trackedWeaponPlan(root, frame, weaponStamp, equippedTarget)) return false;
    const auto trackedHead = xd::nativePoseMetres(frame.poses[xd::head], stereoUnitsPerMetre);
    struct Calibration {
        uintptr_t root = 0, scene = 0;
        uint64_t epoch = 0;
        uint32_t inputGeneration = 0;
        bool physical = false;
        XrQuaternionf headOffset{}, chestOffset{};
        std::array<XrQuaternionf, 2> hands{};
        std::array<bool, 2> handBound{};
    };
    static thread_local Calibration calibration;
    if (calibration.root != root || calibration.scene != frame.scene || calibration.epoch != frame.epoch || calibration.inputGeneration != frame.inputGeneration || calibration.physical != physical) {
        calibration = {};
        calibration.root = root;
        calibration.scene = frame.scene;
        calibration.epoch = frame.epoch;
        calibration.inputGeneration = frame.inputGeneration;
        calibration.physical = physical;
        calibration.headOffset = xd::multiply(xd::conjugate(trackedHead.orientation), headPose.orientation);
        calibration.chestOffset = xd::multiply(xd::conjugate(trackedHead.orientation), chestPose.orientation);
    }
    const auto headDesired = xd::multiply(trackedHead.orientation, calibration.headOffset);
    auto chestDesired = xd::multiply(trackedHead.orientation, calibration.chestOffset);
    XrVector3f bodyTranslation{};
    XrQuaternionf lowerWorld = lowerPose.orientation, pelvisDesired{0, 0, 0, 1};
    if (physical) {
        const auto pelvis = *reinterpret_cast<uintptr_t*>(lower + field::bodyPart::parentPart);
        if (!trackedPart(pelvis, root)) return false;
        const auto achieved = trackedPartPose(pelvis), wanted = xd::nativePoseMetres(frame.poses[xd::waist], stereoUnitsPerMetre);
        const auto delta = xd::limitMotorRotation(xd::multiply(wanted.orientation, xd::conjugate(achieved.orientation)), .35f);
        const auto lean = xd::directionRotation(xd::rotate(delta, xd::sub(headPose.position, achieved.position)), xd::sub(trackedHead.position, wanted.position));
        pelvisDesired = xd::multiply(delta, achieved.orientation);
        lowerWorld = xd::multiply(delta, lowerPose.orientation);
        chestDesired = xd::multiply(lean, xd::multiply(delta, chestPose.orientation));
        const auto chestPosition = xd::add(xd::boundedDriverPosition(achieved.position, wanted.position), xd::rotate(delta, xd::sub(chestPose.position, achieved.position)));
        bodyTranslation = xd::sub(chestPosition, chestPose.position);
    }
    // Limit chest bend; the native motor keeps its stiffness, balance and collision rules.
    const auto chestDelta = xd::limitMotorRotation(xd::multiply(chestDesired, xd::conjugate(chestPose.orientation)), .8f);
    chestDesired = xd::multiply(chestDelta, chestPose.orientation);
    scope.root = root;
    scope.scene = frame.scene;
    scope.tick = frame.tick;
    scope.stream = frame.stream;
    scope.epoch = frame.epoch;
    scope.inputGeneration = frame.inputGeneration;
    scope.weapon = weaponStamp;
    scope.bodyDriven = physical;
    auto add = [&](uintptr_t part, uintptr_t parent, XrQuaternionf parentWorld, XrQuaternionf childWorld) {
        if (scope.count >= scope.entries.size()) throw std::runtime_error("motor scope capacity");
        scope.entries[scope.count++] = {part, parent, xd::motorLocal(parentWorld, childWorld)};
    };
    if (physical) add(lower, *reinterpret_cast<uintptr_t*>(lower + field::bodyPart::parentPart), pelvisDesired, lowerWorld);
    add(chest, lower, lowerWorld, chestDesired);
    add(head, chest, chestDesired, headDesired);
    for (unsigned side = 0; side < 2; ++side) {
        const bool heldArm = equipped && (weaponStamp.required & (1u << side));
        if (equipped && !heldArm && !physical) continue;
        if (!frame.valid[xd::leftHand + side]) { calibration.handBound[side] = false; continue; }
        // The native bind graph puts positive-X (left) in slot 1; XR uses left/right order.
        const auto hand = *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8);
        if (!trackedPart(hand, root)) continue;
        const auto fore = *reinterpret_cast<uintptr_t*>(hand + field::bodyPart::parentPart);
        if (!trackedPart(fore, root)) continue;
        const auto upper = *reinterpret_cast<uintptr_t*>(fore + field::bodyPart::parentPart);
        if (!trackedPart(upper, root)) continue;
        const auto shoulder = *reinterpret_cast<uintptr_t*>(upper + field::bodyPart::parentPart);
        if (!trackedPart(shoulder, root) || *reinterpret_cast<uintptr_t*>(shoulder + field::bodyPart::parentPart) != chest ||
            hand == fore || fore == upper || upper == shoulder || shoulder == chest) continue;
        if ((*reinterpret_cast<unsigned*>(upper + field::bodyPart::flags) | *reinterpret_cast<unsigned*>(fore + field::bodyPart::flags) | *reinterpret_cast<unsigned*>(hand + field::bodyPart::flags)) & (heldArm ? 0x48 : 0x40)) {
            if (heldArm) return false;
            continue;
        }
        const auto hp = trackedPartPose(hand), fp = trackedPartPose(fore), up = trackedPartPose(upper), sp = trackedPartPose(shoulder);
        const auto controller = heldArm ? equippedTarget.hands[side] : xd::nativePoseMetres(frame.poses[xd::leftHand + side], stereoUnitsPerMetre);
        if (!calibration.handBound[side]) {
            calibration.hands[side] = xd::multiply(xd::conjugate(controller.orientation), hp.orientation);
            calibration.handBound[side] = true;
        }
        const auto shoulderPosition = xd::add(xd::add(chestPose.position, bodyTranslation), xd::rotate(chestDelta, xd::sub(up.position, chestPose.position)));
        const auto arm = xd::solveArm(shoulderPosition, controller.position, xd::rotate(chestDesired, {side ? .6f : -.6f, -.8f, .1f}), xd::length(xd::sub(fp.position, up.position)), xd::length(xd::sub(hp.position, fp.position)));
        if (!arm.valid || (heldArm && arm.clamped)) {
            if (heldArm) return false;
            continue;
        }
        const auto uq = xd::multiply(xd::directionRotation(xd::sub(fp.position, up.position), xd::sub(arm.elbow, arm.shoulder)), up.orientation);
        const auto fq = xd::multiply(xd::directionRotation(xd::sub(hp.position, fp.position), xd::sub(arm.wrist, arm.elbow)), fp.orientation);
        const auto sq = xd::multiply(chestDelta, sp.orientation), hq = heldArm ? controller.orientation : xd::multiply(controller.orientation, calibration.hands[side]);
        add(upper, shoulder, sq, uq);
        add(fore, upper, uq, fq);
        add(hand, fore, fq, hq);
    }
    if (equipped && scope.count != (physical ? 9u : 2u + 3u * ((weaponStamp.required == 3) ? 2u : 1u))) return false;
    if (!equipped || physical) trackedLowerPlan(root, frame, scope);
    if (physical && scope.count != 16u) return false;
    memcpy(scope.keptGaze, reinterpret_cast<void*>(root + field::character::gaze), 12);
    for (unsigned i = 0; i < 3; ++i) scope.writtenGaze[i] = -frame.poses[xd::head][6 + i];
    memcpy(reinterpret_cast<void*>(root + field::character::gaze), scope.writtenGaze, 12);
    scope.gazeWritten = true;
    return true;
}
// Lower targets are calibrated to the native pelvis; native root translation and contacts stay authoritative.
static void trackedLowerPlan(uintptr_t root, const TrackedDriveFrame& frame, TrackedMotorScope& scope) {
    const bool physical = frame.bodyDriven;
    struct LowerCalibration {
        uintptr_t root = 0, scene = 0;
        uint64_t epoch = 0, stream = 0, tick = 0;
        uint32_t generation = 0;
        XrQuaternionf offset{0, 0, 0, 1};
        std::array<XrVector3f, 2> feet{};
        std::array<XrQuaternionf, 2> rotation{};
        bool bound = false, feetBound = false, physical = false;
    };
    static thread_local LowerCalibration calibration;
    auto refuse = [&] { calibration.bound = false; };
    if (!frame.valid[xd::waist] || !frame.valid[xd::leftFoot] || !frame.valid[xd::rightFoot]) return refuse();
    const int count = *reinterpret_cast<int*>(root + field::character::partCount), legs = *reinterpret_cast<int*>(root + field::character::legCount);
    const auto parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts), legArray = *reinterpret_cast<uintptr_t*>(root + field::character::legs);
    if (count < 8 || count > 64 || legs != 2 || !readable(parts, static_cast<size_t>(count) * 8) || !readable(legArray, 16)) return refuse();
    const auto pelvis = *reinterpret_cast<uintptr_t*>(parts);
    if (!trackedPart(pelvis, root) || *reinterpret_cast<uintptr_t*>(pelvis + field::bodyPart::rootPart) != pelvis || *reinterpret_cast<uintptr_t*>(pelvis + field::bodyPart::parentPart) != pelvis || !(*reinterpret_cast<unsigned*>(pelvis + field::bodyPart::flags) & 0x40)) return refuse();
    const auto pelvisPose = trackedPartPose(pelvis), sensor = xd::nativePoseMetres(frame.poses[xd::waist], stereoUnitsPerMetre);
    const bool rebind = !calibration.bound || frame.tick - calibration.tick > 100 || calibration.root != root || calibration.scene != frame.scene || calibration.epoch != frame.epoch || calibration.stream != frame.stream || calibration.generation != frame.inputGeneration || calibration.physical != physical;
    const bool rebindFeet = rebind || !calibration.feetBound;
    LowerCalibration next = calibration;
    if (rebind) {
        next = {};
        next.root = root;
        next.scene = frame.scene;
        next.epoch = frame.epoch;
        next.stream = frame.stream;
        next.generation = frame.inputGeneration;
        next.physical = physical;
        next.offset = xd::multiply(xd::conjugate(sensor.orientation), pelvisPose.orientation);
    }
    const auto pelvisDesired = xd::multiply(xd::limitMotorRotation(xd::multiply((physical ? sensor.orientation : xd::multiply(sensor.orientation, next.offset)), xd::conjugate(pelvisPose.orientation)), .35f), pelvisPose.orientation);
    const auto pelvisPosition = physical ? xd::boundedDriverPosition(pelvisPose.position, sensor.position) : pelvisPose.position;
    xd::MotionTarget motion{};
    const bool locomotion = trackedMotion(root, motion);
    const bool walking = *(unsigned char*)(root + field::character::walking) != 0;
    if (walking && !locomotion && !physical) return refuse();
    std::array<TrackedMotorEntry, 7> entries{};
    unsigned n = 0, seen = 0;
    entries[n++] = {pelvis, root, {}, 1, pelvisDesired, pelvisPosition, physical};
    const bool nativeLegs = !physical && (walking || (locomotion && motion.duck > .01f));
    unsigned supports = 0;
    for (unsigned leg = 0; leg < 2; ++leg) {
        const auto shin = *reinterpret_cast<uintptr_t*>(legArray + leg * 8);
        if (!trackedPart(shin, root) || *reinterpret_cast<int*>(shin + field::bodyPart::role) != 0x10) return refuse();
        if (nativeLegs) {
            const int contacts = *reinterpret_cast<int*>(shin + field::bodyPart::contactCount);
            const auto contact = *reinterpret_cast<uintptr_t*>(shin + field::bodyPart::contacts);
            if (contacts < 1 || contacts > 8 || !readable(contact, static_cast<size_t>(contacts) * 0x98)) return refuse();
            const auto flags = *reinterpret_cast<unsigned*>(contact + field::contact::flags);
            if ((flags & 0x12) == 2) ++supports;
            else if ((flags & 0x16) != 4) return refuse();
        }
        const auto thigh = *reinterpret_cast<uintptr_t*>(shin + field::bodyPart::parentPart);
        if (!trackedPart(thigh, root) || *reinterpret_cast<uintptr_t*>(thigh + field::bodyPart::parentPart) != pelvis) return refuse();
        uintptr_t foot = 0;
        for (int i = 1; i < count; ++i) {
            const auto part = *reinterpret_cast<uintptr_t*>(parts + i * 8);
            if (trackedPart(part, root) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::parentPart) == shin && *reinterpret_cast<int*>(part + field::bodyPart::role) == 0x13) {
                if (foot) return refuse();
                foot = part;
            }
        }
        if (!foot || ((*reinterpret_cast<unsigned*>(thigh + field::bodyPart::flags) | *reinterpret_cast<unsigned*>(shin + field::bodyPart::flags) | *reinterpret_cast<unsigned*>(foot + field::bodyPart::flags)) & 0x40)) return refuse();
        const auto tp = trackedPartPose(thigh), sp = trackedPartPose(shin), fp = trackedPartPose(foot);
        const auto hipLocal = xd::rotate(xd::conjugate(pelvisPose.orientation), xd::sub(tp.position, pelvisPose.position));
        if (std::abs(hipLocal.x) < .02f) return refuse();
        const unsigned side = hipLocal.x > 0 ? 0 : 1; // positive native X is left; XR left/right order
        if (seen & (1u << side)) return refuse();
        seen |= 1u << side;
        if (nativeLegs) continue;
        const auto tracker = xd::nativePoseMetres(frame.poses[xd::leftFoot + side], stereoUnitsPerMetre);
        const auto relative = xd::sub(tracker.position, sensor.position);
        if (xd::length(relative) > 1.8f || xd::length(relative) < .15f) return refuse();
        if (rebindFeet) {
            next.feet[side] = xd::sub(xd::sub(fp.position, pelvisPose.position), relative);
            next.rotation[side] = xd::multiply(xd::conjugate(tracker.orientation), fp.orientation);
        }
        const auto target = physical ? xd::boundedDriverPosition(fp.position, tracker.position) : xd::add(pelvisPose.position, xd::add(relative, next.feet[side]));
        if (xd::length(xd::sub(target, fp.position)) > .35f) return refuse();
        const auto hip = xd::add(pelvisPosition, xd::rotate(pelvisDesired, hipLocal));
        const auto legPose = xd::solveArm(hip, target, xd::rotate(pelvisDesired, {0, 0, -1}), xd::length(xd::sub(sp.position, tp.position)), xd::length(xd::sub(fp.position, sp.position)));
        if (!legPose.valid) return refuse();
        const auto tq = xd::multiply(xd::directionRotation(xd::sub(sp.position, tp.position), xd::sub(legPose.elbow, hip)), tp.orientation);
        const auto sq = xd::multiply(xd::directionRotation(xd::sub(fp.position, sp.position), xd::sub(legPose.wrist, legPose.elbow)), sp.orientation);
        const auto fq = xd::multiply(xd::limitMotorRotation(xd::multiply(xd::multiply(tracker.orientation, next.rotation[side]), xd::conjugate(fp.orientation)), .6f), fp.orientation);
        entries[n++] = {thigh, pelvis, {}, 2, tq};
        entries[n++] = {shin, thigh, {}, 2, sq};
        entries[n++] = {foot, shin, {}, 3, fq, target, physical};
    }
    if (nativeLegs) {
        if (!supports) return refuse();
        n = 1; // native gait/posture exclusively owns walking/crouching leg/foot inputs
    }
    if (seen != 3 || scope.count + n > scope.entries.size()) return refuse();
    next.feetBound = !nativeLegs;
    next.bound = true;
    next.tick = frame.tick;
    calibration = next;
    for (unsigned i = 0; i < n; ++i) scope.entries[scope.count++] = entries[i];
}
static bool trackedMotorBeginImpl(uintptr_t root) {
    ++trackedMotorDepth;
    if (trackedMotorDepth > trackedMotorScopes.size()) return false;
    auto& scope = trackedMotorScopes[trackedMotorDepth - 1];
    scope = {};
    try {
        const bool planned = trackedMotorPlan(root, scope);
        if (planned) trackedBodyMotorPublish(scope);
        else {
            trackedBodyMotorClear(root);
            if (scope.weapon.weapon) trackedEquippedDrive.reset();
            scope = {};
        }
        if (root == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot))) trackedMotorRoot = planned ? root : 0;
        return planned;
    } catch (const std::exception&) {
        trackedBodyMotorClear(root);
        trackedEquippedDrive.reset();
        scope = {};
        if (trackedMotorRoot == root) trackedMotorRoot = 0;
        return false;
    }
}
static bool trackedMotorBegin(uintptr_t root) {
    __try { return trackedMotorBeginImpl(root); }
    __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; return false; }
}
static bool trackedMotorOwns(uintptr_t root) {
    TrackedDriveFrame frame{};
    return root && trackedMotorRoot == root && trackedDriveRead(root, frame);
}
static bool trackedMotorGaze(uintptr_t root, float* gaze) {
    if (!trackedMotorDepth || trackedMotorDepth > trackedMotorScopes.size()) return false;
    const auto& scope = trackedMotorScopes[trackedMotorDepth - 1];
    if (scope.root != root || !scope.gazeWritten) return false;
    memcpy(gaze, scope.writtenGaze, 12);
    return true;
}
static void trackedMotorFault() {
    if (trackedMotorDepth && trackedMotorDepth <= trackedMotorScopes.size() && trackedMotorScopes[trackedMotorDepth - 1].root) {
        trackedDriveFault = true;
        trackedDriveClear();
    }
}
static void trackedMotorEnd() {
    if (!trackedMotorDepth) return;
    const unsigned depth = --trackedMotorDepth;
    if (depth >= trackedMotorScopes.size()) return;
    auto& scope = trackedMotorScopes[depth];
    if (scope.gazeWritten && readable(scope.root, 0x1300) && *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) == scope.root &&
        *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) == scope.scene && !memcmp(reinterpret_cast<void*>(scope.root + field::character::gaze), scope.writtenGaze, 12))
        memcpy(reinterpret_cast<void*>(scope.root + field::character::gaze), scope.keptGaze, 12);
    scope = {};
}
static bool trackedScopeCurrent(const TrackedMotorScope& scope) {
    return scope.root && scope.stream == trackedDriveGeneration.load() && scope.inputGeneration == inputOwner.generation() &&
           scope.root == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) && scope.scene == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) &&
           GetTickCount64() - scope.tick <= 100 && (!scope.bodyDriven || trackedBodyOwns(scope.root)) && trackedWeaponCurrent(scope.root, scope.weapon);
}
using TrackedCompose = void (*)(float*, float*, const float*);
static TrackedCompose originalTrackedCompose = nullptr;
// The native affine composition inside the pose builder is the only place a motor input can be
// replaced: the scope planned for this exact call site, output and parent, nothing else.
static void trackedComposeFor(uintptr_t caller, float* out, float* local, const float* parent) {
    const bool live = trackedMotorDepth && trackedMotorDepth <= trackedMotorScopes.size() && trackedDriveRequested && !trackedDriveFault && active && !stopping;
    if (live) {
        const auto& scope = trackedMotorScopes[trackedMotorDepth - 1];
        if (trackedScopeCurrent(scope)) {
            for (unsigned i = 0; i < scope.count; ++i) {
                const auto& entry = scope.entries[i];
                if (!entry.lowerKind || !trackedPart(entry.part, scope.root) || (entry.lowerKind != 1 && !trackedPart(entry.parent, scope.root))) continue;
                const bool pelvis = entry.lowerKind == 1, foot = entry.lowerKind == 3;
                const auto site = pelvis ? fp::sig::xrPelvisComposeReturn : (foot ? fp::sig::xrFootComposeReturn : fp::sig::xrMotorComposeReturn);
                const auto output = entry.part + (pelvis || foot ? field::bodyPart::targetPose : field::bodyPart::animationPose);
                const auto parentAddress = pelvis ? scope.root + field::character::balanceFrame : entry.parent + (foot ? field::bodyPart::targetPose : field::bodyPart::animationPose);
                if (caller != gameAt(site) || reinterpret_cast<uintptr_t>(out) != output || reinterpret_cast<uintptr_t>(parent) != parentAddress || *reinterpret_cast<uintptr_t*>(entry.part + field::bodyPart::parentPart) != (pelvis ? entry.part : entry.parent)) continue;
                ULONG_PTR low = 0, high = 0;
                GetCurrentThreadStackLimits(&low, &high);
                if (pelvis ? reinterpret_cast<uintptr_t>(local) != entry.part + field::bodyPart::animationPose : (reinterpret_cast<uintptr_t>(local) < low || reinterpret_cast<uintptr_t>(local) > high - 48)) continue;
                if (!readable(reinterpret_cast<uintptr_t>(local), 48) || !readable(parentAddress, 48)) break;
                xd::NativePose scratch{};
                try {
                    xd::NativePose frame{}, p{};
                    memcpy(frame.data(), reinterpret_cast<void*>(scope.root + field::character::bodyFrame), 48);
                    memcpy(p.data(), parent, 48);
                    memcpy(scratch.data(), local, 48);
                    const auto desired = xd::multiply(xd::conjugate(xd::nativePoseMetres(frame, stereoUnitsPerMetre).orientation), entry.world);
                    const auto parentPose = xd::nativePoseMetres(p, stereoUnitsPerMetre);
                    const auto axes = xd::motorLocal(parentPose.orientation, desired);
                    memcpy(scratch.data(), axes.data(), 36);
                    if (entry.positioned) {
                        const auto node = *reinterpret_cast<uintptr_t*>(scope.root + field::character::sceneNode);
                        if (!readable(node, field::sceneNode::sector + 8)) break;
                        const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
                        if (!readable(sector, field::sector::origin + 12)) break;
                        XrVector3f origin{};
                        memcpy(&origin, reinterpret_cast<void*>(sector + field::sector::origin), 12);
                        auto worldFrame = xd::nativePoseMetres(frame, stereoUnitsPerMetre);
                        worldFrame.position = xd::add(worldFrame.position, xd::scale(origin, 1 / stereoUnitsPerMetre));
                        const auto localPoint = xd::compose(xd::inverse(parentPose), xd::compose(xd::inverse(worldFrame), XrPosef{entry.world, entry.position})).position;
                        scratch[9] = localPoint.x * stereoUnitsPerMetre;
                        scratch[10] = localPoint.y * stereoUnitsPerMetre;
                        scratch[11] = localPoint.z * stereoUnitsPerMetre;
                    }
                } catch (const std::exception&) { break; }
                originalTrackedCompose(out, scratch.data(), parent);
                // Legs and feet feed the native joint-anchor builder from this stack scratch too.
                if (!pelvis) memcpy(local, scratch.data(), entry.positioned ? 48 : 36);
                ++trackedMotorTargets;
                return;
            }
        }
    }
    if (live && caller == gameAt(fp::sig::xrMotorComposeReturn)) {
        auto& scope = trackedMotorScopes[trackedMotorDepth - 1];
        if (trackedScopeCurrent(scope)) {
            ULONG_PTR low = 0, high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            if (reinterpret_cast<uintptr_t>(local) >= low && reinterpret_cast<uintptr_t>(local) <= high - 48 && readable(reinterpret_cast<uintptr_t>(local), 48))
                for (unsigned i = 0; i < scope.count; ++i) {
                    const auto& entry = scope.entries[i];
                    if (!entry.lowerKind && reinterpret_cast<uintptr_t>(out) == entry.part + field::bodyPart::animationPose && reinterpret_cast<uintptr_t>(parent) == entry.parent + field::bodyPart::animationPose && trackedPart(entry.part, scope.root) &&
                        *reinterpret_cast<uintptr_t*>(entry.part + field::bodyPart::parentPart) == entry.parent) {
                        memcpy(local, entry.local.data(), 36);
                        ++trackedMotorTargets;
                        break;
                    }
                }
        }
    }
    originalTrackedCompose(out, local, parent);
}
static void trackedCompose(float* out, float* local, const float* parent) { trackedComposeFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), out, local, parent); }

// Controller-only pickup: a press runs the native grab initialiser with the controller's ray
// and borrowed render-view admission; native eligibility, traversal and release stay native.
struct TrackedGrabScope {
    uintptr_t viewport = 0, root = 0, scene = 0;
    xd::NativePose controller{};
    unsigned side = 0;
    bool seen = false, invalid = false;
    float hit[3]{};
    bool hitValid = false;
};
static thread_local TrackedGrabScope* trackedGrabScope = nullptr;
using TrackedScreenRay = void (*)(uintptr_t, float, float, float*);
using TrackedGrabStart = void (*)(uintptr_t, uintptr_t*, uintptr_t, unsigned*);
using TrackedGrabKind = unsigned (*)(uintptr_t);
using TrackedGrabRoot = uintptr_t (*)(uintptr_t);
static TrackedScreenRay originalTrackedScreenRay = nullptr;
static TrackedGrabStart trackedGrabStart = nullptr;
static TrackedGrabKind trackedGrabKind = nullptr;
static TrackedGrabRoot trackedGrabRoot = nullptr;
static bool trackedPickScope() { return trackedGrabScope != nullptr; }
static void trackedScreenRayFor(uintptr_t caller, uintptr_t viewport, float x, float y, float* output) {
    const auto* scope = trackedGrabScope;
    if (scope && caller == gameAt(fp::sig::xrGrabRayReturn) && viewport == scope->viewport &&
        scope->root == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) && scope->scene == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene))) {
        const auto camera = *reinterpret_cast<uintptr_t*>(viewport + field::viewport::pickCamera);
        if (readable(camera, field::camera::pose + 48)) {
            const auto* pose = reinterpret_cast<float*>(camera + field::camera::pose);
            float delta[3], direction[3];
            for (unsigned i = 0; i < 3; ++i) { delta[i] = scope->controller[9 + i] - pose[9 + i]; direction[i] = -scope->controller[6 + i]; }
            fp::worldToLocalDirection(pose, delta, output);
            fp::worldToLocalDirection(pose, direction, output + 3);
            return;
        }
        trackedGrabScope->invalid = true;
        originalTrackedScreenRay(viewport, x, y, output);
        return;
    }
    originalTrackedScreenRay(viewport, x, y, output);
}
static void trackedScreenRay(uintptr_t viewport, float x, float y, float* output) { trackedScreenRayFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), viewport, x, y, output); }
static char trackedPickAccept(uintptr_t viewport, char hit, uintptr_t out) {
    auto* scope = trackedGrabScope;
    if (!scope || scope->invalid || viewport != scope->viewport || !hit || !readable(out, field::pick::requiredSize)) return 0;
    scope->seen = true;
    if (!active || stopping || !stereoOwner(savedView) || scope->scene != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene))) return 0;
    const float distance = *reinterpret_cast<float*>(out + field::pick::distance);
    if (!(distance >= 1 && distance <= 30) || !std::isfinite(distance)) return 0;
    if (!trackedGrabRoot) return 0;
    const auto object = trackedGrabRoot(*reinterpret_cast<uintptr_t*>(out));
    if (!readable(object, 0x208) || *reinterpret_cast<uintptr_t*>(object + field::sceneNode::parent) ||
        (*reinterpret_cast<unsigned*>(object + field::sceneNode::flags) & (0x80000 | 0x800000)) || (*reinterpret_cast<unsigned*>(object + field::sceneNode::interactionFlags) & 0x8000) || !trackedGrabKind || trackedGrabKind(object) != 1) return 0;
    const auto body = *reinterpret_cast<uintptr_t*>(object + field::sceneNode::bodyPart);
    if (!readable(body, field::bodyPart::meshNode + 8) || *reinterpret_cast<uintptr_t*>(body + field::bodyPart::meshNode) != object ||
        !(*reinterpret_cast<unsigned*>(body + field::bodyPart::flags) & 0x10000000)) return 0;
    const auto* origin = reinterpret_cast<const float*>(out + field::pick::origin);
    const auto* direction = reinterpret_cast<const float*>(out + field::pick::direction);
    for (unsigned i = 0; i < 3; ++i) scope->hit[i] = origin[i] + direction[i] * distance;
    scope->hitValid = true;
    return 1;
}
// Only cached view admission is borrowed; native visibility, eligibility, self exclusion,
// closest-hit traversal and the initialiser's own filter stay authoritative.
static void trackedPickVisibility(fp::xr::Transaction& undo, uintptr_t viewport) {
    const auto scene = *reinterpret_cast<uintptr_t*>(viewport + field::viewport::sceneManager), view = *reinterpret_cast<uintptr_t*>(viewport + field::viewport::view);
    if (scene != savedScene || view != savedView || !readable(view, field::view::pickPassMask + 4) || !readable(scene + 0x3C4, 12)) throw std::runtime_error("controller scene owner");
    const unsigned pass = *reinterpret_cast<unsigned*>(view + field::view::pickPassMask);
    if (!pass) throw std::runtime_error("controller pick pass");
    const int last = *reinterpret_cast<int*>(scene + 0x3C4);
    const auto sectors = *reinterpret_cast<uintptr_t*>(scene + 0x3C8);
    if (last < 0 || last >= 4096 || !readable(sectors, static_cast<size_t>(last + 1) * 8)) throw std::runtime_error("controller sectors");
    unsigned total = 0;
    for (int i = 0; i <= last; ++i) {
        const auto sector = (reinterpret_cast<uintptr_t*>(sectors))[i];
        if (!readable(sector, field::sector::meshes + 8)) throw std::runtime_error("controller sector");
        const int meshLast = *reinterpret_cast<int*>(sector + field::sector::lastMesh);
        const auto meshes = *reinterpret_cast<uintptr_t*>(sector + field::sector::meshes);
        if (meshLast < -1 || meshLast >= 65536 || total + static_cast<unsigned>(meshLast + 1) > 65536 || (meshLast >= 0 && !readable(meshes, static_cast<size_t>(meshLast + 1) * 8))) throw std::runtime_error("controller mesh bounds");
        total += static_cast<unsigned>(meshLast + 1);
        undo.capture(sector + field::sector::passMask, 4);
        *reinterpret_cast<unsigned*>(sector + field::sector::passMask) |= pass;
        for (int m = 0; m <= meshLast; ++m) {
            const auto mesh = (reinterpret_cast<uintptr_t*>(meshes))[m];
            if (!readable(mesh, field::sceneNode::passMask + 4)) throw std::runtime_error("controller mesh");
            undo.capture(mesh + field::sceneNode::passMask, 4);
            *reinterpret_cast<unsigned*>(mesh + field::sceneNode::passMask) |= pass;
        }
    }
}
static bool trackedGrabCall(uintptr_t drag, uintptr_t* pair, uintptr_t message, unsigned* kind, TrackedGrabScope* scope) {
    auto* previous = trackedGrabScope;
    bool result = false;
    const auto hover = *(unsigned char*)(drag + field::drag::invalidDrop);
    __try {
        // When idle, +14D is the desktop hover result; the controller's own native pick
        // must decide eligibility for this attempt.
        *(unsigned char*)(drag + field::drag::invalidDrop) = 0;
        trackedGrabScope = scope;
        __try { trackedGrabStart(drag, pair, message, kind); result = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; }
    } __finally {
        trackedGrabScope = previous;
        if (scope->root == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) && scope->scene == *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) &&
            readable(drag, 0x152) && !*reinterpret_cast<uintptr_t*>(drag + field::drag::object) && !*(unsigned char*)(drag + field::drag::invalidDrop))
            *(unsigned char*)(drag + field::drag::invalidDrop) = hover;
    }
    return result;
}
static void trackedAcquirePickup(const xd::TrackingFrame& frame, const TrackedGrabScope& scope, uintptr_t object) {
    auto sample = frame.sample;
    for (unsigned sideIndex = 0; sideIndex < 2; ++sideIndex)
        if (sample.hands[sideIndex].tracked) {
            auto& hand = sample.hands[sideIndex];
            xd::NativePose achieved{};
            const int physical = trackedBodyHand(sideIndex, achieved.data());
            if (physical == 1) hand.tracked = false;
            else hand.grip = xd::nativePoseMetres(physical == 2 ? achieved : xd::nativeEye(frame.anchor, frame.calibration.relative(hand.grip), static_cast<float>(vrUnitsPerMetre.load())));
        }
    const xd::InteractionOwner owner{savedScene, savedRoot, holdGeneration.load()};
    trackedHold = {};
    trackedHold.object = object;
    trackedHold.generation = owner.generation;
    trackedHold.epoch = sample.epoch;
    trackedHold.serial = frame.serial;
    // The pickup latch already observed a physical released sample in this epoch.
    auto released = sample;
    released.hands[scope.side].pressed = false;
    trackedHold.hold.step(released, owner);
    xd::NativePose native{};
    memcpy(native.data(), reinterpret_cast<void*>(object + field::sceneNode::transform), 48);
    if (!sceneNodeWorldOrigin(object, native.data() + 9) || !scope.hitValid) return;
    trackedHold.hold.bind(sample, owner, scope.side, object, xd::nativePoseMetres(native, stereoUnitsPerMetre), {scope.hit[0] / stereoUnitsPerMetre, scope.hit[1] / stereoUnitsPerMetre, scope.hit[2] / stereoUnitsPerMetre});
}
static void trackedPickupTick() {
    static uint64_t epoch = 0, serial = UINT64_MAX;
    static uintptr_t root = 0, scene = 0;
    static uint32_t inputGeneration = 0;
    static bool armed[2]{};
    if (!trackedDriveRequested || trackedDriveFault || !stereoRenderContext() || !stereoOwner(savedView) || !gameWindow()) { armed[0] = armed[1] = false; return; }
    xd::TrackingFrame frame{};
    if (!xd::trackingFrame(frame) || !frame.sample.focused) { armed[0] = armed[1] = false; return; }
    if (root != savedRoot || scene != savedScene || epoch != frame.sample.epoch || inputGeneration != inputOwner.generation()) {
        inputGeneration = inputOwner.generation();
        root = savedRoot;
        scene = savedScene;
        epoch = frame.sample.epoch;
        serial = UINT64_MAX;
        armed[0] = armed[1] = false;
    }
    if (serial == frame.serial) return;
    serial = frame.serial;
    trackedPouchPoll();
    for (unsigned side = 0; side < 2; ++side) {
        const auto& hand = frame.sample.hands[side];
        if (!hand.tracked) { armed[side] = false; trackedPouchGrab[side] = false; continue; }
        if (!hand.pressed) { armed[side] = true; trackedPouchGrab[side] = false; continue; }
        const bool pressed = armed[side] || trackedPouchGrab[side]; // a take from the pouch grabs while the hand is still closed
        const bool pouchGrab = trackedPouchGrab[side];
        trackedPouchGrab[side] = false;
        armed[side] = false;
        if (!pressed) continue;
        if (!pouchGrab && trackedPouchReady() && trackedPouchAtHip(frame, hand.grip.position) && root && readable(root, field::character::sceneNode + 8)) {
            // an empty hand closing at the hip: the newest item put away, dropped just ahead of the palm for that hand to grab
            const uintptr_t actor = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
            const uintptr_t sector = readable(actor, field::sceneNode::sector + 8) ? *reinterpret_cast<uintptr_t*>(actor + field::sceneNode::sector) : 0;
            if (readable(sector, field::sector::origin + 12)) {
                const float* origin = reinterpret_cast<const float*>(sector + field::sector::origin);
                const auto native = xd::nativeEye(frame.anchor, frame.calibration.relative(xd::compose(hand.grip, {{0, 0, 0, 1}, {0, 0, -.08f}})), static_cast<float>(vrUnitsPerMetre.load()));
                const float position[3] = {native[9] - origin[0], native[10] - origin[1], native[11] - origin[2]};
                if (std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) && trackedPouchTake(side, position)) continue;
            }
        }
        const auto drag = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::dragManager)), viewport = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::mainViewport)), gui = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiManager));
        if (!trackedGrabStart || !trackedItemRelease || !trackedGrabKind || *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::xrGrabOverride)) || !readable(drag, 0x152) ||
            *reinterpret_cast<unsigned*>(drag + field::drag::flags) || *reinterpret_cast<uintptr_t*>(drag + field::drag::object) || *reinterpret_cast<uintptr_t*>(drag + field::drag::viewport) != viewport ||
            !readable(gui, field::gui::capture + 8) || (*reinterpret_cast<unsigned*>(gui + field::gui::buttons) & 7) ||
            (*reinterpret_cast<uintptr_t*>(gui + field::gui::capture) && *reinterpret_cast<uintptr_t*>(gui + field::gui::capture) != viewport) ||
            !inputEnabled || !closeViewContext() || (*(unsigned char*)(root + field::character::combatStance) && !trackedBodyOwns(root)) ||
            *reinterpret_cast<int*>(gameAt(fp::sig::pointerOverride)) > 0 || (nativeBinding(field::bindings::interact / field::bindings::stride) && (GetAsyncKeyState(nativeBinding(field::bindings::interact / field::bindings::stride)) & 0x8000))) continue;
        try {
            TrackedGrabScope scope{viewport, root, scene, xd::nativeEye(frame.anchor, frame.calibration.relative(hand.grip), static_cast<float>(vrUnitsPerMetre.load())), side};
            xd::NativePose achieved{};
            const int physical = trackedBodyHand(side, achieved.data());
            if (physical == 1) continue;
            if (physical == 2) scope.controller = achieved;
            fp::xr::Transaction masks(readable);
            trackedPickVisibility(masks, viewport);
            alignas(8) unsigned char message[0x28]{};
            uintptr_t pair[2]{};
            unsigned kind = 0;
            const int width = *reinterpret_cast<int*>(viewport + field::viewport::width), height = *reinterpret_cast<int*>(viewport + field::viewport::height);
            if (width < 2 || height < 2) continue;
            *reinterpret_cast<int*>(message + field::gui::pressX) = width / 2;
            *reinterpret_cast<int*>(message + field::gui::pressX + 4) = height / 2;
            const bool called = trackedGrabCall(drag, pair, reinterpret_cast<uintptr_t>(message), &kind, &scope);
            if (*reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)) != scene || *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) != root) { masks.abandon(); continue; }
            masks.restore();
            if (!called) continue;
            const auto object = *reinterpret_cast<uintptr_t*>(drag + field::drag::object);
            if (object && scope.seen && pair[0] == reinterpret_cast<uintptr_t>(trackedItemRelease) && pair[1] == drag && *reinterpret_cast<unsigned*>(drag + field::drag::flags) == 0x2000) {
                trackedAcquirePickup(frame, scope, object);
                trackedHapticRequest(scope.side, .5f, 40);
                ++trackedPickups;
                break;
            }
        } catch (const std::exception&) { trackedDriveFault = true; }
    }
}
static bool trackedDriveHooks[4]{};
static bool createTrackedDriveHooks() {
    wchar_t enabled[4]{};
    trackedDriveRequested = GetEnvironmentVariableW(L"EXCAMERA_XR_DRIVE", enabled, 4) == 1 && enabled[0] == L'1';
    const DWORD bodyKey = GetEnvironmentVariableW(L"EXCAMERA_XR_BODY_DRIVER", enabled, 4);
    trackedBodyRequested = bodyKey == 1 && enabled[0] == L'1';
    trackedBodyAuto = bodyKey == 0; // unset: on while trackers are live
    trackedWeaponsRequested = GetEnvironmentVariableW(L"EXCAMERA_XR_WEAPONS", enabled, 4) == 1 && enabled[0] == L'1';
    const DWORD walkKey = GetEnvironmentVariableW(L"EXCAMERA_XR_LOCOMOTION", enabled, 4);
    trackedLocomotionRequested = walkKey == 1 && enabled[0] == L'1';
    trackedLocomotionAuto = walkKey == 0;
    trackedLowerSeen = 0;
    trackedDriveFault = false;
    trackedDriveClear();
    // The contact collector and commit hooks serve the blade behaviours (blade.inl) in every session and the VR
    // physical contacts when the body driver runs; both bodies gate themselves, so the hooks are installed whenever
    // their targets resolve.
    physicalContactsReady = false;
    if (layout.optionalGroupAvailable("xrPhysicalContacts")) {
        physicalImpact = reinterpret_cast<void (*)(uintptr_t)>(gameAt(fp::sig::xrPhysicalImpact));
        if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrPhysicalContact)), reinterpret_cast<void*>(trackedPhysicalContact), reinterpret_cast<void**>(&originalPhysicalContact)) != MH_OK) return false;
        trackedDriveHooks[2] = true;
        if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrPhysicalCommit)), reinterpret_cast<void*>(trackedPhysicalCommit), reinterpret_cast<void**>(&originalPhysicalCommit)) != MH_OK) return false;
        trackedDriveHooks[3] = true;
        physicalContactsReady = true;
        bladeBind(originalPhysicalContact, physicalImpact);
    }
    if (!trackedDriveRequested) return true;
    if (!layout.optionalGroupAvailable("xrDrive")) { trackedDriveRequested = false; return true; }
    trackedGrabStart = reinterpret_cast<TrackedGrabStart>(gameAt(fp::sig::xrGrabStart));
    trackedGrabKind = reinterpret_cast<TrackedGrabKind>(gameAt(fp::sig::xrGrabKind));
    trackedGrabRoot = reinterpret_cast<TrackedGrabRoot>(gameAt(fp::sig::aimMeshRoot));
    if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrCompose)), reinterpret_cast<void*>(trackedCompose), reinterpret_cast<void**>(&originalTrackedCompose)) != MH_OK) return false;
    trackedDriveHooks[0] = true;
    if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::aimScreenRay)), reinterpret_cast<void*>(trackedScreenRay), reinterpret_cast<void**>(&originalTrackedScreenRay)) != MH_OK) return false;
    trackedDriveHooks[1] = true;
    return true;
}
static MH_STATUS disableTrackedDriveHooks() {
    physicalContactsReady = false;
    trackedBodyRequested = false;
    trackedWeaponsRequested = false;
    trackedLocomotionRequested = false;
    trackedDriveRequested = false;
    trackedDriveClear();
    MH_STATUS result = MH_OK;
    const fp::sig::Target targets[4] = {fp::sig::xrCompose, fp::sig::aimScreenRay, fp::sig::xrPhysicalContact, fp::sig::xrPhysicalCommit};
    for (unsigned i = 0; i < 4; ++i)
        if (trackedDriveHooks[i]) {
            const auto status = nativeDisableHook(reinterpret_cast<void*>(gameAt(targets[i])));
            awaitNativeHookRecovery();
            if (status != MH_OK && status != MH_ERROR_DISABLED) result = status;
        }
    return result;
}
