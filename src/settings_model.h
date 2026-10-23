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
    bladeDrag,
    stabSink,
    bladeGain,
    idleCrosshair,
    headRoll,
    headPitch,
    headYaw,
    aimMarker,
    flickCatchup,
    headBob,
    spineFollow,
    crouchMode,
    nearClip,
    tpCombatAim,
    eyeSetback,
    rotateHeldKey,
    kickKey,
    jumpKey,
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
    vrIpd,
    vrScale,
    vrRenderMode,
    vrHands,
    vrPosture,
    vrHeadset,
    vrTurning,
    vrEffects,
    vrGripAngle,
    vrHaptics,
    vrPanelWidth,
    vrPanelDistance,
    vrComfort,
    vrBody,
    vrHeight,
    vrGrabTurn,
    vrEyeScale,
    vrGripSlide,
    vrKick,
    vrJump,
    vrWeaponWeight,
    vrTeleport,
    vrHandPanel,
    vrPouch,
    vrFingerBones,
    ambientOcclusion,
    depthOfField,
    screenSpaceReflections,
    edgeSmoothing,
    helmetVisor,
    playerGhost,
    fingerPoses,
    fingerPosesOthers,
    conditionEffects,
    heldDetail,
    armourDetail,
    objectDetail,
    levelDetail,
    woundMarks,
    drawDistance,
    objectLights,
    lightSmoothing,
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
    case setting::grabRange:
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
    int ambientOcclusion = 0; // optional screen-space contact shading
    int depthOfField = 0; // optional first-person depth-aware texture softening
    int screenSpaceReflections = 0; // optional screen-space scene reflections
    bool edgeSmoothing = false, helmetVisor = false, playerGhost = false; // optional finishing effects
    bool fingerPoses = true; // finger bones on the skeleton posed to what each hand holds; on by default, as asked
    bool fingerPosesOthers = true; // the same for other characters in the level
    bool bladeDrag = true;   // a blade edge drawn across a body deals damage through the game's own impact
    bool stabSink = true;    // a thrust sinks into a soft target instead of stopping at its surface; experimental, on as asked
    int bladeGainPercent = 100; // the gain on the draw velocity handed to the game's impact, 10..400
    bool conditionEffects = false; // optional: the view reacts to wounds and breathlessness
    int heldDetail = 50; // procedural surface detail on held items, percent; half (the old Subtle) by default (owner's request)
    int armourDetail = 35; // the same on worn armour and clothing, percent; 35 by default (frame cost)
    int objectDetail = 35; // the same on loose items and placed objects near you, percent; 35 by default (frame cost)
    int drawDistance = 60; // metres beyond which small props and far characters are not drawn; 0 draws everything
    int objectLights = 4;  // lights each mesh takes of the game's six; the strongest are kept
    int levelDetail = 30; // walls, floors and ceilings within about 3 m, percent; light by default, drops first when slow
    bool lightSmoothing = true; // the game's light grid read smoothly (shader rewrite at compile time); on by default
    bool woundMarks = true; // cuts, bruises and running blood where weapons got through; on by default (owner's request)
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
    bool idleCrosshairAlways = false; // normal exploration crosshair; the aim marker is separate
    int spineFollow = 60; // how far the spine bends with the look (forward looking down, back looking up)
    int crouchMode = 2; // 2=toggle crouch (checkbox on), 1=hold crouch (off); legacy 0 maps to hold
    int vrIpd = 0;        // mm between the eyes; 0 keeps the headset's own eye poses
    int vrScale = 100;    // game units per metre of tracked motion
    int vrRenderMode = 0; // 0 draws each eye; 1 draws the head once and reconstructs both eyes; 2 the same with holes filled
    int vrHands = 0;      // 1 draws the mod's articulated hands over tracked fingers; off by default
    int vrPosture = 0;    // 0 standing (room walking and tracked legs); 1 seated (the game's own legs, stick only)
    int vrHeadset = 0;    // 1 starts the real OpenXR runtime at the next game start; 0 leaves VR off
    int vrTurning = 1;    // 0 off, 1 snap 30, 2 snap 45, 3 smooth: the right stick turns the body as the mouse does
    int vrEffects = 0;    // 1 runs the View page's ambient occlusion and reflections on each headset eye; never depth of field
    int vrGripAngle = 0;  // degrees of pitch added to a controller grip, -45..45
    int vrHaptics = 100;  // pulse strength in percent, 0..200
    int vrPanelWidth = 120, vrPanelDistance = 150; // the head-locked UI quad, centimetres
    int vrComfort = 0;    // 0 off, 1 edges darken while turning smoothly, 2 also while walking on the stick
    int vrBody = 0;       // 1 hides the character below the head in the headset; hands and held items stay
    int vrHeight = 0;     // centimetres added to the game head, -50..50
    int vrGrabTurn = 0;   // 1 lets an empty squeezing hand sweep the body round; off by default, since it rides the pickup squeeze
    int vrEyeScale = 100; // percent of the native size each eye is drawn at, 50..100
    int vrGripSlide = 0;  // centimetres the hand slides along a held weapon's hilt, -10..10
    int vrWeaponWeight = 1; // 1: the hands lag a heavy weapon by its own heft
    int vrJump = 1;       // 1 lets a physical hop play the game's jump clip with an upward push; on, as asked
    int vrTeleport = 1;   // 1: right stick forward aims a teleport arc, release moves there
    int vrHandPanel = 1;  // 1: the inventory and every open menu are held in the left hand
    int vrPouch = 1;      // 1: the left hip puts items away and takes them back (ScriptRuntime inventory exports)
    int vrKick = 0;       // 1 lets the stick click play the game's kick clip; experimental, off by default
    int vrFingerBones = 0; // 1 adds finger bones to the game's own skeleton for the tracked joints; experimental, off by default
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
    int kickKey = 0, jumpKey = 0; // the game's unused kick and jump clips (virtual-key codes; 0 unbound, the default)
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
        if (row == setting::fingerPoses) { // Off, You, Everyone: one row over the two saved switches
            const int next = ((!fingerPoses ? 0 : fingerPosesOthers ? 2 : 1) + (delta < 0 ? 2 : 1)) % 3;
            fingerPoses = next > 0;
            fingerPosesOthers = next == 2;
            return !(*this == before);
        }
        if (stepSchema(*this,row,delta)) return !(*this == before);
        switch (row) {
        case setting::indicator: indicator = (indicator + (delta < 0 ? 2 : 1)) % 3; break;
        case setting::ambientOcclusion: ambientOcclusion = std::clamp(ambientOcclusion + delta * 10, 0, 100); break;
        case setting::depthOfField: depthOfField = std::clamp(depthOfField + delta * 10, 0, 100); break;
        case setting::screenSpaceReflections:
            screenSpaceReflections = std::clamp(screenSpaceReflections + delta * 10, 0, 100); break;
        case setting::heldDetail: heldDetail = std::clamp(heldDetail + delta * 10, 0, 100); break;
        case setting::armourDetail: armourDetail = std::clamp(armourDetail + delta * 10, 0, 100); break;
        case setting::objectDetail: objectDetail = std::clamp(objectDetail + delta * 10, 0, 100); break;
        case setting::levelDetail: levelDetail = std::clamp(levelDetail + delta * 10, 0, 100); break;
        case setting::drawDistance: drawDistance = std::clamp(drawDistance + delta * 10, 0, 200); break;
        case setting::objectLights: objectLights = std::clamp(objectLights + delta, 1, 6); break;
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
        case setting::kickKey:
            kickKey = nextKey(kickKey, delta);
            break;
        case setting::jumpKey:
            jumpKey = nextKey(jumpKey, delta);
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
        case setting::vrIpd:
            vrIpd = clamp(vrIpd + delta, 0, 100);
            break;
        case setting::vrScale:
            vrScale = clamp(vrScale + delta * 5, 50, 200);
            break;
        case setting::vrRenderMode:
            vrRenderMode = (vrRenderMode + 1) % 3;
            break;
        case setting::vrHands:
            vrHands = vrHands ? 0 : 1;
            break;
        case setting::vrPosture:
            vrPosture = vrPosture ? 0 : 1;
            break;
        case setting::vrHeadset:
            vrHeadset = vrHeadset ? 0 : 1;
            break;
        case setting::vrTurning:
            vrTurning = (vrTurning + 1) % 4;
            break;
        case setting::vrEffects:
            vrEffects = vrEffects ? 0 : 1;
            break;
        case setting::vrGripAngle:
            vrGripAngle = clamp(vrGripAngle + delta * 5, -45, 45);
            break;
        case setting::vrHaptics:
            vrHaptics = clamp(vrHaptics + delta * 10, 0, 200);
            break;
        case setting::vrPanelWidth:
            vrPanelWidth = clamp(vrPanelWidth + delta * 10, 60, 200);
            break;
        case setting::vrPanelDistance:
            vrPanelDistance = clamp(vrPanelDistance + delta * 10, 80, 300);
            break;
        case setting::vrComfort:
            vrComfort = (vrComfort + 1) % 3;
            break;
        case setting::vrBody:
            vrBody = vrBody ? 0 : 1;
            break;
        case setting::vrHeight:
            vrHeight = clamp(vrHeight + delta * 5, -50, 50);
            break;
        case setting::vrGrabTurn:
            vrGrabTurn = vrGrabTurn ? 0 : 1;
            break;
        case setting::vrEyeScale:
            vrEyeScale = clamp(vrEyeScale + delta * 10, 50, 100);
            break;
        case setting::vrGripSlide:
            vrGripSlide = clamp(vrGripSlide + delta, -10, 10);
            break;
        case setting::vrKick:
            vrKick = vrKick ? 0 : 1;
            break;
        case setting::vrJump:
            vrJump = vrJump ? 0 : 1;
            break;
        case setting::vrWeaponWeight:
            vrWeaponWeight = vrWeaponWeight ? 0 : 1;
            break;
        case setting::vrTeleport:
            vrTeleport = vrTeleport ? 0 : 1;
            break;
        case setting::vrHandPanel:
            vrHandPanel = vrHandPanel ? 0 : 1;
            break;
        case setting::vrPouch:
            vrPouch = vrPouch ? 0 : 1;
            break;
        case setting::vrFingerBones:
            vrFingerBones = vrFingerBones ? 0 : 1;
            break;
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
        int rows[32];
        int count;
    };
    // Rows a tab shows; the rest keep their saved values and defaults but have no row. 0..2 and 4 are
    // the Camera, Controls, Combat and Graphics (effects) tabs, 3 the VR tab.
    static constexpr int groupCount = 5, savesGroup = -1;
    static const Group& group(int g) {
        static const Group groups[groupCount] = {
            {"Camera",
             {setting::fov, setting::hideHelmet, setting::helmetVisor,
              setting::tpFov, setting::tpDistance, setting::tpShoulder, setting::tpHeight, setting::tpLag,
              setting::tpTight, setting::tpDown}, 10},
            {"Controls",
             {setting::rotateHeldKey, setting::kickKey, setting::jumpKey, setting::sensitivity,
                            setting::invertY, setting::rawMouse, setting::crouchMode}, 7},
            {"Combat",
             {setting::fpDefense, setting::fpSwingSelection, setting::indicator, setting::tpCombat, setting::tpCombatAim,
              setting::attackCue, setting::cueSize, setting::cueOpacity, setting::cueThrust,
              setting::bladeDrag, setting::stabSink}, 11},
            {"VR",
             {setting::vrHeadset, setting::vrIpd, setting::vrScale, setting::vrRenderMode, setting::vrHands, setting::vrPosture,
              setting::vrTurning, setting::vrEffects, setting::vrGripAngle, setting::vrHaptics, setting::vrPanelWidth,
              setting::vrPanelDistance, setting::vrComfort, setting::vrBody, setting::vrHeight, setting::vrGrabTurn, setting::vrEyeScale, setting::vrGripSlide, setting::vrKick, setting::vrJump, setting::vrWeaponWeight, setting::vrTeleport, setting::vrHandPanel, setting::vrPouch, setting::vrFingerBones, setting::tracking, setting::trackingKey}, 26},
            {"Graphics",
             {setting::ambientOcclusion, setting::screenSpaceReflections, setting::depthOfField, setting::heldDetail,
              setting::armourDetail, setting::objectDetail, setting::levelDetail, setting::woundMarks,
              setting::lightSmoothing, setting::edgeSmoothing, setting::conditionEffects, setting::fingerPoses,
              setting::playerGhost}, 13},
        };
        return groups[g < 0 || g >= groupCount ? 0 : g];
    }
    static const char* groupDescription(int g) {
        static constexpr const char* descriptions[groupCount] = {
            "Field of view, headwear and the third-person camera.",
            "Mouse, keys, cursor and interaction. Explicit leaning is disabled.",
            "Defense, attack indicators and blades.",
            "Headset, hands, comfort and head tracking.",
            "View effects, held-item detail and finger poses.",
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
        case setting::vrIpd:
            return L"VR";
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
        case setting::ambientOcclusion: return ambientOcclusion ? std::to_wstring(ambientOcclusion) + L"%" : L"Off";
        case setting::depthOfField: return depthOfField ? std::to_wstring(depthOfField) + L"%" : L"Off";
        case setting::screenSpaceReflections:
            return screenSpaceReflections ? std::to_wstring(screenSpaceReflections) + L"%" : L"Off";
        case setting::heldDetail: return heldDetail ? std::to_wstring(heldDetail) + L"%" : L"Off";
        case setting::armourDetail: return armourDetail ? std::to_wstring(armourDetail) + L"%" : L"Off";
        case setting::objectDetail: return objectDetail ? std::to_wstring(objectDetail) + L"%" : L"Off";
        case setting::levelDetail: return levelDetail ? std::to_wstring(levelDetail) + L"%" : L"Off";
        case setting::drawDistance: return drawDistance ? std::to_wstring(drawDistance) + L" m" : L"Off";
        case setting::objectLights: return objectLights >= 6 ? std::wstring(L"6 (game)") : std::to_wstring(objectLights);
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
        case setting::kickKey:
            return fp::heldKeyName(kickKey);
        case setting::jumpKey:
            return fp::heldKeyName(jumpKey);
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
        case setting::bladeGain:
            return std::to_wstring(bladeGainPercent) + L"%";
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
        case setting::spineFollow:
            return spineFollow ? std::to_wstring(spineFollow) + L"%" : std::wstring(L"off");
        case setting::nearClip: {
            wchar_t b[24];
            swprintf_s(b, L"%d.%d cm", nearClip / 10, nearClip % 10);
            return b;
        }
        case setting::vrIpd:
            return vrIpd ? std::to_wstring(vrIpd) + L" mm" : std::wstring(L"headset's own");
        case setting::vrScale:
            return std::to_wstring(vrScale) + L" per metre";
        case setting::vrRenderMode:
            return vrRenderMode == 2 ? L"reconstruct, holes filled" : vrRenderMode == 1 ? L"reconstruct from depth" : L"draw each eye";
        case setting::vrHands:
            return vrHands ? L"drawn" : L"off";
        case setting::vrPosture:
            return vrPosture ? L"seated" : L"standing";
        case setting::vrHeadset:
            return vrHeadset ? L"on at next start" : L"off";
        case setting::vrTurning:
            return vrTurning == 1 ? L"snap 30 degrees" : vrTurning == 2 ? L"snap 45 degrees" : vrTurning == 3 ? L"smooth" : L"off";
        case setting::vrEffects:
            return vrEffects ? L"occlusion and reflections per eye" : L"off";
        case setting::vrGripAngle:
            return std::to_wstring(vrGripAngle) + L" degrees";
        case setting::vrHaptics:
            return std::to_wstring(vrHaptics) + L" %";
        case setting::vrPanelWidth:
            return std::to_wstring(vrPanelWidth) + L" cm wide";
        case setting::vrPanelDistance:
            return std::to_wstring(vrPanelDistance) + L" cm ahead";
        case setting::vrComfort:
            return vrComfort == 2 ? L"turning and walking" : vrComfort == 1 ? L"turning" : L"off";
        case setting::vrBody:
            return vrBody ? L"hands only" : L"full";
        case setting::vrHeight:
            return std::to_wstring(vrHeight) + L" cm";
        case setting::vrGrabTurn:
            return vrGrabTurn ? L"on" : L"off";
        case setting::vrEyeScale:
            return std::to_wstring(vrEyeScale) + L" %";
        case setting::vrGripSlide:
            return std::to_wstring(vrGripSlide) + L" cm";
        case setting::vrKick:
            return vrKick ? L"on" : L"off";
        case setting::vrJump:
            return vrJump ? L"on" : L"off";
        case setting::vrWeaponWeight:
            return vrWeaponWeight ? L"on" : L"off";
        case setting::vrTeleport:
            return vrTeleport ? L"on" : L"off";
        case setting::vrHandPanel:
            return vrHandPanel ? L"in hand" : L"in view";
        case setting::vrPouch:
            return vrPouch ? L"on" : L"off";
        case setting::vrFingerBones:
            return vrFingerBones ? L"on" : L"off";
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
    // Two-way rows whose two states have names (Headwear, Knockdown, Aim style, Crouch, Finger grip) are buttons
    // that step through them, not checkboxes.
    bool checkbox() const {
        return editable() && flag && row != setting::hideHelmet && row != setting::tpDown && row != setting::tpCombatAim &&
               row != setting::fingerPoses;
    }
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
        {setting::bladeDrag, L"Blade drag cuts", RowInfo::toggle, nullptr, &ModSettings::bladeDrag,
         L"Combat", L"BladeDrag", nullptr, nullptr,
         L"A blade edge drawn across a body deals damage, for everyone: the sliding contact is handed to the game's own "
         L"weapon impact, which applies its usual edge, speed and armour rules. In VR this is how a throat is cut."},
        {setting::stabSink, L"Stabs sink in", RowInfo::toggle, nullptr, &ModSettings::stabSink,
         L"Combat", L"StabSink", nullptr, nullptr,
         L"Experimental. A thrust that lands point first on a soft, unarmoured part keeps travelling into it for a hand's "
         L"depth instead of stopping at the surface, cutting as it goes; armour and the head still stop it."},
        {setting::bladeGain, L"Blade draw gain", RowInfo::number, &ModSettings::bladeGainPercent, nullptr, L"Combat",
         L"BladeDrawGainPercent", nullptr, nullptr,
         L"How hard a drawn or sinking blade is taken to move for the game's impact rules: 100 is the true speed. Raise it "
         L"if slow cuts do nothing, lower it if they cut too deep.", {10,400,10,18}},
        {setting::cueSize, L"Wind-up arc: size", RowInfo::number, &ModSettings::cueSize, nullptr, L"View",
         L"CueSize", nullptr, nullptr, nullptr, {50,200,10,15}},
        {setting::cueOpacity, L"Wind-up arc: opacity", RowInfo::number, &ModSettings::cueOpacity, nullptr,
         L"View", L"CueOpacity", nullptr, nullptr, nullptr, {20,100,10,8}},
        {setting::cueThrust, L"Wind-up arc: thrusts", RowInfo::toggle, nullptr, &ModSettings::cueThrust,
         L"View", L"CueThrust", nullptr, nullptr, nullptr},
        {setting::idleCrosshair, L"Crosshair out of combat", RowInfo::toggle, nullptr,
         &ModSettings::idleCrosshairAlways, L"View", L"IdleCrosshair", iniAlways, iniAlwaysText,
         L"Controls only the exploration crosshair. Aim marker can still appear while the head catches up. "
         L"Cursor Look uses the game's free pointer."},
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
        {setting::kickKey, L"Kick", RowInfo::keyList, &ModSettings::kickKey, nullptr, L"Input", L"KickKey", fp::heldKeyFromText, nullptr,
         L"Plays the game's own kick, which it never uses itself: in the fighting stance, upright, not mid-swing and with "
         L"most of your stamina. Unbound until you pick a key; the game does not see that key while it is bound here."},
        {setting::jumpKey, L"Jump", RowInfo::keyList, &ModSettings::jumpKey, nullptr, L"Input", L"JumpKey", fp::heldKeyFromText, nullptr,
         L"Plays the game's own jump and pushes your body up: upright, not mid-swing and with half your stamina. "
         L"Unbound until you pick a key; the game does not see that key while it is bound here."},
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
        {setting::vrIpd, L"VR: eye separation", RowInfo::number, &ModSettings::vrIpd, nullptr, L"VR",
         L"IpdMillimetres", nullptr, nullptr,
         L"Distance between the two VR eyes. Zero keeps the headset's own eye poses. The eyes stay rigid "
         L"with the tracked head; nothing smooths or offsets the camera.", {0,100,1,100}},
        {setting::vrScale, L"VR: world scale", RowInfo::number, &ModSettings::vrScale, nullptr, L"VR",
         L"UnitsPerMetre", nullptr, nullptr,
         L"Game units per metre of real movement. 100 matches the mod's centimetre settings.", {50,200,5,30}},
        {setting::vrRenderMode, L"VR: rendering", RowInfo::choice, &ModSettings::vrRenderMode, nullptr, L"VR",
         L"RenderMode", fp::vrRenderModeFromText, fp::vrRenderModeText,
         L"Draw each eye renders the world twice from the real eye positions. Reconstruct from depth draws "
         L"the head once and synthesises both eyes from colour and depth; it needs a reconstruction host and "
         L"otherwise falls back to drawing each eye. Holes filled paints what the head view saw behind an edge "
         L"instead of leaving a black sliver there."},
        {setting::vrHands, L"VR: hands", RowInfo::choice, &ModSettings::vrHands, nullptr, L"VR",
         L"Hands", fp::vrHandsFromText, fp::vrHandsText,
         L"Drawn replaces the character's hand meshes with the mod's own articulated hands that follow "
         L"tracked fingers. The game has no finger bones, so off shows its rigid hands."},
        {setting::vrPosture, L"VR: posture", RowInfo::choice, &ModSettings::vrPosture, nullptr, L"VR",
         L"Posture", fp::vrPostureFromText, fp::vrPostureText,
         L"Standing drives the legs from trackers and walks the room. Seated keeps the game's own legs and "
         L"ignores room walking and crouching, so a chair never reads as a crouch. Recenter after changing it."},
        {setting::vrHeadset, L"VR: headset", RowInfo::choice, &ModSettings::vrHeadset, nullptr, L"VR",
         L"Headset", fp::vrHeadsetFromText, fp::vrHeadsetText,
         L"On starts the OpenXR runtime of the headset the next time the game starts with the mod, using the "
         L"loader shipped beside the mod. The view is the tracked head; nothing else changes until then."},
        {setting::vrTurning, L"VR: turning", RowInfo::choice, &ModSettings::vrTurning, nullptr, L"VR",
         L"Turning", fp::vrTurningFromText, fp::vrTurningText,
         L"The right stick turns the body the way the mouse does, so the view and the body never part. Snap "
         L"turns a fixed angle per flick; smooth turns while held. Off leaves turning to the head and mouse."},
        {setting::vrEffects, L"VR: effects", RowInfo::choice, &ModSettings::vrEffects, nullptr, L"VR",
         L"Effects", fp::vrEffectsFromText, fp::vrEffectsText,
         L"On runs the View page's ambient occlusion and screen-space reflections on each headset eye, at their "
         L"View strengths. Depth of field never runs in the headset: a fixed focus fights your own eyes."},
        {setting::vrGripAngle, L"VR: grip angle", RowInfo::number, &ModSettings::vrGripAngle, nullptr, L"VR",
         L"GripAngleDegrees", nullptr, nullptr,
         L"Tilts the controller's grip so a held hilt sits the way you hold it. Tracked hands are not tilted.", {-45,45,5,18}},
        {setting::vrHaptics, L"VR: haptics", RowInfo::number, &ModSettings::vrHaptics, nullptr, L"VR",
         L"HapticPercent", nullptr, nullptr,
         L"Strength of the controller pulses on grab, on your weapon landing and on being hit. Zero turns them off.", {0,200,10,20}},
        {setting::vrPanelWidth, L"VR: panel width", RowInfo::number, &ModSettings::vrPanelWidth, nullptr, L"VR",
         L"PanelWidthCentimetres", nullptr, nullptr,
         L"Width of the floating panel that shows the game's menus and the F8 panel in the headset.", {60,200,10,14}},
        {setting::vrPanelDistance, L"VR: panel distance", RowInfo::number, &ModSettings::vrPanelDistance, nullptr, L"VR",
         L"PanelDistanceCentimetres", nullptr, nullptr,
         L"How far ahead that panel floats. Nearer is larger and easier to read; farther is easier on the eyes.", {80,300,10,22}},
        {setting::vrComfort, L"VR: comfort vignette", RowInfo::choice, &ModSettings::vrComfort, nullptr, L"VR",
         L"Comfort", fp::vrComfortFromText, fp::vrComfortText,
         L"Darkens the edges of the view while the body turns smoothly, and with walking also while the stick "
         L"walks, which many people find settles the stomach. The centre stays clear."},
        {setting::vrBody, L"VR: body", RowInfo::choice, &ModSettings::vrBody, nullptr, L"VR",
         L"Body", fp::vrBodyFromText, fp::vrBodyText,
         L"Hands only hides the character's own body and worn armour below the head while the headset drives it; "
         L"the hands (the mod's own, drawn as if VR hands were on) and anything held stay. Full shows the body."},
        {setting::vrHeight, L"VR: height", RowInfo::number, &ModSettings::vrHeight, nullptr, L"VR",
         L"HeightCentimetres", nullptr, nullptr,
         L"Lifts or lowers the game head by this much, for a chair or a player taller or shorter than the character. "
         L"Leaning, ducking and turning stay tracked.", {-50,50,5,20}},
        {setting::vrGrabTurn, L"VR: grab turn", RowInfo::choice, &ModSettings::vrGrabTurn, nullptr, L"VR",
         L"GrabTurn", fp::vrGrabTurnFromText, fp::vrGrabTurnText,
         L"On: squeeze an empty hand and sweep it sideways to drag the world round, through the same body turn as the stick. "
         L"A hand holding something, or any drawn weapon, never turns. Off leaves turning to the stick."},
        {setting::vrEyeScale, L"VR: eye scale", RowInfo::number, &ModSettings::vrEyeScale, nullptr, L"VR",
         L"EyeScalePercent", nullptr, nullptr,
         L"How large each eye is drawn, as a share of the window size: lower is cheaper and softer. Applies to the "
         L"per-eye draws only; mirror and reconstruction modes and the desktop stay at full size.", {50,100,10,5}},
        {setting::vrGripSlide, L"VR: grip slide", RowInfo::number, &ModSettings::vrGripSlide, nullptr, L"VR",
         L"GripSlideCentimetres", nullptr, nullptr,
         L"Slides the hand along a held weapon's hilt: positive towards the blade, negative towards the pommel.", {-10,10,1,20}},
        {setting::vrKick, L"VR: kick", RowInfo::choice, &ModSettings::vrKick, nullptr, L"VR",
         L"Kick", fp::vrKickFromText, fp::vrKickText,
         L"Experimental. On: pressing a stick in plays the game's own kick, only in the fighting stance, upright, not mid-swing "
         L"and with most of your stamina. The game never plays this clip itself, so a hit lands by foot contact only."},
        {setting::vrJump, L"VR: jump", RowInfo::choice, &ModSettings::vrJump, nullptr, L"VR",
         L"Jump", fp::vrKickFromText, fp::vrKickText,
         L"On, standing: hopping in real life plays the game's own jump and pushes your body up, when upright, "
         L"not mid-swing and with half your stamina. The game never jumps by itself, so landing is its ordinary physics."},
        {setting::vrWeaponWeight, L"VR: weapon weight", RowInfo::choice, &ModSettings::vrWeaponWeight, nullptr, L"VR",
         L"WeaponWeight", fp::vrKickFromText, fp::vrKickText,
         L"On: a held weapon follows your hands with a lag set by its own heft, so a heavy axe trails a fast swing and a "
         L"dagger does not; a two-handed grip carries it better."},
        {setting::vrTeleport, L"VR: teleport", RowInfo::choice, &ModSettings::vrTeleport, nullptr, L"VR",
         L"Teleport", fp::vrKickFromText, fp::vrKickText,
         L"On: push the right stick forward to aim an arc from your right hand, let go to move there. Walls stop the arc; "
         L"not while dragging something, attacking, down or out of reach. Walking on the left stick stays as it is."},
        {setting::vrHandPanel, L"VR: menus in hand", RowInfo::choice, &ModSettings::vrHandPanel, nullptr, L"VR",
         L"HandPanel", fp::vrKickFromText, fp::vrKickText,
         L"On: the inventory and every open menu sit on a panel held in your left hand, turned towards you; point at it "
         L"with the right hand, the trigger clicks and the grip is the right button. Hold the trigger on an item and point "
         L"out into the world to carry it out; let go to set it down there and pick it up. Off: the menu floats in front of you."},
        {setting::vrPouch, L"VR: hip pouch", RowInfo::choice, &ModSettings::vrPouch, nullptr, L"VR",
         L"HipPouch", fp::vrKickFromText, fp::vrKickText,
         L"On: let go of a held item at your left hip to put it in your inventory; close an empty hand there to take the "
         L"last one back. Needs the script runtime's inventory exports; without them nothing happens."},
        {setting::vrFingerBones, L"VR: finger bones", RowInfo::choice, &ModSettings::vrFingerBones, nullptr, L"VR",
         L"FingerBones", fp::vrFingerBonesFromText, fp::vrFingerBonesText,
         L"Experimental. On: finger bones are added to your body's own skeleton and the skinned body mesh bends its "
         L"fingers to the tracked joints. Shows on the full body; the hands-only body hides that mesh."},
        {setting::ambientOcclusion, L"Ambient occlusion", RowInfo::choice, &ModSettings::ambientOcclusion, nullptr,
         L"View", L"AmbientOcclusion", nullptr, nullptr,
         L"Adds restrained screen-space contact shading around nearby geometry. It uses the presented depth buffer, "
         L"is skipped on unsupported or multisampled frames, and does not alter "
         L"Exanima's dynamic lights or GI. "
         L"Subtle is recommended; Strong can darken narrow gaps.", {0,100,5,20}},
        {setting::depthOfField, L"Depth of field", RowInfo::choice, &ModSettings::depthOfField, nullptr,
         L"View", L"DepthOfField", nullptr, nullptr,
         L"Softens near and far textures while keeping the center of the first-person view in focus. "
         L"This is a restrained screen-space blur, not a physical lens simulation. It is skipped in third person, "
         L"menus and unsupported or multisampled frames. Subtle is recommended for play.", {0,100,5,20}},
        {setting::screenSpaceReflections, L"Screen-space reflections", RowInfo::choice,
         &ModSettings::screenSpaceReflections, nullptr, L"View", L"ScreenSpaceReflections", nullptr, nullptr,
         L"Adds faint screen-space reflections on camera-up-facing surfaces. This is a camera-relative surface proxy, "
         L"not a material reflectivity mask; it complements native material reflections and is limited by what is on screen. "
         L"It is skipped on unsupported or multisampled frames. "
         L"Off is recommended where screen-space artifacts are visible.", {0,100,5,20}},
        {setting::edgeSmoothing, L"Edge smoothing", RowInfo::toggle, nullptr, &ModSettings::edgeSmoothing,
         L"View", L"EdgeSmoothing", nullptr, nullptr,
         L"Smooths jagged edges in one cheap pass. Fine shimmer on distant patterned walls remains. With "
         L"Supersample on, which already smooths edges, it lightly sharpens the detail the averaging softens."},
        {setting::helmetVisor, L"Helmet view", RowInfo::toggle, nullptr, &ModSettings::helmetVisor,
         L"View", L"HelmetView", nullptr, nullptr,
         L"Shades the first-person view next to a shown helmet, in that helmet's own shape: an open face, a nasal "
         L"guard, a great helm's slit. With headwear hidden, a soft opening instead."},
        {setting::playerGhost, L"See yourself through walls", RowInfo::toggle, nullptr, &ModSettings::playerGhost,
         L"View", L"PlayerGhost", nullptr, nullptr,
         L"In third person, shows your character's own outline, faint with a brighter edge, where walls or "
         L"pillars hide you. Your own limbs and held gear do not show through each other."},
        {setting::heldDetail, L"Held item detail", RowInfo::choice, &ModSettings::heldDetail, nullptr,
         L"View", L"HeldItemDetail", nullptr, nullptr,
         L"Adds fine surface detail to what you hold, in first and third person: metal grain, scratches along the "
         L"length, small pits and worn edges. The pattern is fixed to the item and only reshapes the light already "
         L"on it, so an unlit blade stays dark. It adds no new shape. Not in VR.", {0,100,5,20}},
        {setting::armourDetail, L"Armour and clothing detail", RowInfo::choice, &ModSettings::armourDetail, nullptr,
         L"View", L"ArmourDetail", nullptr, nullptr,
         L"The same surface detail on worn armour and clothing, yours and others' within 6 m: plate, mail rings, "
         L"leather grain, quilted padding. Each piece keeps its pattern on the limb it hangs on. Covers more of the "
         L"screen than held items, so it costs more. Not in VR.", {0,100,5,20}},
        {setting::drawDistance, L"Draw distance", RowInfo::number, &ModSettings::drawDistance, nullptr,
         L"View", L"DrawDistance", nullptr, nullptr,
         L"Small props and far characters beyond this distance are not drawn; bigger things stay longer. Off draws "
         L"everything, as the game does.", {0,200,10,20}},
        {setting::objectLights, L"Lights per object", RowInfo::number, &ModSettings::objectLights, nullptr,
         L"View", L"LightsPerObject", nullptr, nullptr,
         L"How many lights each object takes. The game uses six; the strongest are kept.", {1,6,1,5}},
        {setting::levelDetail, L"Level detail", RowInfo::choice, &ModSettings::levelDetail, nullptr,
         L"View", L"LevelDetail", nullptr, nullptr,
         L"Fine surface detail on walls, floors and ceilings: within about 3 m stone grain and mortar, wood fibres, "
         L"rusted metal, dirt and gravel, cracked plaster; out to about 8 m broad weathering and damp. People are "
         L"left alone. It turns itself down as the frame rate drops below about "
         L"55 fps and off below about 40, before anything else. Not in VR.", {0,100,5,20}},
        {setting::objectDetail, L"World object detail", RowInfo::choice, &ModSettings::objectDetail, nullptr,
         L"View", L"ObjectDetail", nullptr, nullptr,
         L"The same surface detail on loose items and placed objects within 6 m: weapons on the floor or a rack, "
         L"chests, doors, furniture. Walls and floors are not included. Not in VR.", {0,100,5,20}},
        {setting::fingerPoses, L"Finger poses", RowInfo::toggle, nullptr, &ModSettings::fingerPoses,
         L"View", L"FingerPoses", nullptr, nullptr,
         L"Experimental. Adds finger bones to your character's own skeleton and poses them to what each hand holds: "
         L"round a weapon's hilt, round a carried item, flat under a shield, open otherwise. In play, in first and third "
         L"person; a headset's tracked hands take over when VR finger bones are on."},
        {setting::fingerPosesOthers, L"Finger poses: others", RowInfo::toggle, nullptr, &ModSettings::fingerPosesOthers,
         L"View", L"FingerPosesOthers", nullptr, nullptr,
         L"The same finger poses for every other character in the level, in play."},
        {setting::conditionEffects, L"Hurt and winded", RowInfo::toggle, nullptr, &ModSettings::conditionEffects,
         L"View", L"ConditionEffects", nullptr, nullptr,
         L"Below half health the view loses colour and darkens at the edges; when out of breath the edges darken "
         L"with each breath, and the eyes take a moment to adjust between light and darkness. Uses the game's own "
         L"health and stamina."},
        {setting::lightSmoothing, L"Smooth lighting", RowInfo::toggle, nullptr, &ModSettings::lightSmoothing,
         L"View", L"SmoothLighting", nullptr, nullptr,
         L"Reads the game's bounce-light grid smoothly, so strong lights such as the arena's red torches fall off in "
         L"round gradients instead of squares. Takes effect from the next level load."},
        {setting::woundMarks, L"Wounds", RowInfo::toggle, nullptr, &ModSettings::woundMarks,
         L"View", L"Wounds", nullptr, nullptr,
         L"Where a weapon gets through, the character keeps the mark and it moves with the limb: a gash along the "
         L"blade's path for a cut, a deep hole for a thrust, a bruise for a blunt blow, with blood running down "
         L"from fresh cuts. Blows the armour stops leave nothing. Needs the Lua script runtime."},
    };
    static const RowInfo none{-1,      L"",     RowInfo::toggle, nullptr, nullptr,
                              nullptr, nullptr, nullptr,         nullptr, nullptr};
    static const auto persisted = [&] {
        std::array<RowInfo, setting::count> rows;
        rows.fill(none); // placed by row id, so the table's order cannot misroute a row
        for (const RowInfo& info : table)
            if (info.row >= 0 && info.row < setting::count) rows[size_t(info.row)] = info;
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
    if (!validSettingBinding(setting::kickKey, kickKey)) kickKey = 0;
    if (!validSettingBinding(setting::jumpKey, jumpKey)) jumpKey = 0;
    if (fpDefense != 1) fpDefense = 0;
    if (tpDefense != 1) tpDefense = 0;
    if (fpSwingSelection != 0) fpSwingSelection = 1;
    if (tpSwingSelection != 0) tpSwingSelection = 1;
    const ModSettings defaults;
    for (int r = 0; r < setting::count; ++r) {
        const RowInfo& i = rowInfo(r);
        if (i.kind == RowInfo::number)
            step(r, 0);
        else if (i.kind == RowInfo::choice && i.range.count) {
            // An effect strength in percent; the old Subtle and Strong (1 and 2) are 50 and 100.
            int& v = this->*i.value;
            v = v == 1 ? 50 : v == 2 ? 100 : std::clamp(v, 0, 100);
        } else if (i.kind == RowInfo::choice && (this->*i.value < 0 || this->*i.value > 2))
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
