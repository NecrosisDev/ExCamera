// F8 settings: read and applied live, saved to first-person.ini.
// Part of first_person.cpp (one translation unit): included there, in order.
#include "../settings_model.h"
// The F8 settings live in the atomics the threads read; panel::ModSettings is their value as a whole, and
// panel::rowInfo says, per row, which field and which [section] key in first-person.ini. Reading, writing
// and comparing go through that table: a setting is described once.
// Revisions belong to settingsLock. Only the session worker writes INI rows;
// it snapshots one revision, releases the lock for disk I/O, then acknowledges
// that revision. An edit accepted during an old write remains pending.
static std::array<uint64_t, panel::setting::count> settingsRevision{}, savedSettingsRevision{};
static void queueFovSaveLocked() { ++settingsRevision[panel::setting::fov]; }
static bool settingsSavePendingLocked() {
    return settingsRevision != savedSettingsRevision;
}
static bool settingsSavePending() {
    SettingsGuard guard;
    return settingsSavePendingLocked();
}
static panel::ModSettings readLiveSettings() { // caller holds settingsLock
    panel::ModSettings s;
    s.fov = selectedFov.load();
    s.sensitivity = int(std::lround(sensitivity.load() / 0.0025f * 100.f));
    s.invertY = invertY.load();
    s.indicator = aimIndicator.load();
    s.headSteady = headSteady.load();
    s.rawMouse = rawMouse;
    s.rawMouseFallback = rawMouse && !rawWanted.load();
    s.savePending = settingsSavePendingLocked();
    s.hideHelmet = helmetMode.load() == fp::HelmetMode::hide;
    s.grabRange = grabRange.load();
    s.hideSwingCrosshair = hideSwingCrosshair.load();
    s.idleCrosshairAlways = idleCrosshairAlways.load();
    s.aimMarker = aimMarker.load();
    s.flickCatchup = flickCatchup.load();
    s.tpDistance = tpDistance.load();
    s.tpShoulder = tpShoulder.load();
    s.tpHeight = tpHeight.load();
    s.tpTight = tpTight.load();
    s.tpCombat = tpCombat.load();
    s.tpFov = tpFov.load();
    s.tpLag = tpLag.load();
    s.tpBlockers = tpBlockers.load();
    s.tpDown = tpDown.load();
    s.tpCombatAimFree = tpCombatAimFree.load();
    s.attackCue = attackCue.load();
    s.cueSize = cueSize.load();
    s.cueOpacity = cueOpacity.load();
    s.cueThrust = cueThrust.load();
    s.fpDefense = fpManualGuard.load();
    s.tpDefense = tpManualGuard.load();
    s.fpSwingSelection = fpSwingSelection.load();
    s.tpSwingSelection = tpSwingSelection.load();
    s.manualDefenseAvailable = manualDefenseReady.load();
    s.rmbMode = rmbMode.load();
    s.freezeWindows = windowsFreeze.load();
    s.crouchMode = crouchMode.load();
    s.nearClip = nearClip.load();
    s.headRoll = headSteadyRoll.load();
    s.headPitch = headSteadyPitch.load();
    s.headYaw = headSteadyYaw.load();
    s.headBob = headBob.load();
    s.spineFollow = spineFollow.load();
    s.fpDeadzoneX = fpDeadzoneX.load();
    s.fpDeadzoneY = fpDeadzoneY.load();
    s.tpDeadzoneX = tpDeadzoneX.load();
    s.tpDeadzoneY = tpDeadzoneY.load();
    s.leanKeysMode = leanKeysMode.load();
    s.leanLeftKey = leanLeftKey.load();
    s.leanRightKey = leanRightKey.load();
    s.eyeSetback = eyeSetback.load();
    s.rotateHeldKey = rotateHeldKey.load();
    s.leanKey = leanKey.load();
    s.holdHeading = holdHeadingOn.load();
    s.autoRecenter = aimAutoRecenter.load();
    s.edgeSoftness = aimEdgeSoftness.load();
    s.tracking = trackingEnabled.load();
    s.trackingKey = trackingRecenterKey.load();
    s.trackingStatus = int(trackingReceiver.status());
    if (s.trackingStatus == 2) {
        const auto sample = trackingReceiver.snapshot();
        const auto now = GetTickCount64();
        if (!sample.valid || now < sample.received || now - sample.received > fp::trackingStaleMs)
            s.trackingStatus = 1;
    }
    return s;
}
static panel::ModSettings currentSettings() { // a coherent display snapshot (any thread)
    SettingsGuard guard;
    return readLiveSettings();
}
static void
writeLiveSettings(const panel::ModSettings& requested) {
    auto s = requested;
    s.normalize(); // also enforced for bulk/internal callers, not just UI and INI loading
    const bool policyChanged = fpManualGuard.load() != s.fpDefense || tpManualGuard.load() != s.tpDefense ||
                               rmbMode.load() != s.rmbMode || rotateHeldKey.load() != s.rotateHeldKey ||
                               leanKey.load() != s.leanKey || leanKeysMode.load() != s.leanKeysMode ||
                               leanLeftKey.load() != s.leanLeftKey || leanRightKey.load() != s.leanRightKey ||
                               crouchMode.load() != s.crouchMode ||
                               fpSwingSelection.load() != s.fpSwingSelection ||
                               tpSwingSelection.load() != s.tpSwingSelection;
    fpManualGuard = s.fpDefense;
    tpManualGuard = s.tpDefense;
    fpSwingSelection = s.fpSwingSelection;
    tpSwingSelection = s.tpSwingSelection;
    selectedFov.store(s.fov);
    sensitivity = 0.0025f * float(s.sensitivity) / 100.f;
    invertY = s.invertY;
    aimIndicator = s.indicator;
    headSteady = s.headSteady;
    if (rawMouse != s.rawMouse) rawWanted = s.rawMouse;
    rawMouse = s.rawMouse;
    helmetMode = s.hideHelmet ? fp::HelmetMode::hide : fp::HelmetMode::show;
    grabRange = s.grabRange;
    hideSwingCrosshair = s.hideSwingCrosshair;
    idleCrosshairAlways = s.idleCrosshairAlways;
    aimMarker = s.aimMarker;
    flickCatchup = s.flickCatchup;
    tpDistance = s.tpDistance;
    tpShoulder = s.tpShoulder;
    tpHeight = s.tpHeight;
    tpTight = s.tpTight;
    tpCombat = s.tpCombat;
    tpFov = s.tpFov;
    tpLag = s.tpLag;
    tpBlockers = s.tpBlockers;
    tpDown = s.tpDown;
    tpCombatAimFree = s.tpCombatAimFree;
    attackCue = s.attackCue;
    cueSize = s.cueSize;
    cueOpacity = s.cueOpacity;
    cueThrust = s.cueThrust;
    rmbMode = s.rmbMode;
    windowsFreeze = s.freezeWindows;
    crouchMode = s.crouchMode;
    nearClip = s.nearClip;
    headSteadyRoll = s.headRoll;
    headSteadyPitch = s.headPitch;
    headSteadyYaw = s.headYaw;
    headBob = s.headBob;
    spineFollow = s.spineFollow;
    fpDeadzoneX = s.fpDeadzoneX;
    fpDeadzoneY = s.fpDeadzoneY;
    tpDeadzoneX = s.tpDeadzoneX;
    tpDeadzoneY = s.tpDeadzoneY;
    leanKeysMode = s.leanKeysMode;
    leanLeftKey = s.leanLeftKey;
    leanRightKey = s.leanRightKey;
    eyeSetback = s.eyeSetback;
    rotateHeldKey = s.rotateHeldKey;
    leanKey = s.leanKey;
    holdHeadingOn = s.holdHeading;
    aimAutoRecenter = s.autoRecenter;
    aimEdgeSoftness = s.edgeSoftness;
    if (trackingEnabled.exchange(s.tracking) != s.tracking) {
        trackingAcceptAfter = GetTickCount64();
        ++trackingReset;
    }
    trackingRecenterKey = s.trackingKey;
    if (policyChanged) inputOwner.policyChanged();
}
static void makeLive(const panel::ModSettings& s) {
    SettingsGuard guard;
    writeLiveSettings(s);
}
// first-person.ini -> settings: a missing key, or a word the row does not know, keeps the default.
static panel::ModSettings readSettings(const wchar_t* path) {
    panel::ModSettings s;
    for (int r = 0; r < panel::setting::count; ++r) {
        const panel::RowInfo& i = panel::rowInfo(r);
        if (!i.key) continue;
        wchar_t text[64];
        GetPrivateProfileStringW(i.section, i.key, L"", text, 64, path);
        if (!text[0]) continue;
        int v = panel::badText;
        if (i.fromText)
            v = i.fromText(text);
        else {
            wchar_t* end = nullptr;
            const long n = wcstol(text, &end, 10);
            const bool parsed = end != text;
            while (end && (*end == L' ' || *end == L'\t')) ++end;
            if (parsed && end && !*end) v = int(n);
        }
        if (v == panel::badText) continue;
        if (i.value)
            s.*i.value = v;
        else if (i.flag)
            s.*i.flag = v != 0;
    }
    {
        wchar_t t[8];
        GetPrivateProfileStringW(L"ThirdPerson", L"Blockers", L"", t, 8,
                                 path); // before Blockers: HideBlockers, 1 hide, 0 pull in
        const int legacy = GetPrivateProfileIntW(L"ThirdPerson", L"HideBlockers", -1, path);
        if (!t[0] && legacy >= 0) s.tpBlockers = legacy ? fp::blockersHide : fp::blockersPush;
    }
    wchar_t indicatorText[24]{};
    GetPrivateProfileStringW(L"View", L"AimIndicator", L"", indicatorText, 24, path);
    if (!indicatorText[0]) {
        wchar_t legacy[24]{};
        GetPrivateProfileStringW(L"View", L"IdleCrosshair", L"", legacy, 24, path);
        s.indicator = panel::iniIs(legacy, L"always") ? 2 :
                      GetPrivateProfileIntW(L"Input", L"AimMarker", 1, path) ? 1 : 0;
    }
    // Prefer the FP policy when a legacy profile split the views; use TP only if FP is absent.
    for (const auto key : {L"Defense", L"SwingSelection"}) {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"FirstPerson", key, L"", value, 32, path);
        if (value[0]) continue;
        GetPrivateProfileStringW(L"ThirdPerson", key, L"", value, 32, path);
        if (!value[0]) continue;
        if (key[0] == L'D') s.fpDefense = fp::defenseModeFromText(value);
        else s.fpSwingSelection = panel::iniSwingSelection(value);
    }
    s.normalize();
    return s;
}
static void loadSettings(const wchar_t* path) {
    makeLive(readSettings(path));
}
static bool saveSetting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return settingsPath[0] && WritePrivateProfileStringW(section, key, value, settingsPath);
}
static bool saveRow(int r, const panel::ModSettings& s) {
    const panel::RowInfo& i = panel::rowInfo(r);
    if (!i.key) return true;
    const int v = i.value ? s.*i.value : (i.flag && s.*i.flag ? 1 : 0);
    if (i.toText) return saveSetting(i.section, i.key, i.toText(v));
    wchar_t t[16];
    swprintf_s(t, L"%d", v);
    return saveSetting(i.section, i.key, t);
}
// Caller owns settingsLock. Publish accepted state immediately; never perform
// disk I/O or wait for the session worker from a settings callback.
static bool commitSettings(const panel::ModSettings& s, const panel::ModSettings& was) {
    writeLiveSettings(s);
    for (int r = 0; r < panel::setting::count; ++r)
        if (panel::rowInfo(r).key && !panel::sameRow(r, s, was)) ++settingsRevision[r];
    return !settingsSavePendingLocked();
}
// Session worker only: the one persistence owner, including shutdown flushes.
// Never call this from Apply or from a second background writer.
static void persistSettingsRow(unsigned row) {
    panel::ModSettings snapshot;
    uint64_t revision;
    {
        SettingsGuard guard;
        revision = settingsRevision[row];
        if (revision == savedSettingsRevision[row]) return;
        snapshot = readLiveSettings();
    }
    const bool written = saveRow(int(row), snapshot); // no runtime lock across disk I/O
    if (written) {
        SettingsGuard guard;
        savedSettingsRevision[row] = revision; // not the potentially newer live revision
        if (row == panel::setting::fov) persistedFov = snapshot.fov;
    }
}
static void persistSettingsRows(unsigned budget) {
    if (!settingsPath[0] || !budget) return;
    persistSettingsRow(panel::setting::fov); // preserve prompt FOV hotkey persistence
    static unsigned next = 0; // worker-owned round-robin cursor
    unsigned attempted = 0;
    for (unsigned scanned = 0; scanned < panel::setting::count && attempted < budget; ++scanned) {
        const unsigned row = next;
        next = (next + 1) % panel::setting::count;
        if (row == panel::setting::fov) continue;
        {
            SettingsGuard guard;
            if (settingsRevision[row] == savedSettingsRevision[row]) continue;
        }
        ++attempted;
        persistSettingsRow(row);
    }
}
static void flushSettings() { persistSettingsRows(panel::setting::count); }
static void awaitRawRegistration(bool enabling) {
    for (int i = 0; i < 20 && enabling && !InterlockedCompareExchange(&rawRegistered, 0, 0) && rawWanted.load();
         ++i)
        Sleep(25); // the worker registers within 100 ms
}
// Native Apply owns a draft. Merge only edited rows into the latest state so a
// menu opened before a FOV key or raw-input fallback cannot overwrite that work.
static void commitSettingsDraft(const panel::ModSettings& initial, const panel::ModSettings& draft) {
    SettingsGuard guard;
    const auto live = readLiveSettings();
    auto merged = live;
    for (int row = 0; row < panel::setting::count; ++row) {
        if (panel::sameRow(row, initial, draft)) continue;
        const auto& info = panel::rowInfo(row);
        if (!info.key) continue;
        if (info.value) merged.*info.value = draft.*info.value;
        if (info.flag) merged.*info.flag = draft.*info.flag;
    }
    merged.normalize();
    commitSettings(merged, live);
    // Registration remains asynchronous on its existing worker. GUI maintenance
    // must never wait for that worker while it owns the native Settings task.
}
// Bulk configuration remains available to the owned tests and explicit callers.
// Interactive controls use applySetting below, which never accepts a stale image.
[[maybe_unused]] static panel::ModSettings applySettings(const panel::ModSettings& want) {
    panel::ModSettings s = want;
    s.normalize();
    bool enablingRaw;
    {
        SettingsGuard guard;
        const panel::ModSettings was = readLiveSettings();
        enablingRaw = s.rawMouse && !was.rawMouse;
        commitSettings(s, was);
    }
    awaitRawRegistration(enablingRaw); // worker reconciliation needs settingsLock too
    return currentSettings();
}
// From the F8 window: choose the latest state and apply one row action while the
// same lock excludes FOV keys, bulk changes, and raw fallback.
[[maybe_unused]] static panel::SettingsChange applySetting(int row, int delta, int group) {
    panel::SettingsChange result;
    bool enablingRaw = false;
    {
        SettingsGuard guard;
        const panel::ModSettings was = readLiveSettings();
        result.settings = was;
        if (row != panel::ModSettings::resetControl && was.inactiveReason(row)) return result;
        result.changed = result.settings.click(row, delta, group);
        if (!result.changed) return result;
        result.settings.normalize();
        enablingRaw = result.settings.rawMouse && !was.rawMouse;
        result.saved = commitSettings(result.settings, was);
    }
    awaitRawRegistration(enablingRaw);
    result.settings = currentSettings();
    result.rawUnavailable = enablingRaw && result.settings.rawMouseFallback;
    return result;
}
