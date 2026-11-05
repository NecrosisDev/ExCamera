// The kick and the jump, every build. The kick: the game's own motion player started on its unused kick clip
// (0xD), behind the gates the attack chooser would apply and a few of our own. The jump plays no clip: every part
// of the body gets an upward push through the game's own rigid-body push (89240, a Verlet velocity change per
// physics step) for several steps (fp::jumpPushStep), so the game's airborne and landing handling takes over.
// Triggered by a key (unbound until the
// player picks one) on a flat screen and by the controllers in VR (xr_drive.inl). Game thread only.
static std::atomic<unsigned> motionKicks{0}, motionJumps{0};
static std::atomic<bool> motionKickFault{false}, motionJumpFault{false};
static uint64_t motionKickLast = 0, motionJumpLast = 0;
// The kick blends in over 200 ms (40 toppled the player) from both feet planted; the jump plays no clip (its clip
// held the motion for about 2 s and fought the push) but stays a busy motion should anything else start it.
static constexpr int kickMotion = 0xD, kickBlendMs = 200, jumpMotion = 0x12, attackFamilyFirst = 5, attackFamilyLast = 12;
static constexpr uint64_t kickRepeatMs = 300, jumpRepeatMs = 900;
static fp::JumpPush jumpPush; // game thread: the push in progress (fp::jumpPushStep)
static std::atomic<unsigned> jumpPushApplied{0}, jumpPushRiseMm{0}; // status: push steps added, last rise
// The shared gates (fp::motionGate): the player root, the pointer captured, upright, alive, not already in an attack,
// a kick or a jump. The refusal, or MotionRefusal::count when the clip may play.
static fp::MotionRefusal motionPlayable(uintptr_t root, bool planted = false) {
    if (!root || !readable(root, 0x1300) || root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)))
        return fp::MotionRefusal::notPlayer;
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    return fp::motionGate(uiOpen() || cursorFreeNow(),
                          motion == kickMotion || motion == jumpMotion || (motion >= attackFamilyFirst && motion <= attackFamilyLast),
                          *reinterpret_cast<unsigned*>(root + field::character::stateFlags),
                          (*reinterpret_cast<unsigned*>(root + field::character::lifeFlags) & 0x100) != 0, planted);
}
static bool motionKickCall(uintptr_t root) { // SEH-only frame
    __try {
        reinterpret_cast<void (*)(uintptr_t, int, int, int)>(gameAt(fp::sig::motionPlay))(root, kickMotion, kickBlendMs, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionKickFault = true; return false; }
}
// One step's worth of the jump's push, `steps` times over, on every part of the body through the game's own
// rigid-body push (a Verlet velocity change per physics step). SEH-only frame.
static bool motionJumpPush(uintptr_t root, float stepHz, int steps) {
    __try {
        const int count = *reinterpret_cast<int*>(root + field::character::partCount);
        const uintptr_t parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts);
        if (count < 8 || count > 64 || !readable(parts, size_t(count) * 8)) return false;
        const float push[3] = {0, fp::jumpStepSpeed * float(steps) / stepHz, 0}, weights[4] = {1, 1, 1, 1};
        const auto impulse = reinterpret_cast<void (*)(uintptr_t, const float*, const float*, bool)>(gameAt(fp::sig::rigidImpulse));
        for (int i = 0; i < count; ++i) {
            const uintptr_t part = *reinterpret_cast<uintptr_t*>(parts + size_t(i) * 8);
            if (part && readable(part, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root)
                impulse(part, push, weights, true);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionJumpFault = true; return false; }
}
// A kick: on both feet. why: the refusal when it returns false.
static bool motionKick(uintptr_t root, fp::MotionRefusal* why = nullptr) {
    auto refuse = [&](fp::MotionRefusal r) { if (why) *why = r; return false; };
    if (motionKickFault.load()) return refuse(fp::MotionRefusal::fault);
    if (!layout.optionalGroupAvailable("kick")) return refuse(fp::MotionRefusal::unavailable);
    const uint64_t now = GetTickCount64();
    if (now - motionKickLast < kickRepeatMs) return refuse(fp::MotionRefusal::tooSoon);
    if (const auto r = motionPlayable(root, true); r != fp::MotionRefusal::count) return refuse(r);
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
    if (const auto r = motionPlayable(root); r != fp::MotionRefusal::count) return refuse(r);
    motionJumpLast = now;
    jumpPush = fp::JumpPush::start(root, *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)), playerSteps.load(),
                                   reinterpret_cast<const float*>(root + field::character::position)[1]);
    if (!motionJumpPush(root, bladeStepHz(), 1)) { jumpPush.active = false; return refuse(fp::MotionRefusal::fault); }
    --jumpPush.left;
    jumpPushApplied.fetch_add(1, std::memory_order_relaxed);
    motionJumps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Every input poll on the game thread: the jump's push for the physics steps since the last one.
static void motionJumpTick(uintptr_t root, bool owned) {
    if (!jumpPush.active) return;
    const bool readableRoot = root && readable(root, 0x1300);
    const int steps = fp::jumpPushStep(
        jumpPush, owned && readableRoot, root, *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene)), playerSteps.load(),
        readableRoot ? reinterpret_cast<const float*>(root + field::character::position)[1] : NAN,
        readableRoot ? *reinterpret_cast<unsigned*>(root + field::character::stateFlags) : 0,
        readableRoot && (*reinterpret_cast<unsigned*>(root + field::character::lifeFlags) & 0x100) != 0);
    jumpPushRiseMm.store(unsigned(std::fmax(0.f, jumpPush.risen) * 10.f), std::memory_order_relaxed);
    if (steps > 0 && motionJumpPush(root, bladeStepHz(), steps)) jumpPushApplied.fetch_add(unsigned(steps), std::memory_order_relaxed);
    else if (steps > 0) jumpPush.active = false;
}
// The keys: their presses are claimed by the window handler (input.inl) and taken here on the game thread. A
// refused press is counted and, where the player can do something about it, told (motionRefused).
static void motionKeysTick(uintptr_t root) {
    fp::MotionRefusal why = fp::MotionRefusal::count;
    if (motionKickRequested.exchange(false) && kickKey.load(std::memory_order_relaxed) && !motionKick(root, &why))
        motionRefused(false, why);
    if (motionJumpRequested.exchange(false) && jumpKey.load(std::memory_order_relaxed) && !motionJump(root, &why))
        motionRefused(true, why);
}
