#pragma once
// The balance shadow's maths (docs/BALANCE-SHADOW.md): pure, no game or Windows access, unit-tested in
// tests/unit/balance_shadow_tests.cpp. It measures a fighter the way a real body is judged (centre of mass,
// where it is heading, the area under the loaded feet) and turns each physics step into problem events, so a
// later balance model can be compared with the game's own one on the same counts. It never decides anything
// for the game.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fp::balance {

// The game's per-step constants are built from 83 steps a second (0x2AE740, read by 0x7EB00) and gravity
// -980.665 units/s^2 (world block 0x2AE690), so one unit is one centimetre.
inline constexpr float stepsPerSecond = 83.f;
inline constexpr float gravity = 980.665f; // cm/s^2

struct V3 { float x = 0, y = 0, z = 0; };
struct V2 { float x = 0, z = 0; };

inline bool finite(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// A convex outline on the ground (x, z), at most `capacity` corners, counter-clockwise. Collinear and repeated
// points are dropped; one point or a segment are kept as they are.
template <unsigned capacity> struct Outline {
    V2 corner[capacity]{};
    unsigned count = 0;
};

inline float cross(V2 o, V2 a, V2 b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); }

// Andrew's monotone chain. `points` is reordered.
template <unsigned capacity> Outline<capacity> outlineOf(V2* points, unsigned n) {
    Outline<capacity> out;
    if (!n) return out;
    std::sort(points, points + n, [](V2 a, V2 b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
    V2 hull[2 * capacity + 2];
    unsigned k = 0;
    for (unsigned i = 0; i < n && k < 2 * capacity; ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    for (int i = int(n) - 2, lower = int(k) + 1; i >= 0 && k < 2 * capacity; --i) {
        while (int(k) >= lower && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    if (k > 1) --k; // the last point repeats the first
    if (k == 1 || (k == 2 && hull[0].x == hull[1].x && hull[0].z == hull[1].z)) k = 1;
    for (unsigned i = 0; i < k && i < capacity; ++i) out.corner[out.count++] = hull[i];
    return out;
}

inline float segmentDistance(V2 p, V2 a, V2 b) {
    const float dx = b.x - a.x, dz = b.z - a.z, length2 = dx * dx + dz * dz;
    float t = length2 > 0 ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / length2 : 0;
    t = std::clamp(t, 0.f, 1.f);
    const float ex = p.x - (a.x + t * dx), ez = p.z - (a.z + t * dz);
    return std::sqrt(ex * ex + ez * ez);
}

// Signed distance from p to the outline: positive inside (distance to the nearest edge), negative outside.
// A point or a segment has no inside, so the result is never positive for those.
template <unsigned capacity> float signedDistance(const Outline<capacity>& o, V2 p) {
    if (!o.count) return -INFINITY;
    if (o.count == 1) return -segmentDistance(p, o.corner[0], o.corner[0]);
    float nearest = INFINITY;
    bool inside = o.count >= 3;
    for (unsigned i = 0; i < o.count; ++i) {
        const V2 a = o.corner[i], b = o.corner[(i + 1) % o.count];
        nearest = (std::min)(nearest, segmentDistance(p, a, b));
        if (cross(a, b, p) < 0) inside = false;
    }
    return inside ? nearest : -nearest;
}

// One fighter at one physics step, as read from the game. Positions in game units (cm), sector-local.
struct Sample {
    const V3* now = nullptr;       // every body-part node, this step (bodyPart::points)
    const V3* before = nullptr;    // the same nodes one step earlier (bodyPart::history)
    const V3* before2 = nullptr;   // two steps earlier (bodyPart::history2)
    unsigned nodes = 0;
    const V2* footprint = nullptr; // ground points of the loaded feet (foot nodes and support contacts)
    unsigned footprintCount = 0;
    float ground = 0;              // ground height under the loaded feet
    bool grounded = false;         // any footprint point at all
};

struct Measure {
    bool valid = false;     // false: no nodes, or a node was not finite
    bool nonFinite = false; // some node position is NaN or infinite (a game-state problem, not ours)
    V3 com{};               // the mean of all nodes: the game's integrator gives every node the same mass (0x89000)
    V3 velocity{};          // cm/s
    float height = 0;       // centre of mass above the ground under the feet, cm
    float omega = 0;        // sqrt(g / height), 1/s
    V2 xcom{};              // extrapolated centre of mass (capture point) on the ground
    float margin = -INFINITY; // cm from the capture point to the edge of the base of support, + inside
    float shake = 0;        // mean |node acceleration| relative to the centre of mass, cm/s^2
    float fastestNode = 0;  // cm/s
};

inline Measure measure(const Sample& s) {
    Measure m;
    if (!s.now || !s.before || !s.before2 || !s.nodes) return m;
    V3 c{}, c1{}, c2{};
    for (unsigned i = 0; i < s.nodes; ++i) {
        if (!finite(s.now[i]) || !finite(s.before[i]) || !finite(s.before2[i])) {
            m.nonFinite = true;
            return m;
        }
        c.x += s.now[i].x, c.y += s.now[i].y, c.z += s.now[i].z;
        c1.x += s.before[i].x, c1.y += s.before[i].y, c1.z += s.before[i].z;
        c2.x += s.before2[i].x, c2.y += s.before2[i].y, c2.z += s.before2[i].z;
    }
    const float n = float(s.nodes), f = stepsPerSecond;
    m.com = {c.x / n, c.y / n, c.z / n};
    const V3 com1{c1.x / n, c1.y / n, c1.z / n}, com2{c2.x / n, c2.y / n, c2.z / n};
    m.velocity = {(m.com.x - com1.x) * f, (m.com.y - com1.y) * f, (m.com.z - com1.z) * f};
    const V3 comAcc{m.com.x - 2 * com1.x + com2.x, m.com.y - 2 * com1.y + com2.y, m.com.z - 2 * com1.z + com2.z};
    float shake = 0, fastest = 0;
    for (unsigned i = 0; i < s.nodes; ++i) {
        const V3 a{s.now[i].x - 2 * s.before[i].x + s.before2[i].x - comAcc.x,
                   s.now[i].y - 2 * s.before[i].y + s.before2[i].y - comAcc.y,
                   s.now[i].z - 2 * s.before[i].z + s.before2[i].z - comAcc.z};
        shake += std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        const V3 v{s.now[i].x - s.before[i].x, s.now[i].y - s.before[i].y, s.now[i].z - s.before[i].z};
        fastest = (std::max)(fastest, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    m.shake = shake / n * f * f;
    m.fastestNode = fastest * f;
    m.valid = true;
    if (!s.grounded) return m;
    m.height = m.com.y - s.ground;
    if (m.height < 1) return m;
    m.omega = std::sqrt(gravity / m.height);
    m.xcom = {m.com.x + m.velocity.x / m.omega, m.com.z + m.velocity.z / m.omega};
    V2 points[64];
    const unsigned count = (std::min)(s.footprintCount, 64u);
    std::copy(s.footprint, s.footprint + count, points);
    const auto outline = outlineOf<32>(points, count);
    m.margin = signedDistance(outline, m.xcom);
    return m;
}

// What the game says about the fighter this step (all read-only game fields).
struct GameState {
    unsigned state = 0;     // character::stateFlags
    int motion = -1;        // character::motion
    bool footDown = false;  // character::footDown
    float uprightness = 1;  // character::uprightness
    bool stepping = false;  // character::stepping
};

// Problem kinds, in report order. Names are what the logs and messages print.
enum class Event : uint8_t {
    nonFinite,      // a node position is NaN or infinite
    flung,          // a node moved faster than flungSpeed
    floating,       // standing, no foot down, not dropping, for floatingSteps
    heldUp,         // standing, capture point far outside the feet, no step, no fall, for heldUpSteps
    shaking,        // standing nearly still but the body shakes over shakeLimit for shakeSteps
    stuckDown,      // on the ground or getting up for longer than stuckSteps, alive
    failedGetUp,    // getting up (0x300) went back to lying (0x200)
    fallPhysical,   // the game dropped the fighter (0x200) and the capture point had left the feet: a real fall
    fallByRule,     // the game dropped the fighter while the capture point was inside the feet: a rule, not physics
    recovered,      // the capture point left the feet and came back for recoverSteps without a fall: it caught itself
    count
};
inline constexpr const char* eventNames[size_t(Event::count)] = {
    "non_finite", "flung", "floating", "held_up", "shaking", "stuck_down", "failed_get_up", "fall_physical",
    "fall_by_rule", "recovered"};
// Problems are flagged on screen; real falls and recoveries are counted only. Resilience is recoveries against
// real falls; a fall by rule is a fault of the balance system, whatever the fall rate (docs/BALANCE-SHADOW.md).
inline constexpr bool eventIsProblem[size_t(Event::count)] = {true, true, true, false, true, true, true, false, true, false};

// Starting limits. They are measured, not known: the logs of stock play give the real spread, and these are
// adjusted from them (docs/BALANCE-SHADOW.md, "Tuning").
inline constexpr float flungSpeed = 3000.f;     // cm/s: 30 m/s
inline constexpr float notDropping = -50.f;     // cm/s: vertical speed above this is not a fall
inline constexpr float heldUpMargin = -15.f;    // cm outside the base of support
inline constexpr float shakeLimit = 4000.f;     // cm/s^2 mean relative node acceleration
inline constexpr float stillSpeed = 30.f;       // cm/s: "nearly still" for the shake test
inline constexpr unsigned floatingSteps = 42;   // 0.5 s
inline constexpr unsigned heldUpSteps = 42;     // 0.5 s
inline constexpr unsigned shakeSteps = 21;      // 0.25 s
inline constexpr unsigned stuckSteps = 83 * 15; // 15 s
inline constexpr unsigned predictWindow = 25;   // 0.3 s: a fall is physical if the capture point left the feet within it
inline constexpr unsigned recoverSteps = 17;    // 0.2 s back inside the feet ends a stumble as a recovery

inline bool dead(unsigned state) { return (state & 0x800) != 0; }   // the game's own death test (CV section 1)
inline bool down(unsigned state) { return (state & 0xA00) != 0; }   // on the ground, getting up, scripted down, dead
inline constexpr int jumpMotion = 18;                              // the game's "Jump" clip
inline constexpr int kickMotion = 13;                              // the game's "Kick" clip
inline constexpr uint64_t kickTailMs = 1500;                       // the kick window runs on this long after it

// The player's kick window: while motion 13 plays and for kickTailMs after its last step (the kick lean log and
// the cause=kick column on the shadow's events). It only labels: no event is counted differently inside it.
struct KickWindow {
    bool active = false;
    uint64_t until = 0;
    bool feed(int motion, uint64_t nowMs) {
        if (motion == kickMotion) active = true, until = nowMs + kickTailMs;
        else if (active && nowMs >= until) active = false;
        return active;
    }
};

// One fighter's running state. feed() returns a bit mask of the events that started this step; a condition
// that lasts reports once, when it reaches its duration, and again only after it has cleared.
struct Tracker {
    unsigned floating = 0, heldUp = 0, shaking = 0, downFor = 0;
    unsigned sinceOutside = ~0u; // steps since the capture point was last outside the feet
    unsigned stumble = 0, backInside = 0; // steps of the current stumble (capture point outside), steps back inside
    float deepest = 0;           // the stumble's most negative margin
    unsigned lastStumbleSteps = 0;
    float lastStumbleDepth = 0;  // of the stumble that just ended in a recovery, for the logs
    unsigned lastState = 0;
    bool seen = false, stuckReported = false;

    static constexpr uint32_t bit(Event e) { return 1u << unsigned(e); }

    uint32_t feed(const GameState& g, const Measure& m) {
        uint32_t events = 0;
        if (m.nonFinite) events |= bit(Event::nonFinite);
        if (m.valid && m.fastestNode > flungSpeed) events |= bit(Event::flung);
        const bool standing = !down(g.state);
        const bool alive = !dead(g.state);
        if (m.valid && std::isfinite(m.margin) && m.margin < 0) sinceOutside = 0;
        else if (sinceOutside != ~0u) ++sinceOutside;

        // Floating: upright, nothing under the feet, and not falling. A jump is excluded.
        if (m.valid && standing && !g.footDown && g.motion != jumpMotion && m.velocity.y > notDropping) {
            if (++floating == floatingSteps) events |= bit(Event::floating);
        } else floating = 0;

        // Held up: the body is past its feet, the game neither steps nor drops it (the "string").
        if (m.valid && standing && g.footDown && !g.stepping && std::isfinite(m.margin) && m.margin < heldUpMargin) {
            if (++heldUp == heldUpSteps) events |= bit(Event::heldUp);
        } else heldUp = 0;

        // Shaking: nearly still but the parts jitter against each other.
        const float speed = std::sqrt(m.velocity.x * m.velocity.x + m.velocity.y * m.velocity.y + m.velocity.z * m.velocity.z);
        if (m.valid && standing && speed < stillSpeed && m.shake > shakeLimit) {
            if (++shaking == shakeSteps) events |= bit(Event::shaking);
        } else shaking = 0;

        // Stumbles: upright on its feet, the capture point leaves the feet; back inside for recoverSteps is a
        // recovery. A fall ends a stumble without one.
        if (m.valid && standing && g.footDown && std::isfinite(m.margin)) {
            if (m.margin < 0) {
                ++stumble, backInside = 0;
                deepest = (std::min)(deepest, m.margin);
            } else if (stumble && ++backInside >= recoverSteps) {
                events |= bit(Event::recovered);
                lastStumbleSteps = stumble, lastStumbleDepth = deepest;
                stumble = backInside = 0, deepest = 0;
            }
        } else if (!standing) stumble = backInside = 0, deepest = 0;

        // Down too long, alive.
        if (down(g.state) && alive) {
            if (++downFor >= stuckSteps && !stuckReported) {
                events |= bit(Event::stuckDown);
                stuckReported = true;
            }
        } else downFor = 0, stuckReported = false;

        if (seen) {
            if ((lastState & 0x300) == 0x300 && g.state == 0x200) events |= bit(Event::failedGetUp);
            if (!down(lastState) && (g.state & 0x200) && alive)
                events |= bit(sinceOutside <= predictWindow ? Event::fallPhysical : Event::fallByRule);
        }
        lastState = g.state;
        seen = true;
        return events;
    }
};

// A running spread of one value: count, mean, peak, and a coarse histogram for percentiles.
struct Spread {
    static constexpr unsigned bins = 32;
    float low = 0, high = 1; // bins cover [low, high) linearly; the last bin also holds everything above
    uint64_t count = 0;
    double sum = 0;
    float peak = -INFINITY;
    uint32_t bin[bins]{};
    void add(float v) {
        if (!std::isfinite(v)) return;
        ++count, sum += v, peak = (std::max)(peak, v);
        int b = int((v - low) / (high - low) * bins);
        bin[std::clamp(b, 0, int(bins) - 1)]++;
    }
    float mean() const { return count ? float(sum / double(count)) : 0.f; }
    // The upper edge of the bin holding the q-th fraction of the samples.
    float quantile(float q) const {
        if (!count) return 0;
        const uint64_t want = uint64_t(std::ceil(q * double(count)));
        uint64_t seen = 0;
        for (unsigned b = 0; b < bins; ++b)
            if ((seen += bin[b]) >= want) return low + (high - low) * float(b + 1) / bins;
        return high;
    }
};

} // namespace fp::balance
