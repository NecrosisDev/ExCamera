#pragma once
// Controller buttons for the mod's own actions (kick, jump, guard, swap shoulder), read with XInput.
//
// Exanima 0.9.5.2g reads a controller only through Steam Input: routine 22E2D0 (reached from 22EC60 when the
// Steam Input handle at 5B26A0 is set) asks Steam for the game's actions and turns them into its key bits
// (keyWord 49CDC0) and buttons. Its own XInput reader 22EAD0 (XInputGetState(0) through the pointer at
// 4F32E0, left stick and left-stick click only) runs only when the byte at 4F32F0 is set, and no code in
// the exe writes 4F32F0 or 4F32E0, so it never runs. The game's actions and their default buttons are
// Steam's controller layout for app 362490 (workshop 2753707016): in its Combat set Y and the right-stick
// click do nothing, and the right-stick click does nothing in any set. The mod cannot add actions to that
// layout, so it reads the pad itself; a button the layout also uses still does the game's action too.
#include <cmath>
#include <cstdint>
#include "input_policy.h"

namespace fp {
enum PadButton : int {
    padNone,
    padA, padB, padX, padY,
    padLB, padRB, padLT, padRT,
    padLS, padRS, padBack, padStart,
    padUp, padDown, padLeft, padRight,
    padButtonCount
};
// XInput's wButtons bits, in PadButton order from padA (triggers are bytes, not bits: 0 here).
inline constexpr uint16_t padButtonMask[padButtonCount] = {
    0,      0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0,
    0x0040, 0x0080, 0x0020, 0x0010, 0x0001, 0x0002, 0x0004, 0x0008};
inline constexpr const wchar_t* padButtonWords[padButtonCount] = {
    L"none", L"a",  L"b",     L"x",    L"y",    L"lb",   L"rb",   L"lt",    L"rt",
    L"ls",   L"rs", L"back",  L"start", L"up",  L"down", L"left", L"right"};
inline constexpr const wchar_t* padButtonNames[padButtonCount] = {
    L"None",       L"A",       L"B",         L"X",         L"Y",       L"LB",        L"RB",
    L"LT",         L"RT",      L"Left stick click", L"Right stick click", L"Back",  L"Start",
    L"D-pad up",   L"D-pad down", L"D-pad left", L"D-pad right"};
// XINPUT_GAMEPAD_TRIGGER_THRESHOLD
inline constexpr uint8_t padTriggerThreshold = 30;

inline const wchar_t* padButtonText(int button) {
    return button > padNone && button < padButtonCount ? padButtonWords[button] : padButtonWords[padNone];
}
inline const wchar_t* padButtonName(int button) {
    return button > padNone && button < padButtonCount ? padButtonNames[button] : padButtonNames[padNone];
}
// The ini word; an unknown word is -1 (the caller keeps its default).
inline int padButtonFromText(const wchar_t* text) {
    if (!text) return -1;
    for (int b = 0; b < padButtonCount; ++b) {
        const wchar_t *t = text, *w = padButtonWords[b];
        while (*t && *w && (*t | 32) == *w) {
            ++t;
            ++w;
        }
        if (!*t && !*w) return b;
    }
    return -1;
}
struct PadState {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t rightX = 0, rightY = 0;
};
inline bool padHeld(int button, const PadState& pad) {
    if (button == padLT) return pad.leftTrigger > padTriggerThreshold;
    if (button == padRT) return pad.rightTrigger > padTriggerThreshold;
    return button > padNone && button < padButtonCount && (pad.buttons & padButtonMask[button]) != 0;
}
// The parry pose a right-stick push points to, as the right-mouse drag would pick it (ParryDrag, screen y
// down; the stick's y is up). Less than a third of the way out is no choice (0).
inline int padParryPose(int16_t x, int16_t y, bool mirror) {
    constexpr float deadZone = 1.f;
    ParryDrag drag;
    drag.move(float(x) / 32767.f * 3.f * deadZone, -float(y) / 32767.f * 3.f * deadZone, deadZone);
    return drag.pose(deadZone, mirror);
}
} // namespace fp
