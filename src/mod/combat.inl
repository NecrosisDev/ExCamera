// Close-view combat input and optional native-defense permission. No contact,
// equipment-capability or motion-success state is fabricated here.
// Native callbacks are the only callers of defenseCleanup.
static bool ordinaryCombat(uintptr_t root, uintptr_t bh) {
    if (!root || !bh || !base || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || viewMode.load() >= 2 ||
        !InterlockedCompareExchange(&cameraReady, 0, 0))
        return false;
    if (root != *(uintptr_t*)gameAt(fp::sig::playerRoot) ||
        *(uintptr_t*)(bh + field::behaviour::character) != root ||
        *(unsigned char*)(bh + field::behaviour::inactive) ||
        (*(unsigned*)(root + field::character::lifeFlags) & 0x100u) ||
        !*(unsigned char*)(root + field::character::combatStance) ||
        *(unsigned*)(root + field::character::stateFlags) == 0x20 ||
        *(uintptr_t*)(root + field::character::actionCallback) ||
        *(unsigned char*)(bh + field::behaviour::interactionLatch) ||
        *(unsigned char*)(bh + field::behaviour::pressBlocked))
        return false;
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager),
                    manager = *(uintptr_t*)gameAt(fp::sig::guiManager);
    // Includes held placement, spring/body drag, doors and hinges, not just the
    // two drag kinds the camera itself adapts.
    if ((drag && *(unsigned*)(drag + field::drag::flags)) ||
        (manager && *(unsigned*)(manager + field::gui::dragFormat)))
        return false;
    const unsigned interact = nativeBinding(field::bindings::interact / field::bindings::stride);
    return !(interact > 0 && interact < 256 && (keyState(int(interact)) & 0x8000));
}
static bool manualCombat(uintptr_t root, uintptr_t bh) {
    return manualDefenseReady.load() &&
           (viewMode.load() == 0 ? fpManualGuard.load() : tpManualGuard.load()) && ordinaryCombat(root, bh);
}
static void publishGuard(const InputOwner::Sample& sample, bool held) {
    // Carry the sampled owner through publication. Reloading these fields here
    // could give an older physical hold a newer handoff or settings revision.
    const GuardRequest next{sample.stamp, held};
    AcquireSRWLockExclusive(&guardLock);
    guardRequest = next;
    ReleaseSRWLockExclusive(&guardLock);
}
// Physical release is authoritative: buffered buttons may retain a down/up pair,
// but a released guard tap is never queued for a later native opportunity.
static void combatInput(uintptr_t root, uintptr_t bh, bool owned, const InputOwner::Sample& ownerSample) {
    const unsigned physical =
        ((keyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) | ((keyState(VK_RBUTTON) & 0x8000) ? 2u : 0u);
    unsigned& buttons = *(unsigned*)gameAt(fp::sig::buttonWord);
    const bool ordinary = owned && ordinaryCombat(root, bh);
    const bool manual = ordinary && manualCombat(root, bh);
    const InputOwner::CombatSample sample = inputOwner.combatSample(buttons, physical, ordinary, manual);
    if (ordinary) {
        // Cursor walking is a separate native command. Analog, digital steps,
        // run, duck, attack timing and accepted travel remain native.
        *(unsigned char*)(bh + field::behaviour::moveIntent) = 0;
    }
    buttons = sample.buttons;
    publishGuard(ownerSample, sample.guard);
}
enum class DefensePermission { native, denied, held };
static DefensePermission defensePermission(uintptr_t root) {
    if (!root || !base || !manualDefenseReady.load()) return DefensePermission::native;
    __try {
        const uintptr_t bh = *(uintptr_t*)(root + field::character::behaviour);
        if (!manualCombat(root, bh)) return DefensePermission::native;
        GuardRequest request;
        AcquireSRWLockShared(&guardLock);
        request = guardRequest;
        ReleaseSRWLockShared(&guardLock);
        const fp::InputStamp& stamp = request.stamp;
        const bool held = request.held && stamp.root == root &&
                          stamp.head == *(uintptr_t*)(root + field::character::headPart) &&
                          stamp.scene == *(uintptr_t*)gameAt(fp::sig::currentScene) &&
                          stamp.mode == viewMode.load() &&
                          stamp.generation == inputOwner.generation() &&
                          stamp.policyRevision == inputOwner.policyRevision() &&
                          InterlockedCompareExchange(&inputEnabled, 0, 0) && controls(bh) &&
                          (keyState(VK_RBUTTON) & 0x8000);
        return held ? DefensePermission::held : DefensePermission::denied;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A failed ownership check must not leave a partially adapted actor.
        return DefensePermission::native;
    }
}
static void withdrawDefense(uintptr_t root) {
    // The attack-associated route can retain an incoming motion with active=0.
    // Cleanup belongs to the native actor callback; never run it from settings,
    // the window procedure or a worker, and never clear equipment flags by hand.
    if (*(unsigned char*)(root + field::character::defenseActive) ||
        *(int*)(root + field::character::defenseIncomingMotion) >= 0) {
        defenseCleanup(root);
        ++guardWithdrawals;
    }
}
static void defenseReactive(uintptr_t root) {
    if (defensePermission(root) == DefensePermission::denied) {
        withdrawDefense(root);
        ++guardDenied;
        return;
    }
    originalDefenseReactive(root);
}
static void defenseAttackUpdate(uintptr_t root) {
    if (defensePermission(root) == DefensePermission::denied) {
        const int motion = *(int*)(root + field::character::motion);
        const uintptr_t head = *(uintptr_t*)(root + field::character::headPart);
        withdrawDefense(root);
        // The normal caller already withdrew reactive defense before entering
        // this attack body. If cleanup nevertheless makes a native posture
        // transition, honor that transition instead of running the old attack.
        if (!readable(root, field::character::combatStance + 1) ||
            *(int*)(root + field::character::motion) != motion ||
            *(uintptr_t*)(root + field::character::headPart) != head)
            return;
    }
    // All offensive execution, including the grip-specific hand setup, still
    // runs. Removing its retained defensive request prevents the independent
    // interception branch without suppressing an attack or physical contact.
    originalDefenseAttackUpdate(root);
}
static uint64_t defenseMotionSelect(uintptr_t root, uintptr_t incoming) {
    if (defensePermission(root) == DefensePermission::denied) return 0xffffffffu;
    return originalDefenseMotionSelect(root, incoming);
}
// Initial optional admission is complete or withheld. Once created, the hooks
// remain registered through F7: changed entries require a restart, and their
// removal identities must survive the failed re-arm.
static std::atomic<int> manualDefenseAvailability{
    0}; // 0 pending, 1 ready, 2 signatures, 3 entry, 4 hooks, 5 retained, 6 fields
static bool manualDefenseHooksCreated = false; // Session owner; never reset while trampolines are retained.
static const char* manualDefenseReasonText() {
    switch (manualDefenseAvailability.load()) {
    case 1:
        return "available";
    case 2:
        return "signatures_unavailable";
    case 3:
        return "native_entry_changed";
    case 4:
        return "hook_creation_failed";
    case 5:
        return "native_entry_changed_after_creation";
    case 6:
        return "native_fields_changed";
    default:
        return "not_initialized";
    }
}
static constexpr fp::sig::Target manualDefenseTargets[] = {
    fp::sig::defenseReactive, fp::sig::defenseAttackUpdate, fp::sig::defenseMotionSelect,
    fp::sig::defenseCleanup};
static void withholdManualDefense(int reason) {
    manualDefenseReady = false;
    manualDefenseAvailability = reason;
    for (const auto target : manualDefenseTargets) layout.rva[target] = 0;
}
static bool nativeDefenseEntryMatches(const fp::ImageView& file, uint32_t rva) {
    if (!rva || uint64_t(rva) + 16 > file.size) return false;
    __try {
        return std::memcmp((const void*)(base + rva), file.data + rva, 16) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool admitManualDefenseEntries(const fp::ImageView& file) {
    int reason = 0;
    // A failed retained-entry check ends this installation even if another
    // writer later restores those bytes. Only a new process can admit it again.
    if (manualDefenseHooksCreated && manualDefenseAvailability.load() == 5)
        reason = 5;
    else if (!layout.optionalGroupAvailable("manualDefense"))
        reason = manualDefenseFieldMismatch ? 6 : 2;
    else {
        for (const auto target : manualDefenseTargets) {
            if (!nativeDefenseEntryMatches(file, layout.rva[target])) {
                reason = 3;
                break;
            }
        }
    }
    if (!reason) return true;
    if (manualDefenseHooksCreated) {
        manualDefenseReady = false;
        manualDefenseAvailability = 5;
        snprintf(faultDetail, sizeof faultDetail,
                 "manual defense code changed after hook creation; retained trampolines cannot be re-used; "
                 "restart required");
        return false;
    }
    withholdManualDefense(reason);
    return true;
}
static bool createManualDefenseHooks() {
    manualDefenseReady = false;
    if (!layout.optionalGroupAvailable("manualDefense")) return true;
    struct Hook {
        fp::sig::Target target;
        void* replacement;
        void** original;
    };
    const Hook hooks[] = {
        {fp::sig::defenseReactive, (void*)defenseReactive, (void**)&originalDefenseReactive},
        {fp::sig::defenseAttackUpdate, (void*)defenseAttackUpdate, (void**)&originalDefenseAttackUpdate},
        {fp::sig::defenseMotionSelect, (void*)defenseMotionSelect, (void**)&originalDefenseMotionSelect}};
    unsigned created = 0;
    for (const Hook& hook : hooks) {
        if (MH_CreateHook((void*)gameAt(hook.target), hook.replacement, hook.original) != MH_OK) {
            bool removed = true;
            while (created) {
                --created;
                removed &= MH_RemoveHook((void*)gameAt(hooks[created].target)) == MH_OK;
            }
            // No hooks have been enabled yet. A failed rollback stops startup;
            // a complete rollback allows the core session with Automatic.
            withholdManualDefense(4);
            return removed;
        }
        ++created;
    }
    defenseCleanup = (Behavior)gameAt(fp::sig::defenseCleanup);
    manualDefenseHooksCreated = true;
    manualDefenseAvailability = 1;
    manualDefenseReady = true;
    return true;
}
