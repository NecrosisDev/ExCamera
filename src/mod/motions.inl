// The kick and the jump, every build. The kick: the game's own motion player started on motion 13, "Kick" in the
// game's motion-set table (Resource.rpk 0x19582826), which its own inputs never start; the game handles it while it
// plays (16B06F: state 0x8000; 1612C9: no leg-support check), behind the gates in fp::motionGate
// (docs/GAME-FIELDS-AUDIT.md). The jump plays no clip: every part of the body gets an upward push through the game's
// own rigid-body push (89240, a Verlet velocity change per physics step) up to one takeoff speed from the game's
// gravity, for a peak under the game's in-air rule (fp::jumpPushStep), so the game's airborne and landing handling
// takes over. Triggered by a key (unbound until the player picks one) on a
// flat screen and by the controllers in VR (xr_drive.inl). Game thread only.
static std::atomic<unsigned> motionKicks{0}, motionJumps{0};
static std::atomic<bool> motionKickFault{false}, motionJumpFault{false};
static uint64_t motionKickLast = 0, motionJumpLast = 0;
// Motions 13 "Kick" and 18 "Jump" (the motion-set table). The kick's blend is motionPlay's third argument, kept at
// +A24 (0x17A05E) in clip-clock units, not milliseconds: progress is (+A20 - +A24) / clip length (0x1761D7) and the
// clock gains +DC8 x 0.6 a step (0x16CB27), +DC8 being 2 x +DCC for the kick (0x16B0A6). 200 held the clip about
// 2 s before it began; 20 is the game's own blend for its recover motions (0x162E58, 0x16A7F4). The jump plays no
// clip (its clip held the motion for about 2 s and fought the push) but stays a busy motion should anything else
// start it.
static constexpr int kickMotion = 0xD, kickBlend = 20, jumpMotion = 0x12, attackFamilyFirst = 5, attackFamilyLast = 12;
static constexpr uint64_t kickRepeatMs = 300, jumpRepeatMs = 900;
static fp::JumpPush jumpPush; // game thread: the push in progress (fp::jumpPushStep)
static std::atomic<unsigned> jumpPushApplied{0}, jumpPushRiseMm{0}; // status: pushes added, last peak rise (mm)
static std::atomic<unsigned> motionRefusedState{0}; // status: character+DF4 at the last "state" refusal
// Status: the last jump's start (travelling +12EA, supportScale +410 in thousandths, its peak target in mm) and how
// many jumps lost +12EA while still in the air.
static std::atomic<unsigned> jumpLastTravelling{0}, jumpLastScaleMilli{0}, jumpLastPeakMm{0}, jumpTravelLost{0};
static bool jumpStartedTravelling = false, jumpTravelLostCounted = false;
// jump.csv (beside the balance logs): one row per input poll of a jump in flight, written by the worker.
struct JumpRow { ULONGLONG at; unsigned step; float y, rise, speed, predicted, push, scale; bool travelling, footDown, pushing; };
static JumpRow jumpRows[512];
static unsigned jumpRowHead = 0, jumpRowCount = 0;
static SRWLOCK jumpRowLock = SRWLOCK_INIT;
// The shared gates (fp::motionGate): the player root, the pointer captured, the game's base state, alive, not already
// in an attack, a kick or a jump. The refusal, or MotionRefusal::count when the clip may play.
// jump: the jump plays no motion (a push only), so walking or running (DF4 1, written each step for motions 0 "Walk"
// and 1 "Run" at 0x16B470, the branch at 0x16B354) does not refuse it: the game starts its own attacks then too
// (attackDispatch 0xAE9D0 -> 0x157700, no DF4 test). Everything else in the game's on-demand rule still refuses it.
static fp::MotionRefusal motionPlayable(uintptr_t root, bool jump = false) {
    if (!root || !readable(root, 0x1300) || root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)))
        return fp::MotionRefusal::notPlayer;
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    const unsigned state = *reinterpret_cast<unsigned*>(root + field::character::stateFlags);
    const auto why = fp::motionGate(uiOpen() || cursorFreeNow(),
                                    motion == kickMotion || motion == jumpMotion || (motion >= attackFamilyFirst && motion <= attackFamilyLast),
                                    jump ? state & ~1u : state, *reinterpret_cast<float*>(root + field::character::balance),
                                    *reinterpret_cast<float*>(root + field::character::balanceMax));
    if (why == fp::MotionRefusal::otherState) motionRefusedState.store(state, std::memory_order_relaxed);
    return why;
}
static bool motionKickCall(uintptr_t root) { // SEH-only frame
    __try {
        reinterpret_cast<void (*)(uintptr_t, int, int, int)>(gameAt(fp::sig::motionPlay))(root, kickMotion, kickBlend, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionKickFault = true; return false; }
}
// A velocity change of `speed` cm/s upward on every part of the body through the game's own rigid-body push: its
// vector moves the current tetra points (+F0) and the Verlet velocity is the difference to the step before, so a
// change of `speed` is a push of speed / stepHz cm. SEH-only frame.
static bool motionJumpPush(uintptr_t root, float stepHz, float speed) {
    __try {
        const int count = *reinterpret_cast<int*>(root + field::character::partCount);
        const uintptr_t parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts);
        if (count < 8 || count > 64 || !readable(parts, size_t(count) * 8)) return false;
        const float push[3] = {0, speed / stepHz, 0}, weights[4] = {1, 1, 1, 1};
        const auto impulse = reinterpret_cast<void (*)(uintptr_t, const float*, const float*, bool)>(gameAt(fp::sig::rigidImpulse));
        for (int i = 0; i < count; ++i) {
            const uintptr_t part = *reinterpret_cast<uintptr_t*>(parts + size_t(i) * 8);
            if (part && readable(part, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root)
                impulse(part, push, weights, true);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionJumpFault = true; return false; }
}
// A kick. why: the refusal when it returns false.
static bool motionKick(uintptr_t root, fp::MotionRefusal* why = nullptr) {
    auto refuse = [&](fp::MotionRefusal r) { if (why) *why = r; return false; };
    if (motionKickFault.load()) return refuse(fp::MotionRefusal::fault);
    if (!layout.optionalGroupAvailable("kick")) return refuse(fp::MotionRefusal::unavailable);
    const uint64_t now = GetTickCount64();
    if (now - motionKickLast < kickRepeatMs) return refuse(fp::MotionRefusal::tooSoon);
    if (const auto r = motionPlayable(root); r != fp::MotionRefusal::count) return refuse(r);
    motionKickLast = now;
    if (!motionKickCall(root)) return refuse(fp::MotionRefusal::fault);
    motionKicks.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static bool motionJump(uintptr_t root, fp::MotionRefusal* why = nullptr) {
    auto refuse = [&](fp::MotionRefusal r) { if (why) *why = r; return false; };
    if (motionJumpFault.load()) return refuse(fp::MotionRefusal::fault);
    if (!layout.optionalGroupAvailable("jump") || !layout.optionalGroupAvailable("kick"))
        return refuse(fp::MotionRefusal::unavailable); // the motion player sits in the kick group
    const uint64_t now = GetTickCount64();
    if (now - motionJumpLast < jumpRepeatMs) return refuse(fp::MotionRefusal::tooSoon);
    if (const auto r = motionPlayable(root, true); r != fp::MotionRefusal::count) return refuse(r);
    motionJumpLast = now;
    // Travelling, the game's own in-air margin is 30 x +410 (0x16F7E9): the hop is sized to it; standing, to 20 cm.
    jumpStartedTravelling = *reinterpret_cast<const uint8_t*>(root + field::character::travelling) != 0;
    jumpTravelLostCounted = false;
    const float scale = *reinterpret_cast<const float*>(root + field::character::supportScale);
    const float peak = jumpStartedTravelling ? fp::jumpRunPeak(scale) : fp::jumpPeakCm;
    jumpLastTravelling = jumpStartedTravelling ? 1 : 0;
    jumpLastScaleMilli = std::isfinite(scale) && scale > 0 ? unsigned(scale * 1000.f) : 0;
    jumpLastPeakMm = unsigned(peak * 10.f);
    jumpPush = fp::JumpPush::start(root, *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)), playerSteps.load(),
                                   reinterpret_cast<const float*>(root + field::character::position)[1], peak);
    if (!motionJumpPush(root, bladeStepHz(), jumpPush.target)) {
        jumpPush.active = jumpPush.tracking = false;
        return refuse(fp::MotionRefusal::fault);
    }
    --jumpPush.left;
    jumpPushApplied.fetch_add(1, std::memory_order_relaxed);
    motionJumps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Every input poll on the game thread: the jump's push for the physics steps since the last one.
static void motionJumpTick(uintptr_t root, bool owned) {
    if (!jumpPush.tracking) return;
    const bool readableRoot = root && readable(root, 0x1300);
    const float stepHz = bladeStepHz();
    const float speed = fp::jumpPushStep(
        jumpPush, owned && readableRoot, root, *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)), playerSteps.load(),
        readableRoot ? reinterpret_cast<const float*>(root + field::character::position)[1] : NAN,
        readableRoot ? *reinterpret_cast<unsigned*>(root + field::character::stateFlags) : 0,
        readableRoot && fp::deadState(*reinterpret_cast<unsigned*>(root + field::character::stateFlags)), stepHz);
    jumpPushRiseMm.store(unsigned(std::fmax(0.f, jumpPush.peak) * 10.f), std::memory_order_relaxed);
    jumpPushActive.store(jumpPush.active, std::memory_order_relaxed);
    if (jumpStartedTravelling && !jumpTravelLostCounted && readableRoot && jumpPush.tracking &&
        !*reinterpret_cast<const uint8_t*>(root + field::character::travelling)) {
        jumpTravelLostCounted = true; // +12EA went while still in the air: the 20 cm standing margin is back
        jumpTravelLost.fetch_add(1, std::memory_order_relaxed);
    }
    if (readableRoot && (jumpPush.tracking || speed > 0.f)) {
        JumpRow row{GetTickCount64(), playerSteps.load(), reinterpret_cast<const float*>(root + field::character::position)[1],
                    jumpPush.risen, jumpPush.speed, jumpPush.predicted, speed,
                    *reinterpret_cast<const float*>(root + field::character::supportScale),
                    *reinterpret_cast<const uint8_t*>(root + field::character::travelling) != 0,
                    *reinterpret_cast<const float*>(root + field::character::footDown) > .5f, jumpPush.active};
        AcquireSRWLockExclusive(&jumpRowLock);
        if (jumpRowCount < 512) jumpRows[(jumpRowHead + jumpRowCount++) % 512] = row;
        ReleaseSRWLockExclusive(&jumpRowLock);
    }
    if (speed > 0.f && motionJumpPush(root, stepHz, speed)) jumpPushApplied.fetch_add(1, std::memory_order_relaxed);
    else if (speed > 0.f) jumpPush.active = jumpPush.tracking = false;
}
// The keys: their presses are claimed by the window handler (input.inl) and taken here on the game thread. A
// refused press is counted and, where the player can do something about it, told (motionRefused).
static void motionKeysTick(uintptr_t root) {
    fp::MotionRefusal why = fp::MotionRefusal::count;
    const bool kick = motionKickRequested.exchange(false) && kickKey.load(std::memory_order_relaxed);
    if ((padKickRequested.exchange(false) || kick) && !motionKick(root, &why)) motionRefused(false, why);
    const bool jump = motionJumpRequested.exchange(false) && jumpKey.load(std::memory_order_relaxed);
    if ((padJumpRequested.exchange(false) || jump) && !motionJump(root, &why)) motionRefused(true, why);
}

// The kick's standing leg, a little stiffer (docs/GAME-BALANCE-PHYSICS.md). During motion 13 the game anchors both
// legs (0x1612C9); once the kicking foot lifts, the lean rule drops uprightness and with it standingFactor +1020,
// which weakens the world pull and the hip spring, and the standing leg folds. The joint motors are not scaled by
// +1020: 0x15DA80 multiplies each part's motor gain by its motorStrength +594 (0x15DE27). So while the player's
// motion is 13 the support leg's thigh, shin and foot get their own +594 times KickStanceStiffness ([Input], 1.2 by
// default, ours; 0 or 1 = stock); the support leg is the one whose foot stays lower once the feet are 3 cm apart. The
// exact values read are put back when the kick ends, at a knockdown or death, at F7, and only where the game has not
// written its own value since (else the game's stands).
struct KickLegHold {
    uintptr_t root = 0, part[3]{};
    float saved[3]{}, written[3]{};
    unsigned count = 0;
};
static KickLegHold kickStanceHeld;
// The kick window for the stance and the upright assist: motion 13, and the Rebalance (33) the game plays straight
// after it (0x16B5F9), for at most kickStanceAfterMs.
static constexpr int rebalanceMotion = 0x21;
static constexpr ULONGLONG kickStanceAfterMs = 3000;
static ULONGLONG kickStanceKickSeen = 0;
// [Balance] KickUprightAssist (0 off, the default; 1 on): inside the kick window, after the game's update, uprightness
// +12B0 is set to 1.0 (never its counter +12B4) while the balance shadow's capture point is inside the loaded feet or
// within kickAssistSlackCm of them; otherwise the game's own value stands. Off until Hunter says go.
static constexpr float kickAssistSlackCm = 5.f;
static int kickUprightAssist = -1;
static std::atomic<unsigned> kickUprightAssistSteps{0}, kickUprightAssistFallbacks{0};
static SRWLOCK kickStanceLock = SRWLOCK_INIT;
static float kickStanceFactor = -1.f;
static std::atomic<unsigned> kickStanceApplied{0}, kickStanceKeptGame{0};
static void kickStanceRestoreLocked() {
    for (unsigned i = 0; i < kickStanceHeld.count; ++i) {
        const uintptr_t p = kickStanceHeld.part[i];
        __try {
            if (p && *reinterpret_cast<float*>(p + field::bodyPart::motorStrength) == kickStanceHeld.written[i])
                *reinterpret_cast<float*>(p + field::bodyPart::motorStrength) = kickStanceHeld.saved[i];
            else if (p) kickStanceKeptGame.fetch_add(1, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    kickStanceHeld = {};
}
static void kickStanceRestore() {
    AcquireSRWLockExclusive(&kickStanceLock);
    kickStanceRestoreLocked();
    ReleaseSRWLockExclusive(&kickStanceLock);
}
static float kickStanceContactY(uintptr_t shin) {
    if (*reinterpret_cast<const int*>(shin + field::bodyPart::contactCount) <= 0) return NAN;
    const uintptr_t contact = *reinterpret_cast<const uintptr_t*>(shin + field::bodyPart::contacts);
    return contact ? reinterpret_cast<const float*>(contact + field::contact::position)[1] : NAN;
}
static void kickStanceApplyLocked(uintptr_t root) {
    const uintptr_t legs = *reinterpret_cast<const uintptr_t*>(root + field::character::legs);
    const uintptr_t shin[2] = {*reinterpret_cast<const uintptr_t*>(legs), *reinterpret_cast<const uintptr_t*>(legs + 8)};
    const float y0 = kickStanceContactY(shin[0]), y1 = kickStanceContactY(shin[1]);
    if (!std::isfinite(y0) || !std::isfinite(y1) || std::abs(y0 - y1) < 3.f) return; // the kicking foot not up yet
    const uintptr_t support = y0 < y1 ? shin[0] : shin[1];
    uintptr_t foot = 0;
    const int count = *reinterpret_cast<const int*>(root + field::character::partCount);
    const uintptr_t parts = *reinterpret_cast<const uintptr_t*>(root + field::character::parts);
    for (int i = 1; i < count && i < 64; ++i) {
        const uintptr_t part = *reinterpret_cast<const uintptr_t*>(parts + size_t(i) * 8);
        if (part && *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::parentPart) == support &&
            *reinterpret_cast<const int*>(part + field::bodyPart::role) == 0x13) foot = part;
    }
    const uintptr_t chain[3] = {*reinterpret_cast<const uintptr_t*>(support + field::bodyPart::parentPart), support, foot};
    KickLegHold held{};
    held.root = root;
    for (const uintptr_t p : chain) {
        if (!p) continue;
        const float now = *reinterpret_cast<const float*>(p + field::bodyPart::motorStrength);
        if (!std::isfinite(now) || now <= 0.f) continue;
        held.part[held.count] = p;
        held.saved[held.count] = now;
        held.written[held.count] = now * kickStanceFactor;
        ++held.count;
    }
    for (unsigned i = 0; i < held.count; ++i)
        *reinterpret_cast<float*>(held.part[i] + field::bodyPart::motorStrength) = held.written[i];
    kickStanceHeld = held;
    if (held.count) kickStanceApplied.fetch_add(1, std::memory_order_relaxed);
}
static void kickStanceStep(uintptr_t root) {
    if (kickStanceFactor < 0) {
        wchar_t text[32]{};
        if (settingsPath[0]) GetPrivateProfileStringW(L"Input", L"KickStanceStiffness", L"1.2", text, 32, settingsPath);
        const float f = text[0] ? float(_wtof(text)) : 1.2f;
        kickStanceFactor = std::isfinite(f) && f > 1.f && f <= 3.f ? f : 0.f;
    }
    // Only the player's own update decides: combatUpdate runs for every character, and another one's update must
    // not put the player's leg back (it did, about 40 times a kick, so the leg was mostly stock).
    if (root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot))) return;
    AcquireSRWLockExclusive(&kickStanceLock);
    __try {
        const unsigned state = *reinterpret_cast<const unsigned*>(root + field::character::stateFlags);
        const int motion = *reinterpret_cast<const int*>(root + field::character::motion);
        const ULONGLONG now = GetTickCount64();
        if (motion == kickMotion) kickStanceKickSeen = now;
        const bool window = !fp::downState(state) &&
                            (motion == kickMotion || (motion == rebalanceMotion && kickStanceKickSeen &&
                                                      now - kickStanceKickSeen < kickStanceAfterMs));
        if (!window) kickStanceKickSeen = motion == rebalanceMotion ? kickStanceKickSeen : 0;
        const bool kicking = kickStanceFactor > 0 && window;
        if (kickUprightAssist < 0)
            kickUprightAssist = settingsPath[0] ? int(GetPrivateProfileIntW(L"Balance", L"KickUprightAssist", 0, settingsPath)) : 0;
        if (kickUprightAssist == 1 && window) {
            // The shadow's last measure (balanceShadowLatest: one step old; false when off, faulted or stale).
            ShadowBalance b{};
            if (balanceShadowLatest(root, b) && std::isfinite(b.margin) && b.margin >= -kickAssistSlackCm) {
                *reinterpret_cast<float*>(root + field::character::uprightness) = 1.f;
                kickUprightAssistSteps.fetch_add(1, std::memory_order_relaxed);
            } else
                kickUprightAssistFallbacks.fetch_add(1, std::memory_order_relaxed); // the game's own value stands
        }
        if (!kicking || (kickStanceHeld.count && kickStanceHeld.root != root)) kickStanceRestoreLocked();
        if (kicking && !kickStanceHeld.count && motion == kickMotion &&
            *reinterpret_cast<const int*>(root + field::character::legCount) == 2)
            kickStanceApplyLocked(root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kickStanceHeld = {};
        kickUprightAssist = 0; // a fault turns the assist off for the session
    }
    ReleaseSRWLockExclusive(&kickStanceLock);
}
// Worker: the jump rows so far, to jump.csv in the balance shadow's folder.
static void jumpLogPulse() {
    JumpRow rows[512];
    unsigned count = 0;
    AcquireSRWLockExclusive(&jumpRowLock);
    while (jumpRowCount && count < 512) {
        rows[count++] = jumpRows[jumpRowHead];
        jumpRowHead = (jumpRowHead + 1) % 512, --jumpRowCount;
    }
    ReleaseSRWLockExclusive(&jumpRowLock);
    if (!count || !balanceShadowEnabled.load()) return;
    static bool header = false;
    if (!header) {
        header = true;
        shadowAppend(L"jump.csv", "ms,step,root_y,rise_cm,speed_cm_s,predicted_peak_cm,push_cm_s,support_scale,travelling,foot_down,pushing\r\n");
    }
    static char text[512 * 120];
    int n = 0;
    for (unsigned i = 0; i < count && n >= 0 && size_t(n) < sizeof text; ++i) {
        const JumpRow& r = rows[i];
        n += snprintf(text + n, sizeof text - size_t(n), "%llu,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%d,%d,%d\r\n",
                      (unsigned long long)(r.at - shadowStarted), r.step, double(r.y), double(r.rise), double(r.speed),
                      double(r.predicted), double(r.push), double(r.scale), r.travelling ? 1 : 0, r.footDown ? 1 : 0,
                      r.pushing ? 1 : 0);
    }
    if (n > 0) {
        text[(std::min)(size_t(n), sizeof text - 1)] = 0;
        shadowAppend(L"jump.csv", text);
    }
}
