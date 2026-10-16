// The session: installing the hooks, the game build and other mods' hooks, F7/F6, the status file, the worker loop, the DLL's exports.
// Part of first_person.cpp (one translation unit): included there, in order.
static void rawReconcile();
static bool arm(const char*& reason, const char*& hooks) {
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        const bool rollback = MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
        reason = rollback ? "hook_enable_failed" : "hook_rollback_failed";
        hooks = rollback ? "removed" : "unknown";
        return false;
    }
    const uintptr_t slot = gameAt(fp::sig::navSlot);
    bool installedBehavior = replace(gameAt(fp::sig::behaviorSlot), (void*)originalBehavior, (void*)behavior);
    bool installedPoint =
        installedBehavior && replace(gameAt(fp::sig::pointSlot), (void*)originalPoint, (void*)point);
    bool installedNav = installedPoint && replace(slot, (void*)original, (void*)nav);
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
    rawWanted = rawMouse;
    rawReconcile();
    return true;
}
// Raw mouse input follows [Input] RawMouse / the F8 setting and the game window. left
// it registered to a window the game had destroyed: no more
// WM_INPUT, and look froze with raw on. Never replaces another registration of this process (an
// overlay); when it cannot register, look uses cursor deltas as before.
static void rawReconcile() {
    const bool want = rawWanted.load() && hookedWindow && IsWindow(hookedWindow);
    const bool have = InterlockedCompareExchange(&rawRegistered, 0, 0) != 0;
    if (have && (!want || rawTarget != hookedWindow)) {
        RAWINPUTDEVICE rid{1, 2, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        InterlockedExchange(&rawRegistered, 0);
        rawTarget = nullptr;
        AcquireSRWLockExclusive(&rawLock);
        rawLook = fp::RawLook{};
        ReleaseSRWLockExclusive(&rawLock);
    }
    if (!want || InterlockedCompareExchange(&rawRegistered, 0, 0)) return;
    UINT count = 0;
    bool mouseTaken = false;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == 0 && count) {
        RAWINPUTDEVICE existing[16];
        UINT n = count < 16 ? count : 16;
        if (count > 16 || GetRegisteredRawInputDevices(existing, &n, sizeof(RAWINPUTDEVICE)) == UINT(-1))
            mouseTaken = true;
        else
            for (UINT i = 0; i < n; ++i)
                if (existing[i].usUsagePage == 1 && existing[i].usUsage == 2) mouseTaken = true;
    }
    RAWINPUTDEVICE rid{1, 2, 0, hookedWindow};
    if (!mouseTaken && RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        rawTarget = hookedWindow;
        InterlockedExchange(&rawRegistered, 1);
    } else
        rawWanted = false; // not possible here: cursor deltas, and the settings page shows it off
}
// Risk audit item 4: if Exanima destroys its window (display mode
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
// Resolves the layout from the game's file. On failure the unresolved targets go to the status file.
static bool resolveGame(const wchar_t* executable) {
    std::vector<uint8_t> mapped;
    fp::ImageView file;
    fp::ResolveReport report;
    if (!readGameFile(executable, mapped, file)) {
        snprintf(faultDetail, sizeof faultDetail, "Exanima.exe could not be read");
        return false;
    }
    if (fp::resolveLayout(file, layout, report)) return true;
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
static bool slotTaken(fp::sig::Target slot, fp::sig::Target holds) {
    return *(uintptr_t*)gameAt(slot) != gameAt(holds);
}
// True to go on: nothing else hooked, or chaining is allowed. The function entries are compared with the
// game's file; the slots with the game's own functions.
static bool otherHooksCheck(const wchar_t* executable) {
    std::vector<uint8_t> mapped;
    fp::ImageView file;
    if (!readGameFile(executable, mapped, file)) {
        snprintf(faultDetail, sizeof faultDetail, "Exanima.exe could not be read");
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
    original = (Nav)n;
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
// first-person-status.json while running (the worker, every second). The launcher reads pid, revision,
// build and installed from it.
static void writeRunningStatus(FILE* f, const Aim& a) {
    fp::JsonOut j(f);
    j.unum("pid", GetCurrentProcessId());
    j.boolean("installed", true);
    j.num("revision", FP_REVISION);
    j.str("build", FP_BUILD);
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
    j.unum("unexpected_relaxed_steps", unexpectedRelaxedSteps.load());
    j.num("fov", selectedFov.load());
    j.boolean("settings_saved", persistedFov == selectedFov.load());
    j.num("cursor_free", InterlockedCompareExchange(&cursorFree, 0, 0));
    j.num("raw_mouse", InterlockedCompareExchange(&rawRegistered, 0, 0));
    j.unum("side_flips", sideFlips.load());
    j.str("helmet", helmetMode.load() == fp::HelmetMode::hide ? "hide" : "show");
    j.unum("head_wear_nodes", headWearNodes.load());
    j.unum("hidden_head_nodes", hiddenHeadNodes.load());
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
    j.unum("body_wear", bodyWearNodes.load());
    j.unum("hidden_body", hiddenBodyNodes.load());
    j.str("view", viewMode.load() == 1 ? "third" : viewMode.load() == 2 ? "to-overhead" : "first");
    j.unum("tp_prop_pulls", tpPropPulls.load());
    j.object("terrain_live")
        .unum("tiles", terrainTilesApplied.load())
        .unum("wakes", terrainWakes.load())
        .unum("light_marks", terrainLightMarks.load())
        .unum("slow_paint", terrainSlowPaint.load())
        .end();
    j.str("game_build", verifiedBuild ? "verified" : "compatible");
    j.str("hook_health", hookHealthText());
    {
        char hooks[200];
        snprintf(hooks, sizeof hooks, "%s%s", chained ? "chained:" : "none", chained ? otherHooksNames : "");
        j.str("other_hooks", hooks);
    }
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
                userSettingsFile(settingsPath, n && n < MAX_PATH ? userSettingsDir(profile) : std::wstring());
            if (user.size() < MAX_PATH) wcscpy_s(settingsPath, user.c_str());
        }
        wchar_t value[64], *end = nullptr;
        GetPrivateProfileStringW(L"Camera", L"FOV", L"75", value, 64, settingsPath);
        long parsed = wcstol(value, &end, 10);
        const bool hasDigits = end != value;
        while (end && (*end == L' ' || *end == L'\t')) ++end;
        persistedFov = (!hasDigits || !end || *end) ? 75 : fp::clampFov(int(parsed));
        selectedFov.store(persistedFov);
        const int sensitivityPercent =
            GetPrivateProfileIntW(L"Input", L"SensitivityPercent", 100, settingsPath);
        sensitivity =
            0.0025f *
            float(sensitivityPercent < 10 ? 10 : (sensitivityPercent > 400 ? 400 : sensitivityPercent)) /
            100.f;
        invertY = GetPrivateProfileIntW(L"Input", L"InvertY", 0, settingsPath) != 0;
        rawMouse = GetPrivateProfileIntW(L"Input", L"RawMouse", 0, settingsPath) != 0;
        attackSideKey = GetPrivateProfileIntW(L"Input", L"AttackSideKey", 0, settingsPath);
        grabRange = fp::clampGrabRange(
            GetPrivateProfileIntW(L"Input", L"GrabRange", fp::grabRangeDefault, settingsPath));
        {
            wchar_t rm[16];
            GetPrivateProfileStringW(L"Input", L"RightMouseFree", L"hold", rm, 16, settingsPath);
            rmbMode = fp::rmbModeFromText(rm);
        }
        {
            wchar_t cm[16];
            GetPrivateProfileStringW(L"Input", L"Crouch", L"toggle", cm, 16, settingsPath);
            crouchMode = fp::crouchModeFromText(cm);
        }
        {
            auto clampI = [](int v, int lo, int hi) {
                return v < lo ? lo : (v > hi ? hi : v);
            };
            tpDistance = clampI(GetPrivateProfileIntW(L"ThirdPerson", L"Distance", 250, settingsPath),
                                fp::tpDistanceMin, fp::tpDistanceMax);
            tpShoulder = clampI(GetPrivateProfileIntW(L"ThirdPerson", L"Shoulder", 40, settingsPath),
                                -fp::tpShoulderMax, fp::tpShoulderMax);
            tpHeight = clampI(GetPrivateProfileIntW(L"ThirdPerson", L"Height", 15, settingsPath),
                              fp::tpHeightMin, fp::tpHeightMax);
            tpTight = GetPrivateProfileIntW(L"ThirdPerson", L"TightSpaces", 1, settingsPath) != 0;
            tpCombat = GetPrivateProfileIntW(L"ThirdPerson", L"CombatFraming", 1, settingsPath) != 0;
            tpLag = clampI(GetPrivateProfileIntW(L"ThirdPerson", L"Lag", 40, settingsPath), 0, 100);
            tpHide = GetPrivateProfileIntW(L"ThirdPerson", L"HideBlockers", 1, settingsPath) != 0;
            tpDown = GetPrivateProfileIntW(L"ThirdPerson", L"KnockdownFollow", 1, settingsPath) != 0;
            {
                const int f = GetPrivateProfileIntW(L"ThirdPerson", L"Fov", 0, settingsPath);
                tpFov = f <= 0 ? 0 : fp::clampFov(f);
            }
        }
        {
            auto axis = [&](const wchar_t* key, int fallback) {
                const int v = GetPrivateProfileIntW(L"Camera", key, fallback, settingsPath);
                return v < 0 ? 0 : (v > 100 ? 100 : v);
            };
            headSteadyRoll = axis(L"HeadRoll", 80);
            headSteadyPitch = axis(L"HeadPitch", 70);
            headSteadyYaw = axis(L"HeadYaw", 50);
            headBob = axis(L"HeadBobSteady", 30);
            spineFollow = axis(L"SpineFollowsAim", 60);
        }
        nearClip = fp::clampNearClip(
            GetPrivateProfileIntW(L"Camera", L"NearClip", fp::nearClipDefaultTenths, settingsPath));
        {
            const int s = GetPrivateProfileIntW(L"Camera", L"HeadStabilisation", 70, settingsPath);
            headSteady = s < 0 ? 0 : (s > 100 ? 100 : s);
        }
        {
            wchar_t sc[16];
            GetPrivateProfileStringW(L"View", L"SwingCrosshair", L"show", sc, 16, settingsPath);
            hideSwingCrosshair = _wcsicmp(sc, L"show") != 0;
        }
        {
            wchar_t ic[16];
            GetPrivateProfileStringW(L"View", L"IdleCrosshair", L"rmb", ic, 16, settingsPath);
            idleCrosshairAlways = _wcsicmp(ic, L"always") == 0;
        }
        windowsFreeze = GetPrivateProfileIntW(L"View", L"FreezeForWindows", 1, settingsPath) != 0;
        aimMarker = GetPrivateProfileIntW(L"Input", L"AimMarker", 1, settingsPath) != 0;
        {
            const int f = GetPrivateProfileIntW(L"Input", L"FlickCatchup", 50, settingsPath);
            flickCatchup = f < 0 ? 0 : (f > 100 ? 100 : f);
        }
        probeKey = GetPrivateProfileIntW(L"Input", L"ProbeKey", 0, settingsPath);
        {
            wchar_t oh[16];
            GetPrivateProfileStringW(L"Compat", L"OtherHooks", L"ask", oh, 16, settingsPath);
            otherHooks = fp::otherHooksFromText(oh);
        }
        if (probeKey < 0 || probeKey > 254) probeKey = 0;
        {
            wchar_t helmet[16];
            GetPrivateProfileStringW(L"View", L"Helmet", L"show", helmet, 16, settingsPath);
            helmetMode = fp::helmetModeFromText(helmet);
        } // HideHelmet is no longer read
        attackCue = GetPrivateProfileIntW(L"View", L"AttackCue", 1, settingsPath) != 0;
        {
            const int z = GetPrivateProfileIntW(L"View", L"CueSize", 100, settingsPath),
                      o = GetPrivateProfileIntW(L"View", L"CueOpacity", 100, settingsPath);
            cueSize = z < 50 ? 50 : (z > 200 ? 200 : z);
            cueOpacity = o < 20 ? 20 : (o > 100 ? 100 : o);
            cueThrust = GetPrivateProfileIntW(L"View", L"CueThrust", 1, settingsPath) != 0;
        }
        if (!fp::attackSideKeyValid(attackSideKey)) attackSideKey = 0; // never a mouse button
    } else
        settingsPath[0] = 0;
    if (!waitForGame()) {
        reportFault("game_not_ready", "not_installed", "not_required");
        return 2;
    }
    base = (uintptr_t)GetModuleHandleW(nullptr);
    original = (Nav)(gameAt(fp::sig::navSampler));
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
    if (MH_CreateHook((void*)(gameAt(fp::sig::cursorAim)), (void*)cursorAim, (void**)&originalCursor) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::explore)), (void*)explore, (void**)&originalExplore) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::combatStep)), (void*)step, (void**)&originalStep) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::poseBuilder)), (void*)poseBuilder, (void**)&originalPose) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::partSettle)), (void*)partSettle, (void**)&originalPartSettle) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::cursorPosition)), (void*)cursorPosition,
                      (void**)&originalCursorPosition) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::pollInput)), (void*)input, (void**)&originalInput) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::travelTarget)), (void*)travelTarget,
                      (void**)&originalTravelTarget) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::walkGait)), (void*)walkGait, (void**)&originalWalkGait) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::sectorUpdate)), (void*)sectorUpdate,
                      (void**)&originalSectorUpdate) != MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::coverPass)), (void*)coverPass, (void**)&originalCoverPass) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::coverDepth)), (void*)coverDepth, (void**)&originalCoverDepth) !=
            MH_OK ||
        MH_CreateHook((void*)(gameAt(fp::sig::dragPlace)), (void*)dragPlace, (void**)&originalDragPlace) !=
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
        MH_CreateHook((void*)(gameAt(fp::sig::hingeHandler)), (void*)hingeHandler,
                      (void**)&originalHingeHandler) != MH_OK ||
        MH_CreateHook((void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglSwapBuffers"), (void*)swap,
                      (void**)&originalSwap) != MH_OK) {
        MH_Uninitialize();
        reportFault("hook_creation_failed", "not_installed", "not_required");
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
        runtimeState = "running";
        for (int i = 0; !InterlockedCompareExchange(&stopping, 0, 0); i++) {
            Sleep(100);
            saveFov();
            if (!viewFaultNoticed && viewFault.load()) {
                viewFaultNoticed = true;
                showNotice(
                    FP_SHOWN_NAME L": view feature error. Helmet handling stopped; first person continues.");
            }
            stopKeyTick();
            hookHealthTick();
            pumpNotice();
            { // F8's window: in play, and at the title (no player character for 2 s).
                const bool title = atTitle();
                atTitleNow = title;
                titleSettled = title && noPlayerSince && GetTickCount64() - noPlayerSince >= 10000;
                {
                    char pending[17]{};
                    memcpy(pending, (const void*)(gameAt(fp::sig::pendingLevelName)),
                           16); // a load in flight changes it (restore waits 10 s after the last change)
                    if (memcmp(pending, pendingLevel, 16)) {
                        memcpy(pendingLevel, pending, 17);
                        pendingLevelSince = GetTickCount64();
                    }
                }
                if (viewMode.load() == 2 && GetTickCount64() - tpBlendStart.load() > 600) {
                    viewMode = 0;
                    InterlockedExchange(&active, 0);
                    AcquireSRWLockExclusive(&aimLock);
                    aim.valid = false;
                    ReleaseSRWLockExclusive(&aimLock);
                } // no nav frame ended the blend (parked, loading): end it as F6 did
                if (!title && f8OpenedAtTitle) {
                    f8OpenedAtTitle = false;
                    InterlockedExchange(&cursorFree, 0);
                } // opened at the title: closed when play starts
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
        atTitleNow = false;
        titleSettled = false; // no stale "main menu" after a stop
        saveFov();
        releaseInput();
        if (InterlockedExchange(&rawRegistered, 0)) {
            RAWINPUTDEVICE rid{1, 2, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            rawTarget = nullptr;
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
        bool ok = rollbackDispatch(true, true, true);
        // Ceilings=1: lids built into this level cannot be unbuilt, so the per-sector
        // update hook alone stays on with every built slab hidden (overhead shows no
        // lid) until the scene changes (maskSector ignores other scenes).
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
        if (InterlockedCompareExchange(&cameraFault, 0, 0)) {
            reportFault("camera_source_lost", "removed", "restored");
            return 9;
        }
        if (InterlockedCompareExchange(&hookFault, 0, 0)) {
            reportFault("hook_not_firing", "removed", "restored",
                        FP_SHOWN_NAME L" stopped: this Exanima version does not run one of its hooks. "
                                      L"Overhead restored.");
            return 11;
        }
        const bool rearmable = fp::rearmAllowed(ok, ok, InterlockedCompareExchange(&cameraFault, 0, 0) != 0,
                                                InterlockedCompareExchange(&restorationUnknown, 0, 0) != 0);
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
        if (slash) {
            FILE* f = openStatus();
            if (f) {
                fp::JsonOut j(f);
                j.unum("pid", GetCurrentProcessId())
                    .num("revision", FP_REVISION)
                    .str("build", FP_BUILD)
                    .boolean("installed", false)
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
// A fault notice outlives the session loop: keep pumping it on this thread.
static DWORD WINAPI run(void*) {
    const DWORD result = runSession();
    while (pumpNotice()) Sleep(50);
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
    HANDLE t = CreateThread(nullptr, 0, sessionEntry, nullptr, 0, nullptr);
    if (!t) {
        InterlockedExchange(&sessionStarted, 0);
        return false;
    }
    CloseHandle(t);
    return true;
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
