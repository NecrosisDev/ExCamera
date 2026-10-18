// Optional defense hooks on synthetic executable storage in this process only.
// No game process, installed game file, window or injection is involved.
#include "first_person.cpp"
#include <cassert>

static unsigned actorCalls = 0, selectorCalls = 0, postureCalls = 0;
static uint32_t nativePosture(uintptr_t) {
    ++postureCalls;
    return 3;
}
static unsigned admissionChecks = 0, admissionFailures = 0;
static void checkAdmission(bool passed, const char* label, fp::sig::Target target) {
    ++admissionChecks;
    if (!passed) ++admissionFailures;
    printf("%s: %s (%s)\n", passed ? "PASS" : "FAIL", label, fp::sig::targetNames[target]);
}
// The old owner returned void and could only continue startup. This adapter
// allows the retained-entry regression to run against that exact owner too.
static bool admitEntries(const fp::ImageView& image) {
#ifdef EXCAMERA_PRE_REVIEW
    admitManualDefenseEntries(image);
    return true;
#else
    return admitManualDefenseEntries(image);
#endif
}
static void nativeActor(uintptr_t) {
    ++actorCalls;
}
static uint64_t nativeSelector(uintptr_t, uintptr_t) {
    ++selectorCalls;
    return 0x16;
}
static void thunk(unsigned char* at, const void* destination) {
    const unsigned char entry[16] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(at, entry, sizeof entry);
    std::memcpy(at + 2, &destination, sizeof destination);
}
static void entryByte(unsigned char* at, unsigned char value) {
    DWORD old = 0, ignored = 0;
    assert(VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old));
    *at = value;
    assert(VirtualProtect(at, 1, old, &ignored));
    assert(FlushInstructionCache(GetCurrentProcess(), at, 1));
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr size_t bytes = 0x600000;
    auto* live = (unsigned char*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(live);
    base = (uintptr_t)live;
    layout = fp::Layout{};
    const fp::Layout known = layout;
    const uintptr_t entries[5] = {gameAt(fp::sig::defenseReactive), gameAt(fp::sig::defenseAttackUpdate),
                                  gameAt(fp::sig::defenseMotionSelect), gameAt(fp::sig::combatPosture),
                                  gameAt(fp::sig::combatUpdate)};
    for (auto target : manualDefenseTargets) {
        thunk(live + layout.rva[target], target == fp::sig::combatPosture ? (const void*)nativePosture
                                         : target == fp::sig::defenseMotionSelect
                                             ? (const void*)nativeSelector
                                             : (const void*)nativeActor);
        DWORD old = 0;
        assert(VirtualProtect(live + layout.rva[target], 16, PAGE_EXECUTE_READ, &old));
    }
    FlushInstructionCache(GetCurrentProcess(), live, bytes);
    std::vector<uint8_t> file(live, live + bytes);
    fp::ImageView image{file.data(), file.size(), base, 0x1000, 0x2ab000};
    assert(MH_Initialize() == MH_OK);

    assert(admitEntries(image));
    assert(!manualDefenseReady && manualDefenseAvailability == 2);
    for (auto target : manualDefenseTargets) assert(!layout.rva[target]);
    assert(!layout.rva[fp::sig::combatUpdateSlot]);
    assert(createManualDefenseHooks() && !manualDefenseReady);
    assert(layout.rva[fp::sig::cursorAim] == known.rva[fp::sig::cursorAim]);
    puts("PASS: unresolved optional group creates no hook and preserves the core layout");

    // Function targets may resolve while the separately reviewed field access changes.
    // The field gate withholds the whole optional group before hook admission.
    layout = known;
    layout.resolved = true;
    for (auto target : manualDefenseTargets) layout.rva[target] = 0;
    manualDefenseFieldMismatch = true;
    assert(admitEntries(image));
    assert(!manualDefenseReady && manualDefenseAvailability == 6);
    assert(!std::strcmp(manualDefenseReasonText(), "native_fields_changed"));
    assert(createManualDefenseHooks() && !manualDefenseHooksCreated);
    assert(layout.rva[fp::sig::cursorAim] == known.rva[fp::sig::cursorAim]);
    manualDefenseFieldMismatch = false;
    puts("PASS: a changed optional field reports its cause and creates no manual-defense hooks");

    layout = known;
    layout.resolved = true;
    assert(admitEntries(image));
    assert(layout.optionalGroupAvailable("manualDefense"));
    assert(createManualDefenseHooks() && manualDefenseReady && manualDefenseAvailability == 1);
    assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    ((Behavior)entries[0])(0);
    ((Behavior)entries[1])(0);
    assert(((uint64_t (*)(uintptr_t, uintptr_t))entries[2])(0, 0) == 0x16);
    defenseCleanup(0);
    assert(((CombatPosture)entries[3])(0) == 3);
    ((Behavior)entries[4])(0);
    assert(actorCalls == 4 && selectorCalls == 1 && postureCalls == 1);
    assert(MH_DisableHook(MH_ALL_HOOKS) == MH_OK);
    assert(admitEntries(image));
    assert(manualDefenseReady && manualDefenseAvailability == 1);
#ifndef EXCAMERA_PRE_REVIEW
    assert(manualDefenseHooksCreated);
#endif
    const fp::Layout retained = layout;
    for (const auto target : manualDefenseTargets) {
        if (target != manualDefenseTargets[0]) {
            assert(admitEntries(image));
            assert(createManualDefenseHooks() && manualDefenseReady && manualDefenseAvailability == 1);
            assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
            assert(MH_DisableHook(MH_ALL_HOOKS) == MH_OK);
        }
        const auto reactiveOriginal = originalDefenseReactive, attackOriginal = originalDefenseAttackUpdate;
        const auto selectOriginal = originalDefenseMotionSelect;
        const auto cleanupOriginal = defenseCleanup;
        const auto postureOriginal = originalCombatPosture;
        const auto updateOriginal = originalCombatUpdate;
        // F7 disables hooks but deliberately retains the registered entries and
        // trampolines. Another writer then changes one stopped native entry.
        auto* entry = live + retained.rva[target];
        const unsigned char original = *entry, changed = static_cast<unsigned char>(original ^ 1u);
        entryByte(entry, changed);
        faultDetail[0] = 0;
        checkAdmission(!admitEntries(image), "changed retained entry rejects re-arm before enable", target);
        bool identities = layout.optionalGroupAvailable("manualDefense");
        for (const auto identity : manualDefenseTargets)
            identities &= layout.rva[identity] == retained.rva[identity];
        checkAdmission(identities, "all six removal and cleanup identities remain intact", target);
        checkAdmission(
            originalDefenseReactive == reactiveOriginal && originalDefenseAttackUpdate == attackOriginal &&
                originalDefenseMotionSelect == selectOriginal && defenseCleanup == cleanupOriginal &&
                originalCombatPosture == postureOriginal && originalCombatUpdate == updateOriginal,
            "retained native callbacks are preserved", target);
        checkAdmission(!manualDefenseReady && manualDefenseAvailability == 5 &&
                           !std::strcmp(manualDefenseReasonText(), "native_entry_changed_after_creation") &&
                           std::strstr(faultDetail, "restart required"),
                       "capability reports the required restart", target);
        checkAdmission(*entry == changed, "admission leaves the foreign entry bytes untouched", target);
        bool registered = true;
        for (uintptr_t hooked : entries) registered &= MH_DisableHook((void*)hooked) == MH_ERROR_DISABLED;
        checkAdmission(registered, "all five hooks remain registered and disabled", target);
#ifndef EXCAMERA_PRE_REVIEW
        assert(manualDefenseHooksCreated);
#endif
        // Restoring the bytes after a conflict cannot revive this retained
        // installation. Test that retry before touching its ownership flag.
        entryByte(entry, original);
        layout = retained;
        if (target == manualDefenseTargets[0]) {
            faultDetail[0] = 0;
            checkAdmission(!admitEntries(image) && !manualDefenseReady && manualDefenseAvailability == 5 &&
                               std::strstr(faultDetail, "restart required"),
                           "restored bytes cannot retry a retained installation after a conflict", target);
        }
        for (uintptr_t hooked : entries) assert(MH_RemoveHook((void*)hooked) == MH_OK);
        // Only this fixture owns these synthetic bytes. It may emulate a new
        // startup for the next independent entry after all hooks are removed.
#ifndef EXCAMERA_PRE_REVIEW
        manualDefenseHooksCreated = false;
#endif
        manualDefenseReady = false;
    }
    for (auto target : manualDefenseTargets)
        assert(!std::memcmp(live + known.rva[target], file.data() + known.rva[target], 16));
    puts("PASS: complete group installs, forwards all native signatures and restores its entries");

    layout = known;
    layout.resolved = true;
    file[layout.rva[fp::sig::defenseCleanup]] ^= 1;
    assert(admitEntries(image));
    assert(!manualDefenseReady && manualDefenseAvailability == 3);
    for (auto target : manualDefenseTargets) assert(!layout.rva[target]);
    assert(!layout.rva[fp::sig::combatUpdateSlot]);
    assert(createManualDefenseHooks() && !manualDefenseReady);
    file[known.rva[fp::sig::defenseCleanup]] ^= 1;
    puts("PASS: a changed non-hooked cleanup entry withholds the entire optional group");

    layout = known;
    layout.resolved = true;
    // The fifth hook cannot be created; the first four must be removed before
    // this can truthfully fall back to Automatic and continue core startup.
    DWORD old = 0;
    assert(VirtualProtect((void*)entries[4], 16, PAGE_READWRITE, &old));
    assert(createManualDefenseHooks() && !manualDefenseReady && manualDefenseAvailability == 4);
#ifndef EXCAMERA_PRE_REVIEW
    assert(!manualDefenseHooksCreated);
#endif
    assert(MH_EnableHook((void*)entries[0]) == MH_ERROR_NOT_CREATED);
    assert(MH_EnableHook((void*)entries[1]) == MH_ERROR_NOT_CREATED);
    assert(MH_EnableHook((void*)entries[2]) == MH_ERROR_NOT_CREATED);
    assert(MH_EnableHook((void*)entries[3]) == MH_ERROR_NOT_CREATED);
    for (auto target : manualDefenseTargets) assert(!layout.rva[target]);
    assert(!layout.rva[fp::sig::combatUpdateSlot]);
    assert(layout.rva[fp::sig::cursorAim] == known.rva[fp::sig::cursorAim]);
    puts("PASS: partial optional hook creation rolls back completely and leaves core admission intact");

    assert(MH_Uninitialize() == MH_OK);
    assert(VirtualFree(live, 0, MEM_RELEASE));
    base = 0;
    printf("Retained-entry admission: %u checks, %u failures\n", admissionChecks, admissionFailures);
    if (!admissionFailures) puts("ALL MANUAL DEFENSE ADMISSION TESTS PASSED");
    return admissionFailures ? 1 : 0;
}
