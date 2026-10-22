// Optional delayed held-body delivery. Runtime and queue ownership are defined
// by held_throw.inl; this file is included only after picking.inl.
static std::atomic<int> heldThrowAvailability{0};
static const char* heldThrowReasonText() {
    switch (heldThrowAvailability.load()) {
    case 1: return "available";
    case 2: return "signatures_unavailable";
    case 3: return "loaded_native_contract_changed";
    case 4: return "hook_creation_failed";
    case 5: return "native_contract_changed_after_creation";
    case 6: return "native_fields_changed";
    case 9: return "reviewed_native_source_unverified";
    case 10: return "physics_ownership_unavailable";
    default: return "not_initialized";
    }
}
static void suspendHeldThrow() {
    // The same runtime lock guards cancellation and the final velocity commit.
    // Return only once every older queued serial can no longer write a body.
    heldThrowReady = false;
    // Worker-only, outside gameplay locks. Any failed MinHook transaction may
    // have suspended a callback owning heldThrowLock. Discharge that debt before
    // cancellation, including partial creation rollback and early run() exits.
    awaitNativeHookRecovery();
    clearHeldThrow();
}
static void withholdHeldThrow(int reason) {
    suspendHeldThrow();
    heldThrowAvailability = reason;
    // Shared physics identities and other optional capabilities are not ours.
    // Retained hooks always keep their removal identities, including failures.
    if (!heldThrowHooksCreated)
        for (unsigned target = 0; target < fp::sig::targetCount; ++target) {
            const char* group = fp::sig::targetOptionalGroups[target];
            if (group && !strcmp(group, "heldThrow")) layout.rva[target] = 0;
        }
}
static bool loadedHeldThrowContracts(const fp::ImageView& file) {
    __try {
        // Full native integration/destruction bodies include their entries.
        // Both hooks are disabled for initial admission and every re-arm.
        if (!loadedOptionalContractsImpl(file, "heldThrow") ||
            !loadedOptionalContractsImpl(file, "physicsOwnership")) return false;
        // Reviewed fixed-base metadata: destructor, allocation/free and the two
        // native update slots. Exact file equality also rejects retargeted cells.
        const uint32_t metadata = layout.rva[fp::sig::heldBodyMetadata];
        for (unsigned offset : {0x60u, 0x68u, 0x70u, 0xD8u, 0xE0u}) {
            const uint64_t at = static_cast<uint64_t>(metadata) + offset;
            if (!metadata || at > file.size || sizeof(uintptr_t) > file.size - at ||
                !readable(base + static_cast<uintptr_t>(at), sizeof(uintptr_t)) ||
                memcmp(reinterpret_cast<const void*>(base + static_cast<uintptr_t>(at)), file.data + at, sizeof(uintptr_t)))
                return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool admitHeldThrowEntries(const fp::ImageView& file, bool sourceVerified) {
    if (heldThrowHooksCreated && heldThrowAvailability.load() == 5) return false;
    int reason = 0;
    if (!sourceVerified)
        reason = 9;
    else if (!layout.optionalGroupAvailable("physicsOwnership"))
        reason = optionalContractFailed("physicsOwnership") ? 6 : 10;
    else if (!layout.optionalGroupAvailable("heldThrow"))
        reason = optionalContractFailed("heldThrow") ? 6 : 2;
    else if (!loadedHeldThrowContracts(file))
        reason = 3;
    if (!reason) return true;
    if (heldThrowHooksCreated) {
        withholdHeldThrow(5);
        snprintf(faultDetail, sizeof faultDetail,
                 "native held-body contract changed after hook creation; retained trampolines "
                 "cannot be re-used; restart required");
        return false;
    }
    withholdHeldThrow(reason);
    return true;
}
static bool createHeldThrowHooks() {
    suspendHeldThrow();
    if (!layout.optionalGroupAvailable("physicsOwnership")) {
        withholdHeldThrow(optionalContractFailed("physicsOwnership") ? 6 : 10);
        return true;
    }
    if (!layout.optionalGroupAvailable("heldThrow")) return true;
    struct Hook {
        fp::sig::Target target;
        void* replacement;
        void** original;
    };
    // Establish lifetime cancellation before an integration callback can exist.
    const Hook hooks[] = {
        {fp::sig::heldBodyDestroy, (void*)heldBodyDestroy, (void**)&originalHeldBodyDestroy},
        {fp::sig::heldBodyIntegrate, (void*)heldBodyIntegrate, (void**)&originalHeldBodyIntegrate}};
    unsigned created = 0;
    for (const Hook& hook : hooks) {
        if (MH_CreateHook((void*)gameAt(hook.target), hook.replacement, hook.original) != MH_OK) {
            bool removed = true;
            while (created) {
                --created;
                removed &= MH_RemoveHook((void*)gameAt(hooks[created].target)) == MH_OK;
            }
            heldThrowHooksCreated = !removed;
            withholdHeldThrow(4);
            return removed;
        }
        ++created;
    }
    heldThrowHooksCreated = true;
    heldThrowAvailability = 1;
    // Do not publish permission until the entire session arm succeeds.
    return true;
}
static void armHeldThrow() {
    heldThrowReady = heldThrowHooksCreated && heldThrowAvailability.load() == 1;
}
