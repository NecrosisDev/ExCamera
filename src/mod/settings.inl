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
    s.sensitivity = static_cast<int>(std::lround(sensitivity.load() / 0.0025f * 100.f));
    s.invertY = invertY.load();
    s.indicator = aimIndicator.load();
    // View effects keep a 0..1 strength (view_effects.inl); these rows store it as before.
    s.ambientOcclusion = int(std::lround(ambientOcclusion.load() * 100.f)); // percent, as the sliders store it
    s.depthOfField = int(std::lround(depthOfField.load() * 100.f));
    s.screenSpaceReflections = int(std::lround(screenSpaceReflections.load() * 100.f));
    s.edgeSmoothing = edgeSmoothing.load() > 0.f;
    s.helmetVisor = helmetVisor.load() > 0.f;
    s.playerGhost = playerGhost.load() > 0.f;
    s.fingerPoses = fingerPoses.load() != 0;
    s.fingerPosesOthers = fingerPosesOthers.load() != 0;
    s.conditionEffects = conditionEffects.load() > 0.f;
    s.heldDetail = int(std::lround(heldDetail.load() * 100.f));
    s.armourDetail = int(std::lround(armourDetail.load() * 100.f));
    s.objectDetail = int(std::lround(objectDetail.load() * 100.f));
    s.levelDetail = int(std::lround(levelDetail.load() * 100.f));
    s.drawDistance = drawDistanceM.load();
    s.objectLights = objectLightCap.load();
    s.woundMarks = woundMarks.load() > 0.f;
    s.lightSmoothing = lightSmoothing.load();
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
    s.vrIpd = vrIpdMm.load();
    s.vrScale = vrUnitsPerMetre.load();
    s.vrRenderMode = vrRenderMode.load();
    s.vrHands = vrHandsDrawn.load();
    s.vrPosture = vrPosture.load();
    s.vrHeadset = vrHeadset.load();
    s.vrTurning = vrTurning.load();
    s.vrEffects = vrEffects.load();
    s.vrGripAngle = vrGripAngle.load();
    s.vrHaptics = vrHapticPercent.load();
    s.vrPanelWidth = vrPanelWidthCm.load();
    s.vrPanelDistance = vrPanelDistanceCm.load();
    s.vrComfort = vrComfort.load();
    s.vrBody = vrBodyHandsOnly.load();
    s.vrHeight = vrHeightCm.load();
    s.vrGripSlide = vrGripSlideCm.load();
    s.vrKick = vrKick.load();
    s.vrJump = vrJump.load();
    s.vrWeaponWeight = vrWeaponWeight.load();
    s.vrTeleport = vrTeleport.load();
    s.vrHandPanel = vrHandPanel.load();
    s.vrPouch = vrPouch.load();
    s.vrFingerBones = vrFingerBones.load();
    s.vrGrabTurn = vrGrabTurn.load();
    s.vrEyeScale = vrEyeScalePercent.load();
    s.tpLag = tpLag.load();
    s.tpBlockers = tpBlockers.load();
    s.tpDown = tpDown.load();
    s.tpCombatAimFree = tpCombatAimFree.load();
    s.attackCue = attackCue.load();
    s.bladeDrag = bladeDrag.load();
    s.stabSink = stabSink.load();
    s.bladeGainPercent = bladeGainPercent.load();
    s.cueSize = cueSize.load();
    s.cueOpacity = cueOpacity.load();
    s.cueThrust = cueThrust.load();
    s.fpDefense = fpManualGuard.load();
    s.tpDefense = tpManualGuard.load();
    s.fpSwingSelection = fpSwingSelection.load();
    s.tpSwingSelection = tpSwingSelection.load();
    s.manualDefenseAvailable = manualDefenseReady.load();
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
    s.kickKey = kickKey.load();
    s.jumpKey = jumpKey.load();
    s.leanKey = leanKey.load();
    s.holdHeading = holdHeadingOn.load();
    s.autoRecenter = aimAutoRecenter.load();
    s.edgeSoftness = aimEdgeSoftness.load();
    s.tracking = trackingEnabled.load();
    s.trackingKey = trackingRecenterKey.load();
    s.trackingStatus = static_cast<int>(trackingReceiver.status());
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
static void writeLiveSettings(const panel::ModSettings& requested) {
    auto s = requested;
    s.normalize(); // also enforced for bulk/internal callers, not just UI and INI loading
    const bool policyChanged = fpManualGuard.load() != s.fpDefense || tpManualGuard.load() != s.tpDefense ||
                               rotateHeldKey.load() != s.rotateHeldKey ||
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
    sensitivity = 0.0025f * static_cast<float>(s.sensitivity) / 100.f;
    invertY = s.invertY;
    aimIndicator = s.indicator;
    ambientOcclusion = effectStrength(s.ambientOcclusion);
    depthOfField = effectStrength(s.depthOfField);
    screenSpaceReflections = effectStrength(s.screenSpaceReflections);
    edgeSmoothing = s.edgeSmoothing ? 0.5f : 0.f; // on/off rows: on is the old look, 0.5
    helmetVisor = s.helmetVisor ? 0.5f : 0.f;
    playerGhost = s.playerGhost ? 0.5f : 0.f;
    fingerPoses = s.fingerPoses ? 1 : 0;
    fingerPosesOthers = s.fingerPosesOthers ? 1 : 0;
    conditionEffects = s.conditionEffects ? 0.5f : 0.f;
    heldDetail = effectStrength(s.heldDetail);
    armourDetail = effectStrength(s.armourDetail);
    objectDetail = effectStrength(s.objectDetail);
    levelDetail = effectStrength(s.levelDetail);
    drawDistanceM = s.drawDistance;
    objectLightCap = s.objectLights;
    woundMarks = s.woundMarks ? 1.f : 0.f;
    lightSmoothing = s.lightSmoothing;
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
    vrIpdMm = s.vrIpd;
    vrUnitsPerMetre = s.vrScale;
    vrRenderMode = s.vrRenderMode;
    vrHandsDrawn = s.vrHands;
    vrPosture = s.vrPosture;
    vrHeadset = s.vrHeadset;
    vrTurning = s.vrTurning;
    vrEffects = s.vrEffects;
    vrGripAngle = s.vrGripAngle;
    vrHapticPercent = s.vrHaptics;
    vrPanelWidthCm = s.vrPanelWidth;
    vrPanelDistanceCm = s.vrPanelDistance;
    vrComfort = s.vrComfort;
    vrBodyHandsOnly = s.vrBody;
    vrHeightCm = s.vrHeight;
    vrGripSlideCm = s.vrGripSlide;
    vrKick = s.vrKick;
    vrJump = s.vrJump;
    vrWeaponWeight = s.vrWeaponWeight;
    vrTeleport = s.vrTeleport;
    vrHandPanel = s.vrHandPanel;
    vrPouch = s.vrPouch;
    vrFingerBones = s.vrFingerBones;
    vrGrabTurn = s.vrGrabTurn;
    vrEyeScalePercent = s.vrEyeScale;
    tpBlockers = s.tpBlockers;
    tpDown = s.tpDown;
    tpCombatAimFree = s.tpCombatAimFree;
    attackCue = s.attackCue;
    bladeDrag = s.bladeDrag;
    stabSink = s.stabSink;
    bladeGainPercent = s.bladeGainPercent;
    cueSize = s.cueSize;
    cueOpacity = s.cueOpacity;
    cueThrust = s.cueThrust;
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
    kickKey = s.kickKey;
    jumpKey = s.jumpKey;
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
            if (parsed && end && !*end) v = static_cast<int>(n);
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
    {
        wchar_t origin[160]{};
        GetPrivateProfileStringW(L"VR", L"RecenterOrigin", L"", origin, 160, path);
        float o[7]{};
        if (origin[0] && swscanf_s(origin, L"%f,%f,%f,%f,%f,%f,%f", &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6]) == 7) {
            memcpy(vrRecenterOrigin, o, sizeof o);
            vrRecenterOriginValid = true;
        }
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
    const bool written = saveRow(static_cast<int>(row), snapshot); // no runtime lock across disk I/O
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
