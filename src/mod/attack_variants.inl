// Optional clean-entry adapter for native 157240/157700 family starts only.
// The native classifier still chooses variant, motion and RNG. Its new F5C
// request is withdrawn at a proven normal tail, without native defense cleanup.
// Thread/scene serialization and the bounded native closure establish lifetime
// here; readable memory and input generation alone are not lifetime locks.
using AttackFamily = void (*)(uintptr_t, unsigned char);
static AttackFamily originalAttackFamilyA, originalAttackFamilyB;
static std::atomic<bool> attackVariantsReady{false};
static std::atomic<unsigned> attackVariantAdmissions{0}, attackVariantWithdrawals{0},
    attackVariantOwnerFaults{0}, attackVariantHandoffs{0};
static std::atomic<int> attackVariantsAvailability{0};
static bool attackVariantsHooksCreated = false;

struct AttackVariantIdentity {
    uintptr_t root = 0, head = 0, behaviour = 0, node = 0, sector = 0, scene = 0,
              context = 0, thread = 0;
    DWORD threadId = 0;
    bool same(const AttackVariantIdentity& other) const {
        return root == other.root && head == other.head && behaviour == other.behaviour &&
               node == other.node && sector == other.sector && scene == other.scene &&
               context == other.context && thread == other.thread && threadId == other.threadId;
    }
};
struct AttackVariantScope {
    uintptr_t root;
    AttackVariantScope* previous;
    AttackVariantIdentity owner{};
    fp::InputStamp stamp{};
    uintptr_t grip = 0;
    bool admitted = false;
};
static thread_local AttackVariantScope* attackVariantScope = nullptr;

// Require the real physics worker while its published scene is in the callback
// loop. Merely running on a remembered thread or observing a live bit is not
// enough. No actor pointer is touched until it is still the controlled root.
static bool attackVariantIdentity(uintptr_t root, AttackVariantIdentity& out) {
    if (!base || !root || root != *(uintptr_t*)gameAt(fp::sig::playerRoot)) return false;
    AttackVariantIdentity next;
    next.root = root;
    next.thread = *(uintptr_t*)gameAt(fp::sig::nativePhysicsThread);
    next.context = *(uintptr_t*)gameAt(fp::sig::nativePhysicsContext);
    next.scene = *(uintptr_t*)gameAt(fp::sig::nativePhysicsScene);
    if (!next.thread || !next.context || !next.scene ||
        next.scene != *(uintptr_t*)gameAt(fp::sig::currentScene) ||
        !readable(next.thread, field::nativeThread::id + sizeof(DWORD)) ||
        !readable(next.context, field::physicsContext::running + 1) ||
        !readable(next.scene, field::scene::physicsContext + sizeof(uintptr_t)))
        return false;
    next.threadId = *(DWORD*)(next.thread + field::nativeThread::id);
    if (!next.threadId || next.threadId != GetCurrentThreadId() ||
        *(uintptr_t*)(next.context + field::physicsContext::scene) != next.scene ||
        !*(unsigned char*)(next.context + field::physicsContext::running) ||
        *(uintptr_t*)(next.scene + field::scene::physicsContext) != next.context)
        return false;
    const unsigned lifecycle = *(unsigned*)(next.scene + field::scene::lifecycleFlags);
    if (!(lifecycle & 0x2000000u) || (lifecycle & 0x4000000u) ||
        !readable(root, field::character::combatStance + 1) ||
        *(uintptr_t*)(root + field::character::metadata) != gameAt(fp::sig::characterMetadata) ||
        !*(unsigned char*)(root + field::character::physicsLive))
        return false;
    next.head = *(uintptr_t*)(root + field::character::headPart);
    next.behaviour = *(uintptr_t*)(root + field::character::behaviour);
    next.node = *(uintptr_t*)(root + field::character::sceneNode);
    if (!next.head || !next.behaviour || !next.node ||
        !readable(next.head, field::bodyPart::character + sizeof(uintptr_t)) ||
        !readable(next.behaviour, field::behaviour::character + sizeof(uintptr_t)) ||
        !readable(next.node, field::sceneNode::sector + sizeof(uintptr_t)) ||
        *(uintptr_t*)(next.head + field::bodyPart::character) != root ||
        *(uintptr_t*)(next.behaviour + field::behaviour::character) != root)
        return false;
    next.sector = *(uintptr_t*)(next.node + field::sceneNode::sector);
    if (!next.sector || !readable(next.sector, field::sector::scene + sizeof(uintptr_t)) ||
        *(uintptr_t*)(next.sector + field::sector::scene) != next.scene)
        return false;
    out = next;
    return true;
}
static bool attackVariantGrip(uintptr_t root, uintptr_t& grip) {
    const uintptr_t table = gameAt(fp::sig::gripTable);
    const uintptr_t entry = *(uintptr_t*)(root + field::character::gripEntry);
    // Native two-handed entries 2/3 have callbacks. The initial bounded adapter
    // deliberately excludes them, even when a callback could appear harmless.
    if (entry != table && entry != table + field::grip::stride &&
        entry != table + 4 * field::grip::stride && entry != table + 5 * field::grip::stride)
        return false;
    if (!readable(entry, field::grip::callback + sizeof(uintptr_t)) ||
        *(uintptr_t*)(entry + field::grip::callback))
        return false;
    grip = entry;
    return true;
}
static bool beginAttackVariant(AttackVariantScope& scope) {
    if (!attackVariantsReady.load() || !manualDefenseReady.load()) return false;
    for (auto* previous = scope.previous; previous; previous = previous->previous)
        if (previous->root == scope.root) return false;
    __try {
        const uint32_t generation = inputOwner.generation();
        const unsigned policy = inputOwner.policyRevision();
        const int mode = viewMode.load();
        if (!attackVariantIdentity(scope.root, scope.owner)) return false;
        const int motion = *(int*)(scope.root + field::character::motion);
        if (motion < 3 || motion > 12 ||
            *(uintptr_t*)(scope.root + field::character::actionCallback) ||
            *(unsigned char*)(scope.root + field::character::defenseActive) ||
            *(int*)(scope.root + field::character::defenseIncomingMotion) >= 0 ||
            !attackVariantGrip(scope.root, scope.grip))
            return false;
        bool known = false;
        const auto permission = defensePermission(scope.root, &known);
        if (!known || permission != DefensePermission::denied ||
            generation != inputOwner.generation() || policy != inputOwner.policyRevision() ||
            mode != viewMode.load())
            return false;
        scope.stamp = {scope.root, scope.owner.head, scope.owner.scene, mode, generation, policy};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool attackVariantClassifier(uintptr_t root) {
    // Every wrapper pushes a frame, even a rejected/NPC invocation. Thus a
    // nested same-actor call cannot borrow an outer admitted frame, and a
    // different actor cannot accidentally expose the outer classifier grant.
    return attackVariantScope && attackVariantScope->admitted && attackVariantScope->root == root;
}
static void faultAttackVariantOwner() {
    attackVariantsReady = false;
    attackVariantsAvailability = 7; // Sticky until process restart; hooks remain registered.
    ++attackVariantOwnerFaults;
}
static bool attackVariantCompletionOwner(const AttackVariantScope& scope) {
    AttackVariantIdentity current;
    uintptr_t grip = 0;
    return attackVariantIdentity(scope.root, current) && scope.owner.same(current) &&
           !*(uintptr_t*)(scope.root + field::character::actionCallback) &&
           !*(unsigned char*)(scope.root + field::character::defenseActive) &&
           attackVariantGrip(scope.root, grip) && grip == scope.grip;
}
static void finishAttackVariant(const AttackVariantScope& scope) {
    __try {
        if (!attackVariantCompletionOwner(scope)) {
            faultAttackVariantOwner();
            return;
        }
        // A settings/input handoff does not establish or destroy actor lifetime.
        // Re-established physical ownership permits a fresh policy decision.
        if (scope.stamp.generation != inputOwner.generation() ||
            scope.stamp.policyRevision != inputOwner.policyRevision() ||
            scope.stamp.mode != viewMode.load())
            ++attackVariantHandoffs;
        bool known = false;
        const auto permission = defensePermission(scope.root, &known);
        if (!known || !attackVariantCompletionOwner(scope)) {
            faultAttackVariantOwner();
            return;
        }
        if (permission == DefensePermission::denied &&
            *(int*)(scope.root + field::character::defenseIncomingMotion) >= 0) {
            // Both admitted native starters unconditionally publish F5C at
            // their tail, with no later native call. Entry was active=0/F5C<0.
            // Do not call 162E80 or touch contact, hand, constraint or pose state.
            *(int*)(scope.root + field::character::defenseIncomingMotion) = -1;
            ++attackVariantWithdrawals;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never try to repair an old or uncertain actor. This does not promise
        // rollback after invalid lifetime, a hardware fault, or another mod.
        faultAttackVariantOwner();
    }
}
static void invokeAttackFamily(uintptr_t root, unsigned char continuation, AttackFamily original) {
    AttackVariantScope scope{root, attackVariantScope};
    scope.admitted = beginAttackVariant(scope);
    if (scope.admitted) ++attackVariantAdmissions;
    attackVariantScope = &scope;
    __try {
        original(root, continuation);
        if (scope.admitted) finishAttackVariant(scope);
    } __finally {
        // Native calls in the admitted closure precede its tail publication.
        // Exceptional exit restores scope metadata only, never actor memory.
        attackVariantScope = scope.previous;
    }
}
static void attackFamilyA(uintptr_t root, unsigned char continuation) {
    invokeAttackFamily(root, continuation, originalAttackFamilyA);
}
static void attackFamilyB(uintptr_t root, unsigned char continuation) {
    invokeAttackFamily(root, continuation, originalAttackFamilyB);
}

static const char* attackVariantsReasonText() {
    switch (attackVariantsAvailability.load()) {
    case 1: return "available";
    case 2: return "signatures_unavailable";
    case 3: return "loaded_native_contract_changed";
    case 4: return "hook_creation_failed";
    case 5: return "native_contract_changed_after_creation";
    case 6: return "native_fields_changed";
    case 7: return "normal_return_owner_unknown";
    case 8: return "manual_defense_unavailable";
    case 9: return "reviewed_native_source_unverified";
    case 10: return "physics_ownership_unavailable";
    default: return "not_initialized";
    }
}
static bool optionalContractFailed(const char* group) {
    const size_t count = sizeof(fp::contract::fields) / sizeof(fp::contract::fields[0]);
    for (size_t i = 0; i < count && i < nativeContracts.why.size(); ++i) {
        const auto& field = fp::contract::fields[i];
        if (field.optionalGroup && !strcmp(field.optionalGroup, group) &&
            nativeContracts.why[i] != fp::ContractFailure::none)
            return true;
    }
    return false;
}
static void withholdAttackVariants(int reason) {
    attackVariantsReady = false;
    attackVariantsAvailability = reason;
    // Never discard an installed hook's removal identity, including F7/F6.
    if (!attackVariantsHooksCreated)
        for (unsigned target = 0; target < fp::sig::targetCount; ++target) {
            const char* group = fp::sig::targetOptionalGroups[target];
            if (group && !strcmp(group, "attackVariants")) layout.rva[target] = 0;
        }
}
#include "optional_contracts.inl"
static bool loadedAttackVariantContractsImpl(const fp::ImageView& file) {
    if (!file.data || !base || !layout.optionalGroupAvailable("attackVariants")) return false;
    const fp::sig::Target entries[] = {fp::sig::attackFamilyA, fp::sig::attackFamilyB};
    for (const auto target : entries) {
        const uint32_t rva = layout.rva[target];
        // Overlap the reviewed full-body windows (which begin after the native
        // prologues), leaving no unchecked gap behind the hook's first bytes.
        constexpr size_t entryBytes = 64;
        if (!rva || static_cast<uint64_t>(rva) + entryBytes > file.size ||
            memcmp(reinterpret_cast<const void*>(base + rva), file.data + rva, entryBytes))
            return false;
    }
    if (!loadedOptionalContractsImpl(file, "attackVariants") ||
        !loadedOptionalContractsImpl(file, "physicsOwnership")) return false;
    for (unsigned index : {0u, 1u, 4u, 5u}) {
        const uintptr_t entry = gameAt(fp::sig::gripTable) + index * field::grip::stride;
        if (!readable(entry, field::grip::callback + sizeof(uintptr_t)) ||
            *(uintptr_t*)(entry + field::grip::callback))
            return false;
    }
    return true;
}
static bool loadedAttackVariantContracts(const fp::ImageView& file) {
    // Keep vector/Pattern destruction outside the function containing SEH.
    __try {
        return loadedAttackVariantContractsImpl(file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool admitAttackVariantsEntries(const fp::ImageView& file, bool sourceVerified) {
    const int previous = attackVariantsAvailability.load();
    if (attackVariantsHooksCreated && previous == 5) return false;
    // Unknown normal-return ownership is sticky but does not waive retained
    // trampoline validation on F7/F6. A healthy re-arm preserves state 7 and
    // broad denial; any changed code upgrades it to restart-required state 5.
    int reason = 0;
    // Masked code patterns alone do not bind every rel32 call to the reviewed
    // native closure on a merely compatible build. This initial partial
    // capability additionally requires a fresh supported-executable hash.
    if (!sourceVerified)
        reason = 9;
    else if (!layout.optionalGroupAvailable("manualDefense"))
        reason = 8;
    else if (!layout.optionalGroupAvailable("physicsOwnership"))
        reason = optionalContractFailed("physicsOwnership") ? 6 : 10;
    else if (!layout.optionalGroupAvailable("attackVariants"))
        reason = optionalContractFailed("attackVariants") ? 6 : 2;
    else if (!loadedAttackVariantContracts(file))
        reason = 3;
    if (!reason) return true;
    if (attackVariantsHooksCreated) {
        withholdAttackVariants(5);
        snprintf(faultDetail, sizeof faultDetail,
                 "native attack-family contract changed after hook creation; retained trampolines "
                 "cannot be re-used; restart required");
        return false;
    }
    withholdAttackVariants(reason);
    return true;
}
static bool createAttackVariantsHooks() {
    attackVariantsReady = false;
    if (!manualDefenseReady.load()) {
        withholdAttackVariants(8);
        return true;
    }
    if (!layout.optionalGroupAvailable("physicsOwnership")) {
        withholdAttackVariants(optionalContractFailed("physicsOwnership") ? 6 : 10);
        return true;
    }
    if (!layout.optionalGroupAvailable("attackVariants")) return true;
    struct Hook {
        fp::sig::Target target;
        void* replacement;
        void** original;
    };
    const Hook hooks[] = {
        {fp::sig::attackFamilyA, (void*)attackFamilyA, (void**)&originalAttackFamilyA},
        {fp::sig::attackFamilyB, (void*)attackFamilyB, (void**)&originalAttackFamilyB}};
    unsigned created = 0;
    for (const Hook& hook : hooks) {
        if (MH_CreateHook((void*)gameAt(hook.target), hook.replacement, hook.original) != MH_OK) {
            bool removed = true;
            while (created) {
                --created;
                removed &= MH_RemoveHook((void*)gameAt(hooks[created].target)) == MH_OK;
            }
            // If rollback failed, retain addresses until the session's full
            // MinHook recovery finishes. Never turn an uncertain hook native.
            attackVariantsHooksCreated = !removed;
            withholdAttackVariants(4);
            return removed;
        }
        ++created;
    }
    attackVariantsHooksCreated = true;
    attackVariantsAvailability = 1;
    attackVariantsReady = true;
    return true;
}
