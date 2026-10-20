#pragma once
// Settings schema and persistence identity, independent of a presentation backend.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cwctype>
#include "camera_motion.h"
#include "input_policy.h"
#include "held_object.h"
namespace panel {
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
    autoRecenter,
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
    edgeSoftness,
    tracking,
    trackingKey,
    invertY,
    indicator,
    count
};
}
inline bool fixedSetting(int row) {
    switch (row) {
    case setting::headSteady: case setting::headRoll: case setting::headPitch:
    case setting::headYaw: case setting::headBob: case setting::autoRecenter:
    case setting::fpDeadzoneX: case setting::fpDeadzoneY:
    case setting::tpDeadzoneX: case setting::tpDeadzoneY: case setting::edgeSoftness:
    case setting::eyeSetback: case setting::nearClip:
    case setting::grabRange: case setting::rmbMode:
    case setting::spineFollow: case setting::holdHeading:
    case setting::tpBlockers:
        return true;
    default: return false;
    }
}

struct ModSettings;
inline bool stepSchema(ModSettings& settings,int row,int delta);

struct ModSettings {
    bool invertY = false;
    int indicator = 1;
    bool tracking = false;
    int trackingKey = 0;
    int trackingStatus = 0; // runtime only: off/waiting/receiving/unavailable
    int fov = 75, sensitivity = 100,
        headSteady = 100;
    bool rawMouse = false, hideHelmet = false, hideSwingCrosshair = false;
    bool rawMouseFallback = false;        // runtime status, not a saved preference
    bool savePending = false;             // runtime status, not a saved preference
    static constexpr int fixedGrabRangeCm = 300, fixedSpineFollow = 60;
    static constexpr bool fixedHeadingCorrection = true;
    int grabRange = fixedGrabRangeCm; // owner-selected fixed maximum reach
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
    int rmbMode = 0; // obsolete RMB free-cursor gesture disabled; manual guard owns RMB
    bool idleCrosshairAlways = false; // normal exploration crosshair; the aim marker is separate
    bool freezeWindows = true;        // the game's windows free the cursor and hold look and movement
    int spineFollow = 60; // how far the spine bends with the look (forward looking down, back looking up)
    int crouchMode = 2; // 2=toggle crouch (checkbox on), 1=hold crouch (off); legacy 0 maps to hold
    // Fixed engineering parameters. The camera is intentionally mounted at the
    // head's center (16 cm setback) for clearance; visibility must be fixed
    // with first-person-only head rendering, NOT by moving the camera.
    static constexpr int fixedNearClipTenths = 10, fixedEyeSetbackCm = 16;
    int nearClip = fixedNearClipTenths;
    int headRoll = 100, headPitch = 100, headYaw = 100, headBob = 100;
    bool aimMarker = false;
    int flickCatchup =
        50; // the crosshair while the head catches up; how much a flick's lead is eased // how much head wobble on each axis stiffens the neck (0 = not at all)
    int tpLag = 40;
    bool tpDown = true;
    int tpBlockers =
        fp::blockersSmart; // what is in the way: hide / push in / smart (characters and placed objects pull in)
    bool tpCombatAimFree = true;
    int fpDeadzoneX = 25, fpDeadzoneY = 25, tpDeadzoneX = 25, tpDeadzoneY = 25;
    bool leanKeysMode = false;
    int leanLeftKey = 0, leanRightKey = 0;
    int eyeSetback = fixedEyeSetbackCm; // centered head camera; collision unaffected
    bool holdHeading = true, autoRecenter = true;
    int edgeSoftness = fp::defaultAimEdgeSoftness; // percent of oval radius used to feather camera turning
    int leanKey = 0x12; // held, the mouse leans the upper body (virtual-key code; 0 unbound)
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
        if (fixedSetting(row)) return false;
        auto clamp = [](int v, int lo, int hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        const ModSettings before = *this;
        if (stepSchema(*this,row,delta)) return !(*this == before);
        switch (row) {
        case setting::indicator: indicator = (indicator + (delta < 0 ? 2 : 1)) % 3; break;
        case setting::trackingKey:
            if (delta) {
                do { trackingKey = nextKey(trackingKey, delta); }
                while (trackingKey && trackingKey < 8);
            }
            break;
        case setting::headSteady:
            headSteady = clamp(headSteady + delta * 10, 0, 100);
            break;
        case setting::tpBlockers:
            tpBlockers = (tpBlockers + (delta < 0 ? 2 : 1)) % 3;
            break; // hide -> push -> smart, or back
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
        case setting::edgeSoftness:
            edgeSoftness = clamp(edgeSoftness + delta * 5, 0, 50);
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
        case setting::headRoll:
            headRoll = clamp(headRoll + delta * 10, 0, 100);
            break;
        case setting::headPitch:
            headPitch = clamp(headPitch + delta * 10, 0, 100);
            break;
        case setting::headYaw:
            headYaw = clamp(headYaw + delta * 10, 0, 100);
            break;
        case setting::headBob:
            headBob = clamp(headBob + delta * 10, 0, 100);
            break;
        case setting::crouchMode:
            crouchMode = crouchMode == 2 ? 1 : 2;
            break; // native checkbox: toggle crouch on or hold to crouch
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
             {setting::fov, setting::hideHelmet, setting::flickCatchup,
              setting::tpFov, setting::tpDistance, setting::tpShoulder, setting::tpHeight,
              setting::tpLag, setting::tpTight, setting::tpCombat,
              setting::tpDown, setting::tpCombatAim}, 12},
            {"Controls",
             {setting::rotateHeldKey, setting::sensitivity,
              setting::invertY, setting::rawMouse, setting::crouchMode,
              setting::freezeWindows, setting::tracking, setting::trackingKey}, 8},
            {"Combat",
             {setting::fpDefense, setting::fpSwingSelection, setting::indicator, setting::attackCue,
              setting::cueSize, setting::cueOpacity, setting::cueThrust}, 7},
        };
        return groups[g < 0 || g >= groupCount ? 0 : g];
    }
    static const char* groupDescription(int g) {
        static constexpr const char* descriptions[groupCount] = {
            "Field of view, framing, camera movement and character pose.",
            "Mouse, keys, cursor and interaction. Explicit leaning is disabled.",
            "Combat policy and attack indicators.",
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
        case setting::autoRecenter:
            return L"VIEW RECENTERING";
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
        case setting::invertY: return invertY ? L"On" : L"Off";
        case setting::indicator: return indicator == 0 ? L"Off" : indicator == 1 ? L"Combat" : L"Always";
        case setting::tracking:
            return !tracking ? L"Off" : trackingStatus == 3 ? L"Unavailable (UDP port in use?)"
                                    : trackingStatus == 2 ? L"OpenTrack UDP - receiving"
                                                          : L"OpenTrack UDP - waiting";
        case setting::trackingKey:
            return fp::heldKeyName(trackingKey);
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
            return tpBlockers == fp::blockersHide   ? L"hide props only"
                   : tpBlockers == fp::blockersPush ? L"pull camera in"
                                                    : L"automatic pull-in";
        case setting::tpCombatAim:
            return tpCombatAimFree ? L"Aim oval" : L"Screen centre";
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
        case setting::autoRecenter:
            return autoRecenter ? L"slowly follow cursor" : L"off (manual only)";
        case setting::edgeSoftness:
            return edgeSoftness == 0 ? L"Off (hard edge)"
                                     : std::to_wstring(edgeSoftness) + L"% of oval radius";
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
            return L"Choose Aim oval above to use this setting.";
        if ((row == setting::headRoll || row == setting::headPitch || row == setting::headYaw ||
             row == setting::headBob || row == setting::holdHeading) &&
            headSteady == 0)
            return L"Enable Head stabilisation to use this setting.";
        if (row == setting::holdHeading && headYaw == 0)
            return L"Increase Head steadiness: yaw to use heading correction.";
        if ((row == setting::cueSize || row == setting::cueOpacity || row == setting::cueThrust) &&
            !attackCue)
            return L"Enable Attack wind-up arc to use this setting.";
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
struct SettingRange {
    int min=0, max=0, step=0, count=0;
    bool inherit=false;
    int actionStep=0; // zero uses step; legacy row actions may be coarser than a slider
};
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
    SettingRange range{};
    bool editable() const { return row >= 0 && !fixedSetting(row); }
    // Headwear uses explicit Show / Hide choices, not an ambiguous negative checkbox.
    bool checkbox() const { return editable() && row != setting::hideHelmet && (flag || row == setting::crouchMode); }
};
inline const RowInfo& rowInfo(int row) {
    static const RowInfo table[setting::count] = {
        {setting::fov, L"Base field of view", RowInfo::number, &ModSettings::fov, nullptr, L"Camera", L"FOV",
         iniFov, nullptr,
         L"Vertical field of view for first person. Third person shares it unless its own field of view is "
         L"set. "
         L"F9 / F10 / F11 lower, raise or reset this base value.", {fp::fovMin,fp::fovMax,5,(fp::fovMax-fp::fovMin)/5}},
        {setting::sensitivity, L"Look sensitivity", RowInfo::number, &ModSettings::sensitivity, nullptr,
         L"Input", L"SensitivityPercent", nullptr, nullptr,
         L"View turning speed. Movement inside the aim oval, the free pointer, leaning and held-object "
         L"rotation use their own mouse movement and are not scaled by this setting.", {10,400,10,39}},
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
         L"Hide all hides verified headwear, including hair. Supported visible headwear stays attached to "
         L"your view and draws its inside faces. Shadows and other camera views keep native animation. "
         L"Moving the eye farther back can make more "
         L"headwear block the view."},
        {setting::grabRange, L"Grab range", RowInfo::number, &ModSettings::grabRange, nullptr, L"Input",
         L"GrabRange", nullptr, nullptr,
         L"Maximum interaction distance measured from the eyes, including in third person. "
         L"Walls, item rules and carrying limits still apply. This does not change weapon reach.", {fp::grabRangeMin,fp::grabRangeMax,10,(fp::grabRangeMax-fp::grabRangeMin)/10}},
        {setting::swingCrosshair, L"Crosshair during a swing", RowInfo::toggle, nullptr,
         &ModSettings::hideSwingCrosshair, L"View", L"SwingCrosshair", iniHidden, iniHiddenText, nullptr},
        {setting::tpDistance, L"Third person: distance", RowInfo::number, &ModSettings::tpDistance, nullptr,
         L"ThirdPerson", L"Distance", nullptr, nullptr,
         L"Preferred distance behind the eyes. Walls and blockers can bring the camera closer. "
         L"Combat framing reduces this distance by 20% while fighting.", {fp::tpDistanceMin,fp::tpDistanceMax,25,(fp::tpDistanceMax-fp::tpDistanceMin)/25}},
        {setting::tpShoulder, L"Third person: shoulder", RowInfo::number, &ModSettings::tpShoulder, nullptr,
         L"ThirdPerson", L"Shoulder", nullptr, nullptr,
         L"Left or right shoulder offset. Tight-space assistance can switch sides. Combat framing "
         L"reduces the offset to 30% while fighting; zero centres the camera.", {-fp::tpShoulderMax,fp::tpShoulderMax,10,2*fp::tpShoulderMax/10}},
        {setting::tpHeight, L"Third person: height", RowInfo::number, &ModSettings::tpHeight, nullptr,
         L"ThirdPerson", L"Height", nullptr, nullptr,
         L"Preferred height relative to the eyes. Negative values put the camera below the eyes. "
         L"Ceilings and camera collisions can limit the achieved height.", {fp::tpHeightMin,fp::tpHeightMax,5,(fp::tpHeightMax-fp::tpHeightMin)/5,false,10}},
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
         L"Lower a custom value past 30 degrees to return to Base.", {0,fp::fovMax,5,1+(fp::fovMax-fp::fovMin)/5,true}},
        {setting::tpLag, L"Third person: camera lag", RowInfo::number, &ModSettings::tpLag, nullptr,
         L"ThirdPerson", L"Lag", nullptr, nullptr,
         L"How softly the third-person camera follows the character.", {0,100,10,10}},
        {setting::tpBlockers, L"Third person: in the way", RowInfo::choice, &ModSettings::tpBlockers, nullptr,
         L"ThirdPerson", L"Blockers", fp::blockersFromText, fp::blockersText,
         L"Characters always remain visible and pull the camera closer. Hide affects verified placed props "
         L"only. Automatic pulls in for characters and placed props but ignores loose items. "
         L"Walls always pull the camera in."},
        {setting::tpDown, L"Third person: knocked down", RowInfo::toggle, nullptr, &ModSettings::tpDown,
         L"ThirdPerson", L"KnockdownFollow", nullptr, nullptr,
         L"Follow the body keeps the third-person camera from diving with the head during a knockdown. "
         L"Follow the head keeps its usual anchor. Both retain camera collision checks."},
        {setting::attackCue, L"Attack wind-up arc", RowInfo::toggle, nullptr, &ModSettings::attackCue,
         L"View", L"AttackCue", nullptr, nullptr, nullptr},
        {setting::cueSize, L"Wind-up arc: size", RowInfo::number, &ModSettings::cueSize, nullptr, L"View",
         L"CueSize", nullptr, nullptr, nullptr, {50,200,10,15}},
        {setting::cueOpacity, L"Wind-up arc: opacity", RowInfo::number, &ModSettings::cueOpacity, nullptr,
         L"View", L"CueOpacity", nullptr, nullptr, nullptr, {20,100,10,8}},
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
         L"far ahead of the physical head.", {0,100,10,10}},
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
         L"when you look up. Combat uses native attack aiming; explicit lean controls still work.", {0,100,10,10}},
        {setting::crouchMode, L"Toggle crouch", RowInfo::choice, &ModSettings::crouchMode, nullptr, L"Input",
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
         L"In third person, Aim oval lets the cursor move inside an ellipse; the view turns at its edge. "
         L"Screen centre turns the view directly and ignores the two aim-oval size settings below. "
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
        {setting::autoRecenter, L"Auto-center camera on cursor", RowInfo::toggle, nullptr,
         &ModSettings::autoRecenter, L"Input", L"AutoRecenter", nullptr, nullptr,
         L"Optional: steadily turn the view toward the cursor inside the aim oval without moving the "
         L"world-space aim target. Middle-click immediately requests the same recenter, even when off. "
         L"Only active while gameplay owns look input; suspended for menus, leaning and held-object "
         L"rotation. "
         L"First person follows the physical head, so the head may take time to achieve the new aim."},
        {setting::fpDeadzoneX, L"First person: aim oval width", RowInfo::number, &ModSettings::fpDeadzoneX,
         nullptr, L"FirstPerson", L"DeadzoneWidth", nullptr, nullptr,
         L"Width of the first-person aim oval, as a percentage of the screen. Movement inside aims; "
         L"movement past the edge turns the view. Zero means direct turning."},
        {setting::fpDeadzoneY, L"First person: aim oval height", RowInfo::number, &ModSettings::fpDeadzoneY,
         nullptr, L"FirstPerson", L"DeadzoneHeight", nullptr, nullptr,
         L"Height of the first-person aim oval, as a percentage of the screen. Zero means direct "
         L"turning. "
         L"Independent of third person."},
        {setting::tpDeadzoneX, L"Third person: aim oval width", RowInfo::number, &ModSettings::tpDeadzoneX,
         nullptr, L"ThirdPerson", L"DeadzoneWidth", nullptr, nullptr,
         L"Width of the third-person aim oval, as a percentage of the screen. "
         L"Used only with Aim oval aiming mode. Zero means direct turning."},
        {setting::tpDeadzoneY, L"Third person: aim oval height", RowInfo::number, &ModSettings::tpDeadzoneY,
         nullptr, L"ThirdPerson", L"DeadzoneHeight", nullptr, nullptr,
         L"Height of the third-person aim oval, as a percentage of the screen. "
         L"Used only with Aim oval aiming mode. Zero means direct turning."},
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
         L"Guard takes priority over a new attack. Release right mouse to stop guarding; a still-held "
         L"left mouse can resume native attacks. Movement "
         L"and aim remain available. Native reach, equipment and recovery still decide success. "
         L"If Manual is unavailable, its choice stays saved and Automatic defense remains active."},
        {setting::tpDefense, L"Third person: defense", RowInfo::choice, &ModSettings::tpDefense, nullptr,
         L"ThirdPerson", L"Defense", fp::defenseModeFromText, fp::defenseModeText,
         L"Uses the same defense choice as first person. In ordinary combat, hold right mouse to ready the weapon "
         L"and request native attack-specific interception; "
         L"it does not guarantee a block or add a timing bonus. Right mouse no longer cursor-walks in "
         L"ordinary close-view combat. Interaction and held objects keep their native controls. "
         L"If Manual is unavailable, its choice stays saved and Automatic defense remains active."},
        {setting::fpSwingSelection, L"First person: swing selection", RowInfo::choice,
         &ModSettings::fpSwingSelection, nullptr, L"FirstPerson", L"SwingSelection", iniSwingSelection,
         iniSwingSelectionText,
         L"Mouse direction: move the mouse left or right before clicking to choose the swing's starting "
         L"side. "
         L"Native facing: Exanima chooses from the angle between your aim and body. Small aim ovals may not "
         L"reach its side-change threshold. The optional AttackSideKey reverses either choice."},
        {setting::tpSwingSelection, L"Third person: swing selection", RowInfo::choice,
         &ModSettings::tpSwingSelection, nullptr, L"ThirdPerson", L"SwingSelection", iniSwingSelection,
         iniSwingSelectionText,
         L"Choose independently for third person. Mouse direction works with both Aim oval and Screen "
         L"centre. "
         L"Move left or right before clicking to choose the swing's starting side; native weapon timing "
         L"still applies."},
        {setting::edgeSoftness, L"Aim oval: edge softness", RowInfo::number, &ModSettings::edgeSoftness,
         nullptr, L"Input", L"AimEdgeSoftness", nullptr, nullptr,
         L"Gradually shares outward mouse movement with camera turning across the outer portion of the "
         L"aim oval. 25% is the default; 0% restores the original hard boundary. "
         L"The cursor still slides along the rim, inward motion is immediate, and the camera does not "
         L"keep drifting after mouse release. Applies in first and third-person Aim oval mode."},
        {setting::tracking, L"Head / eye tracking (OpenTrack UDP)", RowInfo::toggle,
         nullptr, &ModSettings::tracking, L"Input", L"Tracking", nullptr, nullptr,
         L"Receives processed view motion at 127.0.0.1:4243. Choose UDP over network in OpenTrack, "
         L"or a compatible output in Beam. Tobii Game Hub can route through OpenTrack. "
         L"Translation is unused. Device curves, calibration and eye contribution stay in that application."},
        {setting::trackingKey, L"Recenter tracking", RowInfo::keyList,
         &ModSettings::trackingKey, nullptr, L"Input", L"TrackingRecenterKey", nullptr, nullptr,
         L"Records a neutral tracking pose without changing weapon aim. Unbound by default. "
         L"Held-object rotation and lean bindings take precedence when shared."},
        {setting::invertY, L"Invert vertical look", RowInfo::toggle, nullptr, &ModSettings::invertY,
         L"Input", L"InvertY", nullptr, nullptr, L"Reverse vertical mouse look."},
        {setting::indicator, L"Aim indicator", RowInfo::choice, &ModSettings::indicator, nullptr,
         L"View", L"AimIndicator", nullptr, nullptr, L"Show the aim indicator: off, combat only, or always."},
    };
    static const RowInfo none{-1,      L"",     RowInfo::toggle, nullptr, nullptr,
                              nullptr, nullptr, nullptr,         nullptr, nullptr};
    static const auto persisted = [&] {
        std::array<RowInfo, setting::count> rows;
        std::copy(std::begin(table), std::end(table), rows.begin());
        for (int i = 0; i < setting::count; ++i)
            if (fixedSetting(i) || i == setting::tpDefense || i == setting::tpSwingSelection ||
                i == setting::swingCrosshair || i == setting::idleCrosshair || i == setting::aimMarker)
                rows[size_t(i)].key = nullptr;
        return rows;
    }();
    return row >= 0 && row < setting::count ? persisted[size_t(row)] : none;
}
// Member identity, switch semantics and numeric bounds have one schema owner.
// Inherited FOV retains its historical relative-to-base action in ModSettings::step.
inline bool stepSchema(ModSettings& settings,int row,int delta) {
    const auto& info=rowInfo(row);
    if (!info.editable()) return false;
    if (info.flag) { settings.*(info.flag)=!(settings.*(info.flag)); return true; }
    const auto& range=info.range;
    if (!range.count || range.inherit || !info.value) return false;
    settings.*(info.value)=std::clamp(settings.*(info.value)+delta*
        (range.actionStep ? range.actionStep : range.step),range.min,range.max);
    return true;
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
// Shared by interactive capture and INI normalization for active bindings.
inline bool validSettingBinding(int row, int key) {
    return key >= 0 && key <= 255 && !(key >= VK_F6 && key <= VK_F11) &&
        !(row == setting::trackingKey && key > 0 && key < 8);
}
inline void ModSettings::normalize() {
    if (!validSettingBinding(setting::trackingKey, trackingKey)) trackingKey = 0;
    if (!validSettingBinding(setting::rotateHeldKey, rotateHeldKey)) rotateHeldKey = 0;
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
    headSteady = headRoll = headPitch = headYaw = headBob = fp::fixedHeadStabilization;
    fpDeadzoneX = fpDeadzoneY = tpDeadzoneX = tpDeadzoneY = fp::fixedAimHalfExtent;
    edgeSoftness = 100;
    autoRecenter = true;
    eyeSetback = fixedEyeSetbackCm;
    nearClip = fixedNearClipTenths;
    tpBlockers = fp::blockersSmart; // automatic: keep characters visible and pull in for solid blockers
    grabRange = fixedGrabRangeCm;
    rmbMode = 0;
    spineFollow = fixedSpineFollow;
    holdHeading = fixedHeadingCorrection;
    if (crouchMode != 2) crouchMode = 1;
    tpDefense = fpDefense;
    tpSwingSelection = fpSwingSelection;
    hideSwingCrosshair = false;
    idleCrosshairAlways = indicator == 2;
    aimMarker = indicator == 2;
    leanKeysMode = leanLeftKey != 0 || leanRightKey != 0;
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

struct SettingsChange {
    ModSettings settings;
    bool changed = false, rawUnavailable = false, saved = true;
};
} // namespace panel
