#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
namespace fp {
inline int defenseModeFromText(const wchar_t* text) {
    if (!text) return 0;
    const wchar_t* manual = L"manual";
    while (*text && *manual && ((*text | 32) == *manual)) {
        ++text;
        ++manual;
    }
    return !*text && !*manual ? 1 : 0;
}
inline const wchar_t* defenseModeText(int mode) {
    return mode == 1 ? L"manual" : L"automatic";
}
// Native semantic bits, not physical keys. Polling remains owned by the game.
inline unsigned movementKey(unsigned key, const unsigned* bindings) {
    unsigned mask = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (bindings[i] && key == bindings[i]) mask |= 1u << i;
    return mask;
}
// A click held across UI/focus/mode transitions must be released before it can
// become a gameplay click. Buttons are independent; ordinary holds pass intact.
struct ButtonHandoff {
    bool owned = false;
    unsigned blocked = 0;
    void reset() {
        owned = false;
        blocked = 0;
    }
    unsigned sample(bool owns, unsigned nativeButtons) {
        if (!owns) {
            owned = false;
            blocked = nativeButtons & 3u;
            return 0;
        }
        if (!owned) {
            blocked = nativeButtons & 3u;
            owned = true;
        }
        blocked &= nativeButtons;
        return nativeButtons & ~blocked;
    }
};
// Raw relative mouse counts bypass Windows pointer acceleration and integer
// cursor rounding/edge clamping. Absolute-only devices (tablets, remote
// desktop) keep the cursor-delta path. Counts accumulate only while
// first-person gameplay owns look; a release/handoff discards stale motion.
struct RawLook {
    long dx = 0, dy = 0;
    bool relative = false;
    void reset() { dx = dy = 0; }
    void event(bool absolute, long x, long y, bool owns) {
        // Never carry relative counts across an absolute-device/cursor fallback.
        // Reset before adding the first fresh relative packet on a handoff.
        if (relative == absolute) reset();
        relative = !absolute;
        if (!absolute && owns) {
            dx += x;
            dy += y;
        }
    }
    bool take(long& x, long& y) {
        if (!relative) return false;
        x = dx;
        y = dy;
        dx = dy = 0;
        return true;
    }
};
// Native ordinary-attack side request (RVA 0xC24A5-0xC2538, new press only):
// relative = wrap(aim heading behaviour+0x1B0 - body heading root+0xAD4)
// opposite side (request 2) when relative > threshold, else default (0);
// threshold 15 degrees when root+0x1068 == 0, otherwise 20 degrees.
// The approved optional held binding inverts that natural choice. Returns the
// aim heading to present to the classifier for this one press.
inline float wrapHeading(float a) {
    const float pi = 3.14159265f;
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    return a;
}
inline bool oppositeSide(float aim, float body, bool wideThreshold) {
    // Match native 1C8C0: float subtraction, double pi bounds, one float wrap.
    // Do not change wrapHeading: camera and aim normalization have different owners.
    float relative = aim - body;
    constexpr double pi = 3.141592653589793;
    constexpr float tau = 6.2831854820251465f;
    if (double(relative) < -pi) relative += tau;
    else if (double(relative) > pi) relative -= tau;
    return relative > (wideThreshold ? 20.f : 15.f) * 0.0174532925f;
}
inline float flippedSideHeading(float aim, float body, bool wideThreshold) {
    const float threshold = (wideThreshold ? 20.f : 15.f) * 0.0174532925f;
    // Natural opposite -> present the body heading (default). Natural default
    // -> present threshold plus ten degrees, comfortably past the strict test.
    return oppositeSide(aim, body, wideThreshold) ? body : wrapHeading(body + threshold + 0.174532925f);
}
// Close-view swing selection follows a deliberate horizontal mouse gesture,
// before the aim box splits that motion between aiming and camera turning.
// A small reversal cannot change the selected side; vertical motion is neutral.
struct SwingGesture {
    int side = 0; // -1 left origin, +1 right origin, 0 no directional choice yet
    float pending = 0.f;
    void move(float dx, float dy, float width) {
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(width) || !(width > 0.f)) return;
        if (std::fabs(dy) > std::fabs(dx) * 2.f) {
            pending = 0.f;
            return;
        }
        if (!dx) return;
        if (pending * dx < 0.f) pending = 0.f;
        pending += dx;
        const float threshold = std::fmax(3.f, width * .004f);
        if (std::fabs(pending) >= threshold) {
            side = pending < 0.f ? -1 : 1;
            pending = 0.f;
        }
    }
};
// Attack-origin cue: where an ordinary attack would start, or the family the
// game actually accepted. Neutral whenever that is not known with confidence.
// Accepted motion at root+0x9B8 (set by transition 0x179EE0): 5-7 default
// ordinary family, 8-10 opposite family, 11 thrust, 12 rapid-press overhead.
enum class OriginCue : int { neutral = 0, defaultSide = 1, oppositeSide = 2, overhead = 3 };
struct CueInputs {
    bool owned = false, combat = false, upright = false, attackHeld = false, alternateHeld = false,
         pressBlocked = false, modifierHeld = false, wideThreshold = false;
    int motion = 0;
    unsigned clock = 0, lastPress = 0; // behaviour+0x8E4: last ordinary press time.
    float aimHeading = 0, bodyHeading = 0;
};
inline OriginCue originCue(const CueInputs& c) {
    if (!c.owned || !c.combat) return OriginCue::neutral;
    if (c.motion >= 5 && c.motion <= 7) return OriginCue::defaultSide;
    if (c.motion >= 8 && c.motion <= 10) return OriginCue::oppositeSide;
    if (c.motion == 12) return OriginCue::overhead;
    if (c.motion == 11) return OriginCue::neutral; // Thrust has no lateral origin.
    // Held but not accepted (ineligible, recovering, pending): no promise.
    if (c.attackHeld || c.alternateHeld || c.pressBlocked || !c.upright) return OriginCue::neutral;
    // A press inside the native 300-tick window requests the overhead path, whose
    // dispatcher precedence is contextual (0xC1CF0 at behaviour+0x8E4).
    if (uint64_t(c.clock) < uint64_t(c.lastPress) + 300) return OriginCue::neutral;
    if (!std::isfinite(c.aimHeading) || !std::isfinite(c.bodyHeading)) return OriginCue::neutral;
    const bool opposite = oppositeSide(c.aimHeading, c.bodyHeading, c.wideThreshold) != c.modifierHeld;
    return opposite ? OriginCue::oppositeSide : OriginCue::defaultSide;
}
// Weapon hands. The actor keeps up to two weapon records,
// root+0x920 count, +0x928/+0x930 (slot 0 primary, slot 1 second). Each
// is attached to one native hand h: +0xE88+h*8 holds the item (both hands
// for a two-handed grip), and +0xE78+h*8 is the hand part itself (fists
// are the hand parts: 0x155480 copies them into the slots). The hand part's
// mesh is named ..RHand / ..LHand. The hand a slot is attached to comes from
// the grip table at RVA 0x2BAC50 (0xB8 per entry, hand at +8): slot 0 uses
// root+0x1070 (0x152100, from the primary's grip class), slot 1 always
// entry 5. The default family (0x157240) swings slot 0; the opposite
// family (0x157700) swings slot 1 when root+0x1068 & 0x80 (dual wield 0x90,
// unarmed 0x80/0xC0), otherwise slot 0 again (backhand). The accepted swing
// is root+0x988.
struct SwingHands {
    int slot = -1;          // Weapon slot that swings (0/1), -1 unknown.
    int hand = -1;          // Native hand index actually holding it, -1 unknown.
    int expectedHand = -1;  // Hand the native grip entry for that slot expects.
    int handSide = 0;       // Side of that hand from its mesh name: 1 left, 2 right, 0 unknown.
    bool alternate = false; // root+0x1068 & 0x80: opposite family uses slot 1.
};
inline int swingSlot(bool oppositeFamily, bool alternate) {
    return oppositeFamily && alternate ? 1 : 0;
}
// Native hand holding weapon w: the hand whose held item or hand part is w.
// Held in both hands (two-handed grip): the hand the grip entry names.
inline int weaponHand(uintptr_t w, const uintptr_t* held, const uintptr_t* parts, int expected) {
    if (!w) return -1;
    const bool in0 = held[0] == w || parts[0] == w, in1 = held[1] == w || parts[1] == w;
    if (in0 && in1) return expected == 0 || expected == 1 ? expected : -1;
    return in0 ? 0 : (in1 ? 1 : -1);
}
// Hand side from the hand part's mesh name ("17RHand", "18LHand").
inline int handSideFromName(const char* name) {
    if (!name) return 0;
    size_t n = 0;
    while (n < 16 && name[n]) ++n;
    if (n < 5 || name[n - 4] != 'H' || name[n - 3] != 'a' || name[n - 2] != 'n' || name[n - 1] != 'd')
        return 0;
    return name[n - 5] == 'L' ? 1 : (name[n - 5] == 'R' ? 2 : 0);
}
// Origin side of the swing before view roll. Overhead stays on top. A
// lateral side is promised only when the swinging weapon sits in the hand the
// native grip entry expects (otherwise the native motion and the weapon
// disagree, e.g. the second weapon moved into slot 0) and that hand's side is
// known. Forehand (default family, or the opposite family swinging the
// second weapon) starts on that hand's side; the opposite family swinging the
// same weapon is a backhand from the other side. A two-handed grip follows
// its lead hand; a left lead (grip class 0x30, entry 3) mirrors a right one.
enum class CueSide : int { none = 0, left = 1, right = 2, top = 3, bottom = 4 }; // bottom: thrusts
inline CueSide originSide(OriginCue cue, const SwingHands& s) {
    if (cue == OriginCue::overhead) return CueSide::top;
    if (cue != OriginCue::defaultSide && cue != OriginCue::oppositeSide) return CueSide::none;
    if (s.slot < 0 || s.hand < 0 || s.hand != s.expectedHand || (s.handSide != 1 && s.handSide != 2))
        return CueSide::none;
    const CueSide own = s.handSide == 2 ? CueSide::right : CueSide::left;
    const CueSide other = own == CueSide::right ? CueSide::left : CueSide::right;
    if (cue == OriginCue::defaultSide) return s.slot == 0 ? own : CueSide::none;
    if (s.slot == 1) return s.alternate ? own : CueSide::none;
    return s.alternate ? CueSide::none : other;
}
// originSide already resolves the accepted motion's physical hand and whether
// this is a forehand or a backhand. Its screen side does not depend on the sign
// of the game's heading convention. Large roll makes a lateral cue ambiguous.
inline CueSide cueSide(CueSide origin, float cameraRightY) {
    if (!std::isfinite(cameraRightY) || std::fabs(cameraRightY) > 0.5f) return CueSide::none;
    return origin;
}
// Pick clearance. Picks unproject from the view near plane, 1
// unit ahead of the eye. With the eye pressed into a wall that point is past
// the wall's face, and the native occlusion test along the pick ray (level
// query 0x1430F0 from the ray origin, as in 0x117AA0) starts behind it. The
// native line of sight (0x246610 -> 0x2462F0) starts at the head; checking
// head -> pick origin with the same level queries closes the gap.
// pickOrigin: the near-plane point under client pixel (px,py) for a camera
// pose (rows right, up, back; position at 9..11), as 0x8F460 unprojects it.
inline bool pickOrigin(const float* pose, float px, float py, float width, float height, float scale,
                       float nearPlane, float* out) {
    if (!(width > 0) || !(height > 0) || !(scale > 0) || !(nearPlane > 0)) return false;
    const float x = (px - width * .5f) / scale, y = (height * .5f - py) / scale;
    for (int i = 0; i < 3; i++)
        out[i] = pose[9 + i] + nearPlane * (x * pose[i] + y * pose[3 + i] - pose[6 + i]);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// Query: true when level geometry crosses origin + dir*[0,length] (unit dir).
// Returns 1 clear, 0 blocked, -1 not determined (non-finite input).
template <class Query> inline int pickClear(const float* head, const float* origin, Query blocked) {
    float d[3], len = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = origin[i] - head[i];
        len += d[i] * d[i];
    }
    if (!std::isfinite(len)) return -1;
    len = std::sqrt(len);
    if (len < 0.01f) return 1;
    for (float& v : d) v /= len;
    return blocked(head, d, len) ? 0 : 1;
}
// Attack wind-up, fed on the behaviour tick from the native swing: motion root+0x9B8 (5-12 accepted),
// progress root+0xA28 (negative during the start dwell; the hit lands past 0.30, or 0.40 for the thrust,
// motion 11) and the bounce/block bits root+0x1018 & 0x26. There is no hold-to-charge in the game, so the
// wind-up is the progress up to the hit. phase: 0 idle, 1 winding, 2 fading.
struct Windup {
    int motion = 0, phase = 0;
    float p0 = 0, lastP = 0, fill = 0;
    unsigned bitsAtStart = 0;
    unsigned long long fadeFrom = 0;
};
inline constexpr unsigned windupFadeMs = 250;
// The cursor the game wants over its own window, as its WM_SETCURSOR handler (0x22F80) picks it: the
// entry `index` of its cursor table (the title, menus, the overhead view), or none (-1) while it hides
// the system cursor and draws its own crosshair. An index outside the table is treated as none.
inline constexpr int gameCursorSlots = 16;
inline int gameCursorSlot(int index) {
    return index >= 0 && index < gameCursorSlots ? index : -1;
}
// Whether the gameplay crosshair is drawn. Out of combat it stays hidden for a clear view unless the
// player wants it always (right mouse brings up the game's own cursor instead); in combat it shows,
// except during a swing if the player hides it then. Picks use the aim point either way.
inline bool crosshairDrawn(bool combat, bool swinging, bool hideDuringSwing, bool alwaysOutOfCombat,
                           bool catchingUp = false, bool unreachableHover = false) {
    if (!combat && unreachableHover) return false;
    if (!combat) return alwaysOutOfCombat || catchingUp;
    return !(hideDuringSwing && swinging);
}
// Flicks. The view is the head, and the head turns toward the aim at the game's own pace, so a fast flick
// puts the aim well ahead of what is seen. The crosshair marks the aim while it leads the head (the aim
// marker, out of combat too), and once the lead passes flickLeadStart, mouse input that widens it is
// scaled down smoothly: a soft cap, never a stop. Input that closes the lead, and all ordinary motion,
// stays 1:1. Angles in radians.
constexpr float flickLeadStart = 20.f * 0.0174532925f, flickLeadFull = 80.f * 0.0174532925f,
                flickMinGain = 0.3f;
inline float flickGain(float lead, float step, int strength) {
    const int s = strength < 0 ? 0 : (strength > 100 ? 100 : strength);
    if (!s || !std::isfinite(lead) || !std::isfinite(step) || lead * step <= 0.f) return 1.f;
    const float a = std::fabs(lead);
    if (a <= flickLeadStart) return 1.f;
    float x = (a - flickLeadStart) / (flickLeadFull - flickLeadStart);
    if (x > 1.f) x = 1.f;
    const float smooth = x * x * (3.f - 2.f * x);
    return 1.f + (flickMinGain - 1.f) * smooth * float(s) / 100.f;
}
// The aim marker shows from aimMarkerShow of lead until it falls back under aimMarkerHide.
constexpr float aimMarkerShow = 4.f * 0.0174532925f, aimMarkerHide = 1.5f * 0.0174532925f;
inline bool aimMarkerStep(bool shown, float lead) {
    return shown ? lead > aimMarkerHide : lead > aimMarkerShow;
}
// Right mouse frees the cursor. mode: 0 off, 1 hold, 2 toggle. When the mod takes a press it takes the
// matching release too, so the game never sees half a click (a stuck walk or UI button). Returns true when
// the message is swallowed; changed when .on flipped. lost: F6, F8, Esc, or the game ending it.
// focusLost: alt-tab, whose release goes to another window, so none is waited for.
struct RightMouseFree {
    bool on = false, swallowUp = false;
};
enum class RmbEvent { down, up, lost, focusLost };
inline bool rmbStep(RightMouseFree& s, int mode, RmbEvent e, bool eligible, bool& changed) {
    changed = false;
    switch (e) {
    case RmbEvent::down:
        if (mode == 2 && s.on) {
            s.on = false;
            s.swallowUp = true;
            changed = true;
            return true;
        }
        if (mode == 1 && s.on) {
            s.swallowUp = true;
            return true;
        } // the last release went to another window
        if (mode != 0 && !s.on && eligible) {
            s.on = true;
            s.swallowUp = true;
            changed = true;
            return true;
        }
        return false;
    case RmbEvent::up:
        if (!s.swallowUp) return false;
        s.swallowUp = false;
        if (mode != 2 && s.on) {
            s.on = false;
            changed = true;
        }
        return true;
    case RmbEvent::lost:
        if (s.on) {
            s.on = false;
            changed = true;
        }
        return false;
    case RmbEvent::focusLost:
        if (s.on) {
            s.on = false;
            changed = true;
        }
        s.swallowUp = false;
        return false;
    }
    return false;
}
inline int rmbModeFromText(const wchar_t* t) {
    if (!t) return 1;
    auto is = [&](const wchar_t* w) {
        const wchar_t* a = t;
        while (*a && *w && ((*a | 32) == (*w | 32))) {
            ++a;
            ++w;
        }
        return !*a && !*w;
    };
    return is(L"off") || is(L"0") ? 0 : (is(L"toggle") ? 2 : 1);
}
inline const wchar_t* rmbModeText(int m) {
    return m == 0 ? L"off" : (m == 2 ? L"toggle" : L"hold");
}
// Crouch in exploration (first and third person): the game's own duck key (its duck bit in the key mask)
// toggles or holds a crouch the game keeps up while walking. mode: 0 off (the game's short duck only),
// 1 hold, 2 toggle. allowed: out of combat, not knocked down, dead, talking or in the overhead view; when
// not allowed the crouch ends. headroom: false while something is just above the head, so standing up waits.
struct Crouch {
    bool on = false, wanted = false, keyWas = false;
};
inline bool crouchStep(Crouch& c, int mode, bool keyDown, bool allowed, bool headroom) {
    const bool pressed = keyDown && !c.keyWas;
    c.keyWas = keyDown;
    if (!allowed || mode == 0) {
        c.on = c.wanted = false;
        return false;
    }
    if (mode == 1)
        c.wanted = keyDown;
    else if (pressed)
        c.wanted = !c.wanted;
    c.on = c.wanted || (c.on && !headroom); // standing up under a low ceiling waits until there is room
    return c.on;
}
inline int crouchModeFromText(const wchar_t* t) {
    if (!t) return 2;
    auto is = [&](const wchar_t* w) {
        const wchar_t* a = t;
        while (*a && *w && ((*a | 32) == (*w | 32))) {
            ++a;
            ++w;
        }
        return !*a && !*w;
    };
    return is(L"off") || is(L"0") ? 0 : (is(L"hold") ? 1 : 2);
}
inline const wchar_t* crouchModeText(int m) {
    return m == 0 ? L"off" : (m == 1 ? L"hold" : L"toggle");
}
inline void windupStep(Windup& w, bool swinging, int motion, float p, unsigned bits, unsigned long long now) {
    if (!swinging || !std::isfinite(p) || std::fabs(p) > 4.f) {
        if (w.phase == 1) {
            w.phase = 2;
            w.fadeFrom = now;
        } // ended or cut short: fade from where it was
        if (w.phase == 2 && now - w.fadeFrom >= windupFadeMs) w.phase = 0;
        w.motion = 0;
        return;
    }
    if (motion != w.motion || p < w.lastP - 1e-4f) {
        w.motion = motion;
        w.p0 = p < 0 ? p : 0;
        w.phase = 1;
        w.fill = 0;
        w.bitsAtStart = bits & 0x26;
    } // a new swing (or a chained one)
    w.lastP = p;
    if (w.phase != 1) return;
    if ((bits & 0x26) & ~w.bitsAtStart) {
        w.phase = 2;
        w.fadeFrom = now;
        return;
    } // bounced or blocked
    const float strikeAt = motion == 11 ? 0.40f : 0.30f;
    if (p >= strikeAt) {
        w.fill = 1;
        w.phase = 2;
        w.fadeFrom = now;
        return;
    }
    const float f = (p - w.p0) / (strikeAt - w.p0);
    w.fill = f < 0 ? 0 : (f > 1 ? 1 : f);
}
inline float windupAlpha(int phase, unsigned long long fadeFrom, unsigned long long now) {
    if (phase == 1) return 1.f;
    if (phase != 2 || now - fadeFrom >= windupFadeMs) return 0.f;
    return 1.f - float(now - fadeFrom) / float(windupFadeMs);
}
// The behaviour tick stops publishing during a pause, a death or a level load; a state that old is
// not shown, so a swing cut off there does not leave its arc on screen.
inline constexpr unsigned windupStaleMs = 250;
inline float windupShown(int phase, unsigned long long fadeFrom, unsigned long long published,
                         unsigned long long now) {
    return now - published > windupStaleMs ? 0.f : windupAlpha(phase, fadeFrom, now);
}
// One band of the arc as a triangle strip of (x, y, alpha) vertices: angles a0..a1 (screen y down),
// alpha easing from alphaMid at the arc's middle to alphaEnd at the band's ends; the inner edge's alpha
// is that times innerScale (1 = a solid line, 0 = a glow fading toward the crosshair). Returns vertices.
inline int arcBand(float cx, float cy, float r, float w, float a0, float a1, float mid, float alphaMid,
                   float alphaEnd, float* out, int capFloats, float innerScale = 1.f) {
    const float pi = 3.14159265f;
    int seg = int(std::lround(32.f * (a1 - a0) / pi));
    if (seg < 2) seg = 2;
    seg += seg & 1; // even: a vertex sits on the middle
    if ((seg + 1) * 6 > capFloats || !(a1 > a0)) return 0;
    const float reach = std::fmax(std::fabs(a1 - mid), std::fabs(a0 - mid));
    for (int i = 0; i <= seg; ++i) {
        const float a = a0 + (a1 - a0) * float(i) / float(seg), c = std::cos(a), s = std::sin(a);
        const float k = reach > 0 ? std::fabs(a - mid) / reach : 0.f,
                    al = alphaMid + (alphaEnd - alphaMid) * k;
        float* v = out + i * 6;
        v[0] = cx + c * (r - w * .5f);
        v[1] = cy + s * (r - w * .5f);
        v[2] = al * innerScale;
        v[3] = cx + c * (r + w * .5f);
        v[4] = cy + s * (r + w * .5f);
        v[5] = al;
    }
    return (seg + 1) * 2;
}
// The combat crosshair is the circle cell of cursors.rfi, drawn unrotated at the cursor size (0x13E9C0,
// 24 px at GUI scale 1). Measured on that cell, as fractions of the drawn size: the ring's radius and
// line, and the soft gradient just inside the ring (its alpha there; it fades toward the centre).
inline constexpr float cursorRingRadius = 0.41f, cursorRingLine = 0.031f, cursorRingGlowAlpha = 0.39f;
inline constexpr int defaultCursorPx = 24;
// The arc sits just outside the ring, as its own thinner sibling: the gap and its inward glow.
inline constexpr float arcGap = 0.125f, arcGlowWidth = 0.125f;
// The wind-up arc around the crosshair (cx,cy), on the swing's origin side (the thrust: below), drawn
// as the ring is: a hairline over a gradient that fades toward the centre. Three strips: the whole
// semicircle as a faint line, the filled part's glow, then the filled part's line, brightest at the
// middle; the fill grows from the middle toward both ends. sizePercent scales the arc, never inside
// the ring. counts[3] are the strips' vertices.
inline bool cueArc(CueSide side, float cx, float cy, float cursorPx, int sizePercent, float fill, float* out,
                   int capFloats, int* counts) {
    counts[0] = counts[1] = counts[2] = 0;
    if (side == CueSide::none || !(cursorPx > 0) || !std::isfinite(fill)) return false;
    const float pi = 3.14159265f,
                size =
                    cursorPx * float(sizePercent < 25 ? 25 : (sizePercent > 300 ? 300 : sizePercent)) / 100.f;
    const float ring = cursorRingRadius * cursorPx,
                r = std::fmax(size * (cursorRingRadius + arcGap), ring + 1.f);
    const float line = std::fmax(1.f, size * cursorRingLine), glow = size * arcGlowWidth;
    const float mid = side == CueSide::left
                          ? pi
                          : (side == CueSide::right ? 0.f : (side == CueSide::top ? -pi / 2 : pi / 2));
    if (fill < 0) fill = 0;
    if (fill > 1) fill = 1;
    int used = 0;
    counts[0] = arcBand(cx, cy, r, line, mid - pi / 2, mid + pi / 2, mid, 0.3f, 0.3f, out, capFloats);
    used += counts[0] * 3;
    if (fill > 0.005f) {
        const float h = fill * pi / 2;
        counts[1] = arcBand(cx, cy, r - (line + glow) * .5f, glow, mid - h, mid + h, mid, cursorRingGlowAlpha,
                            0.25f, out + used, capFloats - used, 0.f);
        used += counts[1] * 3;
        counts[2] = arcBand(cx, cy, r, line, mid - h, mid + h, mid, 1.f, 0.7f, out + used, capFloats - used);
    }
    return counts[0] > 0;
}
// Fault notice wording (loud and concise). Returns an immutable window
// title (a timed-out cross-thread WM_SETTEXT may still read it later); the
// in-game notice shows it without the first faultTitlePrefix characters.
// Diagnostics stay in first-person-status.json.
#define FP_SHOWN_NAME L"ExCamera"                // the name players see in notices
inline constexpr unsigned faultTitlePrefix = 10; // "Exanima - "
inline const wchar_t* faultMessage(bool restorationVerified, bool hooksRemoved, bool restorationNotRequired) {
    if (restorationVerified)
        return hooksRemoved
                   ? L"Exanima - " FP_SHOWN_NAME L" disabled: camera source lost. Overhead restored."
                   : L"Exanima - " FP_SHOWN_NAME L" stopped: hook removal unverified. Restart Exanima.";
    return restorationNotRequired
               ? L"Exanima "
                 L"- " FP_SHOWN_NAME L" could not activate. Overhead unchanged; restart before retrying."
               : L"Exanima - " FP_SHOWN_NAME L" stopped: camera restoration unverified. Restart Exanima.";
}
// Camera owner change. A camera swap is game flow,
// not a fault: leaving gameplay (death-screen EXIT, ESC EXIT to title) and,
// since, a swap during live play with no menu open (arena round end,
// knockout) all park first person; it re-arms like a fresh F6.
// parkRestore: our camera is still the current one and still carries our
// values (fov written, near 1): restore stock, then park.
// parkClean: our camera is current but was rewritten: nothing of ours left.
// parkRelease: our camera is no longer current. It is never written while not
// current; the retired record restores it if it is ever current
// again with our values. Only a stop (F7) while such a record of
// the current gameplay is unresolved reports restoration unknown.
enum class OwnerChange { parkRestore = 1, parkClean = 2, parkRelease = 3 };
inline OwnerChange classifyOwnerChange(bool savedCameraCurrent, bool ourValuesPresent) {
    if (savedCameraCurrent) return ourValuesPresent ? OwnerChange::parkRestore : OwnerChange::parkClean;
    return OwnerChange::parkRelease;
}
// While parked, the render thread asks each frame whether first person may
// re-attach (as a fresh F6 would). Identity change is latched by the caller.
// Same gameplay, no UI: re-arm only after the grace frames, and not before a
// back-off ends (backoff: repeated live parks, see recordLivePark).
enum class ParkStep { wait, rearm };
inline constexpr unsigned parkGraceFrames = 30;
inline ParkStep parkStep(bool left, bool rootPresent, bool uiOpen, bool backoff, unsigned& stableFrames) {
    if (!rootPresent || uiOpen) {
        stableFrames = 0;
        return ParkStep::wait;
    }
    if (left) {
        stableFrames = 0;
        return ParkStep::rearm;
    }
    // Same gameplay: wait out the grace frames so a flapping owner cannot
    // toggle the view every frame; a back-off holds the park longer still.
    if (backoff) {
        stableFrames = 0;
        return ParkStep::wait;
    }
    if (++stableFrames < parkGraceFrames) return ParkStep::wait;
    stableFrames = 0;
    return ParkStep::rearm;
}
// Repeated live parks in one gameplay (flapping, a head that keeps
// failing): the liveParkLimit-th live park within liveParkWindowMs starts a
// back-off of liveParkWindowMs before the next re-arm. Stock is already
// restored at every park, so this never stops the mod.
inline constexpr unsigned liveParkLimit = 3;
inline constexpr uint64_t liveParkWindowMs = 10000;
struct LiveParks {
    uint64_t at[liveParkLimit]{};
    unsigned count = 0;
};
inline bool recordLivePark(LiveParks& p, uint64_t now) {
    for (unsigned i = liveParkLimit - 1; i > 0; --i) p.at[i] = p.at[i - 1];
    p.at[0] = now;
    if (p.count < liveParkLimit) ++p.count;
    if (p.count < liveParkLimit || now - p.at[liveParkLimit - 1] >= liveParkWindowMs) return false;
    p = LiveParks{};
    return true; // A fresh window after the back-off.
}
// Bad head frame: a non-finite or degenerate head matrix, or a
// head/mesh link that does not read back, during live play with no menu open.
// The view holds the last good first-person pose (input stays) for up to
// headHoldFrames frames; one more bad frame parks with stock restored, and the
// park re-arms once the head reads sane. Never a hard stop.
inline constexpr unsigned headHoldFrames = 30;
struct HeadHold {
    unsigned run = 0, holds = 0, heldFrames = 0, maxHold = 0, parks = 0;
};
enum class HoldStep { hold, park };
inline HoldStep holdStep(HeadHold& h) {
    if (h.run >= headHoldFrames) {
        h.run = 0;
        ++h.parks;
        return HoldStep::park;
    }
    if (!h.run++) ++h.holds;
    ++h.heldFrames;
    if (h.run > h.maxHold) h.maxHold = h.run;
    return HoldStep::hold;
}
inline void holdEnd(HeadHold& h) {
    h.run = 0;
}
// F7: stops the mod only when held for stopHoldMs. A shorter
// press is ignored with a short notice. press/release are tick times of the
// key's first down and its up (0 none); release < press means still held.
// Focus loss while held cancels (the up may never reach the game window).
inline constexpr uint64_t stopHoldMs = 1000;
enum class StopKey { idle, holding, stop, tap, cancel };
inline StopKey stopKeyStep(uint64_t press, uint64_t release, uint64_t now, bool focused) {
    if (!press) return StopKey::idle;
    if (release >= press) return release - press >= stopHoldMs ? StopKey::stop : StopKey::tap;
    if (!focused) return StopKey::cancel;
    return now - press >= stopHoldMs ? StopKey::stop : StopKey::holding;
}
// After an F7 stop, F6 turns the mod back on only when the stop was clean:
// camera restoration verified, hooks and window procedure removed. Any other
// stop keeps the restart requirement.
inline bool rearmAllowed(bool restoredVerified, bool hooksRemoved, bool restorationUnknown) {
    return restoredVerified && hooksRemoved && !restorationUnknown;
}
// Draw hiding. The per-sector update 0x58CD0 rebuilds each
// registered mesh's pass mask (+0xE0) every frame: 0x10000000 from the sector
// (+0x3A0), | 0x1000001 when the mesh is in view. Render passes and the screen
// pick draw/pick a mesh only when (pass mask & +0xE0) != 0. A hidden mesh gets 0:
// the value of a mesh outside the view in a sector without +0x3A0. (Keeping the
// sector bit 0x10000000 is not safe: some pass masks may include it.)
inline unsigned hiddenPassMask(unsigned) {
    return 0;
}
// Sorted pointer sets for the hide list (sorted in place, duplicates removed).
inline unsigned sortUnique(uintptr_t* v, unsigned n) {
    for (unsigned i = 1; i < n; ++i) {
        const uintptr_t x = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = x;
    }
    unsigned k = 0;
    for (unsigned i = 0; i < n; ++i)
        if (!k || v[k - 1] != v[i]) v[k++] = v[i];
    return k;
}
inline bool sortedContains(const uintptr_t* v, unsigned n, uintptr_t x) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && v[lo] == x;
}
// Worn helmet (visible worn
// helmet in first person as long as it does not block the camera).
// [View] Helmet=show (default) or hide. Anything but "hide" means show.
enum class HelmetMode { show, hide };
inline HelmetMode helmetModeFromText(const wchar_t* text) {
    if (!text) return HelmetMode::show;
    const wchar_t hide[] = L"hide";
    unsigned i = 0;
    while (text[i] == L' ' || text[i] == L'\t') ++i;
    for (unsigned k = 0; k < 4; ++k, ++i) {
        wchar_t c = text[i];
        if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
        if (c != hide[k]) return HelmetMode::show;
    }
    while (text[i] == L' ' || text[i] == L'\t') ++i;
    return text[i] ? HelmetMode::show : HelmetMode::hide;
}
// A worn mesh blocks the view when a ray from the eye along the view axis, or
// one of four rays helmetConeDegrees around it, hits it within
// helmetProbeLength units (the head is ~25 units across; the eye sits inside it).
enum class HelmetVerdict { clear = 0, blocks = 1, unknown = -1 };
inline constexpr float helmetConeDegrees = 4.f, helmetProbeLength = 40.f;
// pose: camera basis rows (0 right, 1 up, 2 back; forward = -row 2), as fp::basis.
inline void helmetProbeDirections(const float* pose, float out[5][3]) {
    const float t = std::tan(helmetConeDegrees * 0.0174532925f);
    const float offsets[5][2] = {{0, 0}, {t, 0}, {-t, 0}, {0, t}, {0, -t}};
    for (int k = 0; k < 5; ++k) {
        float n = 0;
        for (int i = 0; i < 3; ++i) {
            out[k][i] = -pose[6 + i] + pose[i] * offsets[k][0] + pose[3 + i] * offsets[k][1];
            n += out[k][i] * out[k][i];
        }
        n = std::sqrt(n);
        for (int i = 0; i < 3; ++i) out[k][i] /= n;
    }
}
// Show mode hides only a mesh that blocks the view or cannot be tested.
inline bool hideHeadWear(HelmetMode mode, HelmetVerdict verdict) {
    return mode == HelmetMode::hide || verdict != HelmetVerdict::clear;
}
// Body wear in view (looking down, no eye movement; hide the body pieces
// that block the view, only while they block it, so legs and feet still show). The character's
// part nodes are numbered (dump: 08Belly 09Chest 10Head 11RShld 12RUArm 14LShld 15LUArm
// 17RHand ...; the skin is _SKIN beside them); worn gear hangs under its part. Only gear under the
// torso, shoulders and upper arms is a candidate, and only when the same eye-ray test as the helmet
// hits it within bodyProbeLength: legs and feet are far below that, held items are under the hands.
inline constexpr float bodyProbeLength = 45.f;
// Head stabilisation (stabilisation lives on the head, never on
// the camera). The neck is a position-based joint motor (0x1760F0 builds the head target, 0x15DA80
// solves); its "slack" (head part +0x778) pulls the target back toward where the head already is,
// rising 0.015 a tick toward a cap (+0x77C, 1.0 by default) while the gaze holds still and dropping
// to 0 the moment it moves. That slack is what lets the head
// wobble while you walk and look steady. Stabilisation lowers the cap, for the player in first person,
// only around the game's own build call: strength 0 = the game's value, 100 = 20 % of it.
// roll comfort,
// on the head, never the camera. The head's roll is the angle about its forward axis
// between its up and the level up (world up made square to forward); 0 when it looks nearly
// straight up or down (no level there). Within 5 degrees roll is natural; beyond it the slack cap is
// lowered further, rising softly (smoothstep) to a quarter at 20 degrees, so the neck pulls the head
// back toward its level target harder the more it rolls, while hits and falls can still roll it.
inline float headRollDegrees(const float* head) { // rows: +3 up, +6 forward (the head mesh pose)
    float f[3] = {head[6], head[7], head[8]}, u[3] = {head[3], head[4], head[5]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(fl > 1e-6f) || !std::isfinite(fl)) return 0.f;
    for (float& v : f) v /= fl;
    if (std::fabs(f[1]) > 0.94f) return 0.f; // within ~20 degrees of straight up/down
    float level[3] = {-f[1] * f[0], 1.f - f[1] * f[1], -f[1] * f[2]}; // world up minus its forward part
    const float ll = std::sqrt(level[0] * level[0] + level[1] * level[1] + level[2] * level[2]);
    for (float& v : level) v /= ll;
    const float d = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
    for (int i = 0; i < 3; ++i) u[i] -= d * f[i]; // the head's up, square to forward
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul > 1e-6f)) return 0.f;
    for (float& v : u) v /= ul;
    const float c = u[0] * level[0] + u[1] * level[1] + u[2] * level[2];
    const float r = std::acos(c < -1.f ? -1.f : (c > 1.f ? 1.f : c)) * 57.2957795f;
    return std::isfinite(r) ? r : 0.f;
}
constexpr float rollComfortDegrees = 5.f, rollFullDegrees = 20.f, rollCapAtFull = 0.25f;
inline float rollSlackScale(float rollDegrees) {
    if (!(rollDegrees > rollComfortDegrees)) return 1.f;
    float x = (rollDegrees - rollComfortDegrees) / (rollFullDegrees - rollComfortDegrees);
    if (x > 1.f) x = 1.f;
    const float s = x * x * (3.f - 2.f * x);
    return 1.f + (rollCapAtFull - 1.f) * s;
}
// Per-axis head steadiness, on top of the master strength and the roll comfort. Each physics tick the
// head's own turn on an axis, minus the turn its gaze target was given (the mouse), is wobble: torso sway,
// steps, hits. Wobble above a small threshold on an axis the player wants steady stiffens the neck drive
// for that tick. The drive has one slack cap, so the axes are not moved separately; each axis only sets
// how much its own wobble stiffens it, and 0 leaves that axis as before. Mouse-commanded turns are not
// wobble and never stiffen or slow anything.
struct HeadAngles {
    float roll = 0, pitch = 0, yaw = 0;
};
inline HeadAngles headAngles(const float* head) { // degrees; rows: +3 up, +6 forward
    HeadAngles a;
    const float* f = head + 6;
    const float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    a.yaw = std::atan2(f[0], f[2]) * 57.2957795f;
    a.pitch = std::atan2(f[1], h) * 57.2957795f;
    a.roll = headRollDegrees(head);
    return a;
}
inline HeadAngles gazeAngles(const float* g) {
    HeadAngles a;
    const float h = std::sqrt(g[0] * g[0] + g[2] * g[2]);
    a.yaw = std::atan2(g[0], g[2]) * 57.2957795f;
    a.pitch = std::atan2(g[1], h) * 57.2957795f;
    return a;
}
inline float angleDelta(float now, float before) {
    float d = now - before;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}
constexpr float wobbleComfortDegrees = 0.1f, wobbleFullDegrees = 1.5f,
                axisCapAtFull = 0.15f; // per physics tick
inline float axisSlackScale(float wobbleDegrees, int strength) {
    const int s = strength < 0 ? 0 : (strength > 100 ? 100 : strength);
    const float w = std::fabs(wobbleDegrees);
    if (!s || !(w > wobbleComfortDegrees) || !std::isfinite(w)) return 1.f;
    float x = (w - wobbleComfortDegrees) / (wobbleFullDegrees - wobbleComfortDegrees);
    if (x > 1.f) x = 1.f;
    const float smooth = x * x * (3.f - 2.f * x);
    return 1.f + (axisCapAtFull - 1.f) * smooth * float(s) / 100.f;
}
struct HeadWobble {
    bool primed = false;
    HeadAngles head, gaze;
};
// The slack scale for this tick from the head's and the gaze's angles now and last tick.
inline float wobbleSlackScale(HeadWobble& w, const HeadAngles& head, const HeadAngles& gaze, int roll,
                              int pitch, int yaw) {
    float scale = 1.f;
    if (w.primed && std::isfinite(head.yaw) && std::isfinite(head.pitch) && std::isfinite(gaze.yaw) &&
        std::isfinite(gaze.pitch)) {
        const float dYaw = angleDelta(head.yaw, w.head.yaw) - angleDelta(gaze.yaw, w.gaze.yaw);
        const float dPitch = angleDelta(head.pitch, w.head.pitch) - angleDelta(gaze.pitch, w.gaze.pitch);
        const float dRoll = head.roll - w.head.roll; // the gaze has no roll
        // looking almost straight up or down the yaw angle is meaningless: it is left out there
        scale = axisSlackScale(dRoll, roll) * axisSlackScale(dPitch, pitch) *
                (std::fabs(head.pitch) < 75.f ? axisSlackScale(dYaw, yaw) : 1.f);
    }
    w.primed = true;
    w.head = head;
    w.gaze = gaze;
    return scale;
}
inline float steadySlackCap(float gameCap, int strength) {
    if (!(gameCap > 0.f) || !std::isfinite(gameCap)) return gameCap;
    const int s = strength < 0 ? 0 : (strength > 100 ? 100 : strength);
    return gameCap * (1.f - 0.8f * float(s) / 100.f);
}
inline int bodyPartNumber(const char* name) {
    if (!name || name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || !name[2]) return -1;
    return (name[0] - '0') * 10 + (name[1] - '0');
}
// The view turned enough since the head wear was last tested to test it again now (8 degrees). A zero
// "tested" (never) is due.
inline constexpr float helmetRetestCos = 0.99027f;
inline bool helmetRetestDue(const float* tested, const float* forward) {
    return tested[0] * forward[0] + tested[1] * forward[1] + tested[2] * forward[2] < helmetRetestCos;
}
inline bool torsoPart(int part) {
    return part == 8 || part == 9 || part == 11 || part == 12 || part == 14 || part == 15;
}
// Mesh-local frame of a node matrix (rows 0-2 axes, row 3 translation), as the
// game's 0x18960 (points) and 0x18B00 (directions): local = R (p - t).
inline void worldToLocalPoint(const float* m, const float* p, float* out) {
    const float d[3] = {p[0] - m[9], p[1] - m[10], p[2] - m[11]};
    for (int r = 0; r < 3; ++r) out[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
}
inline void worldToLocalDirection(const float* m, const float* d, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
}
// The native analog travel vector (0x5B29F0,
// a controller stick) is left alone when no movement key is held and the stick
// is outside a small deadzone, instead of being zeroed by keyboard travel.
inline bool analogActive(const float* v) {
    float m = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i])) return false;
        m += v[i] * v[i];
    }
    return m > 0.2f * 0.2f;
}
// Mouse buttons as the held attack-side key would flip every attack.
inline bool attackSideKeyValid(int key) {
    return key >= 3 && key <= 254 && key != 4 && key != 5 && key != 6;
}
// Another program hooking the game's SwapBuffers over ours
// starves the swap hook (park, re-arm, restore all run there). Starved: camera
// samples keep rising while swap calls stay flat for starvedSeconds checks in a row.
struct SwapWatch {
    unsigned samples = 0, swaps = 0, flat = 0;
    bool starved = false;
};
inline constexpr unsigned starvedSeconds = 3;
inline bool swapWatchStep(SwapWatch& w, unsigned samples, unsigned swaps, bool active) {
    const bool flat = active && samples >= w.samples + 20 && swaps == w.swaps;
    w.flat = flat ? w.flat + 1 : 0;
    w.samples = samples;
    w.swaps = swaps;
    if (w.flat >= starvedSeconds && !w.starved) {
        w.starved = true;
        return true;
    } // report once
    return false;
}
// Held objects and the cursor hand-off.
// Held objects: the native drag puts the object at the pointer ray's first hit
// (0x8F720 inside 0x1183C0). While first person holds one, the hit is moved to a hold distance
// along the ray: the wheel scales it 12% a notch within [holdNear, holdFar]; it eases toward the
// target; it never passes the real hit, a level wall, or the reach sphere around the player.
// Grab range: first person refuses a grab whose picked point is farther than this from the eye, and the
// wheel never holds farther. It is the player's setting (F8), within the game's own placement limit for a
// held object (0x1183C0: 300 from the body, 0x3127D0 = 300^2); hinged things allow 350 but use the same.
constexpr int grabRangeMin = 100, grabRangeMax = 300, grabRangeDefault = 220;
constexpr float holdNear = 60.f;
inline int clampGrabRange(int cm) {
    return cm < grabRangeMin ? grabRangeMin : (cm > grabRangeMax ? grabRangeMax : cm);
}
inline bool grabTooFar(float eyeToHit, float range) {
    return !(eyeToHit <= range);
}
// The sphere round the body a held object never leaves: a little past the range, never past the game's limit.
inline float holdSphere(float range) {
    return std::fmin(range + 40.f, float(grabRangeMax));
}
inline float holdTargetStep(float target, int wheel, float lo, float hi) {
    float t = target > 0 ? target : lo;
    t *= std::pow(1.12f, float(wheel) / 120.f);
    return std::fmin(std::fmax(t, lo), hi);
}
inline float holdApproach(float current, float target, float dt) {
    if (!(current > 0)) return target;
    const float k = 1.f - std::exp(-12.f * std::fmax(dt, 0.f));
    return current + (target - current) * k;
}
// Largest t >= 0 with |origin + dir*t - centre| <= radius (dir unit); -1 when the ray never
// enters the sphere ahead of the origin.
inline float rayInSphere(const float* origin, const float* dir, const float* centre, float radius) {
    float m[3];
    for (int i = 0; i < 3; i++) m[i] = origin[i] - centre[i];
    const float b = m[0] * dir[0] + m[1] * dir[1] + m[2] * dir[2],
                c = m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - radius * radius;
    const float disc = b * b - c;
    if (disc < 0) return -1;
    const float t = -b + std::sqrt(disc);
    return t >= 0 ? t : -1;
}
// A door or lid is held at the grab's distance from the eye, in the direction the pointer's ray takes
// within the plane the game uses for that drag (plane: n.x = d, as {nx, ny, nz, d}): looking up or down
// turns the point about the eye in that plane without pulling it in. False when the ray runs (almost)
// along the plane's normal or anything is unusable.
inline bool heldPointOnPlane(const float* origin, const float* dir, float distance, const float* plane,
                             float* out) {
    const float nn = plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2];
    if (!(nn > 0.5f) || !(distance > 0) || !std::isfinite(nn)) return false;
    const float dn = (dir[0] * plane[0] + dir[1] * plane[1] + dir[2] * plane[2]) / nn,
                on = (origin[0] * plane[0] + origin[1] * plane[1] + origin[2] * plane[2] - plane[3]) / nn;
    float along[3];
    for (int i = 0; i < 3; i++) along[i] = dir[i] - plane[i] * dn;
    const float len = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
    if (!(len > 0.05f) || !std::isfinite(len)) return false;
    for (int i = 0; i < 3; i++) {
        out[i] = origin[i] - plane[i] * on + along[i] / len * distance;
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}
// the F6 press that re-armed the mod (polled) may still
// arrive as WM_KEYDOWN after the window procedure is back; ignore F6 that soon.
inline constexpr uint64_t rearmF6GuardMs = 250;
inline bool rearmF6Accepted(uint64_t now, uint64_t rearmedAt) {
    return !rearmedAt || now - rearmedAt >= rearmF6GuardMs;
}

} // namespace fp
