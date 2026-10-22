#pragma once
#include "frame_contract.h"
#include "eyes.h"
#include "frustum.h"
#include "full_body.h"
#include "reconstruction.h"
#include "upper_body.h"
#include <cstdint>

namespace fp::xr {
// One predicted tracking sample per presentation, copied for consumers on other threads.
struct TrackingFrame {
    BodySample sample{};
    UpperBody rig{};
    LowerSample lower{};
    NativePose anchor{};
    HeadCamera calibration{};
    uint64_t serial = 0, tick = 0;
    bool enabled = false;
};
// The host owns OpenXR and GPU copies only; native world rendering stays with the mod.
// Every call runs on the render owner's thread with its WGL context current.
struct Backend {
    // The mod's settings, applied before the next frame; a mode the host cannot serve falls back.
    void (*configure)(EyeLayout eyes, RenderMode mode) = nullptr;
    bool (*begin)(uint64_t serial, const NativePose& anchor, float clipNear, float clipFar, StereoFrame& out) = nullptr;
    RenderMode (*mode)() = nullptr;
    bool (*saveTarget)(int width, int height) = nullptr;
    bool (*prepareEye)(unsigned eye) = nullptr;
    bool (*copyEye)(unsigned eye) = nullptr;
    // Reconstruction mode: capture the drawn centre view with its depth under `contract`,
    // synthesise both eyes and deliver them. The native target is left untouched.
    bool (*reconstruct)(const FrameContract& contract, const StereoFrame& frame) = nullptr;
    void (*restoreTarget)() = nullptr;
    // Whether the game's own UI is open this frame: the desktop quad replaces the reticle, head-locked, or
    // standing in the level at the recentred origin when the world is drawn around it (the title screen).
    void (*overlay)(bool uiOpen, bool handsDrawn, bool uiInWorld) = nullptr;
    // Forget the play-space origin; the next frame takes the head where it is as the game head.
    void (*recenter)() = nullptr;
    // Comfort darkening at the edges of both eyes for this frame (0 none, 1 full). On a native eye image it draws
    // at once; in reconstruction the host applies it to the warped eyes.
    bool (*comfort)(float strength) = nullptr;
    // One haptic pulse on a hand, from the render thread that owns the session.
    bool (*haptic)(unsigned side, float amplitude, uint64_t durationNs) = nullptr;
    void (*finish)(bool accepted) = nullptr;
    bool (*retire)() = nullptr;
    // The runtime or its session went away for good (exit, loss): retire and try again later.
    bool (*lost)() = nullptr;
    // The recentred origin in the room-fixed stage space, when the runtime offers one and a frame is
    // current; and placing a remembered one back. Both false otherwise.
    bool (*originInStage)(XrPosef& out) = nullptr;
    bool (*restoreOrigin)(const XrPosef& stageOrigin) = nullptr;
    // The wrist menu for this frame: a panel of wristPanelWidth by wristPanelHeight metres at `pose` (local
    // space) with up to wristPanelLines lines, one highlighted; hidden when `shown` is false.
    void (*wrist)(bool shown, const XrPosef& pose, const char* const* lines, unsigned count, int highlight) = nullptr;
    // The pointer beam for this frame, from the aiming controller to the hit (local space), and the hit on
    // the panel image (u right, v down, 0..1) for the dot; hidden when `shown` is false.
    void (*beam)(bool shown, const XrVector3f& from, const XrVector3f& to, float u, float v) = nullptr;
    // The rectangle of the saved target that holds an eye (the eye scale draws into a corner); 0 means all of it.
    void (*eyeRect)(int width, int height) = nullptr;
};
constexpr float wristPanelWidth = .16f, wristPanelHeight = .20f;
constexpr unsigned wristPanelLines = 8;
// The pointer: a thin strip from the controller to where its ray meets a panel, and a dot there.
constexpr float pointerBeamWidth = .006f;
constexpr int pointerDotPixels = 10;
Backend emulatedBackend();
// False when no host owns the current context; stale samples come back unfocused.
bool trackingFrame(TrackingFrame& out);
const char* reconstructionStatus();
}
