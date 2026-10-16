// F8 settings: read and applied live, saved to first-person.ini.
// Part of first_person.cpp (one translation unit): included there, in order.
#include "../settings_window.h"
// settings for the F8 panel (panel thread). Applied live; saved to first-person.ini.
static panel::ModSettings currentSettings() {
    panel::ModSettings s;
    s.fov = selectedFov.load();
    s.sensitivity = int(std::lround(sensitivity.load() / 0.0025f * 100.f));
    s.rawMouse = rawWanted.load();
    s.hideHelmet = helmetMode.load() == fp::HelmetMode::hide;
    s.grabRange = grabRange.load();
    s.hideSwingCrosshair = hideSwingCrosshair.load();
    s.tpDistance = tpDistance.load();
    s.tpShoulder = tpShoulder.load();
    s.tpHeight = tpHeight.load();
    s.tpTight = tpTight.load();
    s.tpCombat = tpCombat.load();
    s.tpFov = tpFov.load();
    s.tpLag = tpLag.load();
    s.tpHide = tpHide.load();
    s.tpDown = tpDown.load();
    s.headSteady = headSteady.load();
    s.attackCue = attackCue.load();
    s.cueSize = cueSize.load();
    s.cueOpacity = cueOpacity.load();
    s.cueThrust = cueThrust.load();
    s.rmbMode = rmbMode.load();
    s.idleCrosshairAlways = idleCrosshairAlways.load();
    s.freezeWindows = windowsFreeze.load();
    s.headRoll = headSteadyRoll.load();
    s.headPitch = headSteadyPitch.load();
    s.headYaw = headSteadyYaw.load();
    s.headBob = headBob.load();
    s.spineFollow = spineFollow.load();
    s.crouchMode = crouchMode.load();
    s.nearClip = nearClip.load();
    s.aimMarker = aimMarker.load();
    s.flickCatchup = flickCatchup.load();
    return s;
}
static void saveSetting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    if (settingsPath[0]) WritePrivateProfileStringW(section, key, value, settingsPath);
}
static panel::ModSettings applySettings(const panel::ModSettings& s) {
    const panel::ModSettings was = currentSettings();
    wchar_t v[16];
    wchar_t v16[16];
    if (s.fov != was.fov) selectedFov.store(fp::clampFov(s.fov)); // saveFov writes it (worker)
    if (s.sensitivity != was.sensitivity) {
        const int pc = s.sensitivity < 10 ? 10 : (s.sensitivity > 400 ? 400 : s.sensitivity);
        sensitivity = 0.0025f * float(pc) / 100.f;
        swprintf_s(v, L"%d", pc);
        saveSetting(L"Input", L"SensitivityPercent", v);
    }
    if (s.rawMouse != was.rawMouse) {
        rawWanted = s.rawMouse;
        rawMouse = s.rawMouse;
        saveSetting(L"Input", L"RawMouse", s.rawMouse ? L"1" : L"0");
        for (int i = 0;
             i < 20 && s.rawMouse && !InterlockedCompareExchange(&rawRegistered, 0, 0) && rawWanted.load();
             ++i)
            Sleep(25);
    } // the worker registers within 100 ms
    if (s.hideHelmet != was.hideHelmet) {
        helmetMode = s.hideHelmet ? fp::HelmetMode::hide : fp::HelmetMode::show;
        saveSetting(L"View", L"Helmet", s.hideHelmet ? L"hide" : L"show");
    }
    if (s.grabRange != was.grabRange) {
        grabRange = fp::clampGrabRange(s.grabRange);
        wchar_t g[16];
        swprintf_s(g, L"%d", grabRange.load());
        saveSetting(L"Input", L"GrabRange", g);
    }
    if (s.rmbMode != was.rmbMode) {
        rmbMode = s.rmbMode < 0 || s.rmbMode > 2 ? 1 : s.rmbMode;
        saveSetting(L"Input", L"RightMouseFree", fp::rmbModeText(rmbMode.load()));
        if (!rmbMode.load()) InterlockedExchange(&rmbFree, 0);
    }
    if (s.hideSwingCrosshair != was.hideSwingCrosshair) {
        hideSwingCrosshair = s.hideSwingCrosshair;
        saveSetting(L"View", L"SwingCrosshair", s.hideSwingCrosshair ? L"hide" : L"show");
    }
    if (s.aimMarker != was.aimMarker) {
        aimMarker = s.aimMarker;
        saveSetting(L"Input", L"AimMarker", s.aimMarker ? L"1" : L"0");
    }
    if (s.flickCatchup != was.flickCatchup) {
        flickCatchup = s.flickCatchup < 0 ? 0 : (s.flickCatchup > 100 ? 100 : s.flickCatchup);
        wchar_t f[16];
        swprintf_s(f, L"%d", flickCatchup.load());
        saveSetting(L"Input", L"FlickCatchup", f);
    }
    if (s.idleCrosshairAlways != was.idleCrosshairAlways) {
        idleCrosshairAlways = s.idleCrosshairAlways;
        saveSetting(L"View", L"IdleCrosshair", s.idleCrosshairAlways ? L"always" : L"rmb");
    }
    if (s.nearClip != was.nearClip) {
        nearClip = fp::clampNearClip(s.nearClip);
        wchar_t v[16];
        swprintf_s(v, L"%d", nearClip.load());
        saveSetting(L"Camera", L"NearClip", v);
    }
    if (s.crouchMode != was.crouchMode) {
        crouchMode = s.crouchMode;
        saveSetting(L"Input", L"Crouch", fp::crouchModeText(s.crouchMode));
    }
    if (s.freezeWindows != was.freezeWindows) {
        windowsFreeze = s.freezeWindows;
        saveSetting(L"View", L"FreezeForWindows", s.freezeWindows ? L"1" : L"0");
    }
    {
        wchar_t t[16];
        if (s.tpDistance != was.tpDistance) {
            tpDistance = fp::clampRange(s.tpDistance, fp::tpDistanceMin, fp::tpDistanceMax);
            swprintf_s(t, L"%d", tpDistance.load());
            saveSetting(L"ThirdPerson", L"Distance", t);
        }
        if (s.tpShoulder != was.tpShoulder) {
            tpShoulder = fp::clampRange(s.tpShoulder, -fp::tpShoulderMax, fp::tpShoulderMax);
            swprintf_s(t, L"%d", tpShoulder.load());
            saveSetting(L"ThirdPerson", L"Shoulder", t);
        }
        if (s.tpLag != was.tpLag) {
            tpLag = s.tpLag < 0 ? 0 : (s.tpLag > 100 ? 100 : s.tpLag);
            swprintf_s(t, L"%d", tpLag.load());
            saveSetting(L"ThirdPerson", L"Lag", t);
        }
        if (s.tpHide != was.tpHide) {
            tpHide = s.tpHide;
            saveSetting(L"ThirdPerson", L"HideBlockers", s.tpHide ? L"1" : L"0");
        }
        if (s.tpDown != was.tpDown) {
            tpDown = s.tpDown;
            saveSetting(L"ThirdPerson", L"KnockdownFollow", s.tpDown ? L"1" : L"0");
        }
        if (s.tpCombat != was.tpCombat) {
            tpCombat = s.tpCombat;
            saveSetting(L"ThirdPerson", L"CombatFraming", s.tpCombat ? L"1" : L"0");
        }
        if (s.tpFov != was.tpFov) {
            tpFov = s.tpFov <= 0 ? 0 : fp::clampFov(s.tpFov);
            swprintf_s(t, L"%d", tpFov.load());
            saveSetting(L"ThirdPerson", L"Fov", t);
        }
        if (s.tpTight != was.tpTight) {
            tpTight = s.tpTight;
            saveSetting(L"ThirdPerson", L"TightSpaces", s.tpTight ? L"1" : L"0");
        }
        if (s.tpHeight != was.tpHeight) {
            tpHeight = fp::clampRange(s.tpHeight, fp::tpHeightMin, fp::tpHeightMax);
            swprintf_s(t, L"%d", tpHeight.load());
            saveSetting(L"ThirdPerson", L"Height", t);
        }
    }
    {
        wchar_t t[16];
        if (s.attackCue != was.attackCue) {
            attackCue = s.attackCue;
            saveSetting(L"View", L"AttackCue", s.attackCue ? L"1" : L"0");
        }
        if (s.cueThrust != was.cueThrust) {
            cueThrust = s.cueThrust;
            saveSetting(L"View", L"CueThrust", s.cueThrust ? L"1" : L"0");
        }
        if (s.cueSize != was.cueSize) {
            cueSize = s.cueSize < 50 ? 50 : (s.cueSize > 200 ? 200 : s.cueSize);
            swprintf_s(t, L"%d", cueSize.load());
            saveSetting(L"View", L"CueSize", t);
        }
        if (s.cueOpacity != was.cueOpacity) {
            cueOpacity = s.cueOpacity < 20 ? 20 : (s.cueOpacity > 100 ? 100 : s.cueOpacity);
            swprintf_s(t, L"%d", cueOpacity.load());
            saveSetting(L"View", L"CueOpacity", t);
        }
    }
    {
        wchar_t t[16];
        auto axis = [&](int want, int had, std::atomic<int>& now, const wchar_t* key) {
            if (want == had) return;
            now = want < 0 ? 0 : (want > 100 ? 100 : want);
            swprintf_s(t, L"%d", now.load());
            saveSetting(L"Camera", key, t);
        };
        axis(s.headRoll, was.headRoll, headSteadyRoll, L"HeadRoll");
        axis(s.headPitch, was.headPitch, headSteadyPitch, L"HeadPitch");
        axis(s.headYaw, was.headYaw, headSteadyYaw, L"HeadYaw");
        axis(s.headBob, was.headBob, headBob, L"HeadBobSteady");
        axis(s.spineFollow, was.spineFollow, spineFollow, L"SpineFollowsAim");
    }
    if (s.headSteady != was.headSteady) {
        const int v = s.headSteady < 0 ? 0 : (s.headSteady > 100 ? 100 : s.headSteady);
        headSteady = v;
        swprintf_s(v16, L"%d", v);
        saveSetting(L"Camera", L"HeadStabilisation", v16);
    }
    return currentSettings();
}
