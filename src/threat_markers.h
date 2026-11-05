#pragma once
// Threat markers (docs/THREAT-MARKERS.md): who counts as a threat to the player and where its marker sits on a
// ring around the screen centre. Pure, no game or Windows access, unit-tested in tests/unit/threat_markers_tests.cpp.
// Every game value it takes is read by mod/threats.inl from fields the audit proves; the range, the ring and
// the fades are our own look.
#include <cmath>

#include "input_policy.h"

namespace fp::threat {

// The setting: off, only while the player is in combat stance (+12FE), or always.
enum Mode : int { off = 0, combatOnly = 1, always = 2 };
inline int modeFromText(const wchar_t* text) {
    auto is = [&](const wchar_t* word) {
        const wchar_t* t = text;
        while (*t && *word && ((*t | 32) == *word)) {
            ++t;
            ++word;
        }
        return !*t && !*word;
    };
    if (!text) return combatOnly;
    return is(L"off") ? off : is(L"always") ? always : combatOnly;
}
inline const wchar_t* modeText(int mode) { return mode == off ? L"off" : mode == always ? L"always" : L"combat"; }

// One character as the game left it after its own update (mod/threats.inl reads it there).
struct Sample {
    unsigned state = 0;          // character::stateFlags (+DF4), a value (docs/GAME-FIELDS-AUDIT.md)
    int motion = -1;             // character::motion (+9B8)
    float progress = 0;          // character::motionProgress (+A28)
    bool stance = false;         // character::combatStance (+12FE)
    bool targetsPlayer = false;  // character::aimOpponent (+E38) is the player's root
    bool self = false;           // this is the player
    float distance = 0;          // cm, from the player
};

inline constexpr float rangeCm = 1000.f; // ours: ten metres
// The game's tests (camera_motion.h): dead & 0x800, down & 0xA00.
inline bool dead(unsigned state) { return (state & 0x800) != 0; }
inline bool down(unsigned state) { return (state & 0xA00) != 0; }
// Motions 5-12 are the attacks (motionPlay 179F60-7A); 3 is "Defence stance" in the game's motion-set table, the
// motion a character spawned for combat starts in (0xF026A, with +12FE set at 0xF0274).
inline bool attacking(int motion) { return motion >= 5 && motion <= 12; }
inline bool guarding(const Sample& s) { return s.stance || s.motion == 3; }
// The game's commit points: 0.30, 0.4 for motion 11, the Stab (input_policy.h windup).
inline float commitPoint(int motion) { return motion == 11 ? .40f : .30f; }
// A swing winding up: an attack before its commit point.
inline bool windingUp(const Sample& s) { return attacking(s.motion) && std::isfinite(s.progress) && s.progress < commitPoint(s.motion); }

// Alive, not the player, within range, aiming at the player, and guarding or attacking.
inline bool isThreat(const Sample& s) {
    return !s.self && !dead(s.state) && !down(s.state) && s.targetsPlayer && std::isfinite(s.distance) &&
           s.distance <= rangeCm && (guarding(s) || attacking(s.motion));
}

// Where the marker goes. delta: the threat minus the camera (world); pose: the camera's basis rows (right, up,
// back) as camera.inl keeps them; scale: the projection's pixels per unit of x/(-z) (projectionScale). A threat
// well inside the view (both screen offsets under `inner` of the half size) gets no marker. Otherwise `angle`
// is the screen direction from the centre, radians, x right and y down: from its projection when it is in front,
// else from above (behind is down).
inline constexpr float inner = .7f;
inline bool markerAngle(const float* delta, const float* pose, float width, float height, float scale, float& angle) {
    float x = 0, y = 0, z = 0;
    for (int i = 0; i < 3; ++i) {
        x += delta[i] * pose[i];
        y += delta[i] * pose[3 + i];
        z += delta[i] * pose[6 + i];
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !(width > 0) || !(height > 0) || !(scale > 0))
        return false;
    if (z < -.01f) {
        const float sx = x / (-z) * scale, sy = -y / (-z) * scale;
        if (std::fabs(sx) < inner * width * .5f && std::fabs(sy) < inner * height * .5f) return false;
        angle = std::atan2(sy, sx);
    } else {
        if (std::fabs(x) < 1e-3f && std::fabs(z) < 1e-3f) return false;
        angle = std::atan2(z, x);
    }
    return std::isfinite(angle);
}

// One marker: a short soft arc on a ring around the screen centre (cx, cy), centred on `angle`, drawn the way the
// wind-up arc is (fp::arcBand): a faint hairline over a glow fading toward the centre, both thinning toward the
// ends. The ring is ringFraction of the smaller screen side. counts[2]: the strips' vertices, (x, y, alpha) each.
inline constexpr float ringFraction = .21f, halfSpan = .17f;
inline bool markerArc(float cx, float cy, float width, float height, float angle, float* out, int capFloats, int* counts) {
    counts[0] = counts[1] = 0;
    if (!std::isfinite(angle) || !(width > 0) || !(height > 0)) return false;
    const float side = std::fmin(width, height), r = side * ringFraction;
    const float line = std::fmax(1.f, side * .0022f), glow = side * .012f;
    counts[0] = arcBand(cx, cy, r - (line + glow) * .5f, glow, angle - halfSpan, angle + halfSpan, angle, .55f, 0.f,
                        out, capFloats, 0.f);
    const int used = counts[0] * 3;
    counts[1] = arcBand(cx, cy, r, line, angle - halfSpan, angle + halfSpan, angle, 1.f, 0.f, out + used,
                        capFloats - used);
    return counts[0] > 0 && counts[1] > 0;
}

// A marker's brightness 0..1 eases toward its aim: on fades in over fadeInMs, off fades out over fadeOutMs.
inline constexpr float fadeInMs = 180.f, fadeOutMs = 260.f, windupBoost = 1.8f;
inline float fade(float shown, float toward, float elapsedMs) {
    if (!(elapsedMs > 0)) return shown;
    const float step = elapsedMs / (toward > shown ? fadeInMs : fadeOutMs);
    return toward > shown ? std::fmin(toward, shown + step) : std::fmax(toward, shown - step);
}

} // namespace fp::threat
