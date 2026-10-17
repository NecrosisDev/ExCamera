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

// The rotation the player has given the held object, applied after the game's own placement each frame.
struct HeldRotation {
    float user[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // the player's rotation since the grab (rows)
    float nativeShape[12]{},
        writtenShape[12]{}; // applyPoints: the game's shape and the one written, about the centre
    bool havePoints = false;
    unsigned turns = 0;
    void reset() { *this = HeldRotation{}; }
    // Mouse movement in pixels, the view's right and up axes (unit).
    void turn(float dx, float dy, const float* viewRight, const float* viewUp) {
        if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.f && dy == 0.f)) return;
        rotateColumns(viewUp, dx * heldRotateRadiansPerPixel);
        rotateColumns(viewRight, dy * heldRotateRadiansPerPixel);
        ++turns;
    }
    // The drag's four target points (the held object's pose as the game drags it; on release it copies them
    // into the object's physics body): turned about their centre. The game moves them every frame; whether it
    // keeps the shape written last frame or sets its own, the result is its shape turned by the player's
    // rotation, once.
    void applyPoints(float (*points)[3]) {
        float centre[3]{};
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i) centre[i] += 0.25f * points[k][i];
        float shape[12];
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i) shape[k * 3 + i] = points[k][i] - centre[i];
        bool kept = havePoints;
        for (int i = 0; i < 12 && kept; ++i) kept = std::fabs(shape[i] - writtenShape[i]) < 1e-3f;
        if (!kept) std::memcpy(nativeShape, shape, 48);
        for (int k = 0; k < 4; ++k) {
            const float* v = nativeShape + k * 3;
            for (int i = 0; i < 3; ++i) {
                const float w = user[i * 3 + 0] * v[0] + user[i * 3 + 1] * v[1] + user[i * 3 + 2] * v[2];
                writtenShape[k * 3 + i] = w;
                points[k][i] = centre[i] + w;
            }
        }
        havePoints = true;
    }

private:
    // user <- R(axis, angle) * user: a turn in the world's frame, after the earlier ones.
    void rotateColumns(const float* axis, float angle) {
        float cols[9];
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < 3; ++i) cols[r * 3 + i] = user[i * 3 + r]; // columns as rows
        rotateAxes(cols, axis, angle);
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < 3; ++i) user[i * 3 + r] = cols[r * 3 + i];
    }
};

// Flinging: let go while the wheel is pushing the object out and it keeps that speed. The game zeroes a released
// object's velocity by making its physics points' history equal to where they are (Verlet: the velocity is
// the difference); the fling writes that history as "where they are, minus the velocity times one physics
// step" instead. Capped, as Half-Life 2 caps a throw.
inline constexpr float flingMaxSpeed = 1200.f;          // cm/s
inline constexpr unsigned long long flingFreshMs = 120; // a push older than this is no longer under way

// How fast the hold distance is growing (cm/s), smoothed over a few frames; 0 when it is not growing.
struct PushRate {
    float last = -1.f, rate = 0.f;
    unsigned long long at = 0;
    void step(float distance, unsigned long long ms) {
        if (last >= 0.f && ms > at && ms - at < 250) {
            const float now = (distance - last) * 1000.f / float(ms - at);
            const float k = 1.f - std::exp(-float(ms - at) / 40.f);
            rate += ((std::isfinite(now) ? now : 0.f) - rate) * k;
        } else
            rate = 0.f;
        last = distance;
        at = ms;
    }
    // The outward speed if a push is under way at `ms`, else 0.
    float pushing(unsigned long long ms) const { return rate > 1.f && ms - at <= flingFreshMs ? rate : 0.f; }
};

// The released body's point history for a velocity (cm/s, capped) and a physics step (s): the previous points
// one step behind along it, the ones before that two steps behind. False (nothing written) without a velocity
// or a known step.
inline bool flingHistory(const float* velocity, float stepSeconds, const float (*now)[3],
                         float (*previous)[3], float (*beforeThat)[3]) {
    float v[3] = {velocity[0], velocity[1], velocity[2]};
    const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(speed > 1.f) || !(stepSeconds > 0.f && stepSeconds < 0.1f)) return false;
    if (speed > flingMaxSpeed)
        for (float& c : v) c *= flingMaxSpeed / speed;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i) {
            previous[k][i] = now[k][i] - v[i] * stepSeconds;
            beforeThat[k][i] = now[k][i] - 2.f * v[i] * stepSeconds;
        }
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
    case 0x02:
        return L"right mouse";
    case 0x04:
        return L"middle mouse";
    case 0x05:
        return L"mouse 4";
    case 0x06:
        return L"mouse 5";
    case 0x10:
        return L"Shift";
    case 0x11:
        return L"Ctrl";
    case 0x12:
        return L"Alt";
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, wchar_t(key));
    return L"key " + std::to_wstring(key);
}
// The F8 row cycles these; the ini takes any key.
inline constexpr int heldKeyChoices[] = {0x02, 0x04, 0x05, 0x06, 'R', 'E', 0x12, 0};

} // namespace fp
