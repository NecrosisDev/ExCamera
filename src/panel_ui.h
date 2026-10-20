#pragma once
// The F8 Settings view: the mod's settings (ModSettings) on sub-pages, custom drawn (GDI) in the game's
// own fonts and stone, so the same paint code serves the live window and offline checks into a bitmap.
#include <set>
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <map>
#include <array>
#include <cstdio>
#include <climits>
#include <cwctype>
#include <locale.h>
#include "input_policy.h"
#include "ui_art.h"

#include "held_object.h" // the rotate key's names and choices
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace panel {

// the game's own menu sounds (Sound.rpk menuclick / menusel, RIFF WAV as shipped), read from the
// game's archive (works without the service), played with PlaySound. The game's own sound path would
// need a game call nobody has traced; this plays beside it.
inline void playMenuSound(bool click) {
    static std::vector<uint8_t> clickWav, selectWav;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::wstring a = uiart::gameFolder() + L"\\Sound.rpk";
        uiart::readMember(a, "menuclick", clickWav);
        uiart::readMember(a, "menusel", selectWav);
    }
    const std::vector<uint8_t>& w = click ? clickWav : selectWav;
    if (w.size() > 44 && !memcmp(w.data(), "RIFF", 4))
        PlaySoundW((LPCWSTR)w.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
// Windows' own dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 2004, 19 before),
// a dark grey with light text; loaded at run time, so an older Windows just keeps its own.
inline void darkCaption(HWND w) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
        const BOOL on = TRUE;
        if (FAILED(set(w, 20, &on, sizeof on))) set(w, 19, &on, sizeof on);
    }
    FreeLibrary(dwm);
    SetWindowPos(w, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

// The first-person mod's own settings: shown on the F8 panel's Settings page, applied
// live and saved by the mod to first-person.ini.
// The Settings rows. A row's number is its place here; rowInfo holds what is generic about it (label,
// control, the value it edits, tooltip), ModSettings::step and value what is particular.
namespace setting {
enum : int {
    fov,
    sensitivity,
    headSteady,
    rawMouse,
    hideHelmet,
    grabRange,
    swingCrosshair,
    tpDistance,
    tpShoulder,
    tpHeight,
    tpTight,
    tpCombat,
    tpFov,
    tpLag,
    tpBlockers,
    tpDown,
    attackCue,
    cueSize,
    cueOpacity,
    cueThrust,
    rmbMode,
    idleCrosshair,
    headRoll,
    headPitch,
    headYaw,
    aimMarker,
    flickCatchup,
    headBob,
    freezeWindows,
    spineFollow,
    crouchMode,
    nearClip,
    tpCombatAim,
    eyeSetback,
    rotateHeldKey,
    leanKey,
    holdHeading,
    turnLead,
    fpDeadzoneX,
    fpDeadzoneY,
    tpDeadzoneX,
    tpDeadzoneY,
    leanKeysMode,
    leanLeftKey,
    leanRightKey,
    fpDefense,
    tpDefense,
    fpSwingSelection,
    tpSwingSelection,
    count
};
}

struct ModSettings {
    int fov = 75, sensitivity = 100,
        headSteady = 70; // degrees, percent, percent (head stabilisation, 0 = the game's)
    bool rawMouse = false, hideHelmet = false, hideSwingCrosshair = false;
    bool rawMouseFallback = false;        // runtime status, not a saved preference
    bool savePending = false;             // runtime status, not a saved preference
    int grabRange = fp::grabRangeDefault; // cm from the eye: the farthest grab and hold
    int tpDistance = 250, tpShoulder = 40,
        tpHeight = 15;   // third person: cm behind, cm to the right (negative: left), cm above the eye
    bool tpTight = true; // in tight spaces the shoulder swaps to the open side and the framing closes in
    bool tpCombat = true;
    int tpFov = 0;
    bool attackCue = true, cueThrust = true;
    int cueSize = 100, cueOpacity = 100;            // the attack wind-up arc
    int fpDefense = 0, tpDefense = 0;               // automatic default, manual holds RMB
    int fpSwingSelection = 1, tpSwingSelection = 1; // mouse direction, or native facing
    bool manualDefenseAvailable = false;            // runtime capability, not a saved row
    int rmbMode = 1;                                // right mouse frees the cursor: 0 off, 1 hold, 2 toggle
    bool idleCrosshairAlways = false; // normal exploration crosshair; the aim marker is separate
    bool freezeWindows = true;        // the game's windows free the cursor and hold look and movement
    int spineFollow = 60; // how far the spine bends with the look (forward looking down, back looking up)
    int crouchMode = 2; // the game's duck key out of combat: 0 off (the game's short duck), 1 hold, 2 toggle
    int nearClip = fp::nearClipDefaultTenths; // the near plane's floor, tenths of a centimetre
    int headRoll = 80, headPitch = 70, headYaw = 50,
        headBob = 30; // how much wobble on each axis is held off, and how much of the head's bob
    bool aimMarker = true;
    int flickCatchup =
        50; // the crosshair while the head catches up; how much a flick's lead is eased // how much head wobble on each axis stiffens the neck (0 = not at all)
    int tpLag = 40;
    bool tpDown = true;
    int tpBlockers =
        fp::blockersSmart; // what is in the way: hide / push in / smart (people hidden, objects push in)
    bool tpCombatAimFree = true;
    int fpDeadzoneX = 15, fpDeadzoneY = 10, tpDeadzoneX = 20, tpDeadzoneY = 20;
    bool leanKeysMode = false;
    int leanLeftKey = 0, leanRightKey = 0;
    int eyeSetback = 0; // cm the viewpoint sits behind the eyes (0: at the eyes)
    bool holdHeading = true,
         turnLead = false; // first person: the view faces the aim's heading; leads fast turns by a frame
    int leanKey = 0x12;    // held, the mouse leans the upper body (virtual-key code; 0 unbound)
    int rotateHeldKey =
        0x02; // the key that turns a held object (virtual-key code; 0 unbound) // in combat the crosshair moves inside a box; the view turns at its edge // camera lag, hide what is in the way, knockdown follow // closer and centred in combat; third person's own field of view (0 = as first person)
    bool operator==(const ModSettings& o) const; // every row's value
    // The next (or previous) of fp::heldKeyChoices; a key set in the ini starts the cycle from the top.
    static int nextKey(int key, int delta) {
        constexpr int n = int(sizeof fp::heldKeyChoices / sizeof *fp::heldKeyChoices);
        int at = -1;
        for (int i = 0; i < n; ++i)
            if (fp::heldKeyChoices[i] == key) at = i;
        if (at < 0) return fp::heldKeyChoices[delta < 0 ? n - 1 : 0];
        return fp::heldKeyChoices[(at + (delta < 0 ? n - 1 : 1)) % n];
    }
    // One click on a row's control: numbers step by delta (within range), switches flip.
    bool step(int row, int delta) {
        auto clamp = [](int v, int lo, int hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        const ModSettings before = *this;
        switch (row) {
        case setting::fov:
            fov = fp::clampFov(fov + delta * 5);
            break;
        case setting::sensitivity:
            sensitivity = clamp(sensitivity + delta * 10, 10, 400);
            break;
        case setting::headSteady:
            headSteady = clamp(headSteady + delta * 10, 0, 100);
            break;
        case setting::rawMouse:
            rawMouse = !rawMouse;
            break;
        case setting::hideHelmet:
            hideHelmet = !hideHelmet;
            break;
        case setting::grabRange:
            grabRange = fp::clampGrabRange(grabRange + delta * 10);
            break;
        case setting::swingCrosshair:
            hideSwingCrosshair = !hideSwingCrosshair;
            break;
        case setting::tpDistance:
            tpDistance = clamp(tpDistance + delta * 25, fp::tpDistanceMin, fp::tpDistanceMax);
            break;
        case setting::tpShoulder:
            tpShoulder = clamp(tpShoulder + delta * 10, -fp::tpShoulderMax, fp::tpShoulderMax);
            break;
        case setting::tpHeight:
            tpHeight = clamp(tpHeight + delta * 10, fp::tpHeightMin, fp::tpHeightMax);
            break;
        case setting::tpTight:
            tpTight = !tpTight;
            break;
        case setting::tpCombat:
            tpCombat = !tpCombat;
            break;
        case setting::tpLag:
            tpLag = clamp(tpLag + delta * 10, 0, 100);
            break;
        case setting::tpBlockers:
            tpBlockers = (tpBlockers + (delta < 0 ? 2 : 1)) % 3;
            break; // hide -> push -> smart, or back
        case setting::tpCombatAim:
            tpCombatAimFree = !tpCombatAimFree;
            break;
        case setting::fpDeadzoneX:
            fpDeadzoneX = fp::clampDeadzone(fpDeadzoneX + delta);
            break;
        case setting::fpDeadzoneY:
            fpDeadzoneY = fp::clampDeadzone(fpDeadzoneY + delta);
            break;
        case setting::tpDeadzoneX:
            tpDeadzoneX = fp::clampDeadzone(tpDeadzoneX + delta);
            break;
        case setting::tpDeadzoneY:
            tpDeadzoneY = fp::clampDeadzone(tpDeadzoneY + delta);
            break;
        case setting::leanKeysMode:
            leanKeysMode = !leanKeysMode;
            break;
        case setting::leanLeftKey:
            leanLeftKey = nextKey(leanLeftKey, delta);
            break;
        case setting::leanRightKey:
            leanRightKey = nextKey(leanRightKey, delta);
            break;
        case setting::eyeSetback:
            eyeSetback = fp::clampEyeSetback(eyeSetback + delta * 2);
            break;
        case setting::rotateHeldKey:
            rotateHeldKey = nextKey(rotateHeldKey, delta);
            break;
        case setting::leanKey:
            leanKey = nextKey(leanKey, delta);
            break;
        case setting::holdHeading:
            holdHeading = !holdHeading;
            break;
        case setting::turnLead:
            turnLead = !turnLead;
            break;
        case setting::tpDown:
            tpDown = !tpDown;
            break;
        case setting::attackCue:
            attackCue = !attackCue;
            break;
        case setting::cueSize:
            cueSize = clamp(cueSize + delta * 10, 50, 200);
            break;
        case setting::cueOpacity:
            cueOpacity = clamp(cueOpacity + delta * 10, 20, 100);
            break;
        case setting::cueThrust:
            cueThrust = !cueThrust;
            break;
        case setting::fpDefense:
            fpDefense = fpDefense == 1 ? 0 : 1;
            break;
        case setting::tpDefense:
            tpDefense = tpDefense == 1 ? 0 : 1;
            break;
        case setting::fpSwingSelection:
            fpSwingSelection = fpSwingSelection == 1 ? 0 : 1;
            break;
        case setting::tpSwingSelection:
            tpSwingSelection = tpSwingSelection == 1 ? 0 : 1;
            break;
        case setting::rmbMode:
            rmbMode = (rmbMode + (delta < 0 ? 2 : 1)) % 3;
            break; // hold -> toggle -> off, or back
        case setting::idleCrosshair:
            idleCrosshairAlways = !idleCrosshairAlways;
            break;
        case setting::headRoll:
            headRoll = clamp(headRoll + delta * 10, 0, 100);
            break;
        case setting::headPitch:
            headPitch = clamp(headPitch + delta * 10, 0, 100);
            break;
        case setting::headYaw:
            headYaw = clamp(headYaw + delta * 10, 0, 100);
            break;
        case setting::aimMarker:
            aimMarker = !aimMarker;
            break;
        case setting::flickCatchup:
            flickCatchup = clamp(flickCatchup + delta * 10, 0, 100);
            break;
        case setting::headBob:
            headBob = clamp(headBob + delta * 10, 0, 100);
            break;
        case setting::freezeWindows:
            freezeWindows = !freezeWindows;
            break;
        case setting::spineFollow:
            spineFollow = clamp(spineFollow + delta * 10, 0, 100);
            break;
        case setting::crouchMode:
            crouchMode = crouchMode == 2 ? 1 : (crouchMode == 1 ? 0 : 2);
            break; // toggle -> hold -> off
        case setting::nearClip:
            nearClip = fp::clampNearClip(nearClip + delta);
            break;
        case setting::tpFov:
            tpFov = tpFov == 0 ? (delta == 0 ? 0 : fp::clampFov(fov + delta * 5))
                               : (tpFov + delta * 5 < fp::fovMin ? 0 : fp::clampFov(tpFov + delta * 5));
            break; // below the minimum: back to "as first person"
        default:
            return false;
        }
        return !(*this == before);
    }
    static constexpr int rows = setting::count;
    // The Settings pages: every row belongs to exactly one (test_controls checks it). A page may have no rows.
    struct Group {
        const char* name;
        int rows[26];
        int count;
    };
    static constexpr int groupCount = 3, savesGroup = -1;
    static const Group& group(int g) {
        static const Group groups[groupCount] = {
            {"Camera",
             {setting::fov,         setting::nearClip,    setting::eyeSetback,  setting::hideHelmet,
              setting::holdHeading, setting::turnLead,    setting::fpDeadzoneX, setting::fpDeadzoneY,
              setting::tpDistance,  setting::tpShoulder,  setting::tpHeight,    setting::tpTight,
              setting::tpCombat,    setting::tpFov,       setting::tpLag,       setting::tpBlockers,
              setting::tpDown,      setting::tpCombatAim, setting::tpDeadzoneX, setting::tpDeadzoneY,
              setting::headSteady,  setting::headRoll,    setting::headPitch,   setting::headYaw,
              setting::headBob,     setting::spineFollow},
             26},
            {"Controls",
             {setting::sensitivity, setting::rawMouse, setting::rmbMode, setting::crouchMode,
              setting::leanKeysMode, setting::leanKey, setting::leanLeftKey, setting::leanRightKey,
              setting::freezeWindows, setting::grabRange, setting::rotateHeldKey},
             11},
            {"Aim & combat",
             {setting::swingCrosshair, setting::idleCrosshair, setting::aimMarker, setting::flickCatchup,
              setting::attackCue, setting::cueSize, setting::cueOpacity, setting::cueThrust,
              setting::fpDefense, setting::tpDefense, setting::fpSwingSelection, setting::tpSwingSelection},
             12},
        };
        return groups[g < 0 || g >= groupCount ? 0 : g];
    }
    static const char* groupDescription(int g) {
        static constexpr const char* descriptions[groupCount] = {
            "First person, third person, and head and body settings.",
            "Mouse, keys, cursor behavior and held objects.",
            "Crosshairs, attack cues and per-view combat preferences.",
        };
        return descriptions[g < 0 || g >= groupCount ? 0 : g];
    }
    // Sections are presentation only; every real row retains its setting ID and owner.
    static const wchar_t* sectionStart(int row) {
        switch (row) {
        case setting::fov:
            return L"FIRST PERSON";
        case setting::tpDistance:
            return L"THIRD PERSON";
        case setting::headSteady:
            return L"HEAD AND BODY";
        case setting::sensitivity:
            return L"MOUSE AND KEYS";
        case setting::grabRange:
            return L"INTERACTION";
        case setting::swingCrosshair:
            return L"CROSSHAIRS";
        case setting::attackCue:
            return L"ATTACK CUES";
        case setting::fpDefense:
            return L"DEFENSE";
        case setting::fpSwingSelection:
            return L"SWING SELECTION";
        default:
            return nullptr;
        }
    }
    std::wstring value(int row) const {
        switch (row) {
        case setting::fov:
            return std::to_wstring(fov) + L"\x00B0 (" +
                   std::to_wstring(int(std::lround(fp::horizontalFov(float(fov), 16.f / 9.f)))) +
                   L"\x00B0 across at 16:9)"; // the setting is vertical: wider screens see more at the sides
        case setting::sensitivity:
            return std::to_wstring(sensitivity) + L"%";
        case setting::headSteady:
            return headSteady ? std::to_wstring(headSteady) + L"%" : std::wstring(L"off (the game's)");
        case setting::rawMouse:
            return rawMouse ? (rawMouseFallback ? L"On; cursor fallback" : L"on") : L"off";
        case setting::hideHelmet:
            return hideHelmet ? L"Hide all" : L"Show when clear";
        case setting::grabRange:
            return std::to_wstring(grabRange) + L" cm";
        case setting::swingCrosshair:
            return hideSwingCrosshair ? L"hidden" : L"shown";
        case setting::tpDistance:
            return std::to_wstring(tpDistance) + L" cm";
        case setting::tpShoulder:
            return tpShoulder == 0 ? std::wstring(L"centre")
                                   : std::to_wstring(tpShoulder < 0 ? -tpShoulder : tpShoulder) +
                                         (tpShoulder < 0 ? L" cm left" : L" cm right");
        case setting::tpHeight:
            return tpHeight == 0 ? L"Level with the eyes"
                                 : std::to_wstring(std::abs(tpHeight)) +
                                       (tpHeight < 0 ? L" cm below the eyes" : L" cm above the eyes");
        case setting::tpTight:
            return tpTight ? L"swap shoulder" : L"off";
        case setting::tpCombat:
            return tpCombat ? L"closer and centred" : L"off";
        case setting::tpLag:
            return tpLag == 0 ? std::wstring(L"off (as before)") : std::to_wstring(tpLag) + L"%";
        case setting::tpBlockers:
            return tpBlockers == fp::blockersHide   ? L"hide blockers"
                   : tpBlockers == fp::blockersPush ? L"pull camera in"
                                                    : L"hide people; pull in";
        case setting::tpCombatAim:
            return tpCombatAimFree ? L"Aim box" : L"Screen centre";
        case setting::fpDeadzoneX:
            return std::to_wstring(fpDeadzoneX * 2) + L"% of screen width";
        case setting::fpDeadzoneY:
            return std::to_wstring(fpDeadzoneY * 2) + L"% of screen height";
        case setting::tpDeadzoneX:
            return std::to_wstring(tpDeadzoneX * 2) + L"% of screen width";
        case setting::tpDeadzoneY:
            return std::to_wstring(tpDeadzoneY * 2) + L"% of screen height";
        case setting::leanKeysMode:
            return leanKeysMode ? L"left / right keys" : L"modifier + mouse";
        case setting::leanLeftKey:
            return fp::heldKeyName(leanLeftKey);
        case setting::leanRightKey:
            return fp::heldKeyName(leanRightKey);
        case setting::eyeSetback:
            return eyeSetback ? std::to_wstring(eyeSetback) + L" cm behind the eyes"
                              : std::wstring(L"at the eyes");
        case setting::rotateHeldKey:
            return fp::heldKeyName(rotateHeldKey);
        case setting::leanKey:
            return fp::heldKeyName(leanKey);
        case setting::holdHeading:
            return holdHeading ? L"faces where you aim" : L"the head's (may slide sideways)";
        case setting::turnLead:
            return turnLead ? L"a frame ahead in fast turns" : L"off";
        case setting::tpDown:
            return tpDown ? L"follow the body" : L"follow the head";
        case setting::attackCue:
            return attackCue ? L"shown while a swing winds up" : L"off";
        case setting::cueSize:
            return std::to_wstring(cueSize) + L"%";
        case setting::cueOpacity:
            return std::to_wstring(cueOpacity) + L"%";
        case setting::cueThrust:
            return cueThrust ? L"below the crosshair" : L"off";
        case setting::fpDefense:
        case setting::tpDefense: {
            const int mode = row == setting::fpDefense ? fpDefense : tpDefense;
            return mode != 1                ? L"Automatic (native)"
                   : manualDefenseAvailable ? L"Manual (hold RMB)"
                                            : L"Automatic fallback";
        }
        case setting::fpSwingSelection:
            return fpSwingSelection ? L"Mouse direction" : L"Native facing";
        case setting::tpSwingSelection:
            return tpSwingSelection ? L"Mouse direction" : L"Native facing";
        case setting::rmbMode:
            return rmbMode == 0 ? L"off"
                                : (rmbMode == 2 ? L"toggle (click on, click off)" : L"hold (out of combat)");
        case setting::idleCrosshair:
            return idleCrosshairAlways ? L"Show" : L"Hide";
        case setting::headRoll:
            return headRoll ? std::to_wstring(headRoll) + L"%" : std::wstring(L"off");
        case setting::headPitch:
            return headPitch ? std::to_wstring(headPitch) + L"%" : std::wstring(L"off");
        case setting::headYaw:
            return headYaw ? std::to_wstring(headYaw) + L"%" : std::wstring(L"off");
        case setting::aimMarker:
            return aimMarker ? L"while the head catches up" : L"off";
        case setting::flickCatchup:
            return flickCatchup ? std::to_wstring(flickCatchup) + L"%" : std::wstring(L"off");
        case setting::headBob:
            return headBob ? std::to_wstring(headBob) + L"%" : std::wstring(L"off");
        case setting::freezeWindows:
            return freezeWindows ? L"on" : L"off";
        case setting::spineFollow:
            return spineFollow ? std::to_wstring(spineFollow) + L"%" : std::wstring(L"off");
        case setting::nearClip: {
            wchar_t b[24];
            swprintf_s(b, L"%d.%d cm", nearClip / 10, nearClip % 10);
            return b;
        }
        case setting::crouchMode:
            return crouchMode == 2
                       ? L"toggle (the game's duck key)"
                       : (crouchMode == 1 ? L"hold (the game's duck key)" : L"off (the game's short duck)");
        case setting::tpFov:
            return tpFov == 0 ? L"Base (" + std::to_wstring(fov) + L"\x00B0)"
                              : std::to_wstring(tpFov) + L"\x00B0";
        default:
            return L"";
        }
    }
    static const wchar_t* label(int row);
    static bool
    isNumber(int row); // a number (or a list to step through): - and +; otherwise one Switch button
    static const wchar_t* tip(int row); // the tooltip, for rows whose effect is not plain from the label
    // Dependency hints are shown beside disabled controls; settings for the other
    // perspective remain configurable so players can prepare either control set.
    const wchar_t* inactiveReason(int row) const {
        if ((row == setting::tpDeadzoneX || row == setting::tpDeadzoneY) && !tpCombatAimFree)
            return L"Choose Aim box above to use this setting.";
        if ((row == setting::headRoll || row == setting::headPitch || row == setting::headYaw ||
             row == setting::headBob || row == setting::holdHeading) &&
            headSteady == 0)
            return L"Enable Head stabilisation to use this setting.";
        if (row == setting::holdHeading && headYaw == 0)
            return L"Increase Head steadiness: yaw to use heading correction.";
        if ((row == setting::cueSize || row == setting::cueOpacity || row == setting::cueThrust) &&
            !attackCue)
            return L"Enable Attack wind-up arc to use this setting.";
        if ((row == setting::leanLeftKey || row == setting::leanRightKey) && !leanKeysMode)
            return L"Choose left / right keys in Lean controls first.";
        if (row == setting::leanKey && leanKeysMode) return L"Used with modifier + mouse in Lean controls.";
        return nullptr;
    }
    // The page's "Reset page" button: its rows back to their defaults. True if anything changed.
    static constexpr int resetControl = 99;
    bool resetGroup(int g);
    // Every value in its range (an ini can hold anything): numbers clamped as their - and + do, unknown choices
    // back to the default.
    void normalize();
    // A click on a Settings control: a row's step, or the page reset.
    bool click(int control, int delta, int g) {
        return control == resetControl ? resetGroup(g) : step(control, delta);
    }
};

// The ini words of the rows that are not plain integers.
inline constexpr int badText = INT_MIN;
inline bool iniIs(const wchar_t* t, const wchar_t* w) {
    while (*t == L' ') ++t;
    for (; *w; ++t, ++w)
        if ((*t | 0x20) != *w) return false;
    while (*t == L' ') ++t;
    return !*t;
}
inline int iniFov(const wchar_t* t) {
    wchar_t* end = nullptr;
    const long v = wcstol(t, &end, 10);
    const bool parsed = end != t;
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    return !parsed || !end || *end ? badText : int(v);
}
inline int iniHelmet(const wchar_t* t) {
    return fp::helmetModeFromText(t) == fp::HelmetMode::hide;
}
inline const wchar_t* iniHelmetText(int v) {
    return v ? L"hide" : L"show";
}
inline int iniHidden(const wchar_t* t) {
    return iniIs(t, L"hide") ? 1 : iniIs(t, L"show") ? 0 : badText;
}
inline const wchar_t* iniHiddenText(int v) {
    return v ? L"hide" : L"show";
}
inline int iniAlways(const wchar_t* t) {
    return iniIs(t, L"always") ? 1 : iniIs(t, L"rmb") ? 0 : badText;
}
inline const wchar_t* iniAlwaysText(int v) {
    return v ? L"always" : L"rmb";
}
inline int iniFree(const wchar_t* t) {
    return iniIs(t, L"centre") || iniIs(t, L"center") ? 0 : iniIs(t, L"free") ? 1 : badText;
}
inline const wchar_t* iniFreeText(int v) {
    return v ? L"free" : L"centre";
}
inline int iniSwingSelection(const wchar_t* text) {
    return iniIs(text, L"mouse") ? 1 : iniIs(text, L"native") ? 0 : badText;
}
inline const wchar_t* iniSwingSelectionText(int mode) {
    return mode ? L"mouse" : L"native";
}
struct RowInfo {
    int row;
    const wchar_t* label;
    enum Kind {
        number,
        toggle,
        choice,
        keyList
    } kind; // number and keyList: - and + (stepping through keys); others: Switch
    int ModSettings::* value;
    bool ModSettings::* flag; // what it edits (neither: a row kept in a nested struct)
    // first-person.ini: [section] key. Without fromText the value is an integer (switches 1/0); fromText returns
    // badText for a word it does not know (the default is used). Without toText it is written as an integer.
    const wchar_t* section;
    const wchar_t* key;
    int (*fromText)(const wchar_t*);
    const wchar_t* (*toText)(int);
    const wchar_t* tip;
};
inline const RowInfo& rowInfo(int row) {
    static const RowInfo table[setting::count] = {
        {setting::fov, L"Base field of view", RowInfo::number, &ModSettings::fov, nullptr, L"Camera", L"FOV",
         iniFov, nullptr,
         L"Vertical field of view for first person. Third person shares it unless its own field of view is "
         L"set. "
         L"F9 / F10 / F11 lower, raise or reset this base value."},
        {setting::sensitivity, L"Look sensitivity", RowInfo::number, &ModSettings::sensitivity, nullptr,
         L"Input", L"SensitivityPercent", nullptr, nullptr,
         L"View turning speed. Movement inside an aim box, the free pointer, leaning and held-object "
         L"rotation use their own mouse movement and are not scaled by this setting."},
        {setting::headSteady, L"Head stabilisation", RowInfo::number, &ModSettings::headSteady, nullptr,
         L"Camera", L"HeadStabilisation", nullptr, nullptr,
         L"How steadily the neck holds the head. In first person it also steadies the chest and neck outside "
         L"combat. "
         L"Zero disables the extra head steadying; the separate axis settings choose their own strengths. "
         L"The camera follows the physical head."},
        {setting::rawMouse, L"Raw mouse input", RowInfo::toggle, nullptr, &ModSettings::rawMouse, L"Input",
         L"RawMouse", nullptr, nullptr,
         L"Reads mouse counts without Windows pointer acceleration. If another component owns raw input, "
         L"cursor input remains active and the requested setting is kept. Switch off and on to retry."},
        {setting::hideHelmet, L"Hide helmet and hair", RowInfo::toggle, nullptr, &ModSettings::hideHelmet,
         L"View", L"Helmet", iniHelmet, iniHelmetText,
         L"First person only. Show when clear keeps visible headwear unless it blocks the view or cannot be "
         L"checked. "
         L"Hide all always hides headwear. Shadows remain. Moving the eye farther back can make more "
         L"headwear block the view."},
        {setting::grabRange, L"Grab range", RowInfo::number, &ModSettings::grabRange, nullptr, L"Input",
         L"GrabRange", nullptr, nullptr,
         L"Maximum interaction distance measured from the eyes, including in third person. "
         L"Walls, item rules and carrying limits still apply. This does not change weapon reach."},
        {setting::swingCrosshair, L"Crosshair during a swing", RowInfo::toggle, nullptr,
         &ModSettings::hideSwingCrosshair, L"View", L"SwingCrosshair", iniHidden, iniHiddenText, nullptr},
        {setting::tpDistance, L"Third person: distance", RowInfo::number, &ModSettings::tpDistance, nullptr,
         L"ThirdPerson", L"Distance", nullptr, nullptr,
         L"Preferred distance behind the eyes. Walls and blockers can bring the camera closer. "
         L"Combat framing reduces this distance by 20% while fighting."},
        {setting::tpShoulder, L"Third person: shoulder", RowInfo::number, &ModSettings::tpShoulder, nullptr,
         L"ThirdPerson", L"Shoulder", nullptr, nullptr,
         L"Left or right shoulder offset. Tight-space assistance can switch sides. Combat framing "
         L"reduces the offset to 30% while fighting; zero centres the camera."},
        {setting::tpHeight, L"Third person: height", RowInfo::number, &ModSettings::tpHeight, nullptr,
         L"ThirdPerson", L"Height", nullptr, nullptr,
         L"Preferred height relative to the eyes. Negative values put the camera below the eyes. "
         L"Ceilings and camera collisions can limit the achieved height."},
        {setting::tpTight, L"Third person: tight spaces", RowInfo::toggle, nullptr, &ModSettings::tpTight,
         L"ThirdPerson", L"TightSpaces", nullptr, nullptr,
         L"Chooses the clearer shoulder and closer framing in confined spaces. Camera collision protection "
         L"remains active when this option is off."},
        {setting::tpCombat, L"Third person: combat framing", RowInfo::toggle, nullptr, &ModSettings::tpCombat,
         L"ThirdPerson", L"CombatFraming", nullptr, nullptr,
         L"While in combat, moves 20% closer and reduces the shoulder offset to 30% of its exploration "
         L"value. "
         L"Off keeps the requested exploration framing."},
        {setting::tpFov, L"Third person: field of view", RowInfo::number, &ModSettings::tpFov, nullptr,
         L"ThirdPerson", L"Fov", iniFov, nullptr,
         L"Base follows the field of view on the View page. Plus or minus starts from that value. "
         L"Lower a custom value past 30 degrees to return to Base."},
        {setting::tpLag, L"Third person: camera lag", RowInfo::number, &ModSettings::tpLag, nullptr,
         L"ThirdPerson", L"Lag", nullptr, nullptr,
         L"How softly the third-person camera follows the character."},
        {setting::tpBlockers, L"Third person: in the way", RowInfo::choice, &ModSettings::tpBlockers, nullptr,
         L"ThirdPerson", L"Blockers", fp::blockersFromText, fp::blockersText,
         L"What the camera does about props and people between it and you: hide them, pull in in front of "
         L"them, or hide people and pull in for objects. Walls always pull it in."},
        {setting::tpDown, L"Third person: knocked down", RowInfo::toggle, nullptr, &ModSettings::tpDown,
         L"ThirdPerson", L"KnockdownFollow", nullptr, nullptr,
         L"Follow the body keeps the third-person camera from diving with the head during a knockdown. "
         L"Follow the head keeps its usual anchor. Both retain camera collision checks."},
        {setting::attackCue, L"Attack wind-up arc", RowInfo::toggle, nullptr, &ModSettings::attackCue,
         L"View", L"AttackCue", nullptr, nullptr, nullptr},
        {setting::cueSize, L"Wind-up arc: size", RowInfo::number, &ModSettings::cueSize, nullptr, L"View",
         L"CueSize", nullptr, nullptr, nullptr},
        {setting::cueOpacity, L"Wind-up arc: opacity", RowInfo::number, &ModSettings::cueOpacity, nullptr,
         L"View", L"CueOpacity", nullptr, nullptr, nullptr},
        {setting::cueThrust, L"Wind-up arc: thrusts", RowInfo::toggle, nullptr, &ModSettings::cueThrust,
         L"View", L"CueThrust", nullptr, nullptr, nullptr},
        {setting::rmbMode, L"Right mouse frees the cursor", RowInfo::choice, &ModSettings::rmbMode, nullptr,
         L"Input", L"RightMouseFree", fp::rmbModeFromText, fp::rmbModeText, nullptr},
        {setting::idleCrosshair, L"Crosshair out of combat", RowInfo::toggle, nullptr,
         &ModSettings::idleCrosshairAlways, L"View", L"IdleCrosshair", iniAlways, iniAlwaysText,
         L"Controls only the exploration crosshair. Aim marker can still appear while the head catches up. "
         L"Right-mouse cursor mode uses the game's free pointer."},
        {setting::headRoll, L"Head steadiness: roll", RowInfo::number, &ModSettings::headRoll, nullptr,
         L"Camera", L"HeadRoll", nullptr, nullptr,
         L"Reduces sideways head tilt. Requires Head stabilisation. First person also steadies the physical "
         L"head while balanced; hits, falls and held doors keep their native movement."},
        {setting::headPitch, L"Head steadiness: pitch", RowInfo::number, &ModSettings::headPitch, nullptr,
         L"Camera", L"HeadPitch", nullptr, nullptr,
         L"Reduces up-and-down head rotation. Requires Head stabilisation. First person also steadies the "
         L"physical head while balanced; intentional look input and impacts remain."},
        {setting::headYaw, L"Head steadiness: yaw", RowInfo::number, &ModSettings::headYaw, nullptr,
         L"Camera", L"HeadYaw", nullptr, nullptr,
         L"Reduces head rotation away from the requested heading. Requires Head stabilisation. "
         L"First-person heading correction also uses this strength."},
        {setting::aimMarker, L"Aim marker", RowInfo::toggle, nullptr, &ModSettings::aimMarker, L"Input",
         L"AimMarker", nullptr, nullptr,
         L"First person only. Shows the aim position when it is noticeably away from the physical head's "
         L"view, "
         L"even if the exploration crosshair is otherwise hidden."},
        {setting::flickCatchup, L"Fast turn damping", RowInfo::number, &ModSettings::flickCatchup, nullptr,
         L"Input", L"FlickCatchup", nullptr, nullptr,
         L"First person only. 0% is off. Higher values reduce additional turn input when your aim runs "
         L"far ahead of the physical head."},
        {setting::headBob, L"Head bob steadiness", RowInfo::number, &ModSettings::headBob, nullptr, L"Camera",
         L"HeadBobSteady", nullptr, nullptr,
         L"First person only. Reduces vertical head movement while balanced. Requires Head stabilisation. "
         L"Hits, falls and held doors retain native motion."},
        {setting::freezeWindows, L"Release controls for menus", RowInfo::toggle, nullptr,
         &ModSettings::freezeWindows, L"View", L"FreezeForWindows", nullptr, nullptr,
         L"Frees the cursor and holds modded look and movement while inventory, loot, shops, dialogue or "
         L"other supported game panels are open. This does not pause the game. Brief item tooltips are "
         L"ignored."},
        {setting::spineFollow, L"Spine follows aim", RowInfo::number, &ModSettings::spineFollow, nullptr,
         L"Camera", L"SpineFollowsAim", nullptr, nullptr,
         L"Outside combat in first and third person, your spine bends forward when you look down and back "
         L"when you look up. Combat uses native attack aiming; explicit lean controls still work."},
        {setting::crouchMode, L"Crouch", RowInfo::choice, &ModSettings::crouchMode, nullptr, L"Input",
         L"Crouch", fp::crouchModeFromText, fp::crouchModeText,
         L"Out of combat, in first and third person, the game's duck key (X unless you changed it) crouches, "
         L"also while you walk. Combat, a knockdown or talking ends it; you stay down while something is "
         L"just above your head."},
        {setting::nearClip, L"Minimum near clip", RowInfo::number, &ModSettings::nearClip, nullptr, L"Camera",
         L"NearClip", nullptr, nullptr,
         L"The minimum distance at which close surfaces are drawn. The camera adjusts this distance "
         L"automatically; "
         L"this setting changes its lower limit near walls. Lower values retain closer surfaces but reduce "
         L"depth "
         L"precision. First and third person."},
        {setting::tpCombatAim, L"Third person: aiming mode", RowInfo::toggle, nullptr,
         &ModSettings::tpCombatAimFree, L"ThirdPerson", L"CombatAim", iniFree, iniFreeText,
         L"In third person, Aim box lets the aim move inside a box; the view turns at its edge. "
         L"Screen centre turns the view directly and ignores the two aim-box size settings below. "
         L"Applies during exploration and combat."},
        {setting::eyeSetback, L"Eye setback", RowInfo::number, &ModSettings::eyeSetback, nullptr, L"Camera",
         L"EyeSetback", nullptr, nullptr,
         L"Moves the first-person view back toward the middle of the head. Third-person framing and "
         L"interaction "
         L"distance use the same shifted eye position. Headwear that blocks the first-person view is hidden; "
         L"its shadow remains."},
        {setting::rotateHeldKey, L"Rotate held object", RowInfo::keyList, &ModSettings::rotateHeldKey,
         nullptr, L"Input", L"RotateHeldKey", fp::heldKeyFromText, nullptr,
         L"Hold it while holding an object to turn the object with the mouse; the view stays still "
         L"meanwhile. Nothing else gets this key while you hold something. Any key can be set in "
         L"first-person.ini ([Input] RotateHeldKey)."},
        {setting::leanKey, L"Lean", RowInfo::keyList, &ModSettings::leanKey, nullptr, L"Input", L"LeanKey",
         fp::heldKeyFromText, nullptr,
         L"Hold it and move the mouse to lean your upper body: left and right, forward and back. The view "
         L"stays where it points and goes with your head. Let go to straighten up. In combat, the game's "
         L"alternate-attack binding takes priority over lean. Any key can be set in "
         L"first-person.ini ([Input] LeanKey)."},
        {setting::holdHeading, L"View heading", RowInfo::toggle, nullptr, &ModSettings::holdHeading,
         L"Camera", L"HoldHeading", nullptr, nullptr,
         L"Looking up or down, the game's head turns a few degrees off where you aim, so the view slid "
         L"sideways. This corrects the physics head heading, and the view follows it. A bigger difference, "
         L"the head still catching up "
         L"with a fast turn, is left as it is."},
        {setting::turnLead, L"Turn lead", RowInfo::toggle, nullptr, &ModSettings::turnLead, L"Camera",
         L"TurnLead", nullptr, nullptr,
         L"The view follows your head, which the game turns a frame behind your mouse. In fast turns this "
         L"turns the view about a frame ahead (at most 3 degrees). Off unless turning feels late."},
        {setting::fpDeadzoneX, L"First person: aim box width", RowInfo::number, &ModSettings::fpDeadzoneX,
         nullptr, L"FirstPerson", L"DeadzoneWidth", nullptr, nullptr,
         L"Total width of the first-person aim box, as a percentage of the screen. Movement inside aims; "
         L"movement past the edge turns the view. Zero means direct turning."},
        {setting::fpDeadzoneY, L"First person: aim box height", RowInfo::number, &ModSettings::fpDeadzoneY,
         nullptr, L"FirstPerson", L"DeadzoneHeight", nullptr, nullptr,
         L"Total height of the first-person aim box, as a percentage of the screen. Zero means direct "
         L"turning. "
         L"Independent of third person."},
        {setting::tpDeadzoneX, L"Third person: aim box width", RowInfo::number, &ModSettings::tpDeadzoneX,
         nullptr, L"ThirdPerson", L"DeadzoneWidth", nullptr, nullptr,
         L"Total width of the third-person aim box, as a percentage of the screen. "
         L"Used only with Aim box aiming mode. Zero means direct turning."},
        {setting::tpDeadzoneY, L"Third person: aim box height", RowInfo::number, &ModSettings::tpDeadzoneY,
         nullptr, L"ThirdPerson", L"DeadzoneHeight", nullptr, nullptr,
         L"Total height of the third-person aim box, as a percentage of the screen. "
         L"Used only with Aim box aiming mode. Zero means direct turning."},
        {setting::leanKeysMode, L"Lean controls", RowInfo::toggle, nullptr, &ModSettings::leanKeysMode,
         L"Input", L"LeanKeysMode", nullptr, nullptr,
         L"Switch between holding the lean modifier with mouse movement and separate lean-left/right keys. "
         L"Release to straighten. New keys start unbound. The game's alternate-attack binding keeps its "
         L"combat action when assigned to lean."},
        {setting::leanLeftKey, L"Lean left key", RowInfo::keyList, &ModSettings::leanLeftKey, nullptr,
         L"Input", L"LeanLeftKey", fp::heldKeyFromText, nullptr,
         L"Used only with left/right lean controls. Hold to lean left, release to straighten; both keys "
         L"together straighten. Starts unbound."},
        {setting::leanRightKey, L"Lean right key", RowInfo::keyList, &ModSettings::leanRightKey, nullptr,
         L"Input", L"LeanRightKey", fp::heldKeyFromText, nullptr,
         L"Used only with left/right lean controls. Hold to lean right, release to straighten; both keys "
         L"together straighten. Starts unbound."},
        {setting::fpDefense, L"First person: defense", RowInfo::choice, &ModSettings::fpDefense, nullptr,
         L"FirstPerson", L"Defense", fp::defenseModeFromText, fp::defenseModeText,
         L"Automatic keeps native defense. In ordinary combat, holding right mouse in Manual readies "
         L"the weapon and actively requests native attack-specific interception. "
         L"Guard takes priority over a new attack; release left mouse before attacking again. Movement "
         L"and aim remain available. Native reach, equipment and recovery still decide success. "
         L"If Manual is unavailable, its choice stays saved and Automatic defense remains active."},
        {setting::tpDefense, L"Third person: defense", RowInfo::choice, &ModSettings::tpDefense, nullptr,
         L"ThirdPerson", L"Defense", fp::defenseModeFromText, fp::defenseModeText,
         L"Choose independently for third person. In ordinary combat, hold right mouse to ready the weapon "
         L"and request native attack-specific interception; "
         L"it does not guarantee a block or add a timing bonus. Right mouse no longer cursor-walks in "
         L"ordinary close-view combat. Interaction and held objects keep their native controls. "
         L"Manual uses right mouse in place of an overlapping lean binding during ordinary combat. "
         L"If Manual is unavailable, its choice stays saved and Automatic defense remains active."},
        {setting::fpSwingSelection, L"First person: swing selection", RowInfo::choice,
         &ModSettings::fpSwingSelection, nullptr, L"FirstPerson", L"SwingSelection", iniSwingSelection,
         iniSwingSelectionText,
         L"Mouse direction: move the mouse left or right before clicking to choose the swing's starting "
         L"side. "
         L"Native facing: Exanima chooses from the angle between your aim and body. Small aim boxes may not "
         L"reach its side-change threshold. The optional AttackSideKey reverses either choice."},
        {setting::tpSwingSelection, L"Third person: swing selection", RowInfo::choice,
         &ModSettings::tpSwingSelection, nullptr, L"ThirdPerson", L"SwingSelection", iniSwingSelection,
         iniSwingSelectionText,
         L"Choose independently for third person. Mouse direction works with both Aim box and Screen centre. "
         L"Move left or right before clicking to choose the swing's starting side; native weapon timing "
         L"still applies."},
    };
    static const RowInfo none{-1,      L"",     RowInfo::toggle, nullptr, nullptr,
                              nullptr, nullptr, nullptr,         nullptr, nullptr};
    return row >= 0 && row < setting::count ? table[row] : none;
}
inline const wchar_t* ModSettings::label(int row) {
    return rowInfo(row).label;
}
inline bool ModSettings::isNumber(int row) {
    const RowInfo::Kind k = rowInfo(row).kind;
    return k == RowInfo::number || k == RowInfo::keyList;
}
inline const wchar_t* ModSettings::tip(int row) {
    return rowInfo(row).tip;
}
inline bool sameRow(int row, const ModSettings& a, const ModSettings& b) {
    const RowInfo& i = rowInfo(row);
    return !(i.value && a.*i.value != b.*i.value) && !(i.flag && a.*i.flag != b.*i.flag);
}
inline void ModSettings::normalize() {
    if (fpDefense != 1) fpDefense = 0;
    if (tpDefense != 1) tpDefense = 0;
    if (fpSwingSelection != 0) fpSwingSelection = 1;
    if (tpSwingSelection != 0) tpSwingSelection = 1;
    const ModSettings defaults;
    for (int r = 0; r < setting::count; ++r) {
        const RowInfo& i = rowInfo(r);
        if (i.kind == RowInfo::number)
            step(r, 0);
        else if (i.kind == RowInfo::choice && (this->*i.value < 0 || this->*i.value > 2))
            this->*i.value = defaults.*i.value;
    }
}
inline bool ModSettings::operator==(const ModSettings& o) const {
    for (int r = 0; r < setting::count; ++r)
        if (!sameRow(r, *this, o)) return false;
    return true;
}
inline bool ModSettings::resetGroup(int g) {
    const ModSettings defaults, before = *this;
    const Group& page = group(g);
    for (int k = 0; k < page.count; ++k) {
        const RowInfo& i = rowInfo(page.rows[k]);
        if (i.value) this->*i.value = defaults.*i.value;
        if (i.flag) this->*i.flag = defaults.*i.flag;
    }
    return !(*this == before);
}

struct NavNode {
    std::string label, right, request; // request: service path that opens this row ("" = folder only)
    std::string full;                  // the whole name when the row shows a shorter one (tooltip)
    int depth = 0;
    bool expanded = false;
    std::vector<NavNode> kids;
};

// What the right side shows.
struct Page {
    enum Kind { none, item, table } kind = none;
    std::string title, tag, description, status;
    std::vector<std::pair<std::string, std::string>> stats; // item stats, or generic rows
    bool settingsPage = false;                              // the first-person settings (no service needed)
};

// ---- drawing ----
// restyle: Exanima's own palette (docs/INGAME.md "Look and feel", uistyle.style): steel dark
// #30302c, steel mid #4e5757, steel light #a9cce2 for what is lit or chosen; text is the game font's
// white tinted light grey (the game's exact menu text colour is not found in its code yet).
struct Theme {
    COLORREF bg = RGB(48, 48, 44), pane = RGB(40, 41, 38), line = RGB(78, 87, 87), text = RGB(242, 240, 232),
             dim = RGB(196, 202, 200), accent = RGB(169, 204, 226), select = RGB(70, 82, 86),
             hover = RGB(58, 66, 68), button = RGB(60, 68, 68), buttonOff = RGB(46, 50, 50);
};
struct Fonts {
    HFONT body = nullptr, note = nullptr, title = nullptr, heading = nullptr;
    Fonts() {
        auto make = [](int h, int w) {
            return CreateFontW(-h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Segoe UI");
        };
        body = make(19, FW_NORMAL);
        note = make(16, FW_NORMAL);
        title = make(32, FW_SEMIBOLD);
        heading = make(16, FW_BOLD); // a step larger
    }
    ~Fonts() {
        DeleteObject(body);
        DeleteObject(note);
        DeleteObject(title);
        DeleteObject(heading);
    }
};

struct Layout {
    RECT nav{}, content{}, header{}, model{}, stats{}, parts{}, actions{};
    static constexpr int navWidth = 240, rowHeight = 34, navTop = 74,
                         line = 30; // larger text, larger rows // the search box sits above the list
    void compute(int w, int h, const Page& page) {
        const int sidebar = std::clamp(w / 4, 190, navWidth);
        nav = {0, 0, sidebar, h};
        content = {sidebar, 0, w, h};
        const int pad = 20, x0 = sidebar + pad, x1 = w - pad;
        const int headerH = page.description.empty() ? 58 : 84;
        header = {x0, pad, x1, pad + headerH};
        actions = {x0, h - 52, x1, h - 12};
        const int top = header.bottom + 8, bottom = actions.top - 12;
        if (page.kind == Page::table) {
            model = {0, 0, 0, 0};
            const int summaryH =
                page.stats.empty() ? 0 : 34 + Layout::line * int((std::min)(page.stats.size(), size_t(6)));
            stats = {x0, top, x1, top + summaryH};
            parts = {x0, stats.bottom + (summaryH ? 14 : 0), x1, bottom}; // the table
        } else {
            model = {0, 0, 0, 0};
            stats = {x0, top, x1, bottom};
            parts = {0, 0, 0, 0};
        }
    }
};

class PanelView {
public:
    Theme theme;
    Fonts fonts;
    Layout layout;
    std::vector<NavNode> roots;
    std::vector<NavNode*> visible; // flattened rows
    int scroll = 0, hoverRow = -1;
    std::wstring tip;
    POINT tipAt{};        // a tooltip, drawn last
    unsigned clipped = 0; // strings too long even when shrunk and wrapped (the render test wants 0)
    void setGuiScale(int s) {
        art();
        guiScale = s;
    } // offline checks
    ModSettings settings;  // as the mod has them (refreshed when the page opens)
    int settingsGroup = 0; // the Settings sub-page last opened (ModSettings::group)
    struct SettingControl {
        RECT r;
        int row, delta;
        bool enabled = true;
    }; // delta 0 = a switch
    std::vector<SettingControl> settingControls;
    int settingAt(POINT p, int& delta) const {
        for (const SettingControl& c : settingControls)
            if (c.enabled && PtInRect(&c.r, p)) {
                delta = c.delta;
                return c.row;
            }
        return -1;
    }
    int settingsScroll = 0;
    RECT settingsViewport{};
    static constexpr int settingsSectionHeight = 32;
    int settingsContentHeight() const {
        const auto& g = ModSettings::group(settingsGroup);
        int height = g.count * settingsPitch;
        for (int i = 0; i < g.count; ++i)
            if (ModSettings::sectionStart(g.rows[i])) height += settingsSectionHeight;
        return height;
    }
    int settingsScrollMax() const {
        return (std::max)(0, settingsContentHeight() - int(settingsViewport.bottom - settingsViewport.top));
    }
    void scrollSettings(int rows) {
        settingsScroll = std::clamp(settingsScroll + rows * settingsPitch, 0, settingsScrollMax());
        tip.clear();
    }
    int settingsPitch = 44; // Compact windows stack labels above values instead of squeezing three columns.
    NavNode* selected = nullptr;
    Page page;
    std::string message = "Loading the catalog...";
    void flatten() {
        visible.clear();
        for (NavNode& n : roots) addVisible(n);
    }
    int rowAt(int x, int y) const {
        if (x >= layout.nav.right || y < Layout::navTop) return -1;
        const int r = (y - Layout::navTop) / Layout::rowHeight + scroll;
        return r >= 0 && r < int(visible.size()) ? r : -1;
    }
    int visibleRows(int h) const { return (std::max)(1, (h - Layout::navTop) / Layout::rowHeight); }
    void scrollBy(int rows, int h) {
        scroll =
            (std::max)(0, (std::min)(scroll + rows, (std::max)(0, int(visible.size()) - visibleRows(h))));
    }

    // Paints everything except the model picture (drawn into layout.model by the caller).
    void paint(HDC dc, int w, int h) {
        layout.compute(w, h, page);
        surface(dc, {0, 0, w, h}, false);
        surface(dc, layout.nav, true);
        fill(dc, {layout.nav.right - 1, 0, layout.nav.right, h}, theme.line);
        SetBkMode(dc, TRANSPARENT);
        // Settings only: a plain title, no search and no service light
        text(dc, fonts.heading, theme.accent, {16, 16, layout.nav.right - 12, 40}, L"EXCAMERA", DT_LEFT);
        const int rows = visibleRows(h);
        for (int i = 0; i < rows && scroll + i < int(visible.size()); ++i) {
            const NavNode* n = visible[size_t(scroll + i)];
            const int y = Layout::navTop + i * Layout::rowHeight;
            RECT r{0, y, layout.nav.right - 1, y + Layout::rowHeight};
            const bool band = n->depth == 0 || !n->kids.empty(); // each category its own band and border
            if (band) {
                fill(dc, r,
                     n->depth == 0   ? RGB(22, 24, 24)
                     : n->depth == 1 ? RGB(28, 31, 31)
                                     : RGB(34, 37, 37));
                fill(dc, {r.left, r.top, r.right, r.top + 1}, theme.line);
                fill(dc, {r.left, r.bottom - 1, r.right, r.bottom}, RGB(16, 17, 17));
                fill(dc, {r.left, r.top, r.left + (n->depth == 0 ? 4 : 2), r.bottom}, theme.accent);
            }
            if (n == selected)
                fill(dc, r, theme.select);
            else if (scroll + i == hoverRow)
                fill(dc, r, theme.hover);
            for (int d = 1; d < n->depth; ++d)
                fill(dc, {8 + d * 14, r.top, 9 + d * 14, r.bottom},
                     RGB(70, 78, 78)); // guide lines down each open category
            const int indent = 14 + n->depth * 14;
            if (!n->kids.empty())
                text(dc, fonts.note, theme.dim, {indent - 13, y, indent, y + Layout::rowHeight},
                     n->expanded ? L"\x25BE" : L"\x25B8", DT_LEFT);
            HFONT f = n->depth == 0 ? fonts.heading : fonts.body;
            COLORREF c =
                n->depth == 0 ? theme.text : (n->request.empty() && n->kids.empty() ? theme.dim : theme.text);
            text(dc, f, c,
                 {indent, y, (n->right.empty() ? layout.nav.right - 12 : layout.nav.right - 52),
                  y + Layout::rowHeight},
                 wide(n->depth == 0 ? upper(n->label) : n->label).c_str(), DT_LEFT | DT_END_ELLIPSIS);
            text(dc, fonts.note, theme.dim,
                 {layout.nav.right - 56, y, layout.nav.right - 12, y + Layout::rowHeight},
                 wide(n->right).c_str(), DT_RIGHT);
        }
        if (int(visible.size()) > rows) { // scroll bar
            const int track = h - Layout::navTop, thumb = (std::max)(24, track * rows / int(visible.size()));
            const int top =
                Layout::navTop + (track - thumb) * scroll / (std::max)(1, int(visible.size()) - rows);
            fill(dc, {layout.nav.right - 5, top, layout.nav.right - 2, top + thumb}, theme.line);
        }
        // content
        if (page.kind == Page::none) {
            text(dc, fonts.body, theme.dim, layout.header, wide(message).c_str(), DT_LEFT);
            return;
        }
        RECT t = layout.header;
        t.bottom = t.top + 34;
        text(dc, fonts.title, theme.text, t, wide(page.title).c_str(), DT_LEFT | DT_END_ELLIPSIS);
        if (!page.description.empty())
            text(dc, fonts.note, theme.dim, {t.left, t.bottom + 4, t.right, layout.header.bottom - 6},
                 wide(page.description).c_str(), DT_LEFT | DT_WORDBREAK);
        fill(dc, {layout.header.left, layout.header.bottom, layout.header.right, layout.header.bottom + 1},
             theme.line);
        if (page.settingsPage) settingsView(dc);
    }
    // a tooltip near the mouse, over everything (the caller paints it after paint).
    void paintTip(HDC dc, int w, int h) {
        if (tip.empty()) return;
        const int width = 380, lines = (std::max)(1, (art() ? gameFont.width(tip, fontSize(fonts.note))
                                                            : int(tip.size()) * 7) /
                                                             (width - 20) +
                                                         1);
        const int height =
            (std::min)(200, 12 + lines * (art() ? gameFont.lineHeight(fontSize(fonts.note)) : 16) + 4);
        int x = int(tipAt.x) + 16, y = int(tipAt.y) + 18;
        if (x + width > w) x = (std::max)(0, w - width - 4);
        const int bottom = page.settingsPage ? (std::min)(h, int(layout.actions.top) - 4) : h;
        if (y + height > bottom) y = (std::max)(0, (std::min)(int(tipAt.y) - height - 6, bottom - height));
        RECT r{x, y, x + width, y + height};
        fill(dc, r, RGB(36, 38, 36));
        frame(dc, r, theme.accent);
        text(dc, fonts.note, theme.text, {r.left + 10, r.top + 6, r.right - 10, r.bottom - 4}, tip.c_str(),
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

private:
    void addVisible(NavNode& n) {
        visible.push_back(&n);
        if (n.expanded)
            for (NavNode& k : n.kids) addVisible(k);
    }
    static std::string upper(std::string s) {
        for (char& c : s) c = char(toupper((unsigned char)c));
        return s;
    }
    static void fill(HDC dc, RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    static void frame(HDC dc, RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &r, b);
        DeleteObject(b);
    }
    static void gdiText(HDC dc, HFONT f, COLORREF c, RECT r, const wchar_t* s, UINT flags) {
        HGDIOBJ old = SelectObject(dc, f);
        const UINT how = flags | ((flags & DT_WORDBREAK) ? 0 : DT_SINGLELINE | DT_VCENTER) | DT_NOPREFIX;
        SetTextColor(dc, RGB(0, 0, 0));
        RECT sr = r;
        OffsetRect(&sr, 2, 2);
        DrawTextW(dc, s, -1, &sr, how); // the drop shadow here too
        SetTextColor(dc, c);
        DrawTextW(dc, s, -1, &r, how);
        SelectObject(dc, old);
    }
    // the game's own UI art, read from the game's Resource.rpk once (works without the service).
    uiart::GameFont gameFont;
    uiart::Bitmap stone, stoneDark, logo;
    bool artTried = false, artOk = false;
    bool art() {
        if (artTried) return artOk;
        artTried = true;
        if (!gameFont.load()) return false;
        uiart::Image s;
        if (uiart::loadTexture("guitex.rfi", s)) {
            stone.make(s, RGB(78, 80, 78));
            stoneDark.make(s, RGB(60, 62, 60));
        } // darker, calmer stone
        guiScale = gameGuiScale();
        artOk = true;
        return true;
    }
    // The GDI font sizes the layout was made for, as pixel sizes for the game font (GUISize scales them).
    int fontSize(HFONT f) const {
        const int unscaled = f == fonts.title ? 32 : f == fonts.body ? 19 : f == fonts.heading ? 17 : 16;
        return (unscaled * guiScale + 50) / 100;
    } // +25 %
    int guiScale = 100; // percent; from Exanima.ini "GUISize" when the game has one
    // The game registers GUISize (0x9C820, default 2) and saves settings to %APPDATA%\Exanima\Exanima.ini
    // as "Key = value" lines; with no GUISize line the game's default is in use.
    // Provisional: the size steps are not read from the game's code (1 = 75 %, 2 = 100 %, 3 = 125 %).
    static int gameGuiScale() {
        wchar_t app[MAX_PATH];
        if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return 100;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (std::wstring(app) + L"\\Exanima\\Exanima.ini").c_str(), L"rb") || !f) return 100;
        char line[256];
        int scale = 100;
        while (fgets(line, sizeof line, f)) {
            if (_strnicmp(line, "GUISize", 7)) continue;
            const char* v = line + 7;
            while (*v == ' ' || *v == '=' || *v == ':' || *v == '\t') ++v;
            const int n = atoi(v);
            if (n >= 1 && n <= 4) scale = 50 + 25 * n;
            break;
        }
        fclose(f);
        return scale;
    }
    void surface(HDC dc, RECT r, bool darker) {
        const uiart::Bitmap& b = darker ? stoneDark : stone;
        if (!art() || !b.dc) {
            fill(dc, r, darker ? theme.pane : theme.bg);
            return;
        }
        for (int y = r.top - (r.top % b.h); y < r.bottom; y += b.h)
            for (int x = r.left - (r.left % b.w); x < r.right; x += b.w) {
                const int x0 = (std::max)(x, int(r.left)), y0 = (std::max)(y, int(r.top)),
                          x1 = (std::min)(x + b.w, int(r.right)), y1 = (std::min)(y + b.h, int(r.bottom));
                if (x1 > x0 && y1 > y0) BitBlt(dc, x0, y0, x1 - x0, y1 - y0, b.dc, x0 - x, y0 - y, SRCCOPY);
            }
    }
    // nothing is ever cut with dots. A line that does not fit
    // is drawn smaller (down to 72 %), then wrapped onto a second line when the box has room; only a box too
    // small for even that clips, and clipped counts it (the render test fails on any).
    static std::wstring noDots(std::wstring s) {
        while (s.size() >= 3 && s.compare(s.size() - 3, 3, L"...") == 0) s.resize(s.size() - 3);
        if (!s.empty() && s.back() == L'\x2026') s.pop_back();
        return s;
    }
    std::vector<std::wstring> wrapLines(const std::wstring& str, int size, int width) const {
        std::vector<std::wstring> lines;
        std::wstring lineNow, word;
        auto flush = [&] {
            lines.push_back(lineNow);
            lineNow.clear();
        };
        for (size_t i = 0; i <= str.size(); ++i) {
            const wchar_t ch = i < str.size() ? str[i] : L' ';
            if (ch == L'\n') {
                if (!word.empty()) {
                    lineNow += (lineNow.empty() ? L"" : L" ") + word;
                    word.clear();
                }
                flush();
                continue;
            }
            if (ch != L' ') {
                word += ch;
                continue;
            }
            if (word.empty()) continue;
            const std::wstring tryLine = lineNow.empty() ? word : lineNow + L" " + word;
            if (gameFont.width(tryLine, size) <= width || lineNow.empty())
                lineNow = tryLine;
            else {
                flush();
                lineNow = word;
            }
            word.clear();
        }
        if (!lineNow.empty()) flush();
        return lines;
    }
    void text(HDC dc, HFONT f, COLORREF c, RECT r, const wchar_t* s, UINT flags) {
        const std::wstring str = noDots(s ? s : L"");
        if (!art() || !gameFont.drawable(str)) {
            gdiText(dc, f, c, r, str.c_str(), flags & ~UINT(DT_END_ELLIPSIS));
            return;
        }
        const int full = fontSize(f), smallest = (std::max)(9, full * 72 / 100), width = r.right - r.left,
                  height = r.bottom - r.top;
        if (width <= 0 || str.empty()) return;
        int size = full;
        std::vector<std::wstring> lines;
        bool wrap = (flags & DT_WORDBREAK) != 0;
        for (;; --size) {
            const int lh = gameFont.lineHeight(size);
            if (wrap) {
                lines = wrapLines(str, size, width);
                bool fits = int(lines.size()) * lh <= height + 2;
                for (const auto& l : lines)
                    if (gameFont.width(l, size) > width) fits = false;
                if (fits) break;
            } else {
                lines = {str};
                if (gameFont.width(str, size) <= width) break;
            }
            if (size <= smallest) {
                if (!wrap) {
                    lines = wrapLines(str, size, width);
                    bool fits = int(lines.size()) * gameFont.lineHeight(size) <= height + 4;
                    for (const auto& l : lines)
                        if (gameFont.width(l, size) > width) fits = false;
                    if (fits && lines.size() > 1) {
                        wrap = true;
                        break;
                    }
                    lines = {str};
                }
                ++clipped;
                break;
            }
        }
        const int lh = gameFont.lineHeight(size);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, r.left - 2, r.top - 2, r.right + 2,
                          r.bottom + 5); // descenders and _ may reach just below the box
        int y = wrap && !(flags & DT_WORDBREAK) ? int(r.top) + (height - int(lines.size()) * lh) / 2
                : (flags & DT_WORDBREAK)        ? int(r.top)
                                                : int(r.top) + (height - lh) / 2;
        for (const std::wstring& l : lines) {
            const int lw = gameFont.width(l, size);
            const int x = (flags & DT_CENTER)  ? int(r.left) + (width - lw) / 2
                          : (flags & DT_RIGHT) ? int(r.right) - lw
                                               : int(r.left);
            for (const POINT o : {POINT{-1, 0}, POINT{1, 0}, POINT{0, -1}, POINT{0, 1}, POINT{2, 2}})
                gameFont.line(dc, x + o.x, y + o.y, l, size, RGB(0, 0, 0)); // outline + drop shadow
            gameFont.line(dc, x, y, l, size, c);
            y += lh;
        }
        RestoreDC(dc, saved);
    }
    void settingsView(HDC dc) {
        settingControls.clear();
        RECT r = layout.stats;
        const ModSettings::Group& g = ModSettings::group(settingsGroup);
        text(dc, fonts.note, theme.dim, {r.left, r.top, r.right, r.top + 22},
             L"Hover a setting for help. Scroll for more options.", DT_LEFT);
        settingsViewport = {r.left, r.top + 30, r.right, layout.actions.top - 10};
        const bool stacked = r.right - r.left < 620;
        const int nextPitch = stacked ? 76 : 44;
        if (nextPitch != settingsPitch) {
            settingsScroll = settingsScroll / settingsPitch * nextPitch;
            settingsPitch = nextPitch;
        }
        settingsScroll = std::clamp(settingsScroll, 0, settingsScrollMax());
        int y = settingsViewport.top - settingsScroll;
        const int pitch = settingsPitch, ch = pitch - 8;
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, settingsViewport.left, settingsViewport.top, settingsViewport.right,
                          settingsViewport.bottom);
        bool thirdPersonSection = false;
        for (int i = 0; i < g.count; ++i, y += pitch) {
            const int row = g.rows[i];
            if (const wchar_t* section = ModSettings::sectionStart(row)) {
                thirdPersonSection = row == setting::tpDistance;
                fill(dc, {r.left, y, r.right - 10, y + settingsSectionHeight - 4}, theme.pane);
                text(dc, fonts.heading, theme.accent,
                     {r.left + 6, y, r.right - 16, y + settingsSectionHeight - 4}, section, DT_LEFT);
                y += settingsSectionHeight;
            }
            if (y + pitch <= settingsViewport.top || y >= settingsViewport.bottom) continue;
            const int x0 = r.left, x1 = r.right - 10, buttons = x1 - 94;
            const int valueX = stacked ? x0 : x0 + (std::max)(140, (buttons - x0 - 16) * 45 / 100);
            const int controlTop = stacked ? y + 28 : y;
            const int controlBottom = controlTop + 36;
            const bool enabled = settings.inactiveReason(row) == nullptr;
            std::wstring label = ModSettings::label(row);
            if (thirdPersonSection && label.rfind(L"Third person: ", 0) == 0) label.erase(0, 14);
            if (!label.empty()) label[0] = wchar_t(towupper(label[0]));
            text(dc, fonts.body, enabled ? theme.text : theme.dim,
                 {x0, y, stacked ? x1 : valueX - 12, stacked ? y + 26 : y + ch}, label.c_str(),
                 DT_LEFT | DT_WORDBREAK);
            const std::wstring value = enabled ? settings.value(row)
                                               : ((row == setting::tpDeadzoneX || row == setting::tpDeadzoneY)
                                                      ? L"Inactive: Screen centre"
                                                      : L"Inactive (hover for help)");
            text(dc, fonts.body, enabled ? theme.accent : theme.dim,
                 {valueX, controlTop, buttons - 12, controlBottom}, value.c_str(), DT_LEFT | DT_WORDBREAK);
            auto button = [&](RECT b, const wchar_t* t, int delta) {
                fill(dc, b, enabled ? theme.button : theme.buttonOff);
                frame(dc, b, theme.line);
                text(dc, fonts.body, enabled ? theme.text : theme.dim, b, t, DT_CENTER);
                RECT visibleRect{};
                if (IntersectRect(&visibleRect, &b, &settingsViewport))
                    settingControls.push_back({visibleRect, row, delta, enabled});
            };
            if (ModSettings::isNumber(row)) {
                button({buttons, controlTop, buttons + 42, controlBottom}, L"\x2212", -1);
                button({buttons + 50, controlTop, buttons + 92, controlBottom}, L"+", 1);
            } else
                button({buttons, controlTop, buttons + 92, controlBottom}, L"Change", 0);
            fill(dc, {x0, y + ch + 4, x1, y + ch + 5}, RGB(44, 47, 51));
        }
        RestoreDC(dc, saved);
        if (settingsScrollMax() > 0) {
            const int height = settingsViewport.bottom - settingsViewport.top;
            const int thumb = (std::max)(20, height * height / settingsContentHeight());
            const int top = settingsViewport.top + (height - thumb) * settingsScroll / settingsScrollMax();
            fill(dc, {r.right - 4, settingsViewport.top, r.right, settingsViewport.bottom}, theme.buttonOff);
            fill(dc, {r.right - 4, top, r.right, top + thumb}, theme.accent);
        }
        if (g.count) { // this page's rows back to their defaults
            const RECT b{layout.actions.right - 114, layout.actions.top, layout.actions.right,
                         layout.actions.bottom};
            fill(dc, b, theme.button);
            frame(dc, b, theme.line);
            text(dc, fonts.body, theme.text, b, L"Reset page", DT_CENTER);
            settingControls.push_back({b, ModSettings::resetControl, 0});
        }
        text(dc, fonts.note, theme.dim,
             {layout.actions.left, layout.actions.top, layout.actions.right - 130, layout.actions.bottom},
             wide(page.status.empty() ? "Changes apply and save automatically." : page.status).c_str(),
             DT_LEFT | DT_WORDBREAK);
    }
    // What a settings row's tooltip says (the rows whose effect is not plain from the label).
public:
    const wchar_t* settingTipAt(POINT p) const {
        if (!page.settingsPage) return nullptr;
        if (!PtInRect(&settingsViewport, p)) return nullptr;
        const ModSettings::Group& g = ModSettings::group(settingsGroup);
        const int position = p.y - settingsViewport.top + settingsScroll;
        int top = 0;
        for (int i = 0; i < g.count; ++i) {
            const int row = g.rows[i];
            if (ModSettings::sectionStart(row)) top += settingsSectionHeight;
            if (position < top) return nullptr; // Section headings are not setting rows.
            if (position < top + settingsPitch) {
                const wchar_t* inactive = settings.inactiveReason(row);
                return inactive ? inactive : ModSettings::tip(row);
            }
            top += settingsPitch;
        }
        return nullptr;
    }
};

} // namespace panel
