#include "settings_presentation.h"
namespace panel {
namespace {
// Key names in the panel are short; the settings schema keeps the canonical names.
std::wstring keyName(int key) {
    const std::wstring name = fp::heldKeyName(key);
    if (name == L"right mouse") return L"RMB";
    if (name == L"left mouse") return L"LMB";
    if (name == L"middle mouse") return L"MMB";
    if (name == L"unbound") return L"None";
    return name;
}
} // namespace

std::wstring settingValue(const ModSettings& settings, int id) {
    switch (id) {
    case setting::fov:
        return std::to_wstring(settings.fov) + L"\u00b0";
    case setting::sensitivity:
        return std::to_wstring(settings.sensitivity)+L"%";
    case setting::headSteady:
        return settings.headSteady ? std::to_wstring(settings.headSteady) + L"%" : std::wstring(L"off (the game's)");
    case setting::rawMouse:
        return settings.rawMouse ? (settings.rawMouseFallback ? L"On; cursor fallback" : L"on") : L"off";
    case setting::hideHelmet:
        return settings.hideHelmet ? L"Always hide" : L"Show unless blocking";
    case setting::grabRange:
        return std::to_wstring(settings.grabRange)+L" cm";
    case setting::swingCrosshair:
        return settings.hideSwingCrosshair ? L"hidden" : L"shown";
    case setting::tpDistance:
        return std::to_wstring(settings.tpDistance)+L" cm";
    case setting::tpShoulder:
        return settings.tpShoulder == 0 ? L"Center" :
        (settings.tpShoulder < 0 ? L"L " : L"R ") +
        std::to_wstring(std::abs(settings.tpShoulder)) + L" cm";
    case setting::tpHeight:
        return std::to_wstring(settings.tpHeight)+L" cm";
    case setting::tpTight:
        return settings.tpTight ? L"swap shoulder" : L"off";
    case setting::tpCombat:
        return settings.tpCombat ? L"closer and centred" : L"off";
    case setting::tpFov:
        return settings.tpFov ? std::to_wstring(settings.tpFov)+L"\u00b0" : L"Same";
    case setting::tpLag:
        return std::to_wstring(settings.tpLag)+L"%";
    case setting::tpBlockers:
        return settings.tpBlockers==fp::blockersSmart ? L"Smart" :
        settings.tpBlockers==fp::blockersHide ? L"Hide" : L"Pull in";
    case setting::tpDown:
        return settings.tpDown ? L"Follow body" : L"Follow head";
    case setting::attackCue:
        return settings.attackCue ? L"shown while a swing winds up" : L"off";
    case setting::cueSize:
        return std::to_wstring(settings.cueSize)+L"%";
    case setting::cueOpacity:
        return std::to_wstring(settings.cueOpacity)+L"%";
    case setting::threatMarkers:
        return settings.threatMarkers == 0 ? L"Off" : settings.threatMarkers == 2 ? L"Always" : L"Combat only";
    case setting::threatOpacity:
        return std::to_wstring(settings.threatOpacity)+L"%";
    case setting::cueThrust:
        return settings.cueThrust ? L"below the crosshair" : L"off";
    case setting::bladeDrag:
        return L"";
    case setting::stabSink:
        return L"";
    case setting::bladeGain:
        return std::to_wstring(settings.bladeGainPercent)+L"%";
    case setting::idleCrosshair:
        return settings.idleCrosshairAlways ? L"Show" : L"Hide";
    case setting::headRoll:
        return settings.headRoll ? std::to_wstring(settings.headRoll) + L"%" : std::wstring(L"off");
    case setting::headPitch:
        return settings.headPitch ? std::to_wstring(settings.headPitch) + L"%" : std::wstring(L"off");
    case setting::headYaw:
        return settings.headYaw ? std::to_wstring(settings.headYaw) + L"%" : std::wstring(L"off");
    case setting::aimMarker:
        return settings.aimMarker ? L"while the head catches up" : L"off";
    case setting::flickCatchup:
        return std::to_wstring(settings.flickCatchup)+L"%";
    case setting::headBob:
        return settings.headBob ? std::to_wstring(settings.headBob) + L"%" : std::wstring(L"off");
    case setting::spineFollow:
        return std::to_wstring(settings.spineFollow)+L"%";
    case setting::crouchMode:
        return settings.crouchMode==1 ? L"Hold" :
        settings.crouchMode==2 ? L"Toggle" : L"Native";
    case setting::nearClip:
        {
        wchar_t b[24];
        swprintf_s(b, L"%d.%d cm", settings.nearClip / 10, settings.nearClip % 10);
        return b;
        }
    case setting::tpCombatAim:
        return settings.tpCombatAimFree ? L"Oval" : L"Centre";
    case setting::eyeSetback:
        return settings.eyeSetback ? std::to_wstring(settings.eyeSetback) + L" cm behind the eyes"
        : std::wstring(L"at the eyes");
    case setting::rotateHeldKey:
        return keyName(settings.rotateHeldKey);
    case setting::kickKey:
        return keyName(settings.kickKey);
    case setting::jumpKey:
        return keyName(settings.jumpKey);
    case setting::swapShoulderKey:
        return keyName(settings.swapShoulderKey);
    case setting::leanKey:
        return keyName(settings.leanKey);
    case setting::holdHeading:
        return settings.holdHeading ? L"faces where you aim" : L"the head's (may slide sideways)";
    case setting::autoRecenter:
        return settings.autoRecenter ? L"slowly follow cursor" : L"off (manual only)";
    case setting::fpDeadzoneX:
        return std::to_wstring(settings.fpDeadzoneX * 2) + L"% of screen width";
    case setting::fpDeadzoneY:
        return std::to_wstring(settings.fpDeadzoneY * 2) + L"% of screen height";
    case setting::tpDeadzoneX:
        return std::to_wstring(settings.tpDeadzoneX * 2) + L"% of screen width";
    case setting::tpDeadzoneY:
        return std::to_wstring(settings.tpDeadzoneY * 2) + L"% of screen height";
    case setting::leanKeysMode:
        return settings.leanKeysMode ? L"left / right keys" : L"modifier + mouse";
    case setting::leanLeftKey:
        return keyName(settings.leanLeftKey);
    case setting::leanRightKey:
        return keyName(settings.leanRightKey);
    case setting::fpDefense:
        return settings.fpDefense ? (settings.manualDefenseAvailable ? L"Hold RMB" : L"Automatic") : L"Automatic";
    case setting::tpDefense:
        {
        const int mode = id == setting::fpDefense ? settings.fpDefense : settings.tpDefense;
        return mode != 1                ? L"Automatic (native)"
        : settings.manualDefenseAvailable ? L"Manual (hold RMB)"
        : L"Automatic fallback";
        }
    case setting::fpSwingSelection:
        return settings.fpSwingSelection ? L"Mouse" : L"Game";
    case setting::tpSwingSelection:
        return settings.tpSwingSelection ? L"Mouse direction" : L"Native facing";
    case setting::edgeSoftness:
        return settings.edgeSoftness == 0 ? L"Off (hard edge)"
        : std::to_wstring(settings.edgeSoftness) + L"% of oval radius";
    case setting::tracking:
        return !settings.tracking ? L"Off" : settings.trackingStatus == 3 ? L"Unavailable (UDP port in use?)"
        : settings.trackingStatus == 2 ? L"OpenTrack UDP - receiving"
        : L"OpenTrack UDP - waiting";
    case setting::trackingKey:
        return keyName(settings.trackingKey);
    case setting::invertY:
        return settings.invertY ? L"On" : L"Off";
    case setting::indicator:
        return settings.indicator==2 ? L"Always" :
        settings.indicator==1 ? L"Combat" : L"Off";
    case setting::vrIpd:
        return settings.vrIpd ? std::to_wstring(settings.vrIpd)+L" mm" : L"Headset";
    case setting::vrScale:
        return std::to_wstring(settings.vrScale)+L"/m";
    case setting::vrRenderMode:
        return settings.vrRenderMode == 2 ? L"Depth, filled" : settings.vrRenderMode == 1 ? L"Depth" : L"Each eye";
    case setting::vrHands:
        return settings.vrHands ? L"Drawn" : L"Off";
    case setting::vrPosture:
        return settings.vrPosture ? L"Seated" : L"Standing";
    case setting::vrHeadset:
        return settings.vrHeadset ? L"On" : L"Off";
    case setting::vrTurning:
        return settings.vrTurning == 1 ? L"Snap 30" : settings.vrTurning == 2 ? L"Snap 45" : settings.vrTurning == 3 ? L"Smooth" : L"Off";
    case setting::vrEffects:
        return settings.vrEffects ? L"On" : L"Off";
    case setting::vrGripAngle:
        return std::to_wstring(settings.vrGripAngle)+L" deg";
    case setting::vrHaptics:
        return std::to_wstring(settings.vrHaptics)+L"%";
    case setting::vrPanelWidth:
        return std::to_wstring(settings.vrPanelWidth)+L" cm";
    case setting::vrPanelDistance:
        return std::to_wstring(settings.vrPanelDistance)+L" cm";
    case setting::vrComfort:
        return settings.vrComfort == 2 ? L"Turn+walk" : settings.vrComfort == 1 ? L"Turning" : L"Off";
    case setting::vrBody:
        return settings.vrBody ? L"Hands only" : L"Full";
    case setting::vrHeight:
        return std::to_wstring(settings.vrHeight)+L" cm";
    case setting::vrGrabTurn:
        return settings.vrGrabTurn ? L"On" : L"Off";
    case setting::vrEyeScale:
        return std::to_wstring(settings.vrEyeScale)+L" %";
    case setting::vrGripSlide:
        return std::to_wstring(settings.vrGripSlide)+L" cm";
    case setting::vrKick:
        return settings.vrKick ? L"On" : L"Off";
    case setting::vrJump:
        return settings.vrJump ? L"On" : L"Off";
    case setting::vrWeaponWeight:
        return settings.vrWeaponWeight ? L"On" : L"Off";
    case setting::vrTeleport:
        return settings.vrTeleport ? L"On" : L"Off";
    case setting::vrHandPanel:
        return settings.vrHandPanel ? L"In hand" : L"In view";
    case setting::vrPouch:
        return settings.vrPouch ? L"On" : L"Off";
    case setting::vrFingerBones:
        return settings.vrFingerBones ? L"On" : L"Off";
    case setting::ambientOcclusion:
    case setting::depthOfField:
        return settings.value(id); // "Off" or a percent
    case setting::edgeSmoothing:
        return L"";
    case setting::helmetVisor:
        return L"";
    case setting::playerGhost:
        return L"";
    case setting::fingerPoses:
        return !settings.fingerPoses ? L"Off" : settings.fingerPosesOthers ? L"Everyone" : L"You";
    case setting::fingerPosesOthers:
        return L"";
    case setting::conditionEffects:
        return L"";
    case setting::heldDetail:
        return settings.heldDetail ? std::to_wstring(settings.heldDetail) + L"%" : L"Off";
    case setting::armourDetail:
        return settings.armourDetail ? std::to_wstring(settings.armourDetail) + L"%" : L"Off";
    case setting::objectDetail:
        return settings.objectDetail ? std::to_wstring(settings.objectDetail) + L"%" : L"Off";
    case setting::woundMarks:
    case setting::woundDebug:
        return L"";
    case setting::drawDistance:
        return settings.drawDistance ? std::to_wstring(settings.drawDistance) + L" m" : L"Off";
    case setting::objectLights:
        return settings.objectLights >= 6 ? std::wstring(L"6 (game)") : std::to_wstring(settings.objectLights);
    case setting::lightSmoothing:
        return L"";
    case setting::farRooms:
        return L"";
    case setting::hitReaction:
        return settings.hitReaction ? std::to_wstring(settings.hitReaction) + L"%" : L"Off";
    case setting::tpLookFollow:
        return L"";
    case setting::fpKnockdown:
        return settings.fpKnockdown ? L"Show third" : L"Stay first";
    case setting::kickStyle:
        return settings.kickStyle ? L"Animated" : L"Classic";
    case setting::padKick: return fp::padButtonName(settings.padKick);
    case setting::padJump: return fp::padButtonName(settings.padJump);
    case setting::padGuard: return fp::padButtonName(settings.padGuard);
    case setting::padShoulder: return fp::padButtonName(settings.padShoulder);
    case setting::parryDirection:
        return settings.parryDirection == fp::parryAssisted ? L"Drag: assisted"
               : settings.parryDirection == fp::parryStrict ? L"Drag: strict"
                                                            : L"Automatic";
    case setting::f6Cycle:
        return settings.f6Cycle ? L"1st/3rd only" : L"1st/3rd/game";
    default: return L"";
    }
}

const wchar_t* settingLabel(int id) {
    switch (id) {
    case setting::fov: return L"FOV";
    case setting::sensitivity: return L"Sensitivity";
    case setting::headSteady: return L"Head steadying";
    case setting::rawMouse: return L"Raw input";
    case setting::hideHelmet: return L"Headwear";
    case setting::grabRange: return L"Grab range";
    case setting::swingCrosshair: return L"Swing crosshair";
    case setting::tpDistance: return L"Distance";
    case setting::tpShoulder: return L"Shoulder";
    case setting::tpHeight: return L"Height";
    case setting::tpTight: return L"Tight spaces";
    case setting::tpCombat: return L"Combat zoom";
    case setting::tpFov: return L"FOV";
    case setting::tpLag: return L"Lag";
    case setting::tpBlockers: return L"Third person: in the way";
    case setting::tpDown: return L"Knockdown";
    case setting::attackCue: return L"Arc";
    case setting::cueSize: return L"Size";
    case setting::cueOpacity: return L"Opacity";
    case setting::cueThrust: return L"Thrusts";
    case setting::bladeDrag: return L"Slicing";
    case setting::stabSink: return L"Deep stabs";
    case setting::bladeGain: return L"Slice power";
    case setting::idleCrosshair: return L"Idle crosshair";
    case setting::headRoll: return L"Steady roll";
    case setting::headPitch: return L"Steady pitch";
    case setting::headYaw: return L"Steady yaw";
    case setting::aimMarker: return L"Aim marker";
    case setting::flickCatchup: return L"Turn damping";
    case setting::headBob: return L"Bob steadiness";
    case setting::spineFollow: return L"Spine follows";
    case setting::crouchMode: return L"Crouch";
    case setting::nearClip: return L"Near clip";
    case setting::tpCombatAim: return L"Aim style";
    case setting::eyeSetback: return L"Eye setback";
    case setting::rotateHeldKey: return L"Turn item";
    case setting::kickKey: return L"Kick";
    case setting::jumpKey: return L"Jump";
    case setting::swapShoulderKey: return L"Swap shoulder";
    case setting::tpLookFollow: return L"Follow look angle";
    case setting::fpKnockdown: return L"Knocked down";
    case setting::kickStyle: return L"Kick style";
    case setting::parryDirection: return L"Parry direction";
    case setting::threatMarkers: return L"Threat markers";
    case setting::threatOpacity: return L"Marker opacity";
    case setting::padKick: return L"Kick button";
    case setting::padJump: return L"Jump button";
    case setting::padGuard: return L"Guard button";
    case setting::padShoulder: return L"Shoulder button";
    case setting::leanKey: return L"Lean";
    case setting::holdHeading: return L"Heading correction";
    case setting::autoRecenter: return L"Auto-center";
    case setting::fpDeadzoneX: return L"Aim oval width";
    case setting::fpDeadzoneY: return L"Aim oval height";
    case setting::tpDeadzoneX: return L"Aim oval width";
    case setting::tpDeadzoneY: return L"Aim oval height";
    case setting::leanKeysMode: return L"Lean controls";
    case setting::leanLeftKey: return L"Lean left key";
    case setting::leanRightKey: return L"Lean right key";
    case setting::fpDefense: return L"Block";
    case setting::tpDefense: return L"Defense";
    case setting::fpSwingSelection: return L"Swing side";
    case setting::tpSwingSelection: return L"Attack side";
    case setting::edgeSoftness: return L"Oval softness";
    case setting::tracking: return L"Head tracking";
    case setting::trackingKey: return L"Recenter";
    case setting::invertY: return L"Invert Y";
    case setting::indicator: return L"Crosshair";
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
    case setting::ambientOcclusion: return L"Contact shadows";
    case setting::depthOfField: return L"Focus blur";
    case setting::edgeSmoothing: return L"Anti-aliasing";
    case setting::helmetVisor: return L"Helmet edges";
    case setting::playerGhost: return L"See-through";
    case setting::fingerPoses: return L"Finger grip";
    case setting::fingerPosesOthers: return L"Finger poses: others";
    case setting::conditionEffects: return L"Hurt vision";
    case setting::heldDetail: return L"Held items";
    case setting::armourDetail: return L"Armour";
    case setting::objectDetail: return L"Objects";
    case setting::woundMarks: return L"Wounds";
    case setting::woundDebug: return L"Wound debug";
    case setting::drawDistance: return L"Draw distance";
    case setting::objectLights: return L"Object lights";
    case setting::lightSmoothing: return L"Smooth light";
    case setting::farRooms: return L"Far rooms";
    case setting::f6Cycle: return L"F6 cycle";
    case setting::hitReaction: return L"Hit reaction";
    default: return L"";
    }
}

const wchar_t* settingTip(int id) {
    switch (id) {
    case setting::fov: return L"How wide you see in first person. Third person uses this too unless it has its own. F9 / F10 narrow or widen, F11 resets.";
    case setting::sensitivity: return L"How fast the view turns with the mouse. Aiming inside the oval and turning items aren't affected.";
    case setting::headSteady: return L"How steadily the neck holds the head. In first person it also steadies the chest and neck outside combat. Zero disables the extra head steadying; the separate axis settings choose their own strengths. The camera follows the physical head.";
    case setting::rawMouse: return L"Reads the mouse directly, without Windows' pointer acceleration.";
    case setting::hideHelmet: return L"Hides your helmet and hair from your own view when they get in the way, or always. Shadows and mirrors still show them.";
    case setting::grabRange: return L"Maximum interaction distance measured from the eyes, including in third person. Walls, item rules and carrying limits still apply. This does not change weapon reach.";
    case setting::swingCrosshair: return L"Hides the combat crosshair while a weapon swing is in motion.";
    case setting::tpDistance: return L"How far behind you the camera sits. Walls and things in the way still pull it closer.";
    case setting::tpShoulder: return L"Which shoulder the camera looks over, and how far off-centre.";
    case setting::tpHeight: return L"Camera height compared to your eyes. Low ceilings can limit it.";
    case setting::tpTight: return L"In narrow places the camera swaps to the open shoulder and moves in. Wall collision works either way.";
    case setting::tpCombat: return L"While fighting, the camera moves 20 % closer and nearly centres behind you.";
    case setting::tpFov: return L"Third person's own field of view, or the same as first person.";
    case setting::tpLag: return L"How loosely the camera trails you when you move. 0 sticks it to you.";
    case setting::tpBlockers: return L"Characters always remain visible and pull the camera closer. Hide affects verified placed props only. Automatic pulls in for characters and placed props but ignores loose items. Walls always pull the camera in.";
    case setting::tpDown: return L"When you're knocked down, follow the body so the camera doesn't dive with your head.";
    case setting::attackCue: return L"An arc round the crosshair fills as your swing winds up, so you can time the release.";
    case setting::cueSize: return L"Arc size.";
    case setting::cueOpacity: return L"Arc opacity.";
    case setting::threatMarkers: return L"Soft arcs round the screen centre point toward attackers off screen or near its edge; brighter while their swing winds up. Combat only: while you are in combat stance.";
    case setting::threatOpacity: return L"Threat marker opacity.";
    case setting::cueThrust: return L"Shows a matching cue under the crosshair for thrusts.";
    case setting::bladeDrag: return L"Dragging an edge across a body cuts it, not only the swing's hit. The game's own edge, speed and armour rules apply. Works for everyone.";
    case setting::stabSink: return L"Experimental. A point-first thrust into unarmoured flesh pushes in a hand's depth instead of stopping at the skin. Armour and skulls still stop it.";
    case setting::bladeGain: return L"How fast a slice counts as moving. 100 % is real speed. Raise if slow slices do nothing, lower if they cut too deep.";
    case setting::idleCrosshair: return L"Controls only the exploration crosshair. Aim marker can still appear while the head catches up. Cursor Look uses the game's free pointer.";
    case setting::headRoll: return L"Reduces sideways head tilt. Requires Head stabilisation. First person also steadies the physical head while balanced; hits, falls and held doors keep their native movement.";
    case setting::headPitch: return L"Reduces up-and-down head rotation. Requires Head stabilisation. First person also steadies the physical head while balanced; intentional look input and impacts remain.";
    case setting::headYaw: return L"Reduces head rotation away from the requested heading. Requires Head stabilisation. First-person heading correction also uses this strength.";
    case setting::aimMarker: return L"First person only. Shows the aim position when it is noticeably away from the physical head's view, even if the exploration crosshair is otherwise hidden.";
    case setting::flickCatchup: return L"First person only. 0% is off. Higher values reduce additional turn input when your aim runs far ahead of the physical head.";
    case setting::headBob: return L"First person only. Reduces vertical head movement while balanced. Requires Head stabilisation. Hits, falls and held doors retain native motion.";
    case setting::spineFollow: return L"Outside combat in first and third person, your spine bends forward when you look down and back when you look up. Combat uses native attack aiming; explicit lean controls still work.";
    case setting::crouchMode: return L"How the game's duck key (X by default) crouches. Combat, a knockdown or talking stands you up.";
    case setting::nearClip: return L"The minimum distance at which close surfaces are drawn. The camera adjusts this distance automatically; this setting changes its lower limit near walls. Lower values retain closer surfaces but reduce depth precision. First and third person.";
    case setting::tpCombatAim: return L"Third person. Oval: the cursor moves freely and the view turns at its edge. Centre: the view turns directly.";
    case setting::eyeSetback: return L"Moves the first-person view back toward the middle of the head. Third-person framing and interaction distance use the same shifted eye position. Headwear that blocks the first-person view is hidden; its shadow remains.";
    case setting::rotateHeldKey: return L"Hold and move the mouse to turn what you're holding. The view stays still meanwhile. Delete clears the key.";
    case setting::kickKey: return L"Plays the game's own kick when you are not mid-swing or reacting to a hit. Not a key the game uses (Alt is its thrust). Delete clears the key.";
    case setting::jumpKey: return L"Plays the game's own jump when you are not mid-swing or reacting to a hit. Not a key the game uses. Delete clears the key.";
    case setting::kickStyle: return L"Classic: the game's own kick. Animated: the same kick, gates and hit, with your body moved like a motion-captured kick that suits what you hold. Experimental.";
    case setting::padKick: return L"In combat, an Xbox-style controller button for the game's own kick. A button your Steam layout also uses does both.";
    case setting::padJump: return L"Xbox-style controller button for the game's own jump. The right-stick click does nothing else in the game's Steam layout.";
    case setting::padGuard: return L"Needs Block on Hold RMB. Held, guards like right mouse; with a Parry direction, push the right stick to pick the parry. Pick a button you do not need in combat.";
    case setting::padShoulder: return L"Third person: an Xbox-style controller button that swaps the camera to your other shoulder.";
    case setting::parryDirection: return L"Needs Block on Hold RMB. Holding RMB keeps the view still and a drag picks the parry: up, left, right, down-left, down-right. Automatic: the game picks. Assisted: your drag picks; a small drag leaves it to the game. Strict: you parry only if your drag matches the incoming swing.";
    case setting::fpKnockdown: return L"Show third: knocked down in first person, the view goes to third person and back a moment after you are up. F6 meanwhile keeps your choice; death never switches.";
    case setting::tpLookFollow: return L"Out of combat, looking down lifts the camera and moves it back so the ground ahead shows; looking up brings it closer and lower. Walls still pull it in.";
    case setting::swapShoulderKey: return L"Third person: slides the camera over your other shoulder and keeps it there. Does nothing with the camera centred. Delete clears the key.";
    case setting::leanKey: return L"Hold it and move the mouse to lean your upper body: left and right, forward and back. The view stays where it points and goes with your head. Let go to straighten up. In combat, the game's alternate-attack binding takes priority over lean. Any key can be set in first-person.ini ([Input] LeanKey).";
    case setting::holdHeading: return L"Looking up or down, the game's head turns a few degrees off where you aim, so the view slid sideways. This corrects the physics head heading, and the view follows it. A bigger difference, the head still catching up with a fast turn, is left as it is.";
    case setting::autoRecenter: return L"Optional: steadily turn the view toward the cursor inside the aim oval without moving the world-space aim target. Middle-click immediately requests the same recenter, even when off. Only active while gameplay owns look input; suspended for menus, leaning and held-object rotation. First person follows the physical head, so the head may take time to achieve the new aim.";
    case setting::fpDeadzoneX: return L"Width of the first-person aim oval, as a percentage of the screen. Movement inside aims; movement past the edge turns the view. Zero means direct turning.";
    case setting::fpDeadzoneY: return L"Height of the first-person aim oval, as a percentage of the screen. Zero means direct turning. Independent of third person.";
    case setting::tpDeadzoneX: return L"Width of the third-person aim oval, as a percentage of the screen. Used only with Aim oval aiming mode. Zero means direct turning.";
    case setting::tpDeadzoneY: return L"Height of the third-person aim oval, as a percentage of the screen. Used only with Aim oval aiming mode. Zero means direct turning.";
    case setting::leanKeysMode: return L"Switch between holding the lean modifier with mouse movement and separate lean-left/right keys. Release to straighten. New keys start unbound. The game's alternate-attack binding keeps its combat action when assigned to lean.";
    case setting::leanLeftKey: return L"Used only with left/right lean controls. Hold to lean left, release to straighten; both keys together straighten. Starts unbound.";
    case setting::leanRightKey: return L"Used only with left/right lean controls. Hold to lean right, release to straighten; both keys together straighten. Starts unbound.";
    case setting::fpDefense: return L"Automatic lets the game block for you. Hold RMB raises your guard yourself; reach, gear and recovery still decide if it works. Both views.";
    case setting::tpDefense: return L"Uses the same defense choice as first person. In ordinary combat, hold right mouse to ready the weapon and request native attack-specific interception; it does not guarantee a block or add a timing bonus. Right mouse no longer cursor-walks in ordinary close-view combat. Interaction and held objects keep their native controls. If Manual is unavailable, its choice stays saved and Automatic defense remains active.";
    case setting::fpSwingSelection: return L"Mouse: flick left or right before clicking to choose which side the swing starts from. Game: Exanima picks from your facing.";
    case setting::tpSwingSelection: return L"Choose independently for third person. Mouse direction works with both Aim oval and Screen centre. Move left or right before clicking to choose the swing's starting side; native weapon timing still applies.";
    case setting::edgeSoftness: return L"Gradually shares outward mouse movement with camera turning across the outer portion of the aim oval. 100% is the default; 0% restores the original hard boundary. The cursor still slides along the rim, inward motion is immediate, and the camera does not keep drifting after mouse release. Applies in first and third-person Aim oval mode.";
    case setting::tracking: return L"Receives processed view motion at 127.0.0.1:4243. Choose UDP over network in OpenTrack, or a compatible output in Beam. Tobii Game Hub can route through OpenTrack. Translation is unused. Device curves, calibration and eye contribution stay in that application.";
    case setting::trackingKey: return L"Records a neutral tracking pose without changing weapon aim. Unbound by default. Held-object rotation and lean bindings take precedence when shared.";
    case setting::invertY: return L"Mouse up looks down.";
    case setting::indicator: return L"When the crosshair shows.";
    case setting::vrIpd: return L"Distance between the two VR eyes. Zero keeps the headset's own eye poses. The eyes stay rigid with the tracked head; nothing smooths or offsets the camera.";
    case setting::vrScale: return L"Game units per metre of real movement. 100 matches the mod's centimetre settings.";
    case setting::vrRenderMode: return L"Draw each eye renders the world twice from the real eye positions. Reconstruct from depth draws the head once and synthesises both eyes from colour and depth; it needs a reconstruction host and otherwise falls back to drawing each eye. Holes filled paints what the head view saw behind an edge instead of leaving a black sliver there.";
    case setting::vrHands: return L"Drawn replaces the character's hand meshes with the mod's own articulated hands that follow tracked fingers. The game has no finger bones, so off shows its rigid hands.";
    case setting::vrPosture: return L"Standing drives the legs from trackers and walks the room. Seated keeps the game's own legs and ignores room walking and crouching, so a chair never reads as a crouch. Recenter after changing it.";
    case setting::vrHeadset: return L"On starts the OpenXR runtime of the headset the next time the game starts with the mod, using the loader shipped beside the mod. The view is the tracked head; nothing else changes until then.";
    case setting::vrTurning: return L"The right stick turns the body the way the mouse does, so the view and the body never part. Snap turns a fixed angle per flick; smooth turns while held. Off leaves turning to the head and mouse.";
    case setting::vrEffects: return L"On runs the View page's ambient occlusion on each headset eye, at their View strengths. Depth of field never runs in the headset: a fixed focus fights your own eyes.";
    case setting::vrGripAngle: return L"Tilts the controller's grip so a held hilt sits the way you hold it. Tracked hands are not tilted.";
    case setting::vrHaptics: return L"Strength of the controller pulses on grab, on your weapon landing and on being hit. Zero turns them off.";
    case setting::vrPanelWidth: return L"Width of the floating panel that shows the game's menus and the F8 panel in the headset.";
    case setting::vrPanelDistance: return L"How far ahead that panel floats. Nearer is larger and easier to read; farther is easier on the eyes.";
    case setting::vrComfort: return L"Darkens the edges of the view while the body turns smoothly, and with walking also while the stick walks, which many people find settles the stomach. The centre stays clear.";
    case setting::vrBody: return L"Hands only hides the character's own body and worn armour below the head while the headset drives it; the hands (the mod's own, drawn as if VR hands were on) and anything held stay. Full shows the body.";
    case setting::vrHeight: return L"Lifts or lowers the game head by this much, for a chair or a player taller or shorter than the character. Leaning, ducking and turning stay tracked.";
    case setting::vrGrabTurn: return L"On: squeeze an empty hand and sweep it sideways to drag the world round, through the same body turn as the stick. A hand holding something, or any drawn weapon, never turns. Off leaves turning to the stick.";
    case setting::vrEyeScale: return L"How large each eye is drawn, as a share of the window size: lower is cheaper and softer. Applies to the per-eye draws only; mirror and reconstruction modes and the desktop stay at full size.";
    case setting::vrGripSlide: return L"Slides the hand along a held weapon's hilt: positive towards the blade, negative towards the pommel.";
    case setting::vrKick: return L"Experimental. On: pressing a stick in plays the game's own kick when not mid-swing or reacting to a hit. A hit lands by foot contact only.";
    case setting::vrJump: return L"On, standing: hopping in real life plays the game's own jump and pushes your body up, when upright and not mid-swing. Landing is the game's ordinary physics.";
    case setting::vrWeaponWeight: return L"On: a held weapon follows your hands with a lag set by its own heft, so a heavy axe trails a fast swing and a dagger does not; a two-handed grip carries it better.";
    case setting::vrTeleport: return L"On: push the right stick forward to aim an arc from your right hand, let go to move there. Walls stop the arc; not while dragging something, attacking, down or out of reach. Walking on the left stick stays as it is.";
    case setting::vrHandPanel: return L"On: the inventory and every open menu sit on a panel held in your left hand, turned towards you; point at it with the right hand, the trigger clicks and the grip is the right button. Hold the trigger on an item and point out into the world to carry it out; let go to set it down there and pick it up. Off: the menu floats in front of you.";
    case setting::vrPouch: return L"On: let go of a held item at your left hip to put it in your inventory; close an empty hand there to take the last one back. Needs the script runtime's inventory exports; without them nothing happens.";
    case setting::vrFingerBones: return L"Experimental. On: finger bones are added to your body's own skeleton and the skinned body mesh bends its fingers to the tracked joints. Shows on the full body; the hands-only body hides that mesh.";
    case setting::ambientOcclusion: return L"Soft shadow in corners, under objects and where things touch. High values can darken narrow gaps.";
    case setting::depthOfField: return L"Softens very near and very far things while the middle stays sharp. First person only.";
    case setting::edgeSmoothing: return L"Smooths jagged edges in one cheap pass. With Supersample on, it lightly sharpens instead.";
    case setting::helmetVisor: return L"Darkens the edges of the view in the shape of the helmet you wear: a great helm's slit, a nasal guard, an open face.";
    case setting::playerGhost: return L"Shows a faint outline of you wherever walls or pillars hide you.";
    case setting::fingerPoses: return L"Fingers wrap round hilts and carried items and lie flat under shields, instead of the game's stiff hands. Experimental.";
    case setting::fingerPosesOthers: return L"The same finger poses for every other character in the level, in play.";
    case setting::conditionEffects: return L"Colour drains and edges darken below half health; eyes adjust between light and dark.";
    case setting::heldDetail: return L"Scratches, metal grain, pits and worn edges on what you hold. It reshapes existing light only, so an unlit blade stays dark.";
    case setting::armourDetail: return L"Plate, mail rings, leather grain and padding detail on worn gear within 6 m. Costs more than held items.";
    case setting::objectDetail: return L"The same detail on loose items, racks, chests, doors and furniture within 6 m. Off by default: it can draw thin dark lines across furniture.";
    case setting::woundMarks: return L"Cuts, stab holes and bruises stay on bodies where weapons got through, with blood running from fresh cuts.";
    case setting::woundDebug: return L"Highlights all procedural wounds, blade blood, and floor spatters in vibrant neon colors (cyan/magenta/green). Toggle with F12.";
    case setting::drawDistance: return L"Small props and far people past this distance aren't drawn; big things stay longer. Off draws everything.";
    case setting::objectLights: return L"Lights each object takes. The game uses six; fewer runs faster and keeps the strongest.";
    case setting::lightSmoothing: return L"Strong lights fall off in round gradients instead of squares. From the next level load.";
    case setting::farRooms: return L"Draws one more room through doorways, so far doorways don't show solid black. Off if it costs too much frame rate.";
    case setting::hitReaction: return L"First person. How hard the view jolts and tilts when a blow lands on you. 0 keeps it still; the damage flash stays. Never in VR.";
    case setting::f6Cycle: return L"What F6 switches between. 1st/3rd/game: each press moves on. 1st/3rd only: a press flips first and third person; hold F6 half a second for the game's camera.";
    default: return L"";
    }
}

} // namespace panel
