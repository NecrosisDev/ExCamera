// Reversible camera adapter with native instruction contracts. Physics and head pose remain authoritative.
#define WIN32_LEAN_AND_MEAN
#include "tracking_udp.h"
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <atomic>
#include <map>
#include <array>
#include <cstdarg>
#include <intrin.h>
#include "camera_motion.h"
#include "supersampling_depth.h"
#include "input_policy.h"
#include "build_identity.h"
#include "hook_profile.h"
#include "native_build.h"
#include "game_layout.h"
#include "native_contracts.h"
#include "hook_health.h"
#include "status_json.h"
#include "game_structs.h"
#include "pointer_set.h"
#include "held_object.h"
namespace field = fp::game; // field::character::combatStance and so on: offsets in the game's objects
#include "head_stabiliser.h"
#include <algorithm>
#include <string>
#include <vector>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include "vendor/minhook/include/MinHook.h"
// Optional XR integration, compiled in only with the OpenXR SDK (build.py --openxr-sdk).
// Earlier parts see these as plain functions either way.
#ifdef FP_OPENXR_EMULATION
#include "xr/backend.h"
static bool trackedBodyOwns(uintptr_t);
static int trackedBodyHand(unsigned, float*);
static void trackedBodyStereo(fp::xr::StereoFrame&);
static bool trackedPhysicalDrained();
static void trackedLocomotionFault();
static bool trackedWalkInput(uintptr_t, float*, float&);
static bool trackedDuckInput(uintptr_t, float&);
static bool trackedAttackHeld();
static float trackedTurnTake();
static void trackedControllerInput(uintptr_t, uintptr_t, bool);
static bool trackedMotorBegin(uintptr_t);
static bool trackedMotorOwns(uintptr_t);
static void trackedMotorEnd();
static void trackedMotorFault();
static bool trackedMotorGaze(uintptr_t, float*);
static bool trackedPickScope();
static char trackedPickAccept(uintptr_t, char, uintptr_t);
static void trackedDrivePublish();
static void trackedDriveClear();
static void trackedPickupTick();
static bool createTrackedDriveHooks();
static MH_STATUS disableTrackedDriveHooks();
static int trackedHeldTarget(uintptr_t, uintptr_t, float*, float*);
static bool trackedHandDraw(uintptr_t, uintptr_t, bool);
static bool trackedHandSubmission(uintptr_t);
static bool trackedBodySubmission(uintptr_t);
static bool trackedBodyHidden(uintptr_t, uintptr_t, bool);
static void stereoBeforePresent();
static void stereoLeavingContext();
static bool stereoDrained();
static bool createStereoHooks();
static MH_STATUS disableStereoHooks();
#else
static bool trackedBodyOwns(uintptr_t) { return false; }
static void trackedLocomotionFault() {}
static bool trackedWalkInput(uintptr_t, float*, float&) { return false; }
static bool trackedDuckInput(uintptr_t, float&) { return false; }
static bool trackedAttackHeld() { return false; }
static float trackedTurnTake() { return 0; }
static bool trackedHandDraw(uintptr_t, uintptr_t, bool) { return false; }
static bool trackedHandSubmission(uintptr_t) { return false; }
static bool trackedBodySubmission(uintptr_t) { return false; }
static bool trackedBodyHidden(uintptr_t, uintptr_t, bool) { return false; }
static void trackedControllerInput(uintptr_t, uintptr_t, bool) {}
static bool trackedMotorBegin(uintptr_t) { return false; }
static bool trackedMotorOwns(uintptr_t) { return false; }
static void trackedMotorEnd() {}
static void trackedMotorFault() {}
static bool trackedMotorGaze(uintptr_t, float*) { return false; }
static bool trackedPickScope() { return false; }
static char trackedPickAccept(uintptr_t, char hit, uintptr_t) { return hit; }
static int trackedHeldTarget(uintptr_t, uintptr_t, float*, float*) { return 0; }
static void stereoBeforePresent() {}
static void stereoLeavingContext() {}
static bool stereoDrained() { return true; }
static bool createStereoHooks() { return true; }
static MH_STATUS disableStereoHooks() { return MH_OK; }
#endif
// The mod is one translation unit; its parts are included in order (later parts use earlier ones).
#include "mod/state.inl"
#include "mod/profiling.inl"
#include "mod/input.inl"
#include "mod/combat.inl"
#include "mod/picking.inl"
#include "mod/view.inl"
#include "mod/supersampling.inl"
#include "mod/overlay.inl"
#include "mod/wounds.inl"
#include "mod/fire.inl"
#include "mod/view_effects.inl"
#include "mod/frame.inl"
#include "mod/headwear_render.inl"
#include "mod/stone_skin.inl"
#include "mod/wounds_bake.inl"
#include "mod/xr_bones.inl"
#include "mod/blade.inl"
#include "mod/motions.inl"
#include "mod/camera.inl"
#include "mod/dev.inl"
#ifdef FP_OPENXR_EMULATION
#include "mod/xr_stereo.inl"
#include "mod/xr_drive.inl"
#include "mod/xr_hand_render.inl"
#include "mod/xr_bones_vr.inl"
#endif
#include "mod/session.inl"
#include "mod/settings.inl"
#include "mod/panel.inl"
