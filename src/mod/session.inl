// The session: installing the hooks, the game build and other mods' hooks, F7/F6, the status file, the worker loop, the DLL's exports.
// Part of first_person.cpp (one translation unit): included there, in order.
static void loadSettings(const wchar_t* path); // mod/settings.inl: every F8 setting from first-person.ini
#include "../raw_registration.h"
static void rawReconcile();
static bool arm(const char*& reason, const char*& hooks) {
    const MH_STATUS enabled = MH_EnableHook(MH_ALL_HOOKS);
    if (enabled != MH_OK) {
        const bool rollback = disableNativeHooks() == MH_OK;
        reason = enabled == MH_ERROR_CONFLICT ? "hook_entry_changed"
                                             : rollback ? "hook_enable_failed" : "hook_rollback_failed";
        if (enabled == MH_ERROR_CONFLICT)
            snprintf(faultDetail, sizeof faultDetail,
                     "native code changed after hook creation; retained trampolines cannot be re-used; restart required");
        hooks = rollback ? "removed" : "unknown";
        return false;
    }
    const uintptr_t slot = gameAt(fp::sig::navSlot);
    bool installedBehavior = replace(gameAt(fp::sig::behaviorSlot), (void*)originalBehavior, (void*)behavior);
    bool installedPoint =
        installedBehavior && replace(gameAt(fp::sig::pointSlot), (void*)originalPoint, (void*)point);
    bool installedNav = installedPoint && replace(slot, (void*)originalNav, (void*)nav);
    if (!installedNav || InterlockedCompareExchange(&protectionFailure, 0, 0)) {
        const bool rollback = rollbackDispatch(installedNav, installedPoint, installedBehavior);
        reason = rollback ? "dispatch_install_failed" : "dispatch_rollback_failed";
        hooks = rollback ? "removed" : "unknown";
        return false;
    }
    // The game window may have been rebuilt while stopped (re-arm): find it again.
    if (!hookedWindow || !IsWindow(hookedWindow)) {
        hookedWindow = nullptr;
        EnumWindows(findWindow, (LPARAM)&hookedWindow);
    }
    originalWindowProc = nullptr;
    if (hookedWindow) {
        const auto current = (WNDPROC)GetWindowLongPtrW(hookedWindow, GWLP_WNDPROC);
        if (current && current != windowProc) {
            resetBindingWindowOwnership();
            originalWindowProc =
                current; // in place before the switch: a message arriving at once is forwarded
            SetLastError(0);
            const auto previous =
                (WNDPROC)SetWindowLongPtrW(hookedWindow, GWLP_WNDPROC, (LONG_PTR)windowProc);
            originalWindowProc =
                previous ? previous : (GetLastError() ? nullptr : current); // whoever was there at the switch
        }
    }
    if (!originalWindowProc) {
        const bool rollback = rollbackDispatch(true, true, true);
        reason = rollback ? "window_install_failed" : "window_rollback_failed";
        hooks = rollback ? "removed" : "unknown";
        return false;
    }
    {
        SettingsGuard guard;
        rawWanted = rawMouse;
    }
    rawReconcile();
    return true;
}
static fp::RawRegistration rawRegistration() {
    UINT count = 0;
    SetLastError(ERROR_SUCCESS);
    const UINT sized = GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    if ((sized == UINT(-1) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) || count > 256) return {};
    fp::RawRegistration result;
    result.known = true;
    if (!count) return result;
    std::vector<RAWINPUTDEVICE> devices(count);
    const UINT found = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    if (found == UINT(-1) || found > devices.size()) return {};
    for (UINT i = 0; i < found; ++i) {
        const RAWINPUTDEVICE& device = devices[i];
        if (device.usUsagePage != 1 || device.usUsage != 2) continue;
        if (result.present) return {}; // An inconsistent snapshot cannot establish ownership.
        result.present = true;
        result.target = uintptr_t(device.hwndTarget);
        result.flags = device.dwFlags;
    }
    return result;
}
static bool rawReconcileTarget(HWND desired) {
    uintptr_t target = uintptr_t(rawTarget);
    const bool ok = fp::reconcileRawRegistration(
        uintptr_t(desired), target, rawRegistration,
        [](uintptr_t window) {
            RAWINPUTDEVICE device{1, 2, 0, HWND(window)};
            return RegisterRawInputDevices(&device, 1, sizeof device) != FALSE;
        },
        [] {
            RAWINPUTDEVICE device{1, 2, RIDEV_REMOVE, nullptr};
            return RegisterRawInputDevices(&device, 1, sizeof device) != FALSE;
        },
        [](bool registered) {
            AcquireSRWLockExclusive(&rawLock);
            const bool previous = InterlockedExchange(&rawRegistered, registered ? 1 : 0) != 0;
            if (!registered || previous != registered) rawLook = fp::RawLook{};
            ReleaseSRWLockExclusive(&rawLock);
        });
    rawTarget = HWND(target);
    return ok;
}
// Recheck even while active: another component's later registration replaces ours.
// Loss clears the relative-device latch so cursor deltas resume on the next sample.
static void rawReconcile() {
    const bool want = rawWanted.load() && hookedWindow && IsWindow(hookedWindow);
    if (!rawReconcileTarget(want ? hookedWindow : nullptr) && want) {
        SettingsGuard guard;
        rawWanted = false; // The settings page reports the effective fallback immediately.
    }
}
// If Exanima destroys its window (display mode
// or resolution change) and makes a new one, the new one is subclassed so F6-F11,
// F8 and focus release keep working. A window that still exists is never touched.
static std::atomic<unsigned> windowRehooks{0};
static fp::SwapWatch swapWatch;
static void rehookWindow() {
    if (!originalWindowProc || (hookedWindow && IsWindow(hookedWindow))) return;
    HWND w = nullptr;
    EnumWindows(findWindow, (LPARAM)&w);
    if (!w) return;
    const auto current = (WNDPROC)GetWindowLongPtrW(w, GWLP_WNDPROC);
    if (current && current != windowProc) {
        resetBindingWindowOwnership();
        originalWindowProc = current; // the new window's chain (the old one is gone)
        if (SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)windowProc)) {
            hookedWindow = w;
            ++windowRehooks;
        }
    }
}
// Worker thread: F7 stops only when held (fp::stopKeyStep); a short press shows
// a hint instead. Clearing compares against the press it decided on.
static const wchar_t* const stopHoldHint = L"Keep holding F7 to stop " FP_SHOWN_NAME L"...";
// EMTK switched the mod off and on again: disable stops as F7 does, a later enable re-arms as F6 does.
// emtkOff is EMTK's switch as last set; a re-arm is asked only by an off -> on change and only while it is on.
static volatile LONG rearmRequested = 0, emtkOff = 0;
// A re-arm done: running again, unless EMTK switched it off meanwhile (disable sets emtkOff before stopping,
// so one of the two writes of stopping here or there comes last and says stop).
static void rearmRunning() {
    InterlockedExchange(&rearmRequested, 0);
    InterlockedExchange(&stopping, 0);
    if (InterlockedCompareExchange(&emtkOff, 0, 0)) InterlockedExchange(&stopping, 1);
}
static void stopKeyTick() {
    ULONGLONG press = stopPress.load();
    const ULONGLONG release = stopRelease.load();
    const fp::StopKey step = fp::stopKeyStep(press, release, GetTickCount64(), gameWindow() != nullptr);
    if (step == fp::StopKey::idle) return;
    if (step == fp::StopKey::holding) {
        if (!noticeWindow || noticeText != stopHoldHint) showNotice(stopHoldHint, 3000);
        return;
    }
    if (!stopPress.compare_exchange_strong(press, 0)) return; // A new press arrived: decide it next tick.
    if (step == fp::StopKey::stop) {
        InterlockedExchange(&rearmRequested, 0);
        InterlockedExchange(&stopping, 1);
        releaseInput();
        return;
    }
    if (step == fp::StopKey::tap) {
        ++stopTaps;
        showNotice(L"F7 ignored. Hold F7 for one second to stop " FP_SHOWN_NAME L".", 4000);
        return;
    }
    if (noticeWindow && noticeText == stopHoldHint) {
        DestroyWindow(noticeWindow);
        noticeWindow = nullptr;
        noticeVisible = false;
    } // cancel: focus lost
}
// Stopped cleanly: the window procedure is gone, so F6 is read from the key
// state, only while a window of the game is frontmost. A key already held at
// the stop must be released first.
static void waitForRearmKey() {
    bool wasDown = true;
    for (;;) {
        Sleep(20);
        pumpNotice();
        if (InterlockedExchange(&rearmRequested, 0) && !InterlockedCompareExchange(&emtkOff, 0, 0))
            return; // EMTK switched the mod back on (enable)
        const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (down && !wasDown && gameWindow()) return;
        wasDown = down;
    }
}
// F6 after a clean F7: the same checks as activation, then a fresh session.
// Any failure is reported as an activation fault (restart required).
// ---- The game build: verified (the build this release was tested on, by its hash) or compatible (every
// signature in game_signatures.h resolves in it, and the hooks then prove they run: fp::HookHealth) ----
static bool verifiedBuild = false;
// The game's executable as the loader would map it, read from disk: the loaded image may already carry
// another mod's detours, and must not be what decides where the targets are.
static bool readGameFile(const wchar_t* path, std::vector<uint8_t>& mapped, fp::ImageView& view) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<uint8_t> file;
    DWORD got = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64ll * 1024 * 1024;
    if (ok) {
        file.resize(size_t(size.QuadPart));
        ok = ReadFile(f, file.data(), DWORD(file.size()), &got, nullptr) && got == file.size();
    }
    CloseHandle(f);
    return ok && fp::mapPeFile(file.data(), file.size(), mapped, view);
}
static wchar_t unsupportedNotice[160]{};
// Targets found although some of their signatures no longer match (status: worth regenerating before an update).
static char degradedTargets[400]{};
// Resolves the layout from the game's file. On failure the unresolved targets go to the status file.
static bool resolveGame(const wchar_t* executable) {
    std::vector<uint8_t> mapped;
    fp::ImageView file;
    fp::ResolveReport report;
    if (!readGameFile(executable, mapped, file)) {
        snprintf(faultDetail, sizeof faultDetail, "Exanima.exe could not be read");
        return false;
    }
    fp::Layout candidate;
    if (fp::resolveLayout(file, candidate, report)) {
        if (!fp::validateNativeContracts(file, candidate, nativeContracts)) {
            snprintf(faultDetail, sizeof faultDetail, "native field contract failed: %s (%s)",
                     nativeContracts.firstRequired ? nativeContracts.firstRequired : "unknown",
                     fp::contractFailureText(nativeContracts.firstRequiredReason));
            swprintf_s(unsupportedNotice,
                       FP_SHOWN_NAME L": this Exanima native layout is not supported (%hs).",
                       nativeContracts.firstRequired ? nativeContracts.firstRequired : "field contract");
            return false;
        }
        layout = candidate;
        manualDefenseFieldMismatch = nativeContracts.optionalFailures != 0;
        int at = 0;
        degradedTargets[0] = 0;
        for (unsigned t = 0; t < fp::sig::targetCount && at >= 0 && at < int(sizeof degradedTargets); ++t)
            if (report.degraded[t])
                at += snprintf(degradedTargets + at, sizeof degradedTargets - at, "%s%s", at ? "," : "",
                               fp::sig::targetNames[t]);
        return true;
    }
    int at = snprintf(faultDetail, sizeof faultDetail, "%u of %u game targets unresolved:", report.failures,
                      unsigned(fp::sig::targetCount));
    for (unsigned t = 0; t < fp::sig::targetCount && at > 0 && at < int(sizeof faultDetail); ++t)
        if (report.why[t] != fp::Unresolved::none)
            at += snprintf(faultDetail + at, sizeof faultDetail - at, " %s (%s)", fp::sig::targetNames[t],
                           fp::unresolvedText(report.why[t]));
    swprintf_s(unsupportedNotice,
               FP_SHOWN_NAME L": this Exanima version is not supported (%u game functions not found). "
                             L"Nothing was changed.",
               report.failures);
    return false;
}
// ---- Other programs' hooks on the functions this mod hooks or calls, or in its dispatch slots
// ([Compat] OtherHooks, fp::chainAllowed). Chained: this mod's hooks go on top of theirs, and its slots
// call through to what they put there (and give it back on stop). ----
static fp::OtherHooks otherHooks = fp::OtherHooks::ask;
static bool chained =
    false; // this game session runs on top of another program's hooks (sticky: F6 re-arms too)
static wchar_t otherHooksNotice[240]{};
static char otherHooksNames[160]{}; // status: which targets were already hooked
// The launcher's prompt: the player said yes for this game process (allowOtherHooks, called before enable).
static volatile LONG playerApproved = 0;
static bool slotTaken(fp::sig::Target slot, fp::sig::Target original) {
    return *(uintptr_t*)gameAt(slot) != gameAt(original);
} // no longer the game's own function
// True to go on: nothing else hooked, or chaining is allowed. The function entries are compared with the
// game's file; the slots with the game's own functions.
static bool otherHooksCheck(const wchar_t* executable) {
    std::vector<uint8_t> mapped;
    fp::ImageView file;
    if (!readGameFile(executable, mapped, file)) {
        snprintf(faultDetail, sizeof faultDetail, "Exanima.exe could not be read");
        return false;
    }
    if (!admitManualDefenseEntries(file)) {
        otherHooksNotice[0] = 0; // Do not reuse a previous successful chaining notice for this restart.
        return false;
    }
    fp::sig::Target found[8];
    unsigned n = fp::patchedEntries(
        file, layout,
        [](uint32_t rva, uint8_t* out) { return memcpy(out, (const void*)(base + rva), 16), true; }, found,
        8);
    const fp::sig::Target slots[3][2] = {{fp::sig::navSlot, fp::sig::navSampler},
                                         {fp::sig::behaviorSlot, fp::sig::playerBehavior},
                                         {fp::sig::pointSlot, fp::sig::aimPoint}};
    for (const auto& sl : slots)
        if (slotTaken(sl[0], sl[1])) {
            if (n < 8) found[n] = sl[0];
            ++n;
        }
    if (!n) return true;
    int at = 0;
    otherHooksNames[0] = 0;
    for (unsigned i = 0; i < n && i < 8 && at >= 0 && at < int(sizeof otherHooksNames); ++i)
        at += snprintf(otherHooksNames + at, sizeof otherHooksNames - at, "%s%s", i ? "," : "",
                       fp::sig::targetNames[found[i]]);
    if (fp::chainAllowed(otherHooks, chained || InterlockedCompareExchange(&playerApproved, 0, 0) != 0)) {
        chained = true;
        swprintf_s(
            otherHooksNotice,
            FP_SHOWN_NAME L": running on top of another mod's hooks (%hs). Hold F7 if anything goes wrong.",
            otherHooksNames);
        return true;
    }
    snprintf(faultDetail, sizeof faultDetail, "already hooked by another program: %s", otherHooksNames);
    swprintf_s(otherHooksNotice,
               otherHooks == fp::OtherHooks::refuse
                   ? FP_SHOWN_NAME L": another mod already hooks %hs. Not started (OtherHooks=refuse)."
                   : FP_SHOWN_NAME L": another mod already hooks %hs. Not started: run ExCamera.exe to "
                                   L"choose, or set OtherHooks=allow.",
               otherHooksNames);
    return false;
}
// The dispatch slots' current owners become what this mod's replacements call: the game's own functions,
// or (chained) another program's. Never this mod's own replacements (a stop that did not roll back).
static uintptr_t nav(void*, float);
static void behavior(uintptr_t);
static void point(uintptr_t, float*);
static bool adoptDispatch() {
    const uintptr_t n = *(uintptr_t*)gameAt(fp::sig::navSlot), b = *(uintptr_t*)gameAt(fp::sig::behaviorSlot),
                    p = *(uintptr_t*)gameAt(fp::sig::pointSlot);
    const bool own = n == gameAt(fp::sig::navSampler) && b == gameAt(fp::sig::playerBehavior) &&
                     p == gameAt(fp::sig::aimPoint);
    if (!own && !chained) return false;
    if (!n || !b || !p || n == (uintptr_t)&nav || b == (uintptr_t)&behavior || p == (uintptr_t)&point)
        return false;
    originalNav = (Nav)n;
    originalBehavior = (Behavior)b;
    originalPoint = (Point)p;
    return true;
}
static fp::HookHealth hookHealth;
static volatile LONG hookFault = 0;
static const char* hookHealthText() {
    static char text[48];
    if (hookHealth.verdict == fp::HealthVerdict::passed) return "passed";
    if (hookHealth.verdict != fp::HealthVerdict::failed) return "watching";
    snprintf(text, sizeof text, "failed:%s", fp::healthHookNames[hookHealth.silent]);
    return text;
}
// Worker thread. On a build that is only compatible, a per-tick hook that never runs during play means the
// game no longer goes through it: the mod stops and restores the game rather than run half-hooked. On the
// verified build the result is only reported (its hooks were checked in play).
static void hookHealthTick() {
    if (hookHealth.decided) return;
    uint32_t hits[fp::healthHooks];
    for (unsigned k = 0; k < fp::healthHooks; ++k) hits[k] = healthHits[k].load(std::memory_order_relaxed);
    const bool inPlay = *(uintptr_t*)gameAt(fp::sig::playerRoot) != 0 && gameWindow() != nullptr;
    if (fp::healthStep(hookHealth, inPlay, *(uint32_t*)gameAt(fp::sig::gameClock), hits) ==
            fp::HealthVerdict::failed &&
        (!verifiedBuild || chained)) {
        snprintf(faultDetail, sizeof faultDetail, "hook not firing during play: %s",
                 fp::healthHookNames[hookHealth.silent]);
        InterlockedExchange(&hookFault, 1);
        InterlockedExchange(&stopping, 1);
    }
}
static bool rearm(const wchar_t* executable) {
    setActivationPhase(fp::ActivationPhase::starting);
    viewMode = 0;                 // a fresh session starts in first person
    rearmedAt = GetTickCount64(); // before the window procedure returns (a queued F6 must not toggle)
    if (!otherHooksCheck(executable)) {
        reportFault("rearm_native_conflict", "removed", "not_required",
                    otherHooksNotice[0] ? otherHooksNotice : nullptr);
        return false;
    }
    if (!adoptDispatch()) {
        reportFault("rearm_dispatch_conflict", "removed", "not_required");
        return false;
    }
    {
        CameraGuard guard; // stopping is still set: callbacks publish nothing meanwhile.
        park = Park{};
        for (Retired& r : retired) r = Retired{};
        InterlockedExchange(&parkedNow, 0);
        savedCamera = savedView = savedController = savedRoot = savedScene = 0;
        InterlockedExchange(&pendingRestore, 0);
        fp::holdEnd(headHold);
        publishHold();
        resetViewHistory();
        inputClock = ~0u;
    }
    stopPress = 0;
    stopRelease = 0;
    // A re-arm is a fresh installation, including newly adopted dispatch-slot
    // owners. A previous session's passed verdict cannot verify these hooks.
    hookHealth = fp::HookHealth{};
    InterlockedExchange(&hookFault, 0);
    const char* reason = "";
    const char* hooks = "";
    if (!arm(reason, hooks)) {
        reportFault(reason, hooks, "not_required");
        return false;
    }
    InterlockedExchange(&restored, 0);
    ++stopRearms;
    rearmedAt = GetTickCount64();
    rearmRunning();
    return true;
}
// Loaded by EMTK at the game's entry point, the DLL starts before the game has made its window (the
// subclass and notices need it) or loaded OpenGL (the frame hook; not a static import of the game). Wait for
// both, bounded; every check after runs unchanged (the dispatch slots are filled from the file before entry,
// so a slot holding someone else's code is still a dispatch conflict). Through our launcher both are there.
static constexpr ULONGLONG gameReadyWaitMs = 300000; // 5 minutes: a slow disk can take long to the title
static bool waitForGame() {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (GetModuleHandleW(
                L"opengl32.dll")) { // the window looked for only then: not a window shown before the game's own
            if (!hookedWindow || !IsWindow(hookedWindow)) {
                hookedWindow = nullptr;
                EnumWindows(findWindow, (LPARAM)&hookedWindow);
            }
            if (hookedWindow) return true;
        }
        if (GetTickCount64() - start >= gameReadyWaitMs) return false;
        Sleep(100);
    }
}
// The worker's running snapshot includes the shared activation acknowledgement.
static void writeRunningStatus(FILE* f, const Aim& a) {
    fp::JsonOut j(f);
    writeActivationStatus(j);
    j.boolean("installed", true);
    j.num("nav_thread", InterlockedCompareExchange(&navThread, 0, 0));
    j.num("swap_thread", InterlockedCompareExchange(&swapThread, 0, 0));
    j.num("active", InterlockedCompareExchange(&active, 0, 0));
    j.unum("samples", samples.load());
    j.unum("controls", controlCalls.load());
    j.unum("point_calls", pointCalls.load());
    j.unum("relaxed_steps", relaxedSteps.load());
    j.num("swap_calls", InterlockedCompareExchange(&swapCalls, 0, 0));
    j.num("native_cursor_updates", InterlockedCompareExchange(&nativeCursorUpdates, 0, 0));
    j.hex("root", a.root);
    j.real("yaw", a.yaw);
    j.real("pitch", a.pitch);
    j.reals("target", a.point, 3);
    j.unum("walk_targets", walkTargets.load());
    j.unum("combat_steps", combatSteps.load());
    j.str("manual_defense_capability", manualDefenseReasonText());
    j.str("fp_defense_requested", fpManualGuard.load() ? "manual" : "automatic");
    j.str("tp_defense_requested", tpManualGuard.load() ? "manual" : "automatic");
    j.str("fp_defense_effective", manualDefenseReady.load() && fpManualGuard.load() ? "manual" : "automatic");
    j.str("tp_defense_effective", manualDefenseReady.load() && tpManualGuard.load() ? "manual" : "automatic");
    j.unum("guard_denied", guardDenied.load());
    j.unum("guard_withdrawals", guardWithdrawals.load());
    j.unum("unexpected_relaxed_steps", unexpectedRelaxedSteps.load());
    j.num("fov", selectedFov.load());
    j.boolean("settings_saved", !settingsSavePending());
    j.boolean("tracking_requested", trackingEnabled.load());
    j.num("tracking_transport", int(trackingReceiver.status()));
    const auto tracker = trackingReceiver.snapshot();
    const auto trackingNow = GetTickCount64();
    j.boolean("tracking_fresh", tracker.valid && trackingNow >= tracker.received &&
                               trackingNow - tracker.received <= fp::trackingStaleMs);
    j.num("cursor_free", InterlockedCompareExchange(&cursorFree, 0, 0));
    j.num("raw_mouse", InterlockedCompareExchange(&rawRegistered, 0, 0));
    j.unum("side_flips", sideFlips.load());
    j.str("helmet", helmetMode.load() == fp::HelmetMode::hide ? "hide" : "show");
    j.unum("head_wear_nodes", headWearNodes.load());
    j.unum("hidden_head_nodes", hiddenHeadNodes.load());
    j.unum("first_person_head_draws_skipped", firstPersonHeadDrawsSkipped.load());
    j.unum("helmet_blocking", helmetBlocking.load());
    j.unum("helmet_unknown", helmetUnknown.load());
    j.unum("helmet_tests", helmetTests.load());
    j.unum("hidden_meshes", hiddenMeshes.load());
    j.unum("masked_draws", maskedDraws.load());
    j.unum("hide_overflow", hideOverflow.load());
    j.num("sector_thread", InterlockedCompareExchange(&sectorThread, 0, 0));
    j.unum("cover_passes_skipped", coverPassesSkipped.load());
    j.unum("aim_holds", aimHolds.load());
    j.unum("head_steady_ticks", steadyTicks.load());
    j.unum("window_freezes", windowFreezes.load());
    j.unum("spine_bend_ticks", spineBendTicks.load());
    j.unum("crouch_ticks", crouchTicks.load());
    j.unum("door_holds", doorHolds.load());
    j.str("drag", dragName, 16);
    j.unum("hold_notches", holdNotches.load());
    j.fixed("hold_target", double(holdTarget.load()), 3);
    j.fixed("hold_applied", double(holdApplied.load()), 3);
    j.unum("hold_placements", holdPlacements.load());
    j.unum("warps_skipped", warpsSkipped.load());
    j.unum("grabs_refused", grabsRefused.load());
    j.unum("drop_spots", dropSpots.load());
    j.num("grab_range", grabRange.load());
    j.unum("window_rehooks", windowRehooks.load());
    j.num("swap_starved", swapWatch.starved ? 1 : 0);
    j.num("cursor_clipped", InterlockedCompareExchange(&cursorClipped, 0, 0));
    j.num("view_fault", viewFault.load());
    j.num("attack_cue", attackCue ? 1 : 0);
    j.num("cue_state", cueState.load());
    j.unum("cue_draws", cueDraws.load());
    j.num("cue_gl", cueGl.load());
    j.unum("ui_reticle_reads", uiReticleReads.load());
    j.num("pick_camera", pickCamera.load());
    j.unum("notices", notices.load());
    j.str("state", InterlockedCompareExchange(&parkedNow, 0, 0)
                       ? "parked"
                       : (holdingNow.load() ? "holding" : (rearms.load() ? "rearmed" : "running")));
    j.unum("parks", parks.load());
    j.unum("rearms", rearms.load());
    j.unum("reclaims", reclaims.load());
    j.num("park_kind", parkKind.load());
    j.num("cue_origin", cueOrigin.load());
    j.object("hand")
        .num("count", handCount.load())
        .num("grip", handGrip.load())
        .num("slot", handSlot.load())
        .num("index", handIndex.load())
        .num("expected", handExpected.load())
        .num("side", handSideSeen.load())
        .array("names")
        .str(nullptr, handNames[0], 15)
        .str(nullptr, handNames[1], 15)
        .end()
        .end();
    j.unum("pick_checks", pickChecks.load());
    j.num("pick_clear", lastClearance.load());
    j.unum("pick_blocked", pickBlocked.load());
    j.unum("clearance_queries", clearanceQueries.load());
    j.unum("held_frames", heldFrames.load());
    j.unum("holds", holds.load());
    j.unum("max_hold", maxHold.load());
    j.unum("hold_parks", holdParks.load());
    j.unum("park_backoffs", parkBackoffs.load());
    j.unum("f7_taps", stopTaps.load());
    j.unum("f7_rearms", stopRearms.load());
    j.array("head_rejects");
    for (const auto& r : headRejects) j.unum(nullptr, r.load());
    j.end();
    j.unum("swing_hides", swingHides.load());
    j.str("look", "head");
    j.real("near", double(renderedNear), 3);
    j.object("supersampling")
        .unum("scopes", supersampleScopes.load())
        .num("in_flight", InterlockedCompareExchange(&renderScopesInFlight, 0, 0))
        .unum("matrices", supersampleCorrections.load())
        .unum("terrain_matrices", supersampleTerrainCorrections.load())
        .unum("faults", supersampleFaults.load())
        .end();
    j.unum("body_wear", bodyWearNodes.load());
    j.unum("hidden_body", hiddenBodyNodes.load());
    j.str("view", viewMode.load() == 1 ? "third" : viewMode.load() == 2 ? "to-overhead" : "first");
    j.unum("tp_prop_pulls", tpPropPulls.load());
    j.str("game_build", verifiedBuild ? "verified" : "compatible");
    j.str("hook_health", hookHealthText());
    {
        char hooks[200];
        snprintf(hooks, sizeof hooks, "%s%s", chained ? "chained:" : "none", chained ? otherHooksNames : "");
        j.str("other_hooks", hooks);
    }
    j.str("signatures_degraded", degradedTargets);
    writeNativeCheckboxStatus(j);
    j.unum("native_fields_checked", nativeContracts.checked);
    j.unum("native_fields_withheld", nativeContracts.optionalFailures);
    j.str("native_field_issue", nativeContracts.firstOptional ? nativeContracts.firstOptional : "none");
    j.object("held")
        .real("distance", holdApplied.load(), 4)
        .real("target", holdTarget.load(), 4)
        .boolean("rotating", heldRotating.load())
        .unum("turns", heldTurns.load())
        .unum("flings", flings.load())
        .real("physics_step", physicsStep.load(), 4)
        .end();
    j.unum("helmet_turn_retests", helmetTurnRetests.load());
    j.unum("prop_near_hits", propNearHits.load());
    j.real("heading_turn", double(headingTurn.load()) * 57.29578, 2);
    j.unum("overhead_inputs_held", overheadInputsHeld.load());
    j.object("lean")
        .boolean("held", leaning.load())
        .real("side", leanSideNow.load(), 3)
        .real("forward", leanForwardNow.load(), 3)
        .end();
    j.object("head")
        .num("eye_setback", eyeSetback.load())
        .real("mid_up", headMid2.load(), 4)
        .real("mid_forward", headMid3.load(), 4)
        .real("reach", headReach.load(), 4)
        .real("eye_up", fp::eyeAlongAxis2, 4)
        .real("eye_forward", fp::eyeAlongAxis3, 4)
        .end();
    j.object("third_person")
        .real("drop", tpDrop.load(), 4)
        .boolean("down", tpDownNow.load())
        .real("ceiling_room", tpCeilingRoom.load(), 4)
        .real("lost_height", tpLostHeight.load(), 4)
        .real("pivot_pulled", tpPivotPulled.load(), 4)
        .real("allowed", tpAllowed.load(), 4)
        .str("blockers", tpBlockers.load() == fp::blockersHide   ? "hide"
                         : tpBlockers.load() == fp::blockersPush ? "push"
                                                                 : "smart")
        .boolean("combat_aim_free", tpAimFree.load())
        .array("aim_box")
        .real(nullptr, tpAimX.load(), 4)
        .real(nullptr, tpAimY.load(), 4)
        .end()
        .end();
    j.finish();
}
static DWORD runSession() {
    QueryPerformanceFrequency(&frequency);
    wchar_t mutexName[80];
    swprintf_s(mutexName, L"Local\\ExanimaFirstPerson-%lu", GetCurrentProcessId());
    instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instanceMutex) CloseHandle(instanceMutex);
        return 1;
    }
    GetModuleFileNameW(module, statusPath, MAX_PATH);
    auto slash = wcsrchr(statusPath, L'\\');
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - statusPath), L"first-person-status.json");
        swprintf_s(statusTempPath, L"%s.tmp", statusPath);
    }
    EnumWindows(findWindow, (LPARAM)&hookedWindow);
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
        reportFault("unsupported_executable", "not_installed", "not_required");
        return 2;
    }
    verifiedBuild = fp::supportedExecutable(executable);
    if (!resolveGame(executable)) {
        reportFault("unsupported_game_build", "not_installed", "not_required",
                    unsupportedNotice[0] ? unsupportedNotice : nullptr);
        return 2;
    }
    GetModuleFileNameW(module, settingsPath, MAX_PATH);
    auto settingsSlash = wcsrchr(settingsPath, L'\\');
    if (settingsSlash) {
        wcscpy_s(settingsSlash + 1, MAX_PATH - (settingsSlash + 1 - settingsPath), L"first-person.ini");
        {
            wchar_t profile[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
            const std::wstring user =
                userSettingsFile(settingsPath, n && n < MAX_PATH ? userSettingsDir(profile, userDocumentsDir())
                                                               : std::wstring());
            if (user.size() < MAX_PATH) wcscpy_s(settingsPath, user.c_str());
        }
        loadSettings(settingsPath);
        persistedFov = selectedFov.load();
        attackSideKey = GetPrivateProfileIntW(L"Input", L"AttackSideKey", 0, settingsPath);
        {
            wchar_t oh[16];
            GetPrivateProfileStringW(L"Compat", L"OtherHooks", L"ask", oh, 16, settingsPath);
            otherHooks = fp::otherHooksFromText(oh);
        }
        if (!fp::attackSideKeyValid(attackSideKey)) attackSideKey = 0; // never a mouse button
    } else
        settingsPath[0] = 0;
    if (!waitForGame()) {
        reportFault("game_not_ready", "not_installed", "not_required");
        return 2;
    }
    base = (uintptr_t)GetModuleHandleW(nullptr);
    originalNav = (Nav)(gameAt(fp::sig::navSampler));
    if (!otherHooksCheck(executable)) {
        reportFault("native_code_conflict", "not_installed", "not_required",
                    otherHooksNotice[0] ? otherHooksNotice : nullptr);
        return 2;
    }
    rebuildProjection = (Projection)(gameAt(fp::sig::rebuildProjection));
    rebuildInverse = (Inverse)(gameAt(fp::sig::rebuildInverse));
    levelRay = (LevelRay)(gameAt(fp::sig::levelRay));
    levelRay2 = (LevelRay)(gameAt(fp::sig::levelRay2));
    if (!adoptDispatch()) {
        reportFault("dispatch_conflict", "not_installed", "not_required");
        return 2;
    }
    if (MH_Initialize() != MH_OK) {
        reportFault("hook_initialization_failed", "not_installed", "not_required");
        return 2;
    }
    copyCameraSnapshots = (void (*)())gameAt(fp::sig::copyCameraSnapshots);
    if (MH_CreateHook((void*)gameAt(fp::sig::analogScale), (void*)analogScale,
                      (void**)&originalAnalogScale) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::analogAdd), (void*)analogAdd, (void**)&originalAnalogAdd) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::cameraOrbit), (void*)cameraOrbit,
                      (void**)&originalCameraOrbit) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::cameraZoom), (void*)cameraZoom, (void**)&originalCameraZoom) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::cameraMouse), (void*)cameraMouse,
                      (void**)&originalCameraMouse) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::aimWorldToLocal), (void*)aimWorldToLocal,
                      (void**)&originalAimWorldToLocal) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::guiMaintenance), (void*)nativeSettingsMaintenance,
                      (void**)&originalGuiMaintenance) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::cursorAim)), (void*)cursorAim, (void**)&originalCursor) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::combatAttention), (void*)combatAttention,
                      (void**)&originalCombatAttention) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::explore)), (void*)explore, (void**)&originalExplore) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::combatStep)), (void*)step, (void**)&originalStep) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::poseBuilder)), (void*)poseBuilder, (void**)&originalPose) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::partSettle)), (void*)partSettle, (void**)&originalPartSettle) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::cursorPosition)), (void*)cursorPosition,
                      (void**)&originalCursorPosition) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::pollInput)), (void*)input, (void**)&originalInput) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::guiMousePress), (void*)guiMousePress,
                      (void**)&originalGuiMousePress) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::guiMousePoll), (void*)guiMousePoll,
                      (void**)&originalGuiMousePoll) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::travelTarget)), (void*)travelTarget,
                      (void**)&originalTravelTarget) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::walkGait)), (void*)walkGait, (void**)&originalWalkGait) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::sectorUpdate)), (void*)sectorUpdate,
                      (void**)&originalSectorUpdate) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::cameraFade), (void*)cameraFade, (void**)&originalCameraFade) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::fadeBounds), (void*)fadeBounds, (void**)&originalFadeBounds) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::meshQueue), (void*)meshQueue,
                      (void**)&originalMeshQueue) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::renderViewport), (void*)renderViewport,
                      (void**)&originalRenderViewport) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::terrainPass), (void*)terrainPass,
                      (void**)&originalTerrainPass) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::multiplyMatrix), (void*)excameraMultiplyMatrixHook,
                      (void**)&excameraOriginalMultiplyMatrix) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::coverPass)), (void*)coverPass, (void**)&originalCoverPass) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::coverDepth)), (void*)coverDepth, (void**)&originalCoverDepth) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::dragPlace)), (void*)dragPlace, (void**)&originalDragPlace) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::heldQuery), (void*)heldQuery, (void**)&originalHeldQuery) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::heldPlace), (void*)heldPlace, (void**)&originalHeldPlace) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::heldSweep), (void*)heldSweep, (void**)&originalHeldSweep) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::dragRotate), (void*)dragRotate, (void**)&originalDragRotate) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::pickRay)), (void*)pickRay, (void**)&originalPickRay) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::cursorWarp)), (void*)cursorWarp, (void**)&originalCursorWarp) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::grabPick)), (void*)grabPick, (void**)&originalGrabPick) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::doorHandler)), (void*)doorHandler,
                      (void**)&originalDoorHandler) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::planeHit)), (void*)planeHit, (void**)&originalPlaneHit) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::dragDrop)), (void*)dragDrop, (void**)&originalDragDrop) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::queueWork), (void*)queueWork, (void**)&originalQueueWork) !=
            MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::applyHeldRelease), (void*)applyHeldRelease,
                      (void**)&originalApplyHeldRelease) != MH_OK ||
        MH_CreateHook((void*)gameAt(fp::sig::constraintHandler), (void*)constraintHandler,
                      (void**)&originalConstraintHandler) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::hingeHandler)), (void*)hingeHandler,
                      (void**)&originalHingeHandler) != MH_OK ||
        MH_CreateHook((void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglMakeCurrent"),
                      (void*)cueMakeCurrent, (void**)&originalCueMakeCurrent) != MH_OK ||
        MH_CreateHook((void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglDeleteContext"),
                      (void*)cueDeleteContext, (void**)&originalCueDeleteContext) != MH_OK ||
        MH_CreateHook((void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglSwapBuffers"), (void*)swap,
                      (void**)&originalSwap) != MH_OK) {
        MH_Uninitialize();
        awaitNativeHookRecovery();
        reportFault("hook_creation_failed", "not_installed", "not_required");
        return 3;
    }
    if (!createManualDefenseHooks()) {
        MH_Uninitialize();
        awaitNativeHookRecovery();
        reportFault("manual_hook_rollback_failed", "not_installed", "not_required");
        return 3;
    }
    {
        const char* reason = "";
        const char* hooks = "";
        if (!arm(reason, hooks)) {
            reportFault(reason, hooks, "not_required");
            return 4;
        }
    }
    if (chained)
        showNotice(otherHooksNotice, 12000);
    else if (!verifiedBuild)
        showNotice(FP_SHOWN_NAME L": untested Exanima version. Every game function was found; hold F7 if "
                                 L"anything looks wrong.",
                   12000);
    bool viewFaultNoticed = false;
    for (;;) {
        InterlockedExchange(&active, 1);
        setActivationPhase(fp::ActivationPhase::running);
        for (int i = 0; !InterlockedCompareExchange(&stopping, 0, 0); i++) {
            Sleep(100);
            trackingReceiver.service(trackingEnabled.load());
            saveFov();
            if (!viewFaultNoticed && viewFault.load()) {
                viewFaultNoticed = true;
                showNotice(
                    FP_SHOWN_NAME L": view feature error. Helmet handling stopped; first person continues.");
            }
            stopKeyTick();
            hookHealthTick();
            if (i % 10 == 0) {
                static unsigned stepsBefore = 0;
                const unsigned steps = playerSteps.load(); // the player's pose builds a second
                physicsStep = steps - stepsBefore >= 10 ? 1.f / float(steps - stepsBefore) : 0.f;
                stepsBefore = steps;
            }
            pumpNotice();
            { // F8's window: in play, and at the title (no player character for 2 s).
                const bool title = atTitle();
                if (viewMode.load() == 2 && GetTickCount64() - tpBlendStart.load() > 600) {
                    viewMode = 0;
                    InterlockedExchange(&active, 0);
                    AcquireSRWLockExclusive(&aimLock);
                    aim.valid = false;
                    ReleaseSRWLockExclusive(&aimLock);
                } // no nav frame ended the blend (parked, loading): end it as F6 did
                f8WindowTick(InterlockedCompareExchange(&cursorFree, 0, 0) && gameWindow() != nullptr &&
                                 (InterlockedCompareExchange(&active, 0, 0) || title),
                             title);
            }
            rehookWindow();
            rawReconcile();
            if (i % 10 == 0) {
                const bool starvedNow = fp::swapWatchStep(
                    swapWatch, samples.load(), unsigned(InterlockedCompareExchange(&swapCalls, 0, 0)),
                    InterlockedCompareExchange(&active, 0, 0) != 0);
                if (starvedNow)
                    showNotice(
                        FP_SHOWN_NAME L": another program took over the game's frame hook (an overlay?). "
                                      L"Park, re-arm and F7 may not work; restart Exanima without it.");
            }
            if (i % 10 == 0 && slash) {
                Aim a;
                AcquireSRWLockShared(&aimLock);
                a = aim;
                ReleaseSRWLockShared(&aimLock);
                if (FILE* f = openStatus()) {
                    writeRunningStatus(f, a);
                    publishStatus(f);
                }
            }
        }
        InterlockedExchange(&stopping, 1);
        trackingReceiver.stop();
        setActivationPhase(fp::ActivationPhase::stopping);
        saveFov();
        releaseInput();
        const bool rawRetired = rawReconcileTarget(nullptr);
        nativeSettingsBeforeStop();
        if (!awaitNativeSettings()) {
            reportFault("native_settings_retirement_pending", "retained", "pending");
            return 7;
        }
        // Missing/minimized/teardown callbacks cannot make shutdown wait forever.
        // A timed-out module stays mapped with pass-through adapters and restoration
        // callbacks intact. Never free a trampoline another thread can still reach.
        if (!awaitRestore()) {
            const bool unknown = InterlockedCompareExchange(&restorationUnknown, 0, 0) != 0;
            reportFault(unknown ? "camera_owner_changed" : "restore_timeout", "retained",
                        unknown ? "unknown" : "pending");
            return 7;
        }
        if (!rawRetired && rawTarget) {
            reportFault("raw_retirement_pending", "retained", "restored");
            return 7;
        }
        if (!awaitCueRetirement()) {
            reportFault("cue_retirement_pending", "retained", "restored");
            return 7;
        }
        bool ok = rollbackDispatch(true, true, true);
        if (hookedWindow && originalWindowProc &&
            IsWindow(hookedWindow)) { // a destroyed window holds no subclass
            const auto current = (WNDPROC)GetWindowLongPtrW(hookedWindow, GWLP_WNDPROC);
            if (current == windowProc) {
                SetLastError(0);
                const auto previous =
                    SetWindowLongPtrW(hookedWindow, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
                ok = (previous != 0 || GetLastError() == 0) && ok;
            } else if (current != originalWindowProc)
                ok = false; // Another subclass still owns a callback chain.
        }
        InterlockedExchange(&restored, ok);
        if (!ok) {
            reportFault("stop_rollback_failed", "unknown", "restored");
            return 8;
        }
        if (InterlockedCompareExchange(&hookFault, 0, 0)) {
            reportFault("hook_not_firing", "removed", "restored",
                        FP_SHOWN_NAME L" stopped: this Exanima version does not run one of its hooks. "
                                      L"Overhead restored.");
            return 11;
        }
        const bool rearmable =
            fp::rearmAllowed(ok, ok, InterlockedCompareExchange(&restorationUnknown, 0, 0) != 0);
        {
            CameraGuard finalGuard;
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(module, path, MAX_PATH);
            auto p = wcsrchr(path, L'\\');
            if (p) {
                wcscpy_s(p + 1, MAX_PATH - (p + 1 - path), L"camera-calibration.json");
                FILE* f = nullptr;
                _wfopen_s(&f, path, L"w");
                if (f) {
                    fp::JsonOut j(f);
                    j.unum("samples", samples.load())
                        .unum("controls", controlCalls.load())
                        .unum("point_calls", pointCalls.load())
                        .unum("relaxed_steps", relaxedSteps.load())
                        .boolean("restored", ok)
                        .reals("head", lastHead, 12)
                        .reals("camera", lastCamera, 12);
                    j.finish();
                    fclose(f);
                }
            }
        }
        setActivationPhase(fp::ActivationPhase::stopped);
        if (slash) {
            FILE* f = openStatus();
            if (f) {
                fp::JsonOut j(f);
                writeActivationStatus(j);
                j.boolean("installed", false)
                    .str("state", "stopped")
                    .boolean("restored", ok);
                if (rearmable)
                    j.str("rearm_key", "F6");
                else
                    j.null("rearm_key");
                j.unum("f7_rearms", stopRearms.load());
                j.finish();
                publishStatus(f);
            }
        }
        // Keep the instance guard and all callback code mapped until process exit.
        if (!rearmable) return 0;
        showNotice(FP_SHOWN_NAME L" stopped (F7). Overhead restored. Press F6 to turn it back on.");
        waitForRearmKey();
        if (!rearm(executable)) return 10;
    }
}
// Pending native cleanup outlives a fault notice. Keep its worker even when
// the notice is absent/dismissed; never re-arm hooks from this completion path.
static void finishSessionWorker(bool (*noticePump)() = pumpNotice) {
    for (;;) {
        awaitNativeHookRecovery();
        const bool notice = noticePump();
        if (!notice && !nativeHookRecoveryPending()) return;
        hookRecoveryWait(50);
    }
}
static DWORD WINAPI run(void*) {
    const DWORD result = runSession();
    trackingReceiver.stop(); // includes early/fault exits and retained-hook sessions
    finishSessionWorker();
    return result;
}
// Started only by `enable` (our launcher calls it right after loading the DLL; EMTK's native plugin loader
// calls it when the mod is switched on), never by DllMain: a mod switched off in EMTK is loaded but must
// not run. Once started it is never started again (the session's own mutex refuses a second one anyway).
static volatile LONG sessionStarted = 0;
static LPTHREAD_START_ROUTINE sessionEntry = run;
extern "C" __declspec(dllexport) bool enable() {
    if (InterlockedCompareExchange(&sessionStarted, 1,
                                   0)) { // already started: re-arm only after EMTK switched it off
        if (InterlockedExchange(&emtkOff, 0)) InterlockedExchange(&rearmRequested, 1);
        return true;
    }
    setActivationPhase(fp::ActivationPhase::starting);
    HANDLE t = CreateThread(nullptr, 0, sessionEntry, nullptr, 0, nullptr);
    if (!t) {
        InterlockedExchange(&sessionStarted, 0);
        setActivationPhase(fp::ActivationPhase::idle);
        return false;
    }
    CloseHandle(t);
    return true;
}
// A launcher request has its own identity even when the DLL is already running.
// Clean F7 stops can re-arm; faults retain the worker's refusal and require restart.
extern "C" __declspec(dllexport) DWORD WINAPI enableWithStatus(void* request) {
    const uint64_t id = uint64_t(uintptr_t(request));
    if (!id) return 0;
    AcquireSRWLockExclusive(&activationLock);
    const fp::ActivationPhase phase = fp::effectiveActivationPhase(
        activationPhase, InterlockedCompareExchange(&stopping, 0, 0) != 0);
    const bool allowed = phase != fp::ActivationPhase::failed;
    if (allowed) activationRequest = id;
    ReleaseSRWLockExclusive(&activationLock);
    if (!allowed || !enable()) return 0;
    if (fp::activationNeedsRearm(phase)) InterlockedExchange(&rearmRequested, 1);
    return 1;
}
// EMTK's switch-off: the same stop as holding F7 (hooks back, cursor and keys released; the session
// thread then waits for enable or F6). The code stays mapped until the process ends: a game thread can be
// inside a trampoline, so the DLL is never unloaded.
// The launcher calls this before enable when the player chose to run on top of another mod's hooks
// ([Compat] OtherHooks=ask). It allows chaining for this game session only.
extern "C" __declspec(dllexport) bool allowOtherHooks() {
    InterlockedExchange(&playerApproved, 1);
    return true;
}
extern "C" __declspec(dllexport) bool disable() {
    if (!InterlockedCompareExchange(&sessionStarted, 0, 0)) return true;
    InterlockedExchange(&rearmRequested, 0);
    InterlockedExchange(&emtkOff, 1);
    InterlockedExchange(&stopping, 1);
    return true; // emtkOff first: a re-arm in flight sees it
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
