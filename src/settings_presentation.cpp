#include "settings_presentation.h"
namespace panel {
std::wstring settingValue(const ModSettings& settings,int id) {
    switch(id) {
    case setting::fov: return std::to_wstring(settings.fov) + L"\u00b0";
    case setting::tpFov: return settings.tpFov ? std::to_wstring(settings.tpFov)+L"\u00b0" : L"As first person";
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
    case setting::fpDefense: return settings.fpDefense ? (settings.manualDefenseAvailable ? L"Hold RMB" : L"Automatic") : L"Automatic";
    case setting::fpSwingSelection: return settings.fpSwingSelection ? L"Mouse" : L"Game";
    case setting::hideHelmet: return settings.hideHelmet ? L"Always hide" : L"Show unless blocking";
    case setting::tpDown: return settings.tpDown ? L"Follow body" : L"Follow head";
    case setting::tpCombatAim: return settings.tpCombatAimFree ? L"Oval" : L"Centre";
    case setting::fingerPoses: return !settings.fingerPoses ? L"Off" : settings.fingerPosesOthers ? L"Everyone" : L"You";
    case setting::indicator: return settings.indicator==2 ? L"Always" :
        settings.indicator==1 ? L"Combat" : L"Off";
    case setting::ambientOcclusion: case setting::depthOfField: case setting::screenSpaceReflections:
    case setting::heldDetail: return settings.value(id); // "Off" or a percent
    case setting::armourDetail: return settings.value(id);
    case setting::objectDetail: return settings.value(id);
    case setting::levelDetail: return settings.value(id);
    case setting::crouchMode: return settings.crouchMode==1 ? L"Hold" :
        settings.crouchMode==2 ? L"Toggle" : L"Native";
    case setting::vrIpd: return settings.vrIpd ? std::to_wstring(settings.vrIpd)+L" mm" : L"Headset";
    case setting::vrScale: return std::to_wstring(settings.vrScale)+L"/m";
    case setting::vrRenderMode: return settings.vrRenderMode == 2 ? L"Depth, filled" : settings.vrRenderMode == 1 ? L"Depth" : L"Each eye";
    case setting::vrHands: return settings.vrHands ? L"Drawn" : L"Off";
    case setting::vrPosture: return settings.vrPosture ? L"Seated" : L"Standing";
    case setting::vrHeadset: return settings.vrHeadset ? L"On" : L"Off";
    case setting::vrTurning: return settings.vrTurning == 1 ? L"Snap 30" : settings.vrTurning == 2 ? L"Snap 45" : settings.vrTurning == 3 ? L"Smooth" : L"Off";
    case setting::vrEffects: return settings.vrEffects ? L"On" : L"Off";
    case setting::vrGripAngle: return std::to_wstring(settings.vrGripAngle)+L" deg";
    case setting::vrHaptics: return std::to_wstring(settings.vrHaptics)+L"%";
    case setting::vrPanelWidth: return std::to_wstring(settings.vrPanelWidth)+L" cm";
    case setting::vrPanelDistance: return std::to_wstring(settings.vrPanelDistance)+L" cm";
    case setting::vrComfort: return settings.vrComfort == 2 ? L"Turn+walk" : settings.vrComfort == 1 ? L"Turning" : L"Off";
    case setting::vrBody: return settings.vrBody ? L"Hands only" : L"Full";
    case setting::vrHeight: return std::to_wstring(settings.vrHeight)+L" cm";
    case setting::vrGrabTurn: return settings.vrGrabTurn ? L"On" : L"Off";
    case setting::vrEyeScale: return std::to_wstring(settings.vrEyeScale)+L" %";
    case setting::vrGripSlide: return std::to_wstring(settings.vrGripSlide)+L" cm";
    case setting::vrKick: return settings.vrKick ? L"On" : L"Off";
    case setting::vrJump: return settings.vrJump ? L"On" : L"Off";
    case setting::vrWeaponWeight: return settings.vrWeaponWeight ? L"On" : L"Off";
    case setting::vrTeleport: return settings.vrTeleport ? L"On" : L"Off";
    case setting::vrHandPanel: return settings.vrHandPanel ? L"In hand" : L"In view";
    case setting::vrPouch: return settings.vrPouch ? L"On" : L"Off";
    case setting::bladeGain: return std::to_wstring(settings.bladeGainPercent)+L"%";
    case setting::vrFingerBones: return settings.vrFingerBones ? L"On" : L"Off";
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
    case setting::fov: return L"FOV";
    case setting::tpFov: return L"FOV";
    case setting::sensitivity: return L"Sensitivity";
    case setting::tpDistance: return L"Distance";
    case setting::tpShoulder: return L"Shoulder";
    case setting::tpHeight: return L"Height";
    case setting::tpLag: return L"Lag";
    case setting::flickCatchup: return L"Turn damping";
    case setting::spineFollow: return L"Spine follows";
    case setting::cueSize: return L"Size";
    case setting::cueOpacity: return L"Opacity";
    case setting::grabRange: return L"Grab range";
    case setting::fpDefense: return L"Block";
    case setting::fpSwingSelection: return L"Swing side";
    case setting::indicator: return L"Crosshair";
    case setting::ambientOcclusion: return L"Contact shadows";
    case setting::depthOfField: return L"Focus blur";
    case setting::screenSpaceReflections: return L"Reflections";
    case setting::edgeSmoothing: return L"Anti-aliasing";
    case setting::helmetVisor: return L"Helmet edges";
    case setting::playerGhost: return L"See-through";
    case setting::fingerPoses: return L"Finger grip";
    case setting::fingerPosesOthers: return L"Finger poses: others";
    case setting::bladeDrag: return L"Slicing";
    case setting::stabSink: return L"Deep stabs";
    case setting::bladeGain: return L"Slice power";
    case setting::conditionEffects: return L"Hurt vision";
    case setting::heldDetail: return L"Held items";
    case setting::armourDetail: return L"Armour";
    case setting::objectDetail: return L"Objects";
    case setting::levelDetail: return L"Level";
    case setting::woundMarks: return L"Wounds";
    case setting::lightSmoothing: return L"Smooth light";
    case setting::crouchMode: return L"Crouch";
    case setting::rotateHeldKey: return L"Turn item";
    case setting::kickKey: return L"Kick";
    case setting::jumpKey: return L"Jump";
    case setting::trackingKey: return L"Recenter";
    case setting::hideHelmet: return L"Headwear";
    case setting::tpTight: return L"Tight spaces";
    case setting::tpCombat: return L"Combat zoom";
    case setting::tpDown: return L"Knockdown";
    case setting::tpCombatAim: return L"Aim style";
    case setting::holdHeading: return L"Heading correction";
    case setting::invertY: return L"Invert Y";
    case setting::rawMouse: return L"Raw input";
    case setting::tracking: return L"Head tracking";
    case setting::vrIpd: return L"VR eye spacing";
    case setting::vrScale: return L"VR world scale";
    case setting::vrRenderMode: return L"VR rendering";
    case setting::vrHands: return L"VR hands";
    case setting::vrPosture: return L"VR posture";
    case setting::vrHeadset: return L"VR headset";
    case setting::vrTurning: return L"VR turning";
    case setting::vrEffects: return L"VR effects";
    case setting::vrGripAngle: return L"VR grip angle";
    case setting::vrHaptics: return L"VR haptics";
    case setting::vrPanelWidth: return L"VR panel width";
    case setting::vrPanelDistance: return L"VR panel depth";
    case setting::vrComfort: return L"VR vignette";
    case setting::vrBody: return L"VR body";
    case setting::vrHeight: return L"VR height";
    case setting::vrGrabTurn: return L"VR grab turn";
    case setting::vrEyeScale: return L"VR eye scale";
    case setting::vrGripSlide: return L"VR grip slide";
    case setting::vrKick: return L"VR kick";
    case setting::vrJump: return L"VR jump";
    case setting::vrWeaponWeight: return L"VR weapon weight";
    case setting::vrTeleport: return L"VR teleport";
    case setting::vrHandPanel: return L"VR menus in hand";
    case setting::vrPouch: return L"VR hip pouch";
    case setting::vrFingerBones: return L"VR finger bones";
    case setting::attackCue: return L"Arc";
    case setting::cueThrust: return L"Thrusts";
    case setting::headSteady: return L"Head steadying";
    case setting::swingCrosshair: return L"Swing crosshair";
    case setting::idleCrosshair: return L"Idle crosshair";
    case setting::headRoll: return L"Steady roll";
    case setting::headPitch: return L"Steady pitch";
    case setting::headYaw: return L"Steady yaw";
    case setting::headBob: return L"Bob steadiness";
    case setting::nearClip: return L"Near clip";
    case setting::autoRecenter: return L"Auto-center";
    case setting::fpDeadzoneX: case setting::tpDeadzoneX: return L"Aim oval width";
    case setting::fpDeadzoneY: case setting::tpDeadzoneY: return L"Aim oval height";
    case setting::tpDefense: return L"Defense";
    case setting::tpSwingSelection: return L"Attack side";
    case setting::edgeSoftness: return L"Oval softness";
    case setting::leanKey: return L"Lean";
    case setting::drawDistance: return L"Draw distance";
    case setting::objectLights: return L"Object lights";
    default: return ModSettings::label(id);
    }
}

// The short help shown for a row in Mod Settings; the long ones stay in the settings schema.
const wchar_t* settingTip(int id) {
    switch (id) {
    case setting::fov: return L"How wide you see in first person. Third person uses this too unless it has its own. F9 / F10 narrow or widen, F11 resets.";
    case setting::hideHelmet: return L"Hides your helmet and hair from your own view when they get in the way, or always. Shadows and mirrors still show them.";
    case setting::helmetVisor: return L"Darkens the edges of the view in the shape of the helmet you wear: a great helm's slit, a nasal guard, an open face.";
    case setting::fingerPoses: return L"Fingers wrap round hilts and carried items and lie flat under shields, instead of the game's stiff hands. Experimental.";
    case setting::tpFov: return L"Third person's own field of view, or the same as first person.";
    case setting::tpDistance: return L"How far behind you the camera sits. Walls and things in the way still pull it closer.";
    case setting::tpShoulder: return L"Which shoulder the camera looks over, and how far off-centre.";
    case setting::tpHeight: return L"Camera height compared to your eyes. Low ceilings can limit it.";
    case setting::tpCombat: return L"While fighting, the camera moves 20 % closer and nearly centres behind you.";
    case setting::tpLag: return L"How loosely the camera trails you when you move. 0 sticks it to you.";
    case setting::tpTight: return L"In narrow places the camera swaps to the open shoulder and moves in. Wall collision works either way.";
    case setting::tpDown: return L"When you're knocked down, follow the body so the camera doesn't dive with your head.";
    case setting::playerGhost: return L"Shows a faint outline of you wherever walls or pillars hide you.";
    case setting::ambientOcclusion: return L"Soft shadow in corners, under objects and where things touch. High values can darken narrow gaps.";
    case setting::screenSpaceReflections: return L"Faint reflections on floors and other upward-facing surfaces, from what's on screen. Turn off if you see smearing.";
    case setting::depthOfField: return L"Softens very near and very far things while the middle stays sharp. First person only.";
    case setting::edgeSmoothing: return L"Smooths jagged edges in one cheap pass. With Supersample on, it lightly sharpens instead.";
    case setting::conditionEffects: return L"Colour drains and edges darken below half health; edges pulse with each breath when winded; eyes adjust between light and dark.";
    case setting::woundMarks: return L"Cuts, stab holes and bruises stay on bodies where weapons got through, with blood running from fresh cuts.";
    case setting::lightSmoothing: return L"Strong lights fall off in round gradients instead of squares. From the next level load.";
    case setting::heldDetail: return L"Scratches, metal grain, pits and worn edges on what you hold. It reshapes existing light only, so an unlit blade stays dark.";
    case setting::armourDetail: return L"Plate, mail rings, leather grain and padding detail on worn gear within 6 m. Costs more than held items.";
    case setting::objectDetail: return L"The same detail on loose items, racks, chests, doors and furniture within 6 m. Not walls or floors.";
    case setting::fpDefense: return L"Automatic lets the game block for you. Hold RMB raises your guard yourself; reach, gear and recovery still decide if it works. Both views.";
    case setting::fpSwingSelection: return L"Mouse: flick left or right before clicking to choose which side the swing starts from. Game: Exanima picks from your facing.";
    case setting::tpCombatAim: return L"Third person. Oval: the cursor moves freely and the view turns at its edge. Centre: the view turns directly.";
    case setting::indicator: return L"When the crosshair shows.";
    case setting::attackCue: return L"An arc round the crosshair fills as your swing winds up, so you can time the release.";
    case setting::cueThrust: return L"Shows a matching cue under the crosshair for thrusts.";
    case setting::cueSize: return L"Arc size.";
    case setting::cueOpacity: return L"Arc opacity.";
    case setting::bladeDrag: return L"Dragging an edge across a body cuts it, not only the swing's hit. The game's own edge, speed and armour rules apply. Works for everyone.";
    case setting::stabSink: return L"Experimental. A point-first thrust into unarmoured flesh pushes in a hand's depth instead of stopping at the skin. Armour and skulls still stop it.";
    case setting::bladeGain: return L"How fast a slice counts as moving. 100 % is real speed. Raise if slow slices do nothing, lower if they cut too deep.";
    case setting::rotateHeldKey: return L"Hold and move the mouse to turn what you're holding. The view stays still meanwhile.";
    case setting::leanKey: return L"Hold and move the mouse to lean your upper body any direction. Let go to straighten up.";
    case setting::kickKey: return L"Plays the game's unused kick. Fighting stance, upright, needs most of your stamina.";
    case setting::jumpKey: return L"Plays the game's unused jump. Upright, needs half your stamina.";
    case setting::sensitivity: return L"How fast the view turns with the mouse. Aiming inside the oval, leaning and turning items aren't affected.";
    case setting::invertY: return L"Mouse up looks down.";
    case setting::rawMouse: return L"Reads the mouse directly, without Windows' pointer acceleration.";
    case setting::crouchMode: return L"How the game's duck key (X by default) crouches. Combat, a knockdown or talking stands you up.";
    case setting::drawDistance: return L"Small props and far people past this distance aren't drawn; big things stay longer. Off draws everything.";
    case setting::objectLights: return L"Lights each object takes. The game uses six; fewer runs faster and keeps the strongest.";
    default: return rowInfo(id).tip;
    }
}

} // namespace panel
