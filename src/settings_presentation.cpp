#include "settings_presentation.h"
namespace panel {
std::wstring settingValue(const ModSettings& settings,int id) {
    switch(id) {
    case setting::fov: return std::to_wstring(settings.fov) + L"\u00b0";
    case setting::tpFov: return settings.tpFov ? std::to_wstring(settings.tpFov)+L"\u00b0" : L"Inherit";
    case setting::sensitivity: return std::to_wstring(settings.sensitivity)+L"%";
    case setting::tpDistance: return std::to_wstring(settings.tpDistance)+L" cm";
    case setting::tpShoulder: return settings.tpShoulder == 0 ? L"Center" :
        (settings.tpShoulder < 0 ? L"L " : L"R ") +
        std::to_wstring(std::abs(settings.tpShoulder)) + L" cm";
    case setting::tpHeight: return std::to_wstring(settings.tpHeight)+L" cm";
    case setting::tpLag: return std::to_wstring(settings.tpLag)+L"%";
    case setting::flickCatchup: return std::to_wstring(settings.flickCatchup)+L"%";
    case setting::spineFollow: return std::to_wstring(settings.spineFollow)+L"%";
    case setting::cueSize: return std::to_wstring(settings.cueSize)+L"%";
    case setting::cueOpacity: return std::to_wstring(settings.cueOpacity)+L"%";
    case setting::grabRange: return std::to_wstring(settings.grabRange)+L" cm";
    case setting::tpBlockers:
        return settings.tpBlockers==fp::blockersSmart ? L"Smart" :
            settings.tpBlockers==fp::blockersHide ? L"Hide" : L"Pull in";
    case setting::fpDefense: return settings.fpDefense ?
        (settings.manualDefenseAvailable ? L"Manual RMB" : L"Automatic fallback") : L"Automatic";
    case setting::fpSwingSelection: return settings.fpSwingSelection ? L"Mouse" : L"Native";
    case setting::indicator: return settings.indicator==2 ? L"Always" :
        settings.indicator==1 ? L"Combat" : L"Off";
    case setting::rmbMode: return settings.rmbMode==1 ? L"Hold" :
        settings.rmbMode==2 ? L"Toggle" : L"Off";
    case setting::crouchMode: return settings.crouchMode==1 ? L"Hold" :
        settings.crouchMode==2 ? L"Toggle" : L"Native";
    default: break;
    }
    std::wstring value=settings.value(id);
    if (value==L"right mouse") return L"RMB";
    if (value==L"left mouse") return L"LMB";
    if (value==L"middle mouse") return L"MMB";
    if (value==L"unbound") return L"None";
    return value;
}

const wchar_t* settingLabel(int id) {
    switch (id) {
    case setting::fov: return L"First-person FOV";
    case setting::tpFov: return L"Third-person FOV";
    case setting::sensitivity: return L"Sensitivity";
    case setting::tpDistance: return L"Distance";
    case setting::tpShoulder: return L"Shoulder";
    case setting::tpHeight: return L"Height";
    case setting::tpLag: return L"Camera lag";
    case setting::flickCatchup: return L"Turn damping";
    case setting::spineFollow: return L"Spine follows";
    case setting::cueSize: return L"Arc size";
    case setting::cueOpacity: return L"Arc opacity";
    case setting::grabRange: return L"Grab range";
    case setting::fpDefense: return L"Defense";
    case setting::fpSwingSelection: return L"Attack side";
    case setting::indicator: return L"Aim indicator";
    case setting::crouchMode: return L"Crouch";
    case setting::rmbMode: return L"RMB cursor";
    case setting::rotateHeldKey: return L"Rotate item";
    case setting::trackingKey: return L"Recenter";
    case setting::hideHelmet: return L"Hide headwear";
    case setting::tpTight: return L"Tight spaces";
    case setting::tpCombat: return L"Combat framing";
    case setting::tpDown: return L"Follow downed body";
    case setting::tpCombatAim: return L"Third-person aim oval";
    case setting::holdHeading: return L"Heading correction";
    case setting::invertY: return L"Invert vertical look";
    case setting::rawMouse: return L"Raw mouse input";
    case setting::freezeWindows: return L"Release for menus";
    case setting::tracking: return L"Head / eye tracking";
    case setting::attackCue: return L"Attack indicator";
    case setting::cueThrust: return L"Thrust indicator";
    default: return ModSettings::label(id);
    }
}

} // namespace panel
