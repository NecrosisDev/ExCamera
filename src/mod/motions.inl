// The kick and the jump, every build: the game's own motion player started on its unused kick (0xD) and jump
// (0x12) clips, behind the gates the attack chooser would apply and a few of our own. The jump also gives every
// part of the body one upward push through the game's own rigid-body push (89240, a Verlet velocity change per
// physics step), so the game's airborne and landing handling takes over. Triggered by a key (unbound until the
// player picks one) on a flat screen and by the controllers in VR (xr_drive.inl). Game thread only.
static std::atomic<unsigned> motionKicks{0}, motionJumps{0};
static std::atomic<bool> motionKickFault{false}, motionJumpFault{false};
static uint64_t motionKickLast = 0, motionJumpLast = 0;
static constexpr int kickMotion = 0xD, kickBlendMs = 40, jumpMotion = 0x12, jumpBlendMs = 60, attackFamilyFirst = 5, attackFamilyLast = 12;
static constexpr uint64_t kickRepeatMs = 300, jumpRepeatMs = 900;
static constexpr float kickStaminaShare = .8f; // the chooser's own floor for a kick
static constexpr float jumpStaminaShare = .5f;
static constexpr float jumpTakeoffSpeed = 240.f; // cm/s of upward push: about a 30 cm jump
// The shared gates: the player root, in play, upright and free, alive, not in an attack, a kick or a jump.
static bool motionPlayable(uintptr_t root, float staminaShare) {
    if (!root || !readable(root, 0x1300) || root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) || uiOpen() || cursorFreeNow()) return false;
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    const unsigned state = *reinterpret_cast<unsigned*>(root + field::character::stateFlags);
    const float stamina = *reinterpret_cast<float*>(root + field::character::stamina), staminaMax = *reinterpret_cast<float*>(root + field::character::staminaMax);
    if (motion == kickMotion || motion == jumpMotion || (motion >= attackFamilyFirst && motion <= attackFamilyLast)) return false;
    if (state != 0 && state != 8) return false;                                                   // upright and free
    if (*reinterpret_cast<unsigned*>(root + field::character::lifeFlags) & 0x100) return false;  // dead
    return std::isfinite(stamina) && std::isfinite(staminaMax) && staminaMax > 0 && stamina >= staminaShare * staminaMax;
}
static bool motionKickCall(uintptr_t root) { // SEH-only frame
    __try {
        reinterpret_cast<void (*)(uintptr_t, int, int, int)>(gameAt(fp::sig::motionPlay))(root, kickMotion, kickBlendMs, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionKickFault = true; return false; }
}
static bool motionJumpCall(uintptr_t root, float stepHz) { // SEH-only frame
    __try {
        reinterpret_cast<void (*)(uintptr_t, int, int, int)>(gameAt(fp::sig::motionPlay))(root, jumpMotion, jumpBlendMs, 0);
        const int count = *reinterpret_cast<int*>(root + field::character::partCount);
        const uintptr_t parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts);
        if (count < 8 || count > 64 || !readable(parts, size_t(count) * 8)) return true;
        const float push[3] = {0, jumpTakeoffSpeed / stepHz, 0}, weights[4] = {1, 1, 1, 1};
        const auto impulse = reinterpret_cast<void (*)(uintptr_t, const float*, const float*, bool)>(gameAt(fp::sig::rigidImpulse));
        for (int i = 0; i < count; ++i) {
            const uintptr_t part = *reinterpret_cast<uintptr_t*>(parts + size_t(i) * 8);
            if (part && readable(part, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root)
                impulse(part, push, weights, true);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { motionJumpFault = true; return false; }
}
// A kick: in the fighting stance only, as the chooser kicks.
static bool motionKick(uintptr_t root) {
    if (motionKickFault.load() || !layout.optionalGroupAvailable("kick")) return false;
    const uint64_t now = GetTickCount64();
    if (now - motionKickLast < kickRepeatMs || !motionPlayable(root, kickStaminaShare) || !*(unsigned char*)(root + field::character::combatStance)) return false;
    motionKickLast = now;
    if (!motionKickCall(root)) return false;
    motionKicks.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static bool motionJump(uintptr_t root) {
    if (motionJumpFault.load() || !layout.optionalGroupAvailable("jump") || !layout.optionalGroupAvailable("kick")) return false; // the motion player sits in the kick group
    const uint64_t now = GetTickCount64();
    if (now - motionJumpLast < jumpRepeatMs || !motionPlayable(root, jumpStaminaShare)) return false;
    motionJumpLast = now;
    if (!motionJumpCall(root, bladeStepHz())) return false;
    motionJumps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// The keys: their presses are claimed by the window handler (input.inl) and taken here on the game thread.
static void motionKeysTick(uintptr_t root) {
    if (motionKickRequested.exchange(false) && kickKey.load(std::memory_order_relaxed)) motionKick(root);
    if (motionJumpRequested.exchange(false) && jumpKey.load(std::memory_order_relaxed)) motionJump(root);
}
