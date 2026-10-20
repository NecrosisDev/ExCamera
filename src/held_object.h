#pragma once
// Holding an object (an item dragged with the hand, drag flag 0x2000), as in Half-Life 2: it stays at the
// distance it was grabbed at, measured from the eye, wherever the pointer's ray starts (the eye in first
// person, the camera behind the shoulder in third); the wheel pushes it out or pulls it in; the rotate key
// (default right mouse) turns it with the mouse. Pure: first_person.cpp feeds it the game's numbers.
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

namespace fp {

// Where along the pointer's ray the held object goes: `distance` from the eye. From the eye itself that is
// `distance`; from a camera behind it, the far side of the sphere of that radius around the eye (in front of
// the eye). -1: the ray never comes within `distance` of the eye (pointing well away from the character).
inline float holdAlongRay(const float* origin, const float* dir, const float* eye, float distance) {
    float m[3];
    for (int i = 0; i < 3; ++i) m[i] = origin[i] - eye[i];
    const float b = m[0] * dir[0] + m[1] * dir[1] + m[2] * dir[2];
    const float c = m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - distance * distance;
    const float disc = b * b - c;
    if (!(disc >= 0.f) || !(distance > 0.f)) return -1.f;
    const float t = -b + std::sqrt(disc);
    return t >= 0.f ? t : -1.f;
}

// The eye-to-point distance of a point on a ray (the grab's start: where the click's pick hit).
inline float eyeDistance(const float* origin, const float* dir, float t, const float* eye) {
    float s = 0.f;
    for (int i = 0; i < 3; ++i) {
        const float d = origin[i] + dir[i] * t - eye[i];
        s += d * d;
    }
    return std::sqrt(s);
}

// Turns the three axes of a 3x3 orientation (rows) about a unit axis through the origin (Rodrigues).
inline void rotateAxes(float* axes, const float* axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1.f - c;
    for (int r = 0; r < 3; ++r) {
        float* v = axes + r * 3;
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
                            axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i) v[i] = v[i] * c + x[i] * s + axis[i] * d * k;
    }
}

// Mouse to rotation while the rotate key is held: moving right turns the object about the view's up axis,
// moving up tips it about the view's right axis, both through the object's own centre. radiansPerPixel: the
// mouse's turn rate.
inline constexpr float heldRotateRadiansPerPixel = 0.006f;

// A fresh outward wheel gesture requests release velocity, independently of placement.
// Strength matches holdApproach's initial rate (12 * requested distance increase).
inline constexpr float flingMaxSpeed = 1200.f;
inline constexpr unsigned long long flingFreshMs = 120;
inline float wheelThrowSpeed(float distance, int wheel) {
    if (!(distance > 0.f) || !std::isfinite(distance) || wheel <= 0) return 0.f;
    const float speed = 12.f * distance * std::expm1(std::log(1.12f) * float(wheel) / 120.f);
    return std::isfinite(speed) ? std::fmin(speed, flingMaxSpeed) : 0.f;
}

// Preserve the grabbed point and proportional wheel motion in the physical plane.
inline bool heldPlaneRadiusScale(const float* origin, const float* grabbed, const float* plane,
                                  float rayDistance, float& scale) {
    const float nn = plane[0]*plane[0] + plane[1]*plane[1] + plane[2]*plane[2];
    if (!(nn > .5f) || !std::isfinite(nn) || !(rayDistance > 0.f)) return false;
    float delta[3], dot = 0.f;
    for (int i = 0; i < 3; ++i) { delta[i] = grabbed[i] - origin[i]; dot += delta[i]*plane[i]; }
    float length2 = 0.f;
    for (int i = 0; i < 3; ++i) { delta[i] -= plane[i]*dot/nn; length2 += delta[i]*delta[i]; }
    if (!(length2 > 1.e-6f) || !std::isfinite(length2) || !std::isfinite(rayDistance)) return false;
    scale = std::sqrt(length2) / rayDistance;
    return std::isfinite(scale) && scale > 0.f;
}

// Native 088D30 adds (history - history2) to current points, then 089330 rolls
// history -> history2 and current -> history. At the live pre-integration seam,
// changing ONLY history2 adds velocity without doubling it after that rollover.
// Preserve existing translation/rotation and stage all12 finite results before writing.
inline bool addFlingHistoryDelta(const float* velocity, float stepSeconds, float (*older)[3]) {
    if (!(stepSeconds > 0.f && stepSeconds < .1f) || !std::isfinite(stepSeconds)) return false;
    double v[3] = {velocity[0], velocity[1], velocity[2]};
    for (double x : v) if (!std::isfinite(x)) return false;
    const double speed = std::hypot(std::hypot(v[0], v[1]), v[2]);
    if (!(speed > 1.) || !std::isfinite(speed)) return false;
    if (speed > flingMaxSpeed)
        for (double& x : v) x *= double(flingMaxSpeed) / speed;
    float staged[4][3];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(older[k][i])) return false;
            staged[k][i] = float(double(older[k][i]) - v[i] * double(stepSeconds));
            if (!std::isfinite(staged[k][i])) return false;
        }
    std::memcpy(older, staged, sizeof staged);
    return true;
}

// [Input] RotateHeldKey: a name (rmb, mmb, mouse4, mouse5, shift, ctrl, alt), a letter, or a virtual-key code as
// a number (so "0" is unbound, as every binding in first-person.ini). Unknown text: unbound.
inline int heldKeyFromText(const wchar_t* t) {
    if (!t) return 0;
    while (*t == L' ') ++t;
    wchar_t s[16]{};
    size_t n = 0;
    for (; t[n] && t[n] != L' ' && n < 15; ++n)
        s[n] = wchar_t(t[n] >= L'A' && t[n] <= L'Z' ? t[n] + 32 : t[n]);
    if (!n) return 0;
    const struct {
        const wchar_t* name;
        int key;
    } names[] = {{L"rmb", 0x02},   {L"mmb", 0x04},  {L"mouse4", 0x05}, {L"mouse5", 0x06},
                 {L"shift", 0x10}, {L"ctrl", 0x11}, {L"alt", 0x12}};
    for (const auto& e : names)
        if (!wcscmp(s, e.name)) return e.key;
    if (n == 1 && s[0] >= L'a' && s[0] <= L'z') return s[0] - 32; // a letter key
    wchar_t* end = nullptr;
    const long v = std::wcstol(s, &end, 10);
    return end && !*end && v > 0 && v < 255 ? int(v) : 0;
}
inline std::wstring heldKeyName(int key) {
    switch (key) {
    case 0:
        return L"unbound";
    case 0x01:
        return L"left mouse";
    case 0x02:
        return L"right mouse";
    case 0x04:
        return L"middle mouse";
    case 0x05:
        return L"mouse 4";
    case 0x06:
        return L"mouse 5";
    case 0x08:
        return L"Backspace";
    case 0x09:
        return L"Tab";
    case 0x0d:
        return L"Enter";
    case 0x10:
        return L"Shift";
    case 0x11:
        return L"Ctrl";
    case 0x12:
        return L"Alt";
    case 0x13:
        return L"Pause";
    case 0x14:
        return L"Caps Lock";
    case 0x1b:
        return L"Esc";
    case 0x20:
        return L"Space";
    case 0x21:
        return L"Page Up";
    case 0x22:
        return L"Page Down";
    case 0x23:
        return L"End";
    case 0x24:
        return L"Home";
    case 0x25:
        return L"Left";
    case 0x26:
        return L"Up";
    case 0x27:
        return L"Right";
    case 0x28:
        return L"Down";
    case 0x2d:
        return L"Insert";
    case 0x2e:
        return L"Delete";
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, wchar_t(key));
    if (key >= 0x70 && key <= 0x87) return L"F" + std::to_wstring(key - 0x70 + 1);
    return L"key " + std::to_wstring(key);
}
// The F8 row cycles these; the ini takes any key.
inline constexpr int heldKeyChoices[] = {0x02, 0x04, 0x05, 0x06, 'R', 'E', 0x12, 0};

} // namespace fp
