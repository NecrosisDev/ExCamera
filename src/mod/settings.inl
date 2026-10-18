// F8 settings: read and applied live, saved to first-person.ini.
// Part of first_person.cpp (one translation unit): included there, in order.
#include "../settings_window.h"
// The F8 settings live in the atomics the threads read; panel::ModSettings is their value as a whole, and
// panel::rowInfo says, per row, which field and which [section] key in first-person.ini. Reading, writing
// and comparing go through that table: a setting is described once.
// Only row identities are queued: every retry reads the latest live value under
// settingsLock, so an older failed save cannot overwrite a newer edit.
static std::array<bool, panel::setting::count> pendingSettingsRows{};
static bool settingsSavePendingLocked() {
    return selectedFov.load() != persistedFov.load() ||
           std::any_of(pendingSettingsRows.begin(), pendingSettingsRows.end(), [](bool dirty) { return dirty; });
}
static bool settingsSavePending() {
    SettingsGuard guard;
    return settingsSavePendingLocked();
}
static panel::ModSettings readLiveSettings() { // caller holds settingsLock
    panel::ModSettings s;
    s.fov = selectedFov.load();
    s.sensitivity = int(std::lround(sensitivity.load() / 0.0025f * 100.f));
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
    s.turnLead = turnLeadOn.load();
    return s;
}
static panel::ModSettings currentSettings() { // a coherent display snapshot (any thread)
    SettingsGuard guard;
    return readLiveSettings();
}
static void
writeLiveSettings(const panel::ModSettings& s) { // caller holds settingsLock; s is normalized
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
    if (!s.rmbMode) InterlockedExchange(&rmbFree, 0);
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
    turnLeadOn = s.turnLead;
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
// Caller holds settingsLock through persistence and publication. A worker save
// must never finish after a newer panel write and mark the older FOV as saved.
static bool commitSettings(const panel::ModSettings& s, const panel::ModSettings& was) {
    bool saved = true;
    for (int r = 0; r < panel::setting::count; ++r)
        if (!panel::sameRow(r, s, was) || pendingSettingsRows[r]) {
            const bool written = saveRow(r, s);
            saved = written && saved;
            if (r == panel::setting::fov && written) persistedFov = s.fov;
            if (r != panel::setting::fov) pendingSettingsRows[r] = !written;
        }
    writeLiveSettings(s);
    return saved;
}
// The worker retries at most `budget` rows per tick, in round-robin order. FOV
// keeps its existing selected/persisted marker and is saved by saveFov itself.
static void retrySettingsRowsLocked(unsigned budget) {
    if (!settingsPath[0] || !budget) return;
    static unsigned next = 0;
    const panel::ModSettings current = readLiveSettings();
    unsigned attempted = 0;
    for (unsigned scanned = 0; scanned < panel::setting::count && attempted < budget; ++scanned) {
        const unsigned row = next;
        next = (next + 1) % panel::setting::count;
        if (!pendingSettingsRows[row]) continue;
        ++attempted;
        if (saveRow(int(row), current)) pendingSettingsRows[row] = false;
    }
}
static void awaitRawRegistration(bool enabling) {
    for (int i = 0; i < 20 && enabling && !InterlockedCompareExchange(&rawRegistered, 0, 0) && rawWanted.load();
         ++i)
        Sleep(25); // the worker registers within 100 ms
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
// same lock excludes FOV keys, bulk changes, raw fallback, and persistence.
static panel::SettingsChange applySetting(int row, int delta, int group) {
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
