// Reversible camera adapter with native instruction contracts. Physics and head pose remain authoritative.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <map>
#include <array>
#include <cstdarg>
#include <intrin.h>
#include "camera_motion.h"
#include "input_policy.h"
#include "build_identity.h"
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
// The mod is one translation unit; its parts are included in order (later parts use earlier ones).
#include "mod/state.inl"
#include "mod/input.inl"
#include "mod/combat.inl"
#include "mod/picking.inl"
#include "mod/view.inl"
#include "mod/overlay.inl"
#include "mod/frame.inl"
#include "mod/camera.inl"
#include "mod/session.inl"
#include "mod/settings.inl"
#include "mod/panel.inl"
