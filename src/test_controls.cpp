// Owned-process integration: actual window procedure and input adapter, fake
// exact-build storage. No game process, injection, or native gameplay claims.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HWND testForeground() {
    return (HWND)1;
}
static DWORD testWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#include <vector>
#include <random>
#include <cassert>
static unsigned nativeKeys = 0, nativeButtons = 0;
static int forwarded = 0;
static void poll() {
    *(unsigned*)(base + 0x49cdc0) = nativeKeys;
    *(unsigned*)(base + 0x49cdd0) = nativeButtons;
}
static LRESULT CALLBACK fallback(HWND, UINT, WPARAM, LPARAM) {
    ++forwarded;
    return 42;
}
static uintptr_t stationaryNav(void*, float) {
    return 123;
}
static void projection(uintptr_t) {}
static void inverse(uintptr_t, uintptr_t) {}
static float gaitThrust[3];
static void nativeGaitFixture(uintptr_t root, const float* axis) {
    // Native 163EF0 selects a negative 12C0+3C thrust for backpedalling.
    const float thrust = (*(unsigned*)(root + 0x10f4) & 2) ? -.937f : 1.07f;
    for (int i = 0; i < 3; ++i) gaitThrust[i] = axis[i] * thrust;
}
static bool steadyFaults(uintptr_t r, void (*build)(uintptr_t, float, unsigned char)) {
    __try {
        steadyHeadAround(r, build, 0, 0);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
// A model of the head on a walking body (not the game's solver): a Verlet tetrahedron pulled toward the
// mouse's gaze by a soft neck drive, its pivot swayed and bobbed by the steps and tilted by the torso.
// Measures how far the head's facing strays from the gaze (wobble, degrees RMS), the head's roll, and its
// vertical bob (cm RMS about its own average), with fp::steadyHead off or on.
struct HeadSim {
    float wobble = 0, roll = 0, bob = 0, settle = 0, lag = 0, lead = 0;
};
static HeadSim simulateHead(const fp::HeadSteadyStrength& s, bool steady, float stepHz, float sway,
                            float turnDegPerS = 0) {
    const float coeffs[16] = {
        -.1f, .1f, 0, 0,   -.1f, 0, .1f, 0,
        -.1f, 0,   0, .1f, 1,    0, 0,   0}; // right, up, forward from the points; position point 0
    float pts[4][3], prev[4][3];
    const float rest[4][3] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 3; ++j) pts[k][j] = prev[k][j] = rest[k][j] + (j == 1 ? 160.f : 0.f);
    fp::HeadSteadyState st;
    unsigned seed = 12345;
    auto noise = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(int(seed >> 9) % 2001 - 1000) / 1000.f;
    };
    const float dt = 1.f / 60.f;
    double w2 = 0, r2 = 0, b2 = 0;
    int n = 0;
    float yAvg = 160.f;
    float gazeYaw = 0;
    float settleAt = -1, turnEnd = -1;
    float torsoRoll = 0, torsoPitch = 0;
    float lagSum = 0, lead = 0;
    int lagN = 0;
    for (int tick = 0; tick < 900; ++tick) {
        const float t = float(tick) * dt;
        if (turnDegPerS != 0) {
            if (t >= 1.f && t < 2.f) gazeYaw += turnDegPerS * dt * 0.0174533f;
            if (t >= 2.f && turnEnd < 0) turnEnd = t;
        }
        const float g[3] = {std::sin(gazeYaw), 0, std::cos(gazeYaw)};
        // Verlet
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j) {
                const float v = (pts[k][j] - prev[k][j]) * 0.9f;
                prev[k][j] = pts[k][j];
                pts[k][j] += v;
            }
        // the torso tilts with each step and passes part of its turn to the head; the pivot sways and bobs
        const float ph = 6.2831853f * stepHz * t, newRoll = sway * 0.05f * std::sin(ph) + 0.01f * noise(),
                    newPitch = sway * 0.03f * std::sin(2 * ph + 0.7f) + 0.01f * noise();
        {
            const float imp[3] = {(newPitch - torsoPitch) * 0.6f, 0, (newRoll - torsoRoll) * 0.6f};
            torsoRoll = newRoll;
            torsoPitch = newPitch;
            float c[3]{};
            for (int k = 0; k < 4; ++k)
                for (int j = 0; j < 3; ++j) c[j] += 0.25f * pts[k][j];
            for (int k = 0; k < 4; ++k) {
                float d[3], o[3];
                for (int j = 0; j < 3; ++j) d[j] = pts[k][j] - c[j];
                fp::turn3(imp, d, o);
                for (int j = 0; j < 3; ++j) pts[k][j] = c[j] + o[j];
            }
        }
        const float pivot[3] = {sway * 3.f * std::sin(ph * 0.5f),
                                160.f + sway * 2.5f * std::fabs(std::sin(ph)), 0};
        // the neck drive: a share of the turn toward the gaze (level up), and of the way back to the pivot
        {
            float pose[12];
            fp::tetraPose(pts, coeffs, pose);
            float f[3] = {pose[6], pose[7], pose[8]}, u[3] = {pose[3], pose[4], pose[5]};
            fp::unit3(f);
            fp::unit3(u);
            float r[3];
            fp::cross3(u, f, r);
            fp::unit3(r);
            float tu[3] = {0, 1, 0}, tf[3] = {g[0], g[1], g[2]}, tr[3];
            fp::cross3(tu, tf, tr);
            const float* a[3] = {r, u, f};
            const float* b[3] = {tr, tu, tf};
            float rv[3];
            fp::frameTurn(a, b, rv);
            for (float& v : rv) v *= 0.12f;
            float c[3]{};
            for (int k = 0; k < 4; ++k)
                for (int j = 0; j < 3; ++j) c[j] += 0.25f * pts[k][j];
            for (int k = 0; k < 4; ++k) {
                float d[3], o[3];
                for (int j = 0; j < 3; ++j) d[j] = pts[k][j] - c[j];
                fp::turn3(rv, d, o);
                for (int j = 0; j < 3; ++j) pts[k][j] = c[j] + o[j] + (pivot[j] - c[j]) * 0.25f;
            }
        }
        if (steady) fp::steadyHead(st, pts, coeffs, g, s);
        float pose[12];
        fp::tetraPose(pts, coeffs, pose);
        float f[3] = {pose[6], pose[7], pose[8]}, u[3] = {pose[3], pose[4], pose[5]};
        fp::unit3(f);
        fp::unit3(u);
        const float off = std::acos(std::clamp(fp::dot3(f, g), -1.f, 1.f)) * 57.29578f;
        if (turnEnd > 0 && settleAt < 0 && off < 1.f) settleAt = t - turnEnd;
        if (turnDegPerS != 0 && t >= 1.5f && t < 2.f) {
            lagSum += off;
            ++lagN;
        }
        if (turnDegPerS != 0) {
            const float ahead = std::remainder(std::atan2(f[0], f[2]) - gazeYaw, 6.2831853f) * 57.29578f;
            if (ahead > lead) lead = ahead;
        }
        float c[3]{};
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j) c[j] += 0.25f * pts[k][j];
        yAvg += (c[1] - yAvg) * 0.02f;
        if (tick >= 180 && turnDegPerS == 0) {
            float r[3];
            fp::cross3(u, f, r);
            w2 += double(off) * off;
            const float rl = std::asin(std::clamp(r[1], -1.f, 1.f)) * 57.29578f;
            r2 += double(rl) * rl;
            b2 += double(c[1] - yAvg) * (c[1] - yAvg);
            ++n;
        }
    }
    HeadSim h;
    if (n) {
        h.wobble = float(std::sqrt(w2 / n));
        h.roll = float(std::sqrt(r2 / n));
        h.bob = float(std::sqrt(b2 / n));
    }
    h.settle = settleAt;
    h.lag = lagN ? lagSum / float(lagN) : 0.f;
    h.lead = lead;
    return h;
}
static void headStabiliserTests() {
    const fp::HeadSteadyStrength off{}, full{100, 100, 100, 100}, defaults{80, 70, 50, 30};
    for (int run = 0; run < 2; ++run) {
        const float hz = run ? 2.6f : 1.8f, sway = run ? 2.f : 1.f;
        const HeadSim a = simulateHead(off, false, hz, sway), b = simulateHead(defaults, true, hz, sway),
                      c = simulateHead(full, true, hz, sway);
        printf("MODEL %s: wobble %.2f -> %.2f (defaults) -> %.2f deg (100%%); roll %.2f -> %.2f -> %.2f deg; "
               "bob %.2f -> %.2f -> %.2f cm\n",
               run ? "run " : "walk", a.wobble, b.wobble, c.wobble, a.roll, b.roll, c.roll, a.bob, b.bob,
               c.bob);
        assert(c.wobble < a.wobble * 0.6f && c.roll < a.roll * 0.6f && b.wobble < a.wobble && c.bob < a.bob &&
               std::isfinite(c.wobble));
    }
    // a mouse turn: the head reaches the gaze no later with the stabiliser than without (the view never lags more)
    const HeadSim t0 = simulateHead(off, false, 1.8f, 0.f, 90.f),
                  t1 = simulateHead(full, true, 1.8f, 0.f, 90.f);
    printf("MODEL turn 90 deg/s: head behind the mouse %.2f deg off, %.2f deg at 100%%; within 1 deg %.3f s "
           "/ %.3f s after the mouse stops\n",
           t0.lag, t1.lag, t0.settle, t1.settle);
    assert(t0.settle >= 0 && t1.settle >= 0 && t1.lag <= t0.lag + 0.05f && t1.settle <= t0.settle + 0.02f &&
           t1.lead <= t0.lead + 0.05f); // never ahead of the mouse more than without
    // the gaze (the mouse's) is only read; bad input changes nothing
    {
        fp::HeadSteadyState st;
        float pts[4][3] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, keep[4][3];
        memcpy(keep, pts, sizeof pts);
        const float coeffs[16] = {-.1f, .1f, 0, 0, -.1f, 0, .1f, 0, -.1f, 0, 0, .1f, 1, 0, 0, 0};
        const float bad[3] = {0, 0, 0};
        assert(!fp::steadyHead(st, pts, coeffs, bad, full) && !memcmp(pts, keep, sizeof pts));
        const float g[3] = {0, 0, 1};
        assert(fp::steadyHead(st, pts, coeffs, g, full) &&
               !memcmp(pts, keep, sizeof pts)); // the first tick only primes
        float wild[4][3];
        memcpy(wild, pts, sizeof pts);
        for (auto& p : wild) {
            const float x = p[0];
            p[0] = p[1];
            p[1] = -x;
        } // a 90-degree jolt in one tick
        assert(fp::steadyHead(st, wild, coeffs, g, full));
        float pose[12];
        fp::tetraPose(wild, coeffs, pose); // corrected by at most 2 degrees
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j) assert(std::isfinite(wild[k][j]));
    }
    puts("PASS: head stabiliser: less wobble, roll and bob in the model; mouse turns never slowed; clamped; "
         "bad input ignored");
    { // spine follows aim: looking down bends the spine forward, looking up back, a share of the gaze's pitch
        const float down60[3] = {0, -std::sin(1.0472f), std::cos(1.0472f)}, level[3] = {0, 0, 1},
                    up45[3] = {0, 0.7071f, 0.7071f}, up89[3] = {0, 0.9998f, 0.0175f};
        assert(std::fabs(fp::spineBend(down60, 100) - 1.0472f * fp::spineBendShare) < 1e-3f &&
               fp::spineBend(level, 100) == 0.f && fp::spineBend(down60, 0) == 0.f);
        assert(fp::spineBend(up45, 100) < 0 &&
               fp::spineBend(up89, 100) >= -fp::spineBendMax - 1e-6f); // looking up bends back, clamped
        // which way the part's top goes: its up axis (local Y) turned by the extra rotation about its local X
        auto topAfter = [](const float* pose, const float* q, float* top) {
            const float ang = 2.f * std::atan2(q[0], q[3]);
            const float y[3] = {0, std::cos(ang), std::sin(ang)}; // local Y turned about local X
            for (int j = 0; j < 3; ++j) top[j] = pose[3 + j] * y[1] + pose[6 + j] * y[2];
        };
        for (float side : {1.f, -1.f}) { // the part's X along or against the side axis: the sign follows it
            const float pose[12] = {side, 0, 0, 0, 1, 0, 0, 0, side, 0, 0, 0};
            float q[4], top[3];
            assert(fp::spineBendQuat(pose, pose, down60, 0.3f, q));
            topAfter(pose, q, top);
            assert(top[2] > 0.2f && top[1] > 0.9f); // forward: toward where you look (+z)
            assert(fp::spineBendQuat(pose, pose, up45, -0.3f, q));
            topAfter(pose, q, top);
            assert(top[2] < -0.2f);
        } // back
        {
            const float pose[12] = {0, 0, 1, 0, 1, 0, -1, 0, 0, 0, 0, 0};
            float q[4];
            assert(fp::spineBendQuat(pose, pose, down60, 0.3f, q) && std::fabs(q[2]) > 0.1f);
        } // a rotated parent expresses the world side axis in its local Z, not only X
        {
            const float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                        twisted[12] = {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0};
            float q[4];
            assert(fp::spineBendQuat(pose, twisted, down60, 0.3f, q) && std::fabs(q[1]) > 0.1f);
        } // the resolved parent frame handles disagreement without dropping the bend
        puts("PASS: spine follows aim: forward looking down, back looking up, a share of the pitch, clamped, "
             "about the side axis only");
    }
    { // lean: the mouse bends the upper body; right is the view's right (yaw 0: -x), forward is the heading
        auto topAfter = [](const float* pose, const float* q,
                           float* top) { // the part's local Y turned by q, in the world
            const float v[3] = {0, 1, 0}, u[3] = {q[0], q[1], q[2]}, w = q[3];
            float t[3] = {2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]),
                          2 * (u[0] * v[1] - u[1] * v[0])};
            const float r[3] = {v[0] + w * t[0] + (u[1] * t[2] - u[2] * t[1]),
                                v[1] + w * t[1] + (u[2] * t[0] - u[0] * t[2]),
                                v[2] + w * t[2] + (u[0] * t[1] - u[1] * t[0])};
            for (int j = 0; j < 3; ++j) top[j] = pose[j] * r[0] + pose[3 + j] * r[1] + pose[6 + j] * r[2];
        };
        const float ahead[3] = {0, 0, 1};
        float q[4], top[3];
        for (float s : {1.f, -1.f}) {
            const float pose[12] = {s, 0, 0, 0, 1, 0, 0, 0, s, 0, 0, 0}; // either way round, as the bend
            assert(fp::spineLeanQuat(pose, pose, ahead, 0, 0.3f, q));
            topAfter(pose, q, top);
            assert(top[0] < -0.2f && std::fabs(top[2]) < 1e-4f); // right
            assert(fp::spineLeanQuat(pose, pose, ahead, 0, -0.3f, q));
            topAfter(pose, q, top);
            assert(top[0] > 0.2f); // left
            assert(fp::spineLeanQuat(pose, pose, ahead, 0.3f, 0, q));
            topAfter(pose, q, top);
            assert(top[2] > 0.2f && std::fabs(top[0]) < 1e-4f); // forward, as spineBendQuat
            float qb[4];
            assert(fp::spineBendQuat(pose, pose, ahead, 0.3f, qb));
            for (int k = 0; k < 4; ++k) assert(std::fabs(q[k] - qb[k]) < 1e-5f);
        }
        {
            const float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                        twisted[12] = {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0};
            assert(fp::spineLeanQuat(pose, twisted, ahead, 0, 0.3f, q));
            float reference[4];
            assert(fp::spineLeanQuat(pose, pose, ahead, 0.25f, 0.3f, reference));
            // An achieved torso already rolled 30 degrees used to reject a valid lean (cos(30)<0.9).
            const float rolled[12] = {0.8660254f, 0.5f, 0, -0.5f, 0.8660254f, 0, 0, 0, 1, 0, 0, 0};
            assert(fp::spineLeanQuat(rolled, pose, ahead, 0.25f, 0.3f, q));
            for (int i = 0; i < 4; ++i) assert(std::fabs(q[i] - reference[i]) < 1e-6f);
        }
        fp::Lean l;
        l.step(50, -20, true, 1.f / 60);
        assert(std::fabs(l.side - 50 * fp::leanRadiansPerPixel) < 1e-6f && l.forward > 0);
        for (int i = 0; i < 100; ++i) l.step(100, -100, true, 1.f / 60);
        assert(l.side == fp::leanSideMax && l.forward == fp::leanForwardMax); // held: clamped
        l.step(0, 0, false, 0.15f);
        assert(l.side < fp::leanSideMax * 0.4f && l.side > 0); // let go: eases back
        for (int i = 0; i < 120; ++i) l.step(500, 500, false, 1.f / 60);
        assert(!l.active()); // upright again; the mouse no longer leans
        for (int i = 0; i < 60; ++i) l.keys(-1, 1.f / 60.f);
        assert(l.side < -0.49f);
        for (int i = 0; i < 120; ++i) l.keys(0, 1.f / 60.f);
        assert(!l.active());
        for (int i = 0; i < 60; ++i) l.keys(1, 1.f / 60.f);
        assert(l.side > 0.49f);
        l = {};
        const panel::ModSettings leanDefaults;
        assert(!leanDefaults.leanKeysMode && leanDefaults.leanLeftKey == 0 && leanDefaults.leanRightKey == 0);
        l.step(NAN, 0, true, 1.f / 60);
        assert(!l.active());
        assert(panel::ModSettings().value(panel::setting::fov) ==
               L"75\x00B0 (108\x00B0 across at 16:9)"); // FOV is vertical; the 16:9 width beside it
        {
            const float never[3]{}, front[3] = {0, 0, 1}, seven[3] = {0, std::sin(0.12f), std::cos(0.12f)},
                                    nine[3] = {std::sin(0.16f), 0, std::cos(0.16f)};
            assert(fp::helmetRetestDue(never, front) && !fp::helmetRetestDue(front, front) &&
                   !fp::helmetRetestDue(front, seven) && fp::helmetRetestDue(front, nine));
        } // head wear: re-tested after 8 degrees
        puts("PASS: lean: the mouse bends the upper body right, left, forward (the same rotation as the "
             "bend), clamped, eased back upright");
    }
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // a crash keeps the lines before it

    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    headStabiliserTests();
    assert(!fp::crosshairDrawn(false, false, false, false, true, true));
    assert(!fp::crosshairDrawn(false, false, false, true, true, true));
    assert(fp::crosshairDrawn(true, false, false, false, false, true));
    {
        const float coeffs[16] = {-.1f, .1f, 0, 0, -.1f, 0, .1f, 0, -.1f, 0, 0, .1f, 1, 0, 0, 0};
        for (float pitch : {-1.15f, 0.f, 1.15f}) {
            const float axes[9] = {
                1, 0, 0, 0, std::cos(pitch), -std::sin(pitch), 0, std::sin(pitch), std::cos(pitch)};
            float pts[4][3] = {{0, 160, 0}, {10, 160, 0}, {0, 170, 0}, {0, 160, 10}};
            for (int k = 0; k < 4; ++k) {
                float out[3]{};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        out[i] += axes[j * 3 + i] * (pts[k][j] - (j == 1 ? 160.f : 0.f));
                for (int i = 0; i < 3; ++i) pts[k][i] = out[i] + (i == 1 ? 160.f : 0.f);
            }
            const float gaze[3] = {std::sin(.12f) * std::cos(pitch), std::sin(pitch),
                                   std::cos(.12f) * std::cos(pitch)};
            float before[12];
            fp::tetraPose(pts, coeffs, before);
            assert(fp::steadyHeading(pts, coeffs, gaze, 0) == 0.f);
            float turn = fp::steadyHeading(pts, coeffs, gaze, 100), after[12];
            fp::tetraPose(pts, coeffs, after);
            assert(turn > 0.f && turn <= fp::steadyTurnClamp && std::atan2(after[6], after[8]) > 0.f);
            assert(std::fabs(after[7] - before[7]) < 1e-5f); // yaw correction leaves pitch unchanged
        }
        puts("PASS: heading bias is corrected on head points at steep pitch, bounded per tick and disabled "
             "at zero");
    }
    { // starting: loading starts nothing; enable starts the session once (our launcher, or EMTK with the mod on);
        // disable stops as F7 does and a later enable re-arms; the game is waited for until its slots are filled
        static volatile LONG starts = 0;
        sessionEntry = [](void*) -> DWORD {
            InterlockedIncrement(&starts);
            return 0;
        };
        assert(!sessionStarted);
        assert(enable() && enable());
        for (int i = 0; i < 100 && !starts; ++i) Sleep(10);
        Sleep(50);
        assert(starts == 1);                                   // once
        assert(!rearmRequested);                               // running: enable again re-arms nothing
        assert(disable() && stopping == 1 && !rearmRequested); // EMTK off: the F7 stop
        assert(enable() && rearmRequested == 1 &&
               starts == 1); // EMTK on again: the F6 re-arm, no second session
        waitForRearmKey();
        assert(!rearmRequested); // the waiting session takes it (returns at once, no F6)
        rearmRunning();
        assert(stopping == 0 && !emtkOff);   // re-armed: running
        assert(enable() && !rearmRequested); // on, and on again: nothing
        disable();
        rearmRunning();
        assert(stopping == 1); // switched off while a re-arm finished: stopped again, never lost
        InterlockedExchange(&emtkOff, 0);
        InterlockedExchange(&stopping, 0);
        { // EMTK's framework console reports the game's process: it is never taken for the game's window (hidden test windows)
            WNDCLASSW c{};
            c.lpfnWndProc = DefWindowProcW;
            c.hInstance = GetModuleHandleW(nullptr);
            c.lpszClassName = L"ConsoleWindowClass";
            RegisterClassW(&c);
            WNDCLASSW o{};
            o.lpfnWndProc = DefWindowProcW;
            o.hInstance = GetModuleHandleW(nullptr);
            o.lpszClassName = L"GameLikeWindow";
            RegisterClassW(&o);
            HWND con = CreateWindowW(L"ConsoleWindowClass", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                     c.hInstance, nullptr),
                 game = CreateWindowW(L"GameLikeWindow", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      o.hInstance, nullptr);
            assert(con && game && consoleWindow(con) && !consoleWindow(game));
            DestroyWindow(con);
            DestroyWindow(game);
        }
        puts("PASS: start: enable once (launcher or EMTK), disable = the F7 stop, enable again = the F6 "
             "re-arm; a switch-off during a re-arm is kept");
    }
    { // the hook: only the player's head (and the neck and chest under it), first person, never knocked down
        const uintptr_t root = base + 0x5c0000, head = base + 0x5c4000, neck = base + 0x5c6000,
                        chest = base + 0x5c8000, rootPart = base + 0x5ca000;
        const uintptr_t keptRoot = *(uintptr_t*)(base + 0x4f0830);
        *(uintptr_t*)(base + 0x4f0830) = root;
        *(uintptr_t*)(root + 0x11e0) = head;
        *(unsigned char*)(root + 0x4c8) = 1;
        *(float*)(root + 0x10b8) = 1;
        *(float*)(root + 0x10bc) = 1;
        *(uintptr_t*)(head + 0x428) = neck;
        *(uintptr_t*)(neck + 0x428) = chest;
        for (uintptr_t p : {head, neck, chest}) *(uintptr_t*)(p + 0x420) = rootPart;
        const float coeffs[16] = {-.1f, .1f, 0, 0, -.1f, 0, .1f, 0, -.1f, 0, 0, .1f, 1, 0, 0, 0},
                    pts[4][3] = {{0, 160, 0}, {10, 160, 0}, {0, 170, 0}, {0, 160, 10}};
        for (uintptr_t p : {head, neck, chest}) {
            memcpy((void*)(p + 0xb0), coeffs, 64);
            memcpy((void*)(p + 0xf0), pts, 48);
        }
        *(float*)(root + 0xcb0) = 0;
        *(float*)(root + 0xcb4) = 0;
        *(float*)(root + 0xcb8) = 1;
        static int settled = 0;
        settled = 0;
        originalPartSettle = [](uintptr_t) {
            ++settled;
        };
        InterlockedExchange(&active, 1);
        viewMode = 0;
        auto tilt = [&](uintptr_t p) {
            float* q = (float*)(p + 0xf0);
            for (int k = 0; k < 4; ++k) {
                const float y = q[k * 3 + 1] - 160.f, x = q[k * 3];
                q[k * 3] = x * 0.998f - y * 0.0628f;
                q[k * 3 + 1] = 160.f + x * 0.0628f + y * 0.998f;
            }
        }; // a 3.6-degree roll
        const uintptr_t keptDrag = *(uintptr_t*)(base + 0x4efdd0);
        *(uintptr_t*)(base + 0x4efdd0) = 0;
        partSettle(head);
        partSettle(head);
        tilt(head);
        float before[12];
        memcpy(before, (void*)(head + 0xf0), 48);
        partSettle(head); // a new head, then the first tick primes
        assert(settled == 3 && memcmp(before, (void*)(head + 0xf0), 48) != 0 &&
               steadyTicks.load() >= 1); // steadied, the game's own step still runs
        partSettle(neck);
        tilt(neck);
        memcpy(before, (void*)(neck + 0xf0), 48);
        partSettle(head);
        partSettle(neck);
        assert(memcmp(before, (void*)(neck + 0xf0), 48) != 0); // out of combat the neck is steadied too
        // The actual poseBuilder's scoped extra-rotation ownership, exception
        // restoration and stop race are exercised by test_pose_ownership.cpp.
        *(unsigned*)(root + 0xdf4) = 0x200;
        tilt(head);
        memcpy(before, (void*)(head + 0xf0), 48);
        partSettle(head);
        assert(memcmp(before, (void*)(head + 0xf0), 48) == 0); // knocked down: the game's alone
        *(unsigned*)(root + 0xdf4) = 0;
        viewMode = 1;
        partSettle(head);
        tilt(head);
        memcpy(before, (void*)(head + 0xf0), 48);
        partSettle(head);
        assert(memcmp(before, (void*)(head + 0xf0), 48) == 0);
        viewMode = 0; // third person: off
        *(unsigned char*)(root + 0x12fe) = 1;
        partSettle(head);
        partSettle(neck);
        tilt(neck);
        memcpy(before, (void*)(neck + 0xf0), 48);
        partSettle(neck);
        assert(memcmp(before, (void*)(neck + 0xf0), 48) == 0);
        *(unsigned char*)(root + 0x12fe) = 0; // in combat the neck is the game's
        memcpy(before, (void*)(base + 0x5d0000), 48);
        partSettle(base + 0x5d0000);
        assert(memcmp(before, (void*)(base + 0x5d0000), 48) == 0); // any other body: untouched
        InterlockedExchange(&active, 0);
        *(uintptr_t*)(base + 0x4f0830) = keptRoot;
        *(uintptr_t*)(base + 0x4efdd0) = keptDrag;
        memset((void*)root, 0, 0x10000);
        headSteadyState = fp::HeadSteadyState();
        steadiedHead = steadiedRoot = steadyNeck = steadyChest = 0;
        puts("PASS: the head stabiliser hook steadies only the player's head in first person and lets go "
             "when knocked down, in third person and (chest, neck) in combat");
    }
    { // the game's picks never meet your own body in first and third person: your meshes carry the skip bit for
        // the call only, and only the bits set here are cleared again
        const uintptr_t keptPlayer = *(uintptr_t*)(base + 0x4f0830), r = base + 0x5c0000,
                        mesh = base + 0x5d0000, arm = base + 0x5d1000, shield = base + 0x5d2000,
                        kids = base + 0x5d3000;
        *(uintptr_t*)(base + 0x4f0830) = r;
        *(uintptr_t*)(r + 0x18) = mesh;
        *(int*)(mesh + 0x118) = 1;
        *(uintptr_t*)(mesh + 0x120) = kids;
        ((uintptr_t*)kids)[0] = arm;
        ((uintptr_t*)kids)[1] = shield;
        *(int*)(arm + 0x118) = -1;
        *(int*)(shield + 0x118) = -1;
        *(unsigned*)(shield + 8) = pickSkipBit | 0x89;
        *(unsigned*)(arm + 8) = 0x89;
        *(unsigned*)(mesh + 8) = 0x89;
        InterlockedExchange(&active, 1);
        viewMode = 0;
        static unsigned seenArm = 0, seenMesh = 0;
        seenArm = seenMesh = 0;
        const PickRay fake = [](uintptr_t, float, float, uintptr_t) -> char {
            seenArm = *(unsigned*)(base + 0x5d1000 + 8);
            seenMesh = *(unsigned*)(base + 0x5d0000 + 8);
            return 1;
        };
        assert(pickPastSelf(fake, 0, 0, 0, 0) == 1 && (seenArm & pickSkipBit) &&
               (seenMesh & pickSkipBit)); // skipped during the pick
        assert(*(unsigned*)(arm + 8) == 0x89 && *(unsigned*)(mesh + 8) == 0x89 &&
               *(unsigned*)(shield + 8) == (pickSkipBit | 0x89) &&
               !selfPickSet); // put back; a bit the game had set stays
        viewMode = 2;
        pickPastSelf(fake, 0, 0, 0, 0);
        assert(!(seenArm & pickSkipBit));
        viewMode = 0; // the overhead view: the game's own pick
        InterlockedExchange(&active, 0);
        pickPastSelf(fake, 0, 0, 0, 0);
        assert(!(seenArm & pickSkipBit));
        memset((void*)mesh, 0, 0x4000);
        *(uintptr_t*)(r + 0x18) = 0;
        *(uintptr_t*)(base + 0x4f0830) = keptPlayer;
        puts("PASS: the game's picks skip your own body in first and third person, for the call only");
    }
    { // crouch: the game's duck key toggles or holds it; combat, a knockdown, death, talking or overhead end it;
        // standing up waits for headroom
        fp::Crouch c;
        assert(!fp::crouchStep(c, 2, false, true, true) && fp::crouchStep(c, 2, true, true, true) &&
               fp::crouchStep(c, 2, true, true, true) &&
               fp::crouchStep(c, 2, false, true, true)); // toggle: a press, held, let go: still down
        assert(!fp::crouchStep(c, 2, true, true, true) &&
               !fp::crouchStep(c, 2, false, true, true)); // the next press stands up
        assert(fp::crouchStep(c, 2, true, true, true) && fp::crouchStep(c, 2, false, true, true) &&
               fp::crouchStep(c, 2, true, true, false) &&
               fp::crouchStep(c, 2, false, true, false)); // a ceiling above: stays down
        assert(!fp::crouchStep(c, 2, false, true, true)); // room again: stands
        fp::Crouch h;
        assert(fp::crouchStep(h, 1, true, true, true) && !fp::crouchStep(h, 1, false, true, true)); // hold
        assert(fp::crouchStep(h, 1, true, true, true) && !fp::crouchStep(h, 1, true, false, true) &&
               !fp::crouchStep(h, 1, true, false, false)); // combat (not allowed) ends it, ceiling or not
        fp::Crouch o;
        assert(!fp::crouchStep(o, 0, true, true, true)); // off: the game's short duck only
        assert(fp::crouchModeFromText(L"hold") == 1 && fp::crouchModeFromText(L"OFF") == 0 &&
               fp::crouchModeFromText(L"toggle") == 2 && fp::crouchModeFromText(nullptr) == 2 &&
               !wcscmp(fp::crouchModeText(1), L"hold"));
        // the frame step: out of combat, first and third person; talking or combat ends it
        const uintptr_t keptPlayer = *(uintptr_t*)(base + 0x4f0830), r = base + 0x5c0000;
        *(uintptr_t*)(base + 0x4f0830) = r;
        InterlockedExchange(&active, 1);
        viewMode = 0;
        crouchMode = 2;
        crouchState = fp::Crouch();
        duckKey = true;
        crouchFrame();
        duckKey = false;
        crouchFrame();
        assert(crouchHeld.load());
        viewMode = 1;
        crouchFrame();
        assert(crouchHeld.load());
        viewMode = 2;
        crouchFrame();
        assert(!crouchHeld.load());
        viewMode = 0;
        duckKey = true;
        crouchFrame();
        duckKey = false;
        crouchFrame();
        assert(crouchHeld.load());
        *(unsigned char*)(r + 0x12fe) = 1;
        crouchFrame();
        assert(!crouchHeld.load());
        *(unsigned char*)(r + 0x12fe) = 0;
        {
            const uintptr_t choices = base + 0x5e6000;
            *(unsigned char*)(choices + 0x79) = 1;
            duckKey = true;
            crouchFrame();
            duckKey = false;
            crouchFrame();
            assert(crouchHeld.load());
            *(unsigned char*)gameAt(fp::sig::dialogueActive) = 1;
            *(uintptr_t*)gameAt(fp::sig::dialogueChoices) = choices;
            crouchFrame();
            assert(!crouchHeld.load());
            *(unsigned char*)gameAt(fp::sig::dialogueActive) = 0;
            *(uintptr_t*)gameAt(fp::sig::dialogueChoices) = 0;
            *(unsigned char*)(choices + 0x79) = 0;
        }
        InterlockedExchange(&windowFree, 1);
        duckKey = true;
        crouchFrame();
        duckKey = false;
        crouchFrame();
        assert(!crouchHeld.load());
        InterlockedExchange(&windowFree, 0); // a press with a window holding the view: no crouch
        duckKey = true;
        crouchFrame();
        duckKey = false;
        crouchFrame();
        assert(crouchHeld.load());
        *(unsigned*)(r + 0xdf4) = 0x200;
        crouchFrame();
        assert(!crouchHeld.load());
        *(unsigned*)(r + 0xdf4) = 0; // knocked down
        InterlockedExchange(&active, 0);
        crouchFrame();
        assert(!crouchHeld.load());
        *(uintptr_t*)(base + 0x4f0830) = keptPlayer;
        crouchState = fp::Crouch();
        puts("PASS: crouch: the game's duck key toggles or holds it out of combat; combat, talking, overhead "
             "and a stop end it; a ceiling keeps you down");
    }
    { // the game's windows free the cursor and hold look and movement (first and third person), then let go
        const uintptr_t task = base + 0x5e0000, forms = base + 0x5e1000, inv = base + 0x5e2000,
                        hud = base + 0x5e3000, endMenu = base + 0x5e4000, mgr = base + 0x5e5000;
        const uintptr_t kept[4] = {*(uintptr_t*)(base + 0x4997a0), *(uintptr_t*)(base + 0x499780),
                                   *(uintptr_t*)(base + 0x4f01b0), *(uintptr_t*)(base + 0x49a960)};
        *(uintptr_t*)(base + 0x4997a0) = task;
        *(uintptr_t*)(base + 0x499780) = task;
        *(uintptr_t*)(base + 0x49a960) = mgr;
        *(int*)(task + 0xa8) = 2;
        *(uintptr_t*)(task + 0xb0) = forms;
        ((uintptr_t*)forms)[0] = hud;
        ((uintptr_t*)forms)[1] = inv;
        *(unsigned char*)(hud + 0x79) = 1;
        *(unsigned*)(hud + 0xb8) = 0x12345; // the viewport and HUD: always shown, never a window
        *(unsigned*)(inv + 0xb8) = windowInventory;
        InterlockedExchange(&active, 1);
        viewMode = 0;
        windowsFreeze = true;
        InterlockedExchange(&windowFree, 0);
        windowFreeStep();
        assert(!InterlockedCompareExchange(&windowFree, 0, 0) && !cursorFreeNow()); // nothing open
        *(unsigned char*)(inv + 0x79) = 1;
        windowFreeStep();
        assert(InterlockedCompareExchange(&windowFree, 0, 0) && cursorFreeNow()); // inventory open
        { // movement held even with the pointer over the window (the game's view is then not the hovered one)
            const uintptr_t keptPlayer = *(uintptr_t*)gameAt(fp::sig::playerRoot), r = base + 0x5c0000,
                            b = base + 0x5cc000,
                            keptBehavior = *(uintptr_t*)(r + field::character::behaviour),
                            keptCharacter = *(uintptr_t*)(b + field::behaviour::character);
            const LONG keptReady = cameraReady, keptStopping = stopping;
            const int keptMode = viewMode.load();
            const unsigned keptKeys = nativeKeys, keptButtons = nativeButtons;
            auto keptIn = originalInput;
            *(uintptr_t*)gameAt(fp::sig::playerRoot) = r;
            *(uintptr_t*)(r + field::character::behaviour) = b;
            *(uintptr_t*)(b + field::behaviour::character) = r;
            cameraReady = 1;
            stopping = 0;
            originalInput = poll;
            nativeKeys = 1;
            nativeButtons = 1;
            input();
            assert((*(unsigned*)(base + 0x49cdc0) & 15) == 0 && *(unsigned*)(base + 0x49cdd0) == 0);
            InterlockedExchange(&windowFree, 0);
            viewMode = 2; // explicitly handed to native overhead: its unfiltered keys pass
            input();
            assert((*(unsigned*)(base + 0x49cdc0) & 15) == 1);
            InterlockedExchange(&windowFree, 1); // restore the outer fixture's open window
            viewMode = keptMode;
            cameraReady = keptReady;
            stopping = keptStopping;
            nativeKeys = keptKeys;
            nativeButtons = keptButtons;
            originalInput = keptIn;
            *(uintptr_t*)gameAt(fp::sig::playerRoot) = keptPlayer;
            *(uintptr_t*)(r + field::character::behaviour) = keptBehavior;
            *(uintptr_t*)(b + field::behaviour::character) = keptCharacter;
        }
        *(unsigned char*)(inv + 0x11b) = 1;
        windowFreeStep();
        assert(!cursorFreeNow()); // closing: let go
        *(unsigned char*)(inv + 0x11b) = 0;
        for (unsigned kind : {windowLoot, windowContainer, windowDescription}) {
            *(unsigned*)(inv + 0xb8) = kind;
            windowFreeStep();
            assert(cursorFreeNow());
        }
        *(unsigned*)(mgr + 0x70) = 0xfb01;
        *(uintptr_t*)(mgr + 0x48) = inv;
        *(unsigned char*)(inv + 0x79) = 0;
        windowFreeStep();
        assert(cursorFreeNow()); // carrying an item out of the window: still free
        *(unsigned*)(mgr + 0x70) = 0;
        *(uintptr_t*)(mgr + 0x48) = 0;
        windowFreeStep();
        assert(!cursorFreeNow()); // dropped: look and movement back
        *(unsigned char*)(inv + 0x79) = 1;
        *(unsigned*)(inv + 0xb8) = windowInventory;
        windowFreeStep();
        *(unsigned char*)(inv + 0x79) = 0;
        *(unsigned*)(mgr + 0x70) = 0xfb01;
        windowFreeStep();
        assert(!cursorFreeNow());
        *(unsigned*)(mgr + 0x70) = 0; // a drag format left behind without the button held: let go
        *(unsigned*)(mgr + 0x70) = 0xfb01;
        *(uintptr_t*)(mgr + 0x48) = inv;
        windowFreeStep();
        assert(!cursorFreeNow());
        *(unsigned*)(mgr + 0x70) = 0;
        *(uintptr_t*)(mgr + 0x48) = 0; // a world pick-up's drag alone never freezes
        *(unsigned*)(inv + 0xb8) = windowDescription;
        *(unsigned char*)(inv + 0x79) = 1;
        windowFreeStep();
        assert(!cursorFreeNow()); // a description passed over: not yet
        descriptionSince = GetTickCount64() - descriptionSettleMs;
        windowFreeStep();
        assert(cursorFreeNow()); // it stays: held
        *(unsigned char*)(inv + 0x79) = 0;
        windowFreeStep();
        assert(!cursorFreeNow());
        for (unsigned kind : {windowShop, windowArsenal}) {
            *(unsigned*)(inv + 0xb8) = kind;
            *(unsigned char*)(inv + 0x79) = 1;
            windowFreeStep();
            assert(cursorFreeNow());
            *(unsigned char*)(inv + 0x79) = 0;
            windowFreeStep();
            assert(!cursorFreeNow());
        } // the arena merchant
        *(unsigned char*)gameAt(fp::sig::dialogueActive) = 1;
        windowFreeStep();
        assert(!cursorFreeNow()); // a flag left set without the choices on screen: never holds the view
        *(uintptr_t*)gameAt(fp::sig::dialogueChoices) = hud;
        windowFreeStep();
        assert(cursorFreeNow());
        *(unsigned char*)gameAt(fp::sig::dialogueActive) = 0;
        windowFreeStep();
        assert(!cursorFreeNow());
        *(uintptr_t*)gameAt(fp::sig::dialogueChoices) = 0; // dialogue choices
        *(uintptr_t*)(base + 0x4f01b0) = endMenu;
        *(unsigned char*)(endMenu + 0x79) = 1;
        windowFreeStep();
        assert(cursorFreeNow()); // the arena's NEXT FIGHT
        viewMode = 1;
        windowFreeStep();
        assert(cursorFreeNow());
        viewMode = 2;
        windowFreeStep();
        assert(!cursorFreeNow());
        viewMode = 0; // third person yes, overhead the game's
        windowsFreeze = false;
        windowFreeStep();
        assert(!cursorFreeNow());
        windowsFreeze = true; // the F8 switch
        InterlockedExchange(&active, 0);
        windowFreeStep();
        assert(!cursorFreeNow());
        *(uintptr_t*)(base + 0x4997a0) = kept[0];
        *(uintptr_t*)(base + 0x499780) = kept[1];
        *(uintptr_t*)(base + 0x4f01b0) = kept[2];
        *(uintptr_t*)(base + 0x49a960) = kept[3];
        memset((void*)task, 0, 0x6000);
        InterlockedExchange(&windowFree, 0);
        puts("PASS: inventory, loot, containers, item descriptions and the arena's end menu free the cursor "
             "and hold look and movement; closing, dropping and overhead let go");
    }
    uintptr_t root = base + 0x550000, bh = base + 0x552000, view = base + 0x554000;
    *(uintptr_t*)(base + 0x4f0830) = root;
    *(uintptr_t*)(bh + 0x28) = root;
    *(uintptr_t*)(root + 0xd40) = bh;
    *(uintptr_t*)(bh + 0x898) = view;
    originalInput = poll;
    originalWindowProc = fallback;
    active = 1;
    inputEnabled = 1;
    cameraReady = 1;
    const unsigned keys[4] = {'I', 'J', 'K', 'L'};
    for (unsigned i = 0; i < 4; ++i) *(unsigned*)(base + 0x2b72e0 + i * 16) = keys[i];
    assert(keyMask('W') == 0 && keyMask('I') == 1 && keyMask('J') == 2 && keyMask('K') == 4 &&
           keyMask('L') == 8);
    static bool down[256]{};
    keyState = [](int vk) -> SHORT {
        return vk >= 0 && vk < 256 && down[vk] ? SHORT(-0x8000) : SHORT(0);
    };
    { // Mode ownership is independent of a stale inputEnabled during F6's blend.
        const int keptMode = viewMode.load();
        const LONG keptFree = cursorFree;
        static int nativeCameraCalls = 0, cacheCalls = 0;
        static int orbitCalls = 0, zoomCalls = 0;
        originalCameraOrbit = [](uintptr_t self, float x, float y, int flags) {
            assert(self == 123 && x == 2.f && y == -3.f && flags == 0);
            ++orbitCalls;
        };
        originalCameraZoom = [](uintptr_t self, int amount) -> int {
            assert(self == 123 && amount == 10);
            ++zoomCalls;
            return amount;
        };
        originalCameraMouse = []() -> char {
            ++nativeCameraCalls;
            return 7;
        };
        copyCameraSnapshots = []() {
            ++cacheCalls;
        };
        for (int mode : {0, 1}) {
            viewMode = mode;
            cursorFree = 0;
            assert(controls(bh));
            *(unsigned char*)gameAt(fp::sig::cameraMouseActive) = 1;
            *(unsigned char*)gameAt(fp::sig::cameraMouseTurn) = 1;
            *(unsigned*)gameAt(fp::sig::keyWord) = 0x321;
            assert(cameraMouse() == 1 && nativeCameraCalls == 0);
            const int beforeOrbit = orbitCalls, beforeZoom = zoomCalls;
            cameraOrbit(123, 2.f, -3.f, 0);
            assert(cameraZoom(123, 10) == 10 && orbitCalls == beforeOrbit && zoomCalls == beforeZoom);
            assert(!*(unsigned char*)gameAt(fp::sig::cameraMouseActive) &&
                   !*(unsigned char*)gameAt(fp::sig::cameraMouseTurn) &&
                   *(unsigned*)gameAt(fp::sig::keyWord) == 0x21);
            cursorFree = 1; // Native camera is still hidden with a free pointer.
            assert(!controls(bh) && cameraMouse() == 1 && nativeCameraCalls == 0);
            rmbFree = 1;
            inputEnabled = 0;
            forwarded = 0;
            assert(windowProc(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0) == 0 && !forwarded);
            rmbFree = 0;
            assert(windowProc(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0) == 0 && !forwarded);
            inputEnabled = 1;
            viewMode = 2; // active/cameraReady can still be true during the blend.
            assert(!controls(bh) && !gameplayOwnsPointer());
            nativeKeys = 0x25;
            nativeButtons = 3;
            input(); // Free-cursor freezing must not erase overhead input either.
            assert(*(unsigned*)gameAt(fp::sig::keyWord) == nativeKeys &&
                   *(unsigned*)gameAt(fp::sig::buttonWord) == nativeButtons);
            const int previousCalls = nativeCameraCalls;
            assert(cameraMouse() == 7 && nativeCameraCalls == previousCalls + 1);
            cameraOrbit(123, 2.f, -3.f, 0);
            assert(cameraZoom(123, 10) == 10 && orbitCalls == beforeOrbit + 1 && zoomCalls == beforeZoom + 1);
            nativeCameraCalls = 0;
            forwarded = 0;
            assert(windowProc(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0) == 42 && forwarded == 1);
            assert(windowProc(nullptr, WM_MBUTTONDOWN, 0, 0) == 42 && forwarded == 2);
            const int fov = selectedFov.load();
            assert(windowProc(nullptr, WM_KEYDOWN, VK_F9, 1) == 42 && selectedFov.load() == fov);
        }
        assert(cacheCalls == 4);
        viewMode = 0;
        cursorFree = 0;
        *(int*)gameAt(fp::sig::uiOpenCount) = 1;
        assert(cameraMouse() == 7); // UI keeps its own native handler.
        *(int*)gameAt(fp::sig::uiOpenCount) = 0;
        // Lean bound to the native crouch/run/alternate key consumes that action.
        const int keptLean = leanKey.load();
        const bool keptLeanMode = leanKeysMode.load();
        leanKeysMode = false;
        const unsigned semantic[3] = {0x10, 0x20, 0x80};
        for (unsigned i = 0; i < 3; ++i) {
            const uintptr_t slot = gameAt(fp::sig::movementKeys) + (i + 4) * 16;
            const unsigned oldBinding = *(unsigned*)slot;
            *(unsigned*)slot = 'U';
            leanKey = 'U';
            input(); // admit the new binding while neutral, before a fresh press
            down['U'] = true;
            nativeKeys = semantic[i];
            nativeButtons = 0;
            input();
            assert(leaning && !(*(unsigned*)gameAt(fp::sig::keyWord) & semantic[i]));
            if (i == 0) assert(!duckKey);
            down['U'] = false;
            input();
            assert(!leaning && (*(unsigned*)gameAt(fp::sig::keyWord) & semantic[i]));
            *(unsigned*)slot = oldBinding;
        }
        leanKey = keptLean;
        leanKeysMode = keptLeanMode;
        viewMode = keptMode;
        cursorFree = keptFree;
        nativeKeys = nativeButtons = 0;
        inputOwner.releaseGameplay();
        puts("PASS: overhead polling/gestures remain native; FP/TP block native camera and consume lean "
             "actions");
    }
    down['I'] = true; // the keyboard (high bit: down): forward held
    { // a stuck walk: a held key whose release never arrived walks only while it is really down
        const unsigned keptNative = nativeKeys;
        const LONG keptFree = cursorFree;
        cursorFree = 0;
        nativeKeys = 0;
        windowProc(nullptr, WM_KEYDOWN, 'J', 1);
        down['J'] = true;
        input();
        assert((*(unsigned*)(base + 0x49cdc0) & 2) == 2); // held: walks
        inputOwner.discardMovementTaps();
        down['J'] = false;
        input();
        assert((*(unsigned*)(base + 0x49cdc0) & 2) == 0 &&
               !(inputOwner.heldMovement() & 2)); // let go with no WM_KEYUP (an overlay ate it): stops
        windowProc(nullptr, WM_KEYDOWN, 'J', 1);
        inputOwner.discardMovementTaps();
        windowProc(nullptr, WM_SYSKEYUP, 'J', 0);
        assert(!(inputOwner.heldMovement() & 2)); // released with Alt held: WM_SYSKEYUP clears it too
        nativeKeys = 4;
        input();
        assert((*(unsigned*)(base + 0x49cdc0) & 4) ==
               0); // the game's own bit for a key not down (it missed the release) is dropped too
        windowProc(nullptr, WM_KEYDOWN, 'L', 1);
        windowProc(nullptr, WM_KEYUP, 'L', 0);
        nativeKeys = 0;
        input();
        assert((*(unsigned*)(base + 0x49cdc0) & 8) == 8); // a tap within the frame still steps
        input();
        assert((*(unsigned*)(base + 0x49cdc0) & 8) == 0);
        { // the wheel would zoom the overhead view the player is not looking at (and returns to)
            const unsigned held = overheadInputsHeld;
            forwarded = 0;
            assert(windowProc(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0) == 0 &&
                   overheadInputsHeld == held + 1 && forwarded == 0);
            puts("PASS: in first and third person the wheel does not zoom the hidden overhead view");
        }
        { // combat (a lunge or a swing mid-walk), and a state that takes control away (behaviour +0x58) while the key goes up unseen
            *(unsigned char*)(root + 0x12fe) = 1;
            windowProc(nullptr, WM_KEYDOWN, 'J', 1);
            down['J'] = true;
            inputOwner.discardMovementTaps();
            input();
            assert((*(unsigned*)(base + 0x49cdc0) & 2) == 2);
            *(unsigned char*)(bh + 0x58) = 1;
            assert(!controls(bh));
            down['J'] = false;
            input(); // not ours meanwhile; the release is lost
            *(unsigned char*)(bh + 0x58) = 0;
            nativeKeys = 2;
            input();
            assert((*(unsigned*)(base + 0x49cdc0) & 2) == 0 &&
                   !(inputOwner.heldMovement() & 2)); // ours again: neither our bit nor the game's own walks on
            nativeKeys = 0;
            *(unsigned char*)(root + 0x12fe) = 0;
        }
        for (int vk : {'J', 'K', 'L'}) down[vk] = false;
        nativeKeys = keptNative;
        cursorFree = keptFree;
        inputOwner.movementUp(~0u);
        inputOwner.discardMovementTaps();
        puts("PASS: movement only while its key is physically down: a lost release (overlay, Alt) never "
             "leaves the character walking");
    }
    windowProc(nullptr, WM_KEYDOWN, 'I', 1);
    assert(inputOwner.heldMovement() == 1 && inputOwner.pendingMovement() == 1);
    {
        const LONG keptActive = active, keptInput = inputEnabled, keptCaptured = mouseCaptured;
        const int keptMode = viewMode.load();
        active = 1;
        viewMode = 0;
        rearmedAt = 0; // F6 cycles first -> third -> overhead -> first
        windowProc(nullptr, WM_KEYDOWN, VK_F6, 1);
        assert(active == 1 && viewMode.load() == 1);
        windowProc(nullptr, WM_KEYDOWN, VK_F6, 1);
        assert(active == 1 && viewMode.load() == 2); // third -> overhead blends first (the nav ends it)
        windowProc(nullptr, WM_KEYDOWN, VK_F6, 1);
        assert(active == 1 && viewMode.load() == 2); // F6 during the blend is ignored
        viewMode = 0;
        active = 0; // what the nav does when the 0.3 s are over
        windowProc(nullptr, WM_KEYDOWN, VK_F6, 1);
        assert(active == 1 && viewMode.load() == 0);
        active = keptActive;
        inputEnabled = keptInput;
        mouseCaptured = keptCaptured;
        viewMode = keptMode;
        puts("PASS: F6 cycles first person -> third person -> overhead -> first person");
    }
    windowProc(nullptr, WM_KEYUP, 'I', 0);
    assert(!inputOwner.heldMovement());
    inputOwner.discardMovementTaps();
    forwarded = 0;
    windowProc(nullptr, WM_MBUTTONDOWN, 0, 0);
    assert(forwarded == 0);
    windowProc(nullptr, WM_MBUTTONUP, 0, 0);
    assert(forwarded == 1);
    releaseInput();
    windowProc(nullptr, WM_MBUTTONDOWN, 0, 0);
    assert(forwarded == 1); // a free pointer still must not rotate the hidden overhead camera
    inputEnabled = 1;
    nativeButtons = 0;
    input();
    nativeKeys = 1;
    nativeButtons = 2;
    input();
    assert(*(unsigned*)(base + 0x49cdc0) == 1 && *(unsigned*)(base + 0x49cdd0) == 2);
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 2); // RMB+forward and holds remain native.
    cursorFree = 1;
    assert(!controls(bh));
    assert(playerContext(bh));
    input();
    assert(*(unsigned*)(base + 0x49cdc0) == 0 && *(unsigned*)(base + 0x49cdd0) == 0);
    Aim a;
    aim.root = root;
    aim.valid = true;
    assert(!getAim(bh, a)); // Native picking wins.
    cursorFree = 0;
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 0); // Still-held RMB cannot transfer.
    nativeButtons = 0;
    input();
    nativeButtons = 2;
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 2);
    *(uintptr_t*)(base + 0x49aa60) = view + 1;
    assert(!controls(bh));
    input();
    *(uintptr_t*)(base + 0x49aa60) = view;
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 0);
    nativeButtons = 0;
    input();
    nativeButtons = 1;
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 1);
    windowProc(nullptr, WM_KILLFOCUS, 0, 0);
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 0);
    cursorFree = 1;
    input();
    cursorFree = 0;
    nativeButtons = 0;
    inputOwner.buttonDown(1);
    input();
    assert(*(unsigned*)(base + 0x49cdd0) == 0); // Short UI click cannot transfer either.
    stopping = 1;
    forwarded = 0;
    selectedFov = 75;
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    assert(forwarded == 1 && selectedFov == 75);
    puts("PASS: remapped movement, MMB ownership, RMB+forward hold, native free-cursor picking, UI/focus "
         "release guard, inert stop");
    {
        stopping = 0;
        cursorFree = 0;
        active = 1;
        cameraReady = 1;
        const uintptr_t head = base + 0x557000;
        *(uintptr_t*)(root + 0x11e0) = head;
        *(unsigned char*)(root + 0x12ea) = 1;
        aim = {};
        aim.root = root;
        aim.head = head;
        aim.valid = true;
        aim.travelValid = true;
        originalWalkGait = nativeGaitFixture;
        for (float yaw : {0.f, .5f, 2.4f, -2.7f})
            for (unsigned mask : {1u, 2u, 4u, 8u, 5u, 10u})
                for (unsigned backward : {0u, 2u}) {
                    if (!fp::travel(mask, yaw, aim.travel)) continue;
                    const float facing[3] = {sinf(yaw), 0, cosf(yaw)};
                    *(unsigned*)(root + 0x10f4) = 0x40 | backward;
                    walkGait(root, facing);
                    float dot = 0;
                    for (int i = 0; i < 3; ++i) dot += gaitThrust[i] * aim.travel[i];
                    assert(dot > .9f); // Signed native thrust must follow each requested direction.
                    assert(*(unsigned*)(root + 0x10f4) == (0x40 | backward)); // Native gait state survives.
                }
        *(unsigned*)(root + 0x10f4) = 0;
        *(unsigned char*)(root + 0x12fe) = 1;
        const float facing[3] = {0, 0, 1};
        walkGait(root, facing);
        assert(gaitThrust[0] == 0 && gaitThrust[2] == 1.07f); // Combat stays native.
        *(unsigned char*)(root + 0x12fe) = 0;
        *(unsigned char*)(root + 0x12ea) = 0;
        puts("PASS: native signed gait thrust follows forward/backward/strafe intent, combat unchanged");
    }
    {
        fp::RawLook raw;
        long x = 0, y = 0;
        assert(!raw.take(x, y)); // No relative device seen: cursor-delta fallback.
        raw.event(false, 3, -2, true);
        raw.event(false, 4, 1, true);
        raw.event(false, 50, 50, false);
        assert(raw.take(x, y) && x == 7 && y == -1 && raw.take(x, y) && x == 0 && y == 0);
        raw.event(false, 5, 5, true);
        raw.reset();
        assert(raw.take(x, y) && x == 0 && y == 0);
        raw.event(true, 30000, 30000, true);
        assert(!raw.take(x, y)); // Absolute device.
        rawLook.event(false, 9, 9, true);
        inputEnabled = 1;
        releaseInput();
        long rx = 1, ry = 1;
        assert(rawLook.take(rx, ry) && rx == 0 && ry == 0); // Handoffs discard motion.
        puts("PASS: raw relative look accumulation, ownership gating, release discard, absolute-device "
             "fallback");
    }
    // A native stationary sampler does not rebuild orientation/FOV. It must
    // receive its previous stock values on every call and after F6/F7.
    uintptr_t viewport = base + 0x555000, camera = base + 0x556000, head = base + 0x557000,
              mesh = base + 0x558000, scene = base + 0x559000;
    *(uintptr_t*)(base + 0x4efc90) = viewport;
    *(uintptr_t*)(viewport + 0x1a8) = view;
    *(uintptr_t*)(view + 0x50) = camera;
    *(uintptr_t*)(camera + 0x160) = 42;
    *(uintptr_t*)(root + 0x11e0) = head;
    *(uintptr_t*)(head + 0x580) = root;
    *(uintptr_t*)(head + 0x18) = mesh;
    *(uintptr_t*)(mesh + 0x160) = head;
    *(uintptr_t*)(mesh + 0x130) = scene;
    auto h = (float*)(mesh + 0x40);
    h[0] = h[4] = h[8] = 1;
    float initial[12]{};
    fp::basis(.7f, .3f, initial);
    initial[9] = 1;
    initial[10] = 2;
    initial[11] = 3;
    memcpy((void*)(camera + 0x40), initial, 48);
    *(float*)(camera + 0x1c8) = 37;
    *(float*)(camera + 0x1cc) = 100;
    originalNav = stationaryNav;
    rebuildProjection = projection;
    rebuildInverse = inverse;
    stopping = 0;
    active = 1;
    inputEnabled = 0;
    assert(nav((void*)42, 0) == 123);
    assert(savedCamera == camera && pendingRestore && *(float*)(camera + 0x1cc) == 1);
    nav((void*)42, 0);
    assert(stockFov == 37 && memcmp(stockPose, initial, 48) == 0);
    active = 0;
    nav((void*)42, 0);
    assert(!savedCamera && !pendingRestore && *(float*)(camera + 0x1c8) == 37 &&
           *(float*)(camera + 0x1cc) == 100);
    assert(memcmp((void*)(camera + 0x40), initial, 48) == 0);
    pendingRestore = 1;
    const auto began = GetTickCount64();
    assert(!awaitRestore(50));
    assert(GetTickCount64() - began < 500);
    assert(pendingRestore == 1);
    pendingRestore = 0;
    assert(awaitRestore(50));
    {
        const Aim keptAim = aim;
        const auto keptStep = originalStep;
        const LONG keptActive = active, keptReady = cameraReady, keptStopping = stopping;
        const int keptMode = viewMode.load();
        aim = {};
        aim.root = root;
        aim.head = head;
        aim.valid = true;
        active = 1;
        stopping = 0;
        cameraReady = 1;
        viewMode = 1;
        cursorFree = rmbFree = windowFree = 0;
        static unsigned observedPending = 0;
        originalStep = [](uintptr_t at) {
            observedPending = *(unsigned*)(at + field::behaviour::pendingMove);
        };
        *(unsigned*)gameAt(fp::sig::keyWord) = 0; // the game has polled the released backward key
        *(unsigned*)(bh + field::behaviour::pendingMove) = 4;
        step(bh);
        assert(observedPending == 0); // no second step from the old digital direction
        *(unsigned*)gameAt(fp::sig::keyWord) = 4;
        *(unsigned*)(bh + field::behaviour::pendingMove) = 4;
        step(bh);
        assert(observedPending == 4); // held input stays native
        *(unsigned*)gameAt(fp::sig::keyWord) = 0;
        *(unsigned*)(bh + field::behaviour::pendingMove) = 0x8000;
        step(bh);
        assert(observedPending == 0x8000); // controller buffer stays native
        *(unsigned*)(bh + field::behaviour::pendingMove) = 0;
        *(float*)gameAt(fp::sig::analogStick) = 0.7f;
        *(unsigned*)(bh + field::behaviour::pendingMove) = 4;
        step(bh);
        assert(observedPending == 4); // active stick keeps its native request too
        *(float*)gameAt(fp::sig::analogStick) = 0;
        *(unsigned*)(bh + field::behaviour::pendingMove) = 0;
        leaning = heldRotating = true;
        leanMouseX = heldTurnX = 30.f;
        leanKeySide = 1;
        releaseInput();
        assert(!leaning && !heldRotating && leanKeySide == 0 && leanMouseX == 0 && heldTurnX == 0);
        aim = keptAim;
        originalStep = keptStep;
        active = keptActive;
        cameraReady = keptReady;
        stopping = keptStopping;
        viewMode = keptMode;
        puts("PASS: combat digital buffer cancels on release, held and analog input stay native; input "
             "handoff clears lean/rotation state");
    }
    puts("PASS: stationary native camera restoration, FOV/near restoration, missing-callback timeout retains "
         "pending restore");
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    assert(!fp::supportedExecutable(exe));
    assert(!fp::supportedExecutable(L"Z:\\missing-exanima-test.exe"));
    auto slot = (void**)(base + 0x3040a0);
    *slot = (void*)originalNav;
    assert(!replace((uintptr_t)slot, (void*)42, (void*)nav) && *slot == (void*)originalNav);
    assert(replace((uintptr_t)slot, (void*)originalNav, (void*)nav) && *slot == (void*)nav);
    assert(replace((uintptr_t)slot, (void*)nav, (void*)originalNav));
    puts("PASS: unsupported executable rejection and compare-exchange hook ownership/rollback");
    {
        const float d = 0.0174532925f;
        for (int wide = 0; wide < 2; ++wide)
            for (int b = -180; b <= 180; b += 15) {
                const float body = b * d;
                for (int r = -60; r <= 60; r += 1) {
                    const float heading = fp::wrapHeading(body + r * d);
                    const bool natural = fp::oppositeSide(heading, body, wide != 0);
                    assert(natural == (r > (wide ? 20 : 15)) || r == (wide ? 20 : 15));
                    assert(fp::oppositeSide(fp::flippedSideHeading(heading, body, wide != 0), body,
                                            wide != 0) != natural);
                }
            }
        puts("PASS: held attack-side binding inverts the native 15/20 degree classifier, including the yaw "
             "seam");
    }
    {
        // hiding: a listed mesh gets, right after the per-sector update
        // (0x58CD0), the pass mask of a mesh outside the view. Nothing is stored in
        // the mesh, so leaving the list (or F6/F7) shows it again with no undo.
        assert(fp::hiddenPassMask(0x11000001) == 0 && fp::hiddenPassMask(0x1000001) == 0);
        {
            uintptr_t v[6] = {9, 3, 7, 3, 1, 9};
            assert(fp::sortUnique(v, 6) == 4 && v[0] == 1 && v[1] == 3 && v[2] == 7 && v[3] == 9);
            assert(fp::sortedContains(v, 4, 7) && !fp::sortedContains(v, 4, 8) &&
                   !fp::sortedContains(v, 4, 0) && !fp::sortedContains(v, 0, 1));
        }
        assert(fp::helmetModeFromText(L"hide") == fp::HelmetMode::hide &&
               fp::helmetModeFromText(L" HIDE ") == fp::HelmetMode::hide);
        assert(fp::helmetModeFromText(L"show") == fp::HelmetMode::show &&
               fp::helmetModeFromText(L"hidden") == fp::HelmetMode::show &&
               fp::helmetModeFromText(L"") == fp::HelmetMode::show &&
               fp::helmetModeFromText(nullptr) == fp::HelmetMode::show);
        assert(fp::hideHeadWear(fp::HelmetMode::hide, fp::HelmetVerdict::clear) &&
               !fp::hideHeadWear(fp::HelmetMode::show, fp::HelmetVerdict::clear) &&
               fp::hideHeadWear(fp::HelmetMode::show, fp::HelmetVerdict::blocks) &&
               fp::hideHeadWear(fp::HelmetMode::show, fp::HelmetVerdict::unknown));
        assert(fp::rearmF6Accepted(1000, 0) && !fp::rearmF6Accepted(1100, 1000) &&
               fp::rearmF6Accepted(1250, 1000));
        // Drag kinds by the manager's flags; doors and lids held in place.
        {
            {
                const float o[3] = {0, 100, 0}, fwd[3] = {0, 0, 2}, flat[4] = {0, 1, 0, 50};
                float q[3];
                assert(fp::heldPointOnPlane(o, fwd, 200, flat, q) && q[0] == 0 && q[1] == 50 &&
                       q[2] == 200); // on the grab height, the grab's distance from the eye
                const float steep[3] = {0, -1, 1};
                assert(fp::heldPointOnPlane(o, steep, 100, flat, q) && std::fabs(q[2] - 100) < 1e-4f &&
                       q[1] == 50); // looking down: not pulled in
                const float level[3] = {1, 0, 0};
                assert(fp::heldPointOnPlane(o, level, 80, flat, q) && q[0] == 80 &&
                       q[1] == 50); // a ray along the plane: no division, still a point
                const float none[3] = {0, 0, 0}, bad[4] = {0, 0, 0, 1}, straightDown[3] = {0, -1, 0};
                assert(!fp::heldPointOnPlane(o, none, 80, flat, q) &&
                       !fp::heldPointOnPlane(o, fwd, 80, bad, q) &&
                       !fp::heldPointOnPlane(o, fwd, 0, flat, q) &&
                       !fp::heldPointOnPlane(o, straightDown, 80, flat, q));
            }
            const uintptr_t drag = base + 0x5b0000, object = base + 0x5b1000;
            *(uintptr_t*)(base + 0x4efdd0) = drag;
            *(unsigned*)(drag + 8) = 0x2000;
            *(uintptr_t*)(drag + 0x10) = object;
            uintptr_t got = 0;
            assert(dragKind(&got) == 1 && got == object && !strcmp(dragName, "0x2000")); // object drag
            for (unsigned door : {0x4000u, 0x10000u, 0x80000u}) {
                *(unsigned*)(drag + 8) = door;
                assert(dragKind() == 2);
            } // door / hand grabs
            *(unsigned*)(drag + 8) = 0x100;
            assert(dragKind() == 0);
            *(unsigned*)(drag + 8) = 0;
            assert(dragKind() == 0 && !dragName[0]);
            { // the four plane calls of the door and hinge handlers get the held point; any other caller the game's
                static int native = 0;
                static unsigned lastMessage = ~0u;
                native = 0;
                originalPlaneHit = [](float* out, uintptr_t, int, int, const float*) {
                    ++native;
                    out[0] = out[1] = out[2] = -1;
                };
                originalPickRay = [](uintptr_t, float, float, uintptr_t out) -> char {
                    float* d = (float*)(out + 0x14);
                    float* o = (float*)(out + 0x20);
                    d[0] = 0;
                    d[1] = 0;
                    d[2] = 1;
                    o[0] = 0;
                    o[1] = 100;
                    o[2] = 0;
                    return 0;
                };
                *(unsigned*)(drag + 8) = 0x4000;
                *(uintptr_t*)(drag + 0x10) = object;
                float* pivot = (float*)(drag + 0xd0);
                pivot[0] = 0;
                pivot[1] = 50;
                pivot[2] = 0;
                *(uintptr_t*)(base + 0x4efdd0) = drag;
                InterlockedExchange(&active, 1);
                InterlockedExchange(&stopping, 0);
                viewMode = 0;
                doorDistance = 150;
                lastDoorObject = 0;
                const float flat[4] = {0, 1, 0, 50};
                float out[3];
                planeHitFor(base + 0x11b5aa, out, 0, 0, 0, flat);
                assert(native == 0 && out[0] == 0 && out[1] == 50 && out[2] == 150 &&
                       std::fabs(*(float*)(base + 0x49c310) - std::sqrt(150.f * 150 + 50 * 50)) < 1e-2f);
                planeHitFor(base + 0x123456, out, 0, 0, 0, flat);
                assert(native == 1 && out[0] == -1); // another caller: the game's
                doorDistance = 5;
                planeHitFor(base + 0x11b5aa, out, 0, 0, 0, flat);
                assert(native == 1 && out[2] == 150); // over the pivot: the last good point
                *(unsigned*)(drag + 8) = 0x10000;
                doorDistance = 120;
                const float wall[4] = {1, 0, 0, 30};
                planeHitFor(base + 0x11bd90, out, 0, 0, 0, wall);
                assert(native == 1 && out[0] == 30 && out[2] == 120); // a lid's plane
                viewMode = 2;
                planeHitFor(base + 0x11bd90, out, 0, 0, 0, wall);
                assert(native == 2);
                viewMode = 0; // overhead: the game's
                // once a frame the handler runs as for a pointer move; a stop lets go; F6 lets go
                const uintptr_t keptUi = *(uintptr_t*)(base + 0x49a960);
                *(uintptr_t*)(base + 0x49a960) = base + 0x5b2000;
                const LONG keptNav = navThread;
                navThread = 0;
                originalHingeHandler = [](uintptr_t, uintptr_t, int m) -> unsigned long long {
                    lastMessage = unsigned(m);
                    return 0;
                };
                originalDoorHandler = originalHingeHandler;
                const uintptr_t keptFocus = *(uintptr_t*)(base + 0x49a980);
                *(uintptr_t*)(base + 0x49a980) =
                    *(uintptr_t*)(base + 0x4efc90); // the game viewport has the pointer
                heldDoorStep();
                assert(lastMessage == 0);
                {
                    *(uintptr_t*)(base + 0x49a980) =
                        base + 0x5b3000; // another view has the pointer (a menu): no move, as 0x121AF0
                    lastMessage = ~0u;
                    heldDoorStep();
                    assert(lastMessage == ~0u && doorDistance.load() > 0);
                    *(uintptr_t*)(base + 0x49a980) = *(uintptr_t*)(base + 0x4efc90);
                    lastMessage = ~0u;
                    heldDoorStep();
                    assert(lastMessage == 0);
                    *(uintptr_t*)(base + 0x49a980) = keptFocus;
                }
                InterlockedExchange(&stopping, 1);
                lastMessage = ~0u;
                heldDoorStep();
                assert(lastMessage == 0x80);
                InterlockedExchange(&stopping, 0);
                doorDistance = 100;
                lastMessage = ~0u;
                releaseHeldDoor();
                assert(lastMessage == 0x80 && doorDistance.load() == 0);
                doorDistance = 100;
                navThread = LONG(GetCurrentThreadId() + 1);
                lastMessage = ~0u;
                releaseHeldDoor();
                assert(lastMessage == ~0u && doorReleaseWanted.load()); // asked from another thread: deferred
                navThread = 0;
                heldDoorStep();
                assert(lastMessage == 0x80 && doorDistance.load() == 0 &&
                       !doorReleaseWanted.load()); // the game's thread lets go
                {
                    static int itemDrops = 0;
                    itemDrops = 0;
                    originalDragDrop = [](uintptr_t) {
                        ++itemDrops;
                    };
                    InterlockedExchange(&active, 1);
                    *(unsigned*)(drag + 8) = 0x4000;
                    doorDistance = 100;
                    lastMessage = ~0u;
                    dragDrop(drag);
                    assert(lastMessage == 0x80 && itemDrops == 0 &&
                           doorDistance.load() == 0); // combat toggle on a door: let go, never the item path
                    *(unsigned*)(drag + 8) = 0x10000;
                    lastMessage = ~0u;
                    dragDrop(drag);
                    assert(lastMessage == 0x80 && itemDrops == 0);
                    *(unsigned*)(drag + 8) = 0x2000;
                    lastMessage = ~0u;
                    dragDrop(drag);
                    assert(lastMessage == ~0u && itemDrops == 1); // an item: the game's path
                    *(unsigned*)(drag + 8) = 0x4000;
                }
                lastMessage = ~0u;
                heldDoorStep();
                assert(lastMessage == ~0u); // nothing held: nothing runs
                *(unsigned*)(drag + 8) = 0;
                doorDistance = 100;
                heldDoorStep();
                assert(doorDistance.load() == 0); // the drag ended: forgotten
                *(uintptr_t*)(base + 0x49a960) = keptUi;
                navThread = keptNav;
                InterlockedExchange(&active, 0);
                puts("PASS: doors and lids held in place: the held point on the drag's plane, only at the "
                     "handlers' calls, pivot guard, once a frame, released on stop and F6");
            }
            *(uintptr_t*)(base + 0x4efdd0) = 0;
            assert(dragKind() == 0);
        }
        // Grab range: eye-to-hit beyond the range refuses (NaN refuses too); the range stays within the game's limit.
        {
            assert(!fp::grabTooFar(0, 220) && !fp::grabTooFar(220, 220) && fp::grabTooFar(220.01f, 220) &&
                   fp::grabTooFar(NAN, 220));
            assert(fp::clampGrabRange(50) == fp::grabRangeMin &&
                   fp::clampGrabRange(999) == fp::grabRangeMax && fp::holdSphere(220) == 260.f &&
                   fp::holdSphere(300) == 300.f);
            static int picks = 0;
            originalGrabPick = [](uintptr_t, float, float, uintptr_t out, void*) -> char {
                ++picks;
                *(float*)(out + 0x2c) = 1000;
                return 1;
            };
            alignas(8) unsigned char farHit[0x40]{};
            InterlockedExchange(&active, 1);
            assert(grabPick(0, 0, 0, (uintptr_t)farHit, nullptr) == 1 && picks == 1 &&
                   grabsRefused == 0); // other callers keep far hits
            InterlockedExchange(&active, 0);
        }
        {
            f8WindowTick(false, false);
            f8WindowTick(false, true);
            assert(f8Window.load() ==
                   nullptr); // the Settings window is made only by F8, never at the title by itself
            puts("PASS: F8's Settings window is made only when F8 frees the cursor");
        }
        // the F8 settings page: each control's range, live values, the ini, raw input bookkeeping
        namespace R = panel::setting;
        {
            int seen
                [panel::ModSettings::
                     rows]{}; // every settings row is on exactly one sub-page, and only real rows are listed
            for (int g = 0; g < panel::ModSettings::groupCount; ++g) {
                const auto& grp = panel::ModSettings::group(g);
                for (int i = 0; i < grp.count; ++i) {
                    assert(grp.rows[i] >= 0 && grp.rows[i] < panel::ModSettings::rows);
                    ++seen[grp.rows[i]];
                }
            }
            for (int row = 0; row < panel::ModSettings::rows; ++row)
                assert(seen[row] == 1 && *panel::ModSettings::label(row) &&
                       panel::rowInfo(row).row == row); // the table is in row order
            assert(!strcmp(panel::ModSettings::group(0).name, "View") && panel::ModSettings::rows == 47 &&
                   panel::ModSettings::groupCount == 6 && panel::ModSettings::savesGroup < 0);
            puts("PASS: every F8 setting is on exactly one Settings page, and the row table is in row order");
        }
        { // Every row's change is seen by == (so it is applied and saved) and undone by its page's Reset page; resetting
            // one page leaves the others alone.
            const panel::ModSettings defaults;
            for (int row = 0; row < panel::ModSettings::rows; ++row) {
                panel::ModSettings t;
                assert(t.step(row, 1) || t.step(row, -1));
                assert(!(t == defaults));
                int page = -1;
                for (int g = 0; g < panel::ModSettings::groupCount; ++g) {
                    const auto& grp = panel::ModSettings::group(g);
                    for (int i = 0; i < grp.count; ++i)
                        if (grp.rows[i] == row) page = g;
                }
                assert(page >= 0 &&
                       !t.click(panel::ModSettings::resetControl, 0,
                                (page + 1) % panel::ModSettings::groupCount) &&
                       !(t == defaults)); // another page: untouched
                assert(t.click(panel::ModSettings::resetControl, 0, page) && t == defaults &&
                       !t.click(panel::ModSettings::resetControl, 0, page));
            }
            puts("PASS: every row's change counts as a change, and Reset page restores exactly that page's "
                 "defaults");
        }
        {
            panel::ModSettings s;
            assert(!s.step(R::fov, -100) || s.fov == 30);
            s = panel::ModSettings();
            for (int i = 0; i < 20; i++) s.step(R::fov, 1);
            assert(s.fov == 130);
            for (int i = 0; i < 40; i++) s.step(R::sensitivity, -1);
            assert(s.sensitivity == 10);
            assert(s.step(R::rawMouse, 0) && s.rawMouse && s.value(R::rawMouse) == L"on" &&
                   s.step(R::tpTight, 0) && !s.tpTight && s.step(R::tpTight, 0) && s.tpTight &&
                   s.step(R::swingCrosshair, 0) && s.hideSwingCrosshair && s.step(R::swingCrosshair, 0) &&
                   !s.hideSwingCrosshair && !s.step(60, 1) && s.nearClip == fp::nearClipDefaultTenths &&
                   s.step(R::nearClip, 9) && s.nearClip == fp::nearClipMaxTenths &&
                   s.value(R::nearClip) == L"1.0 cm" && s.crouchMode == 2 && s.step(R::crouchMode, 0) &&
                   s.crouchMode == 1 && s.step(R::crouchMode, 0) && s.crouchMode == 0 &&
                   s.step(R::crouchMode, 0) && s.crouchMode == 2 && s.spineFollow == 60 &&
                   s.step(R::spineFollow, 1) && s.spineFollow == 70 && s.freezeWindows &&
                   s.step(R::freezeWindows, 0) && !s.freezeWindows && s.value(R::freezeWindows) == L"off" &&
                   s.step(R::tpFov, 1) && s.tpFov == 80 && s.step(R::tpFov, -1) && s.tpFov == 75 &&
                   s.step(R::tpLag, -10) && s.tpLag == 0 && s.step(R::tpBlockers, 0) &&
                   s.tpBlockers == fp::blockersHide && s.step(R::tpDown, 0) && !s.tpDown &&
                   panel::ModSettings().headSteady == 70);
            {
                panel::ModSettings t;
                assert(t.tpBlockers == fp::blockersSmart && t.tpCombatAimFree &&
                       t.value(R::tpBlockers) == L"hide people; pull in");
                assert(t.step(R::tpBlockers, 1) && t.tpBlockers == fp::blockersHide &&
                       t.step(R::tpBlockers, 1) && t.tpBlockers == fp::blockersPush &&
                       t.step(R::tpBlockers, -1) && t.tpBlockers == fp::blockersHide);
                assert(t.step(R::tpCombatAim, 0) && !t.tpCombatAimFree &&
                       t.value(R::tpCombatAim) == L"held at the centre" &&
                       *panel::ModSettings::label(R::tpCombatAim));
                assert(t.eyeSetback == 0 && t.value(R::eyeSetback) == L"at the eyes" &&
                       t.step(R::eyeSetback, 3) && t.eyeSetback == 6 &&
                       t.value(R::eyeSetback) == L"6 cm behind the eyes" && t.step(R::eyeSetback, 100) &&
                       t.eyeSetback == fp::eyeSetbackMax && t.step(R::eyeSetback, -100) && t.eyeSetback == 0);
                assert(t.rotateHeldKey == VK_RBUTTON && t.value(R::rotateHeldKey) == L"right mouse" &&
                       t.step(R::rotateHeldKey, 1) && t.rotateHeldKey == VK_MBUTTON &&
                       t.step(R::rotateHeldKey, -1) && t.rotateHeldKey == VK_RBUTTON &&
                       t.step(R::rotateHeldKey, -1) && t.rotateHeldKey == 0 &&
                       t.value(R::rotateHeldKey) == L"unbound");
            }
            assert(s.attackCue && s.step(R::attackCue, 0) && !s.attackCue &&
                   s.value(R::attackCue) == L"off" && s.step(R::cueSize, 1) && s.cueSize == 110 &&
                   s.step(R::cueSize, 100) && s.cueSize == 200 && s.step(R::cueOpacity, -100) &&
                   s.cueOpacity == 20 && s.step(R::cueThrust, 0) && !s.cueThrust &&
                   panel::ModSettings::isNumber(R::cueSize) && panel::ModSettings::isNumber(R::cueOpacity) &&
                   !panel::ModSettings::isNumber(R::cueThrust) && s.rmbMode == 1 && s.step(R::rmbMode, 0) &&
                   s.rmbMode == 2 && s.step(R::rmbMode, 0) && s.rmbMode == 0 &&
                   s.value(R::rmbMode) == L"off" && s.step(R::rmbMode, 0) && s.rmbMode == 1 &&
                   !s.idleCrosshairAlways && s.value(R::idleCrosshair) == L"only with right mouse" &&
                   s.step(R::idleCrosshair, 0) && s.idleCrosshairAlways && s.headRoll == 80 &&
                   s.step(R::headRoll, -1) && s.headRoll == 70 && s.headBob == 30 && s.step(R::headBob, 1) &&
                   s.headBob == 40 && s.value(R::headBob) == L"40%" && s.step(R::headYaw, -10) &&
                   s.headYaw == 0 && s.value(R::headYaw) == L"off" &&
                   panel::ModSettings::isNumber(R::headPitch) && s.aimMarker && s.step(R::aimMarker, 0) &&
                   !s.aimMarker && s.flickCatchup == 50 && s.step(R::flickCatchup, 1) &&
                   s.flickCatchup == 60 && panel::ModSettings::isNumber(R::flickCatchup) &&
                   s.grabRange == 220 && s.step(R::grabRange, 1) && s.grabRange == 230 &&
                   s.value(R::grabRange) == L"230 cm" && s.step(R::grabRange, 100) && s.grabRange == 300 &&
                   panel::ModSettings::isNumber(
                       R::grabRange)); // wind-up arc rows, right mouse, crosshair out of combat
            { // Heading follows the mouse yaw at any pitch, even when the aim point lies behind the body (third
                // person looking steeply down from behind the shoulder).
                const int savedMode = viewMode.load();
                const bool savedFree = tpAimFree.load(), savedSet = tpAimSet;
                viewMode = 1;
                tpAimFree = true;
                tpAimSet = true;
                const uintptr_t body = base + 0x570000;
                float* pivot = (float*)(body + 0xab4);
                pivot[0] = 100;
                pivot[1] = 50;
                pivot[2] = 200;
                for (float yaw : {0.f, 1.f, -2.5f})
                    for (float pitch = 0; pitch >= -1.15f; pitch -= 0.05f) {
                        Aim probe{};
                        probe.root = body;
                        probe.yaw = yaw;
                        probe.pitch = pitch;
                        for (int i = 0; i < 3; ++i) probe.point[i] = pivot[i];
                        probe.point[0] -= 40 * std::sin(yaw);
                        probe.point[2] -= 40 * std::cos(yaw);
                        probe.point[1] = 0; // behind the body
                        float d[3];
                        headingToPoint(probe, d);
                        assert(std::fabs(d[0] - std::sin(yaw)) < 1e-5f && d[1] == 0 &&
                               std::fabs(d[2] - std::cos(yaw)) < 1e-5f);
                    }
                viewMode = savedMode;
                tpAimFree = savedFree;
                tpAimSet = savedSet;
                puts("PASS: body heading follows the mouse yaw through the whole pitch range, aim point "
                     "behind the body included");
            }
            { // Head wobble: the head turning with its gaze (the mouse) is not wobble, at any speed; the head
                // turning on its own stiffens the neck on the axes asked for, never on an axis set to 0.
                fp::HeadWobble w;
                fp::HeadAngles headNow, gaze;
                assert(fp::wobbleSlackScale(w, headNow, gaze, 60, 50, 30) ==
                       1.f); // first tick: nothing to compare
                for (int i = 1; i <= 30; ++i) {
                    headNow.yaw = gaze.yaw = float(i) * 12.f;
                    headNow.pitch = gaze.pitch = float(i) * -2.f;
                    assert(fp::wobbleSlackScale(w, headNow, gaze, 100, 100, 100) == 1.f);
                } // a fast mouse turn passes untouched
                headNow.yaw += 1.5f;
                const float yawOnly = fp::wobbleSlackScale(w, headNow, gaze, 0, 0, 100);
                assert(yawOnly < 0.2f); // 1.5 degrees of head-only yaw in a tick: stiff
                headNow.yaw += 1.5f;
                assert(fp::wobbleSlackScale(w, headNow, gaze, 100, 100, 0) ==
                       1.f); // the same with yaw set to 0: as before
                headNow.pitch += 0.05f;
                assert(fp::wobbleSlackScale(w, headNow, gaze, 100, 100, 100) ==
                       1.f); // below the threshold: as before
                headNow.roll += 0.8f;
                const float rollHalf = fp::wobbleSlackScale(w, headNow, gaze, 50, 0, 0);
                headNow.roll += 0.8f;
                const float rollFull = fp::wobbleSlackScale(w, headNow, gaze, 100, 0, 0);
                assert(rollHalf < 1.f && rollFull < rollHalf);                  // stronger setting, stiffer
                assert(std::fabs(fp::angleDelta(-179.f, 179.f) - 2.f) < 1e-4f); // across the wrap
                float m[12]{1, 0, 0, 0, 1, 0, 0, 0, 1};
                const fp::HeadAngles flat = fp::headAngles(m);
                assert(std::fabs(flat.yaw) < 1e-4f && std::fabs(flat.pitch) < 1e-4f && flat.roll == 0.f);
                puts("PASS: head wobble stiffens the neck per axis, never for mouse-commanded turns (any "
                     "speed), 0 = as before");
            }
            { // Flicks: ordinary motion 1:1; past 20 degrees of lead, input that widens it is eased, input that
                // closes it is not; a simulated flick ends on the aim with no overshoot.
                const float deg = 0.0174532925f;
                assert(fp::flickGain(10 * deg, 0.01f, 100) == 1.f &&
                       fp::flickGain(60 * deg, -0.01f, 100) == 1.f &&
                       fp::flickGain(60 * deg, 0.01f, 0) == 1.f);
                assert(fp::flickGain(50 * deg, 0.01f, 100) < 1.f &&
                       std::fabs(fp::flickGain(80 * deg, 0.01f, 100) - fp::flickMinGain) < 1e-5f &&
                       fp::flickGain(80 * deg, 0.01f, 50) > fp::flickMinGain);
                for (int strength : {0, 50, 100}) {
                    float target = 0, headYaw = 0, maxHeadYaw = 0;
                    for (int t = 0; t < 200; ++t) {
                        if (t < 6) {
                            const float step = 25 * deg;
                            target += step * fp::flickGain(target - headYaw, step, strength);
                        } // a 150-degree flick over six ticks
                        headYaw += std::clamp(target - headYaw, -1.8f * deg, 1.8f * deg);
                        maxHeadYaw = (std::max)(maxHeadYaw, headYaw);
                    } // the game turns the head at 1.8 degrees a tick
                    assert(std::fabs(headYaw - target) < 1e-4f && maxHeadYaw <= target + 1e-5f);
                } // lands on the aim, never past it
                {
                    float slow = 0;
                    for (int t = 0; t < 100; ++t)
                        slow += 0.2f * deg * fp::flickGain(0.5f * deg, 0.2f * deg, 100);
                    assert(std::fabs(slow - 20 * deg) < 1e-4f);
                } // slow turning: exactly 1:1
                bool shown = false;
                shown = fp::aimMarkerStep(shown, 3 * deg);
                assert(!shown);
                shown = fp::aimMarkerStep(shown, 5 * deg);
                assert(shown);
                shown = fp::aimMarkerStep(shown, 2 * deg);
                assert(shown);
                shown = fp::aimMarkerStep(shown, 1 * deg);
                assert(!shown);
                assert(fp::crosshairDrawn(false, false, false, false, true) &&
                       !fp::crosshairDrawn(false, false, false, false, false));
                puts("PASS: flicks: 1:1 below 20 degrees of lead, the widening part eased above it, no "
                     "overshoot, aim marker with hysteresis");
            }
            assert(fp::gameCursorSlot(0) == 0 && fp::gameCursorSlot(8) == 8 && fp::gameCursorSlot(-1) == -1 &&
                   fp::gameCursorSlot(99) ==
                       -1); // title, menus, overhead: the game's cursor; its own crosshair: none
            assert(
                !fp::crosshairDrawn(false, false, false, false) &&
                fp::crosshairDrawn(false, false, false, true) &&
                fp::crosshairDrawn(true, false, true, false) &&
                fp::crosshairDrawn(true, true, false, false) &&
                !fp::crosshairDrawn(
                    true, true, true,
                    true)); // exploring: hidden unless always; combat: shown, swings per setting // no Head bob row
            assert(s.step(R::tpDistance, 1) && s.tpDistance == 275 && s.step(R::tpShoulder, -10) &&
                   s.tpShoulder == -60 && s.value(R::tpShoulder) == L"60 cm left" && s.step(R::tpHeight, 1) &&
                   s.tpHeight == 25 && panel::ModSettings::isNumber(R::tpShoulder));
            {
                panel::ModSettings w;
                w.step(R::tpDistance, 100);
                w.step(R::tpShoulder, -100);
                w.step(R::tpHeight, 100);
                assert(w.tpDistance == 1200 && w.tpShoulder == -160 && w.tpHeight == 300);
                w.step(R::tpDistance, -100);
                w.step(R::tpHeight, -100);
                assert(w.tpDistance == 50 && w.tpHeight == -100);
            } // the wider third-person ranges // third person rows
            for (int i = 0; i < 20; i++) s.step(R::headSteady, -1);
            assert(s.headSteady == 0 && s.value(R::headSteady) == L"off (the game's)");
            for (int i = 0; i < 20; i++) s.step(R::headSteady, 1);
            assert(s.headSteady == 100);
            wchar_t keptPath[MAX_PATH];
            wcscpy_s(keptPath, settingsPath);
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            wcscat_s(tmp, L"fp-settings-test.ini");
            DeleteFileW(tmp);
            wcscpy_s(settingsPath, tmp);
            const float keptSens = sensitivity.load();
            const int keptFov = selectedFov.load();
            panel::ModSettings want = currentSettings();
            want.hideSwingCrosshair = false;
            want.sensitivity = 150;
            want.hideHelmet = true;
            want.grabRange = 150;
            want.fov = 90;
            const panel::ModSettings got = applySettings(want);
            assert(got.sensitivity == 150 && std::fabs(sensitivity.load() - 0.00375f) < 1e-7f &&
                   helmetMode.load() == fp::HelmetMode::hide && grabRange.load() == 150 &&
                   selectedFov.load() == 90);
            assert(!hideSwingCrosshair.load() &&
                   int(GetPrivateProfileIntW(L"Camera", L"HeadBob", -1, tmp)) == -1);
            assert(GetPrivateProfileIntW(L"Input", L"SensitivityPercent", 0, tmp) == 150 &&
                   GetPrivateProfileIntW(L"Input", L"GrabRange", -1, tmp) == 150);
            {
                wchar_t v[16];
                GetPrivateProfileStringW(L"View", L"Helmet", L"", v, 16, tmp);
                assert(!wcscmp(v, L"hide"));
            }
            { // The ini through the row table: every row written by applySettings reads back the same; a missing key,
                // an unknown word or an out-of-range number gives the default or the clamp; the old HideBlockers key is read.
                panel::ModSettings all;
                for (int row = 0; row < panel::ModSettings::rows; ++row)
                    if (!all.step(row, 1)) all.step(row, -1);
                assert(applySettings(all) == all);
                panel::ModSettings back = readSettings(tmp);
                assert(back == all);
                WritePrivateProfileStringW(L"ThirdPerson", L"Distance", L"99999", tmp);
                WritePrivateProfileStringW(L"View", L"Helmet", L"sideways", tmp);
                WritePrivateProfileStringW(L"Input", L"Crouch", L"7", tmp);
                WritePrivateProfileStringW(L"Camera", L"FOV", L"wide", tmp);
                WritePrivateProfileStringW(L"ThirdPerson", L"Blockers", nullptr, tmp);
                WritePrivateProfileStringW(L"ThirdPerson", L"HideBlockers", L"0", tmp);
                WritePrivateProfileStringW(L"ThirdPerson", L"CombatAim", L"center", tmp);
                const panel::ModSettings odd = readSettings(tmp), defaults;
                assert(odd.tpDistance == fp::tpDistanceMax && !odd.hideHelmet && odd.fov == defaults.fov &&
                       odd.tpBlockers == fp::blockersPush && !odd.tpCombatAimFree);
                assert(odd.crouchMode == fp::crouchModeFromText(L"7")); // the row's own words decide
                WritePrivateProfileStringW(L"ThirdPerson", nullptr, nullptr, tmp);
                assert(readSettings(tmp).tpDistance == defaults.tpDistance);
                applySettings(want);
            }
            puts("PASS: first-person.ini goes through the row table: every row reads back as written; bad "
                 "values fall back or clamp");
            { // settings outlive release folders: the user file starts from the release ini edited last, and keeps changes
                wchar_t t2[MAX_PATH];
                GetTempPathW(MAX_PATH, t2);
                const std::wstring folder =
                    std::wstring(t2) + L"fp-userini-" + std::to_wstring(GetCurrentProcessId());
                const std::wstring rel = folder + L"\\release", user = folder + L"\\Documents\\ExCamera";
                CreateDirectoryW(folder.c_str(), nullptr);
                CreateDirectoryW(rel.c_str(), nullptr);
                CreateDirectoryW((folder + L"\\Documents").c_str(), nullptr);
                auto touch = [](const std::wstring& f, const char* s, int ageSeconds) {
                    FILE* h = nullptr;
                    _wfopen_s(&h, f.c_str(), L"wb");
                    fputs(s, h);
                    fclose(h);
                    HANDLE fh =
                        CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    FILETIME ft;
                    GetSystemTimeAsFileTime(&ft);
                    ULARGE_INTEGER u{};
                    u.LowPart = ft.dwLowDateTime;
                    u.HighPart = ft.dwHighDateTime;
                    u.QuadPart -= ULONGLONG(ageSeconds) * 10000000ull;
                    ft.dwLowDateTime = u.LowPart;
                    ft.dwHighDateTime = u.HighPart;
                    SetFileTime(fh, nullptr, nullptr, &ft);
                    CloseHandle(fh);
                };
                for (const wchar_t* r : {L"\\ExanimaFirstPerson-old", L"\\ExanimaFirstPerson-edited",
                                         L"\\ExanimaFirstPerson-new"})
                    CreateDirectoryW((rel + r).c_str(), nullptr);
                touch(rel + L"\\ExanimaFirstPerson-old\\ExanimaFirstPerson.dll", "x", 3000);
                touch(rel + L"\\ExanimaFirstPerson-old\\first-person.ini", "[Camera]\r\nFOV=75\r\n",
                      3000); // never edited
                touch(rel + L"\\ExanimaFirstPerson-edited\\ExanimaFirstPerson.dll", "x", 2000);
                touch(rel + L"\\ExanimaFirstPerson-edited\\first-person.ini", "[Camera]\r\nFOV=105\r\n",
                      600); // edited after its build
                touch(rel + L"\\ExanimaFirstPerson-new\\ExanimaFirstPerson.dll", "x", 5);
                touch(rel + L"\\ExanimaFirstPerson-new\\first-person.ini", "[Camera]\r\nFOV=75\r\n",
                      5); // this build, fresh
                const std::wstring moduleIni = rel + L"\\ExanimaFirstPerson-new\\first-person.ini";
                const std::wstring path = userSettingsFile(moduleIni, user);
                assert(path == user + L"\\first-person.ini" &&
                       GetPrivateProfileIntW(L"Camera", L"FOV", 0, path.c_str()) ==
                           105); // carried over from the edited release
                WritePrivateProfileStringW(L"Camera", L"FOV", L"120", path.c_str());
                assert(userSettingsFile(moduleIni, user) == path &&
                       GetPrivateProfileIntW(L"Camera", L"FOV", 0, path.c_str()) ==
                           120); // a restart reads the change back
                assert(GetPrivateProfileIntW(L"Camera", L"FOV", 0, moduleIni.c_str()) ==
                       75); // the release's defaults untouched
                assert(userSettingsFile(moduleIni, L"") == moduleIni);
                WritePrivateProfileStringW(
                    nullptr, nullptr, nullptr,
                    path.c_str()); // flush the profile cache before removing the folder
                for (const wchar_t* f : {L"\\ExanimaFirstPerson-old", L"\\ExanimaFirstPerson-edited",
                                         L"\\ExanimaFirstPerson-new"}) {
                    DeleteFileW((rel + f + L"\\ExanimaFirstPerson.dll").c_str());
                    DeleteFileW((rel + f + L"\\first-person.ini").c_str());
                    RemoveDirectoryW((rel + f).c_str());
                }
                DeleteFileW(path.c_str());
                RemoveDirectoryW(user.c_str());
                RemoveDirectoryW((folder + L"\\Documents").c_str());
                RemoveDirectoryW(rel.c_str());
                RemoveDirectoryW(folder.c_str());
                puts("PASS: settings live in one user file: carried over from the last edited release, kept "
                     "across restarts");
            }
            DeleteFileW(tmp);
            wcscpy_s(settingsPath, keptPath);
            sensitivity = keptSens;
            selectedFov = keptFov;
            helmetMode = fp::HelmetMode::show;
            grabRange = fp::grabRangeDefault;
            hideSwingCrosshair = false;
            // raw input: a registration for a window that is gone is removed
            const HWND keptHooked = hookedWindow;
            hookedWindow = (HWND)0x12345;
            InterlockedExchange(&rawRegistered, 1);
            rawTarget = (HWND)0x777;
            rawWanted = true;
            rawReconcile();
            assert(!InterlockedCompareExchange(&rawRegistered, 0, 0) && rawTarget == nullptr);
            rawWanted = false;
            hookedWindow = keptHooked;
            {
                float bad[12]{};
                bad[3] = std::nanf("");
                assert(viewClearance(bad) < 0);
                assert(viewClearance(lastCamera) < 0);
            } // a bad pose, or no level grid (the tests): unknown -> the tight near plane
            // a retired camera still carrying our dynamic near plane (not 1) is recognised and restored
            {
                std::vector<unsigned char> cam(0x200, 0);
                const uintptr_t c = (uintptr_t)cam.data();
                Retired keptR[retiredSlots];
                memcpy(keptR, retired, sizeof retired);
                Retired& r = retired[0];
                r = Retired{};
                r.camera = c;
                r.writtenFov = 80;
                r.writtenNear = 2.5f;
                r.fov = 30;
                r.nearPlane = 10;
                *(float*)(c + 0x1c8) = 80;
                *(float*)(c + 0x1cc) = 2.5f;
                const unsigned before = reclaims.load();
                reclaimRetired(0, c);
                assert(reclaims.load() == before + 1 && *(float*)(c + 0x1c8) == 30 &&
                       *(float*)(c + 0x1cc) == 10 && !retired[0].camera);
                r = Retired{};
                r.camera = c;
                r.writtenFov = 30;
                r.writtenNear = 2.5f;
                *(float*)(c + 0x1cc) = 7;
                reclaimRetired(0, c);
                assert(*(float*)(c + 0x1cc) == 7); // the game rewrote it: left alone
                memcpy(retired, keptR, sizeof retired);
            }
            puts("PASS: F8 settings: ranges, live values, saved to first-person.ini; raw input dropped for a "
                 "window that is gone");
        }
        // head stabilisation: the head part's slack cap is lowered only inside the game's build call
        {
            assert(fp::steadySlackCap(1.f, 0) == 1.f &&
                   std::fabs(fp::steadySlackCap(1.f, 50) - 0.6f) < 1e-6f &&
                   std::fabs(fp::steadySlackCap(1.f, 100) - 0.2f) < 1e-6f &&
                   fp::steadySlackCap(1.f, 500) == fp::steadySlackCap(1.f, 100));
            assert(std::isnan(fp::steadySlackCap(std::nanf(""), 50)) && fp::steadySlackCap(0.f, 50) == 0.f);
            std::vector<uint8_t> rr(0x1400, 0), hh(0x800, 0);
            const uintptr_t r0 = (uintptr_t)rr.data(), h0 = (uintptr_t)hh.data();
            *(uintptr_t*)(r0 + 0x11e0) = h0;
            *(float*)(h0 + 0x77c) = 0.9f;
            static float seen = 0;
            auto build = [](uintptr_t r, float, unsigned char) {
                const uintptr_t hd = *(uintptr_t*)(r + 0x11e0);
                seen = hd ? *(float*)(hd + 0x77c) : -1.f;
            };
            const int keptAxes[3] = {headSteadyRoll.load(), headSteadyPitch.load(), headSteadyYaw.load()};
            headSteadyRoll = headSteadyPitch = headSteadyYaw = 0; // the master and roll comfort alone
            headSteady = 50;
            steadyHeadAround(r0, build, 0, 0);
            assert(std::fabs(seen - 0.54f) < 1e-6f &&
                   *(float*)(h0 + 0x77c) == 0.9f); // lowered for the call, put back
            {
                std::vector<uint8_t> mm(0x80, 0);
                const uintptr_t m0 = (uintptr_t)mm.data();
                *(uintptr_t*)(h0 + 0x18) = m0;
                float* hm = (float*)(m0 + 0x40);
                const float r = 12.f / 57.2957795f;
                hm[0] = std::cos(r);
                hm[1] = std::sin(r);
                hm[3] = -std::sin(r);
                hm[4] = std::cos(r);
                hm[8] = 1; // the head rolled 12 degrees
                steadyHeadAround(r0, build, 0, 0);
                assert(std::fabs(lastHeadRoll - 12.f) < 0.05f && seen < 0.54f - 1e-3f &&
                       std::fabs(seen - 0.54f * fp::rollSlackScale(lastHeadRoll)) < 1e-5f &&
                       *(float*)(h0 + 0x77c) == 0.9f);
                hm[0] = 1;
                hm[1] = 0;
                hm[3] = 0;
                hm[4] = 1;
                steadyHeadAround(r0, build, 0, 0);
                assert(lastHeadRoll < 0.05f && std::fabs(seen - 0.54f) < 1e-6f); // level: no extra
                headSteadyRoll = 100;
                hm[0] = std::cos(r);
                hm[1] = std::sin(r);
                hm[3] = -std::sin(r);
                hm[4] = std::cos(r);
                steadyHeadAround(r0, build, 0, 0); // rolled 12 degrees in one tick, roll steadiness on
                assert(seen < 0.54f * fp::rollSlackScale(lastHeadRoll) - 1e-3f &&
                       *(float*)(h0 + 0x77c) == 0.9f);
                headSteadyRoll = 0;
                hm[0] = 1;
                hm[1] = 0;
                hm[3] = 0;
                hm[4] = 1;
                steadyHeadAround(r0, build, 0, 0);
                *(uintptr_t*)(h0 + 0x18) = 0;
            } // roll comfort, on the head only
            headSteady = 0;
            steadyHeadAround(r0, build, 0, 0);
            assert(seen == 0.9f); // off: the game's value
            headSteady = 100;
            *(float*)(h0 + 0x77c) = std::nanf("");
            steadyHeadAround(r0, build, 0, 0);
            assert(std::isnan(seen)); // a value we do not know: untouched
            *(uintptr_t*)(r0 + 0x11e0) = 0;
            steadyHeadAround(r0, build, 0, 0);
            assert(seen == -1.f); // no head: the build still runs
            *(uintptr_t*)(r0 + 0x11e0) = h0;
            *(float*)(h0 + 0x77c) = 0.9f;
            headSteady = 50;
            assert(steadyFaults(r0, [](uintptr_t, float, unsigned char) { *(volatile int*)nullptr = 0; }) &&
                   *(float*)(h0 + 0x77c) == 0.9f); // put back after a fault
            steadyHeadAround(
                r0,
                [](uintptr_t r, float, unsigned char) {
                    *(float*)(*(uintptr_t*)(r + 0x11e0) + 0x77c) = 0.7f;
                },
                0, 0);
            assert(*(float*)(h0 + 0x77c) == 0.7f); // the game's own write kept
            headSteady = 50;
            headSteadyRoll = keptAxes[0];
            headSteadyPitch = keptAxes[1];
            headSteadyYaw = keptAxes[2];
            puts("PASS: head stabilisation lowers the head's slack cap only inside the game's build call and "
                 "puts it back");
        }
        // the drop-spot pick sees a short downward ray onto the hold point; grab start forgets the hold.
        {
            const uintptr_t player = base + 0x5b3000, pmesh = base + 0x5b4000, pscene = base + 0x5b5000;
            const uintptr_t heldRoot = *(uintptr_t*)(base + 0x4f0830);
            *(uintptr_t*)(base + 0x4f0830) = player;
            *(uintptr_t*)(player + 0x18) = pmesh;
            *(uintptr_t*)(pmesh + 0x130) = pscene;
            float* offset = (float*)(pscene + 0x64);
            offset[0] = offset[1] = offset[2] = 0;
            float* feet = (float*)(player + 0xab4);
            feet[0] = feet[1] = feet[2] = 0;
            holdApplied = 100;
            for (auto& e : eyeGlobal) e = 0.f; // the eye at the rays' origin (first person)
            auto ray = [](unsigned char* out, float dist) {
                memset(out, 0, 0x38);
                ((float*)(out + 0x14))[2] = 1;
                *(float*)(out + 0x2c) = dist;
                ((float*)(out + 8))[2] = dist;
            };
            alignas(8) unsigned char o1[0x40];
            ray(o1, 900);
            assert(dropSpot(1, (uintptr_t)o1) == 1); // far wall: hold point, ray down
            {
                const float* p = (float*)(o1 + 8);
                const float* d = (float*)(o1 + 0x14);
                const float* o = (float*)(o1 + 0x20);
                assert(p[0] == 0 && p[1] == 0 && p[2] == 100 && d[1] == -1 && d[0] == 0 && d[2] == 0 &&
                       o[1] == 4 && o[2] == 100 && *(float*)(o1 + 0x2c) == 4);
            }
            ray(o1, 900);
            assert(dropSpot(0, (uintptr_t)o1) == 1 && ((float*)(o1 + 8))[2] == 100); // open air: hold point
            ray(o1, 50);
            assert(dropSpot(1, (uintptr_t)o1) == 1 && ((float*)(o1 + 8))[2] == 50 &&
                   ((float*)(o1 + 0x14))[2] == 1); // nearer surface: native
            memset(o1, 0, sizeof o1);
            assert(dropSpot(0, (uintptr_t)o1) == 0); // off view: native
            feet[2] = -200;
            ray(o1, 900);
            dropSpot(1, (uintptr_t)o1);
            assert(fabsf(((float*)(o1 + 8))[2] - 60) < 1e-3f);
            feet[2] = 0; // reach sphere
            // heldDropWanted: exactly an 0x2000 drag of the remembered object, the mod owning the pointer.
            const uintptr_t drag = base + 0x5b0000, object = base + 0x5b1000;
            *(uintptr_t*)(base + 0x4efdd0) = drag;
            *(unsigned*)(drag + 8) = 0x2000;
            *(uintptr_t*)(drag + 0x10) = object;
            holdObject = object;
            InterlockedExchange(&active, 1);
            InterlockedExchange(&stopping, 0);
            InterlockedExchange(&inputEnabled, 1);
            InterlockedExchange(&cursorFree, 0);
            assert(heldDropWanted());
            *(unsigned*)(drag + 8) = 0x4000;
            assert(!heldDropWanted());
            *(unsigned*)(drag + 8) = 0x2000; // doors: no
            InterlockedExchange(&cursorFree, 1);
            assert(!heldDropWanted());
            InterlockedExchange(&cursorFree, 0); // F8: no
            *(int*)(base + 0x499860) = 1;
            assert(!heldDropWanted());
            *(int*)(base + 0x499860) = 0; // a window: no
            holdObject = base + 0x5b1100;
            assert(!heldDropWanted());
            holdObject = object; // another object: no
            InterlockedExchange(&active, 0);
            assert(!heldDropWanted());
            InterlockedExchange(&active, 1);
            // grab start forgets the hold; 17c gate unchanged.
            ray(o1, 900);
            assert(grabPickResult(base + 0x11a79e, 1, (uintptr_t)o1) == 0 && holdObject.load() == 0);
            ray(o1, 100);
            assert(grabPickResult(base + 0x11a79e, 1, (uintptr_t)o1) == 1);
            ray(o1, 900);
            assert(grabPickResult(base + 0x11e76c, 1, (uintptr_t)o1) == 0 &&
                   grabPickResult(base + 0x11e76c + 1, 1, (uintptr_t)o1) == 1);
            assert(hoverOutOfRange.load());
            ray(o1, 100);
            assert(grabPickResult(base + 0x11e76c, 1, (uintptr_t)o1) == 1 && !hoverOutOfRange.load());
            ray(o1, 900);
            grabPickResult(base + 0x11e76c, 1, (uintptr_t)o1);
            assert(hoverOutOfRange.load());
            grabPickResult(base + 0x11e76c, 0, (uintptr_t)o1);
            assert(!hoverOutOfRange.load());
            // third person: reach is measured from the eye (the pick ray starts at the camera behind it), and so is the hold
            {
                float keptEye[3];
                memcpy(keptEye, lastHeadGlobal, 12);
                lastHeadGlobal[0] = 0;
                lastHeadGlobal[1] = 0;
                lastHeadGlobal[2] = 300;
                publishReachHead(lastHeadGlobal);
                viewMode = 1;
                ray(o1, 350);
                assert(grabPickResult(base + 0x11a79e, 1, (uintptr_t)o1) ==
                       1); // 350 from the camera, 50 from the eye: in reach
                ray(o1, 600);
                assert(grabPickResult(base + 0x11a79e, 1, (uintptr_t)o1) == 0); // 300 from the eye: too far
                holdObject = object;
                assert(heldDropWanted());
                holdObject = 0; // third person: held too (from the eye)
                InterlockedExchange(&stopping, 1);
                viewMode = 0;
                windowProc(nullptr, WM_KEYDOWN, VK_F6, 1);
                assert(viewMode.load() == 0);
                InterlockedExchange(&stopping, 0); // F6 while stopping: never third person
                InterlockedExchange(&active, 1);
                viewMode = 0;
                memcpy(lastHeadGlobal, keptEye, 12);
                publishReachHead(lastHeadGlobal);
            }
            InterlockedExchange(&active, 0);
            ray(o1, 900);
            assert(grabPickResult(base + 0x11a79e, 1, (uintptr_t)o1) == 1); // top-down: stock
            holdApplied = 0;
            holdObject = 0;
            *(uintptr_t*)(base + 0x4efdd0) = 0;
            *(uintptr_t*)(base + 0x4f0830) = heldRoot;
            InterlockedExchange(&inputEnabled, 0);
        }
        // held objects: wheel step, reach sphere, hold point in the pick result, warp pass-through.
        {
            assert(fabsf(fp::holdTargetStep(0, 120, 60, 240) - 60 * 1.12f) < 1e-3f &&
                   fabsf(fp::holdTargetStep(112, -120, 60, 240) - 100) < 1e-3f);
            assert(fp::holdTargetStep(100, 120 * 30, 60, 240) == 240 &&
                   fp::holdTargetStep(100, -120 * 30, 60, 240) == 60);
            assert(fp::holdApproach(0, 150, 0.016f) == 150 && fp::holdApproach(100, 150, 0) == 100 &&
                   fp::holdApproach(100, 150, 1.f) > 149.f);
            const float a1 = fp::holdApproach(100, 150, 0.016f);
            assert(a1 > 100 && a1 < 120); // one frame: a step, not a jump
            const float o[3] = {0, 0, 0}, px[3] = {1, 0, 0}, nx[3] = {-1, 0, 0}, c0[3] = {0, 0, 0},
                        c1[3] = {500, 0, 0}, c2[3] = {500, 300, 0};
            assert(fabsf(fp::rayInSphere(o, px, c0, 260) - 260) < 1e-3f &&
                   fabsf(fp::rayInSphere(o, px, c1, 260) - 760) < 1e-3f);
            assert(fp::rayInSphere(o, nx, c1, 260) == -1 &&
                   fp::rayInSphere(o, px, c2, 100) == -1); // behind; never entered
            const uintptr_t drag = base + 0x5b0000, object = base + 0x5b1000, player = base + 0x5b3000;
            *(uintptr_t*)(base + 0x4efdd0) = drag;
            *(unsigned*)(drag + 8) = 0x2000;
            *(uintptr_t*)(drag + 0x10) = object;
            const uintptr_t heldRoot = *(uintptr_t*)(base + 0x4f0830);
            *(uintptr_t*)(base + 0x4f0830) = player;
            const uintptr_t pmesh = base + 0x5b4000, pscene = base + 0x5b5000;
            *(uintptr_t*)(player + 0x18) = pmesh;
            *(uintptr_t*)(pmesh + 0x130) = pscene;
            float* offset = (float*)(pscene + 0x64);
            offset[0] = offset[1] = offset[2] = 0;
            float* feet = (float*)(player + 0xab4);
            feet[0] = feet[1] = feet[2] = 0;
            for (auto& e : eyeGlobal) e = 0.f;
            static float pickDist = 150;
            static char pickHit = 1;
            static float placedZ = 0;
            originalPickRay = [](uintptr_t, float, float, uintptr_t out) -> char { // ray +z from the origin
                memset((void*)out, 0, 0x38);
                ((float*)(out + 0x14))[2] = 1;
                *(float*)(out + 0x2c) = pickDist;
                ((float*)(out + 8))[2] = pickDist;
                return pickHit;
            };
            originalDragPlace = [](uintptr_t, uintptr_t camera, float) -> int {
                alignas(8) unsigned char hitOut[0x40]{};
                pickRay(camera, 0, 0, (uintptr_t)hitOut);
                placedZ = ((float*)(hitOut + 8))[2];
                return 0;
            };
            InterlockedExchange(&active, 1);
            InterlockedExchange(&stopping, 0);
            holdObject = 0;
            const unsigned placed0 = holdPlacements.load();
            grabStartDistance = 150; // the grab's own pick met the object 150 from the eye
            dragPlace(object, 0, -100000);
            assert(placedZ == 150 && holdTarget.load() == 150 &&
                   holdPlacements == placed0); // new object: the grab's distance (the game's point there)
            holdTarget = 100;
            holdApplied = 100;
            dragPlace(object, 0, -100000);
            assert(placedZ == 100 && holdPlacements == placed0 + 1); // wheel-set distance
            pickDist = 80;
            dragPlace(object, 0, -100000);
            assert(placedZ == 80); // a surface nearer: native
            pickDist = 1e5f;
            pickHit = 0;
            dragPlace(object, 0, -100000);
            assert(placedZ == 100); // nothing hit: still held at 100
            feet[2] = -200;
            pickDist = 150;
            pickHit = 1;
            dragPlace(object, 0, -100000);
            assert(fabsf(placedZ - 60) < 1e-3f); // reach sphere (260 round the feet)
            feet[2] = -1200;
            offset[2] = 1000;
            dragPlace(object, 0, -100000);
            assert(fabsf(placedZ - 60) < 1e-3f); // feet are scene-local: + scene offset
            offset[2] = 0;
            feet[2] = 0;
            static bool offView = false;
            originalPickRay = [](uintptr_t, float, float, uintptr_t out) -> char {
                if (offView) return 0;
                memset((void*)out, 0, 0x38);
                ((float*)(out + 0x14))[2] = 1;
                *(float*)(out + 0x2c) = pickDist;
                ((float*)(out + 8))[2] = pickDist;
                return pickHit;
            };
            offView = true;
            placedZ = -1;
            {
                alignas(8) unsigned char junk[0x40];
                memset(junk, 0x7f, sizeof junk);
                placingThread = GetCurrentThreadId();
                assert(pickRay(0, -1, -1, (uintptr_t)junk) == 0 && ((float*)(junk + 0x14))[1] == 0);
                placingThread = 0;
            } // pointer off view: no placement, cleared ray
            offView = false;
            // third person: the ray starts at the camera 300 behind the eye; the object sits 100 from the eye, not the camera
            eyeGlobal[2] = 300.f;
            feet[2] = 300.f;
            pickDist = 1e5f;
            pickHit = 0;
            holdTarget = 100;
            holdApplied = 100;
            dragPlace(object, 0, -100000);
            assert(fabsf(placedZ - 400.f) < 1e-3f);
            feet[2] = 0.f; // the character (feet under the eye) in reach
            {
                alignas(8) unsigned char g[0x40]{};
                ((float*)(g + 0x14))[2] = 1;
                *(float*)(g + 0x2c) = 350;
                grabPickResult(base + 0x11a79e, 1, (uintptr_t)g);
                assert(fabsf(grabStartDistance.load() - 50.f) < 1e-3f);
            } // grabbed 350 from the camera: 50 from the eye
            eyeGlobal[2] = 0.f;
            pickDist = 150;
            pickHit = 1;
            grabStartDistance = 150;
            // Rotating: the rotate key's mouse turns go to the drag's target points (the object's pose as the game drags
            // it), about their centre, after the game's placement; without turns nothing is written, and the body never is.
            {
                float(*pts)[3] = (float(*)[3])(drag + 0x40);
                const float start[4][3] = {{110, 170, 0}, {100, 180, 0}, {100, 170, 10}, {95, 165, -5}};
                memcpy(pts, start, 48);
                const uintptr_t body = base + 0x5b6000;
                *(uintptr_t*)(object + 0x160) = body;
                memset((void*)body, 0x5a, 0x300);
                viewYaw = 0.f;
                viewPitch = 0.f;
                dragPlace(object, 0, -100000);
                assert(!memcmp(pts, start, 48)); // never turned: untouched
                heldTurnX = 100.f;
                dragPlace(object, 0, -100000);
                assert(heldTurnX.load() == 0.f && heldTurns.load() > 0);
                float c[3]{};
                for (auto& v : start)
                    for (int k = 0; k < 3; ++k) c[k] += 0.25f * v[k];
                float axes[12];
                fp::basis(0, 0, axes);
                for (int p = 0; p < 4; ++p) {
                    float want[9] = {
                        start[p][0] - c[0], start[p][1] - c[1], start[p][2] - c[2], 0, 0, 0, 0, 0, 0};
                    fp::rotateAxes(want, axes + 3, -100 * fp::heldRotateRadiansPerPixel);
                    for (int k = 0; k < 3; ++k) assert(fabsf(pts[p][k] - (c[k] + want[k])) < 1e-3f);
                }
                float after[4][3];
                memcpy(after, pts, 48);
                for (int f = 0; f < 100; ++f) dragPlace(object, 0, -100000);
                for (int p = 0; p < 4; ++p)
                    for (int k = 0; k < 3; ++k)
                        assert(fabsf(after[p][k] - pts[p][k]) <
                               1e-3f); // 100 more frames: not turned again (rounding only)
                for (int k = 0; k < 0x300; ++k)
                    assert(((unsigned char*)body)[k] == 0x5a); // the physics body: never written
                *(uintptr_t*)(object + 0x160) = 0;
            }
            // Flinging: let go while the wheel pushes and the game's release (copies the target points into the body and
            // makes its history equal: no velocity) is followed by a history for the push's speed. Not without a push.
            {
                const uintptr_t body = base + 0x5b6000;
                *(uintptr_t*)(object + 0x160) = body;
                holdObject = object;
                static uintptr_t releasedDrag = 0;
                originalDragDrop = [](uintptr_t d) {
                    const uintptr_t o = *(uintptr_t*)(d + 0x10);
                    const uintptr_t b = *(uintptr_t*)(o + 0x160);
                    memcpy((void*)(b + 0xf0), (void*)(d + 0x40), 48);
                    memcpy((void*)(b + 0x120), (void*)(b + 0xf0), 48);
                    memcpy((void*)(b + 0x150), (void*)(b + 0x120), 48);
                    *(uintptr_t*)(d + 0x10) = 0;
                    releasedDrag = d;
                };
                auto releaseWith = [&](float speed) {
                    *(uintptr_t*)(drag + 0x10) = object;
                    *(unsigned*)(drag + 8) = 0x2000;
                    pushSpeed = speed;
                    pushAt = GetTickCount64();
                    pushDir[0] = 0.f;
                    pushDir[1] = 0.f;
                    pushDir[2] = 1.f;
                    physicsStep = 1.f / 60;
                    dragDrop(drag);
                };
                const unsigned flung = flings.load();
                releaseWith(0.f);
                assert(releasedDrag == drag && !memcmp((void*)(body + 0x120), (void*)(body + 0xf0), 48) &&
                       flings.load() == flung); // no push: stops
                releaseWith(300.f);
                const float* now = (const float*)(body + 0xf0);
                const float* prev = (const float*)(body + 0x120);
                const float* prev2 = (const float*)(body + 0x150);
                for (int k = 0; k < 4; ++k) {
                    assert(fabsf(prev[k * 3 + 2] - (now[k * 3 + 2] - 5.f)) < 1e-3f &&
                           fabsf(prev2[k * 3 + 2] - (now[k * 3 + 2] - 10.f)) < 1e-3f &&
                           prev[k * 3] == now[k * 3]);
                }
                assert(flings.load() == flung + 1);
                pushAt = GetTickCount64() - 1000;
                *(uintptr_t*)(drag + 0x10) = object;
                *(unsigned*)(drag + 8) = 0x2000;
                dragDrop(drag); // a push that ended a second ago: stops
                assert(!memcmp((void*)(body + 0x120), (void*)(body + 0xf0), 48) &&
                       flings.load() == flung + 1);
                *(uintptr_t*)(drag + 0x10) = object;
                *(unsigned*)(drag + 8) = 0x2000;
                pushAt = GetTickCount64();
                holdObject = base + 0x5b1100;
                dragDrop(drag); // not the mod's hold: the game's
                assert(!memcmp((void*)(body + 0x120), (void*)(body + 0xf0), 48));
                holdObject = object;
                *(uintptr_t*)(drag + 0x10) = object;
                *(unsigned*)(drag + 8) = 0x2000;
                *(uintptr_t*)(object + 0x160) = 0;
                physicsStep = 0.f;
                pushSpeed = 0.f;
                puts("PASS: letting go during a push flings at its speed (one physics step of history); "
                     "otherwise the game's release stops it");
            }
            // The rotate key (right mouse by default) is the held item's while one is held: the game and the right-mouse
            // cursor never see it; with nothing held, combat retains native right mouse.
            {
                alignas(8) unsigned char playerBehavior[0xa00]{};
                const uintptr_t behavior = (uintptr_t)playerBehavior,
                                keptBehavior = *(uintptr_t*)(player + field::character::behaviour),
                                keptUiView = *(uintptr_t*)gameAt(fp::sig::uiView);
                const unsigned char keptCombat = *(unsigned char*)(player + field::character::combatStance);
                const LONG keptReady = cameraReady, keptInput = inputEnabled;
                // The surrounding placement fixture only needed mesh/feet. Its
                // window messages also need a live controlled behavior and view.
                *(uintptr_t*)(player + field::character::behaviour) = behavior;
                *(uintptr_t*)(behavior + field::behaviour::character) = player;
                *(uintptr_t*)(behavior + field::behaviour::view) = view;
                *(uintptr_t*)gameAt(fp::sig::uiView) = view;
                *(unsigned char*)(player + field::character::combatStance) = 1;
                cameraReady = 1;
                InterlockedExchange(&inputEnabled, 1);
                rotateHeldKey = VK_RBUTTON;
                rmbMode = 1;
                InterlockedExchange(&rmbFree, 0);
                assert(windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0) == 0 &&
                       !InterlockedCompareExchange(&rmbFree, 0, 0) &&
                       !(inputOwner.pendingButtons() & 2));
                *(unsigned*)(drag + 8) =
                    0; // let go of the item while the key is down: its release is still the item's
                assert(windowProc(nullptr, WM_RBUTTONUP, 0, 0) == 0);
                *(unsigned*)(drag + 8) = 0x2000;
                *(unsigned*)(drag + 8) = 0;
                inputOwner.discardButtons(); // nothing held: right mouse goes where it went before
                windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
                assert(inputOwner.pendingButtons() & 2);
                windowProc(nullptr, WM_RBUTTONUP, 0, 0);
                inputOwner.discardButtons();
                *(unsigned*)(drag + 8) = 0x2000;
                puts("PASS: holding an item: the rotate key turns it once per turn (not compounding), and "
                     "right mouse is consumed; with nothing held it is as before");
                InterlockedExchange(&inputEnabled, keptInput);
                cameraReady = keptReady;
                *(unsigned char*)(player + field::character::combatStance) = keptCombat;
                *(uintptr_t*)gameAt(fp::sig::uiView) = keptUiView;
                *(uintptr_t*)(player + field::character::behaviour) = keptBehavior;
            }
            holdTarget = 200;
            holdApplied = 200;
            *(unsigned*)(drag + 8) = 0;
            dragPlace(object, 0, -100000);
            assert(holdObject.load() == 0); // released: next grab starts fresh
            *(unsigned*)(drag + 8) = 0x2000;
            dragPlace(object, 0, -100000);
            assert(placedZ == 150 && holdTarget.load() == 150);
            *(unsigned*)(drag + 8) = 0x80000;
            dragPlace(object, 0, -100000);
            assert(placedZ == 150); // hand grab: native
            *(unsigned*)(drag + 8) = 0x2000;
            InterlockedExchange(&active, 0);
            dragPlace(object, 0, -100000);
            assert(placedZ == 150); // first person off: native
            alignas(8) unsigned char plain[0x40]{};
            pickRay(0, 0, 0, (uintptr_t)plain);
            assert(((float*)(plain + 8))[2] == 150); // other picks untouched
            static int warped = 0;
            originalCursorWarp = [](int x, int) {
                warped = x;
            };
            cursorWarp(7, 8);
            assert(warped == 7 && warpsSkipped == 0); // other warps pass
            *(uintptr_t*)(base + 0x4efdd0) = 0;
            *(uintptr_t*)(base + 0x4f0830) = heldRoot;
            holdObject = 0;
        }
        // tile covers: skipped only while a first-person frame is live.
        {
            static int covers = 0, depths = 0;
            originalCoverPass = [](uintptr_t) -> uint64_t {
                ++covers;
                return 1;
            };
            originalCoverDepth = [](uintptr_t) {
                ++depths;
            };
            skipCovers = false;
            assert(coverPass(1) == 1 && covers == 1);
            coverDepth(1);
            assert(depths == 1); // overhead: stock
            skipCovers = true;
            assert(coverPass(1) == 1 && covers == 1 && coverPassesSkipped == 1);
            coverDepth(1);
            assert(depths == 1);
            skipCovers = false;
        }
        // A controller stick is left alone; mouse buttons are no attack-side key; a starved swap hook is reported once.
        {
            const float none[3]{}, stick[3] = {0.3f, 0, 0.1f}, tiny[3] = {0.1f, 0, 0.1f},
                                   bad[3] = {NAN, 0, 0};
            assert(!fp::analogActive(none) && fp::analogActive(stick) && !fp::analogActive(tiny) &&
                   !fp::analogActive(bad));
        }
        assert(!fp::attackSideKeyValid(0) && !fp::attackSideKeyValid(1) && !fp::attackSideKeyValid(2) &&
               !fp::attackSideKeyValid(4) && fp::attackSideKeyValid(0x10) && fp::attackSideKeyValid('Q') &&
               !fp::attackSideKeyValid(255));
        {
            fp::SwapWatch w;
            bool fired = false;
            unsigned s = 0;
            fp::swapWatchStep(w, s, 100, true); // first observation primes
            for (int k = 0; k < 2; ++k) {
                s += 60;
                fired |= fp::swapWatchStep(w, s, 100, true);
            }
            assert(!fired && !w.starved); // two flat checks: not yet
            s += 60;
            assert(fp::swapWatchStep(w, s, 100, true) && w.starved);
            s += 60;
            assert(!fp::swapWatchStep(w, s, 100, true)); // third: once
            fp::SwapWatch v;
            for (int k = 0; k < 6; ++k) {
                s += 60;
                assert(!fp::swapWatchStep(v, s, 100 + k, true));
            } // swaps moving: fine
            fp::SwapWatch idle;
            for (int k = 0; k < 6; ++k)
                assert(!fp::swapWatchStep(idle, s, 100, true)); // no samples: paused, fine
            fp::SwapWatch off;
            for (int k = 0; k < 6; ++k) {
                s += 60;
                assert(!fp::swapWatchStep(off, s, 100, false));
            }
        } // mod inactive: not watched
        {
            float pose[12];
            fp::basis(0.3f, 0.2f, pose);
            float d[5][3];
            fp::helmetProbeDirections(pose, d);
            for (int i = 0; i < 3; ++i)
                assert(std::fabs(d[0][i] + pose[6 + i]) < 1e-5f); // centre ray: the view axis
            for (int k = 1; k < 5; ++k) {
                float dot = 0, n = 0;
                for (int i = 0; i < 3; ++i) {
                    dot += d[k][i] * d[0][i];
                    n += d[k][i] * d[k][i];
                }
                assert(std::fabs(n - 1) < 1e-5f &&
                       std::fabs(std::acos(dot) - fp::helmetConeDegrees * 0.0174532925f) < 1e-3f);
            }
        }
        {
            const float m[12] = {0, 1, 0, -1, 0, 0, 0, 0, 1, 10, 20, 30};
            float p[3] = {11, 20, 30}, o[3];
            fp::worldToLocalPoint(m, p, o);
            assert(o[0] == 0 && o[1] == -1 && o[2] == 0);
            const float d[3] = {0, 1, 0};
            fp::worldToLocalDirection(m, d, o);
            assert(o[0] == 1 && o[1] == 0 && o[2] == 0);
        }
        const uintptr_t manager = base + 0x560000, sectors = base + 0x561000, q1 = base + 0x562000,
                        q1List = base + 0x563000, children = base + 0x564000, helmet = base + 0x565000,
                        other = base + 0x566000;
        *(uintptr_t*)(base + 0x49b300) = manager;
        *(int*)(manager + 0x3c4) = 0;
        *(uintptr_t*)(manager + 0x3c8) = sectors;
        ((uintptr_t*)sectors)[0] = q1;
        *(int*)(q1 + 0x1c8) = 1;
        *(uintptr_t*)(q1 + 0x1d0) = q1List;
        ((uintptr_t*)q1List)[0] = helmet;
        ((uintptr_t*)q1List)[1] = other;
        *(int*)(mesh + 0x118) = 0;
        *(uintptr_t*)(mesh + 0x120) = children;
        ((uintptr_t*)children)[0] = helmet;
        strcpy_s((char*)(helmet + 0x140), 16, "Helmet");
        *(int*)(helmet + 0x118) = -1;
        *(uintptr_t*)(helmet + 0x130) = q1;
        // 0x58CD0 stand-in: every listed mesh in view this frame.
        originalSectorUpdate = [](uintptr_t q) -> uintptr_t {
            const uintptr_t list = *(uintptr_t*)(q + 0x1d0);
            for (int j = 0; j <= *(int*)(q + 0x1c8); ++j)
                *(unsigned*)(*(uintptr_t*)(list + j * 8) + 0xe0) = 0x11000001;
            return 5;
        };
        viewFault = 0;
        helmetMode = fp::HelmetMode::hide;
        viewFeatures(true);
        assert(hiddenHeadNodes == 1 && headWearNodes == 1 && hideMeshCount == 1 && hideMeshes[0] == helmet);
        assert(sectorUpdate(q1) == 5 && *(unsigned*)(helmet + 0xe0) == 0 &&
               *(unsigned*)(other + 0xe0) == 0x11000001 && maskedDraws == 1);
        assert(*(unsigned*)(helmet + 0xc) == 0); // flags are never written
        viewFeatures(false);
        assert(!hiddenHeadNodes && !hideMeshCount);
        sectorUpdate(q1);
        assert(*(unsigned*)(helmet + 0xe0) == 0x11000001); // shown again, nothing to undo
        // Helmet=show: a worn mesh is tested with its own ray routine from the eye.
        helmetMode = fp::HelmetMode::show;
        viewFeatures(true);
        assert(hiddenHeadNodes == 1 && helmetUnknown == 1); // flags8 & 0x89 != 0x89: cannot test, hidden
        *(unsigned*)(helmet + 8) = 0x89;
        *(uintptr_t*)(helmet + 0x3b0) = base + 0x2000;
        *(uintptr_t*)(helmet + 0x3b8) = 0x1234;
        {
            float* m = (float*)(helmet + 0x40);
            memset(m, 0, 48);
            m[0] = m[4] = m[8] = 1;
            m[11] = -5;
        }
        fp::basis(0, 0, lastCamera);
        lastCamera[9] = lastCamera[10] = lastCamera[11] = 0;
        static int rays = 0;
        static bool blockRay = false;
        static RayQuery first{};
        meshRay = [](uintptr_t, uintptr_t context, RayQuery* q) -> int {
            if (!rays++) first = *q;
            assert(context == 0x1234);
            return blockRay ? 3 : -1;
        };
        headWearCount = 0;
        viewFeatures(true);
        assert(!hiddenHeadNodes && !helmetBlocking && !helmetUnknown && rays == 5 && !hideMeshCount);
        // First ray: from the eye in the mesh's frame (eye - translation), along the view axis, probe length.
        assert(first.distance == fp::helmetProbeLength && first.origin[0] == 0 && first.origin[1] == 0 &&
               first.origin[2] == 5);
        assert(std::fabs(first.direction[2] + lastCamera[8]) < 1e-6f &&
               std::fabs(first.direction[0] + lastCamera[6]) < 1e-6f);
        blockRay = true;
        headWearCount = 0;
        viewFeatures(true);
        assert(hiddenHeadNodes == 1 && helmetBlocking == 1 && rays == 6); // first hit decides
        for (int i = 0; i < 29; ++i) viewFeatures(true);
        assert(rays == 6);
        viewFeatures(true);
        assert(rays == 7); // re-tested every 30 frames
        *(uintptr_t*)(helmet + 0x3b0) = 0x10;
        headWearCount = 0;
        viewFeatures(true); // routine outside the game image: never called
        assert(rays == 7 && helmetUnknown == 1 && hiddenHeadNodes == 1);
        viewFeatures(false);
        assert(!hideMeshCount); // F6 overhead: worn gear drawn normally
        meshRay = callMeshRay;
        *(int*)(mesh + 0x118) = -1;
        *(int*)(q1 + 0x1c8) = -1;
        headWearCount = 0;
        // body wear in view: only gear under torso / shoulder / upper-arm parts, only when the eye ray hits it.
        {
            assert(fp::bodyPartNumber("09Chest") == 9 && fp::bodyPartNumber("17RHand") == 17 &&
                   fp::bodyPartNumber("_SKIN") == -1 && fp::bodyPartNumber("wa") == -1 &&
                   fp::bodyPartNumber("09") == -1);
            assert(fp::torsoPart(9) && fp::torsoPart(11) && fp::torsoPart(15) && !fp::torsoPart(10) &&
                   !fp::torsoPart(17) && !fp::torsoPart(4) && !fp::torsoPart(-1));
            std::vector<uint8_t> mem(0x10000, 0);
            auto at = [&](int i) {
                return (uintptr_t)mem.data() + uintptr_t(i) * 0x800;
            };
            const uintptr_t wa = at(0), chest = at(1), pauldron = at(2), foot = at(3), boot = at(4),
                            skin = at(5), hand = at(6), sword = at(7), kids = at(8), owner = at(9);
            auto node = [&](uintptr_t n, const char* name, std::vector<uintptr_t> children) {
                strcpy_s((char*)(n + 0x140), 16, name);
                *(int*)(n + 0x118) = int(children.size()) - 1;
                if (!children.empty()) {
                    const uintptr_t list = kids + uintptr_t(n - at(0)) / 0x800 * 0x40;
                    for (size_t i = 0; i < children.size(); ++i) ((uintptr_t*)list)[i] = children[i];
                    *(uintptr_t*)(n + 0x120) = list;
                }
                *(unsigned*)(n + 8) = 0x89;
                *(uintptr_t*)(n + 0x3b0) = base + 0x2000;
                *(uintptr_t*)(n + 0x3b8) = 0x1234;
                *(uintptr_t*)(n + 0x130) = owner;
                float* m = (float*)(n + 0x40);
                m[0] = m[4] = m[8] = 1;
            };
            node(wa, "wa", {chest, foot, skin, hand});
            node(chest, "09Chest", {pauldron});
            node(pauldron, "pauldron r", {});
            node(foot, "04RFoot", {boot});
            node(boot, "boot r", {});
            node(skin, "_SKIN", {});
            node(hand, "17RHand", {sword});
            node(sword, "sword_2ha1", {});
            std::vector<uint8_t> me(0x1400, 0);
            const uintptr_t me0 = (uintptr_t)me.data();
            *(uintptr_t*)(me0 + 0x18) = wa;
            const uintptr_t keptRoot3 = *(uintptr_t*)(base + 0x4f0830);
            *(uintptr_t*)(base + 0x4f0830) = me0;
            fp::basis(0, -1.f, lastCamera);
            lastCamera[9] = lastCamera[10] = lastCamera[11] = 0; // looking down
            static int bodyRays = 0;
            meshRay = [](uintptr_t, uintptr_t, RayQuery* q) -> int {
                ++bodyRays;
                assert(q->distance == fp::bodyProbeLength);
                return 2;
            }; // everything is "in front"
            pendingHideCount = 0;
            bodyWearCount = 0;
            updateBodyWear(true);
            assert(bodyRays > 0 && bodyWearNodes == 1 && hiddenBodyNodes == 1 && pendingHideCount == 1 &&
                   pendingHide[0] == pauldron); // the boot, skin and sword are never candidates
            meshRay = [](uintptr_t, uintptr_t, RayQuery*) -> int {
                return -1;
            };
            pendingHideCount = 0;
            bodyWearCount = 0;
            updateBodyWear(true);
            assert(hiddenBodyNodes == 0 && pendingHideCount == 0); // looking ahead: drawn
            *(unsigned*)(pauldron + 8) = 0;
            pendingHideCount = 0;
            bodyWearCount = 0;
            updateBodyWear(true);
            assert(hiddenBodyNodes == 0); // cannot be tested: left drawn
            *(unsigned*)(pauldron + 8) = 0x89;
            fp::basis(0, 0, lastCamera);
            bodyWearCount = 0;
            pendingHideCount = 0;
            updateBodyWear(true);
            assert(bodyWearNodes == 0 && pendingHideCount == 0); // looking ahead: not even walked
            updateBodyWear(false);
            assert(bodyWearNodes == 0 && hiddenBodyNodes == 0);
            meshRay = callMeshRay;
            *(uintptr_t*)(base + 0x4f0830) = keptRoot3;
            pendingHideCount = 0;
            puts("PASS: body wear in view: only torso/shoulder/upper-arm gear the eye ray hits is hidden; "
                 "skin, hands, legs and feet never");
        }
        puts("PASS: hiding masks the pass mask after 0x58CD0 only for listed meshes, writes no flags, needs "
             "no undo; Helmet=hide hides head wear; Helmet=show hides only gear whose own ray routine is hit "
             "from the eye (or that cannot be tested), re-tested every 30 frames");
    }
    {
        // Attack-origin cue: accepted native family wins; otherwise the exact native
        // classifier inputs predict a new press; anything uncertain is neutral.
        const float d = 0.0174532925f;
        fp::CueInputs c;
        c.owned = c.combat = c.upright = true;
        c.clock = 10000;
        c.lastPress = 1000;
        c.bodyHeading = 0.3f;
        c.aimHeading = 0.3f;
        assert(fp::originCue(c) == fp::OriginCue::defaultSide);
        c.aimHeading = 0.3f + 16 * d;
        assert(fp::originCue(c) == fp::OriginCue::oppositeSide);
        c.wideThreshold = true;
        assert(fp::originCue(c) == fp::OriginCue::defaultSide); // 20-degree profile.
        c.aimHeading = 0.3f + 21 * d;
        assert(fp::originCue(c) == fp::OriginCue::oppositeSide);
        c.modifierHeld = true;
        assert(fp::originCue(c) == fp::OriginCue::defaultSide); // Held binding inverts.
        c.aimHeading = 0.3f;
        assert(fp::originCue(c) == fp::OriginCue::oppositeSide);
        c.modifierHeld = false;
        c.wideThreshold = false;
        c.bodyHeading = 3.1f;
        c.aimHeading = fp::wrapHeading(3.1f + 16 * d);
        assert(fp::originCue(c) == fp::OriginCue::oppositeSide); // Yaw seam.
        c.bodyHeading = 0.3f;
        c.aimHeading = 0.3f;
        for (int m = 5; m <= 7; ++m) {
            c.motion = m;
            c.attackHeld = true;
            assert(fp::originCue(c) == fp::OriginCue::defaultSide);
        }
        for (int m = 8; m <= 10; ++m) {
            c.motion = m;
            assert(fp::originCue(c) == fp::OriginCue::oppositeSide);
        } // Not the cleared +0x1BC.
        c.motion = 12;
        assert(fp::originCue(c) == fp::OriginCue::overhead);
        c.motion = 11;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.motion = 0;
        assert(fp::originCue(c) == fp::OriginCue::neutral); // Held, not accepted.
        c.attackHeld = false;
        c.alternateHeld = true;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.alternateHeld = false;
        c.pressBlocked = true;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.pressBlocked = false;
        c.upright = false;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.upright = true;
        c.lastPress = 9800;
        assert(fp::originCue(c) == fp::OriginCue::neutral); // Rapid-press window.
        c.lastPress = 9700;
        assert(fp::originCue(c) == fp::OriginCue::defaultSide);
        c.lastPress = 0xffffff00u;
        c.clock = 10;
        assert(fp::originCue(c) == fp::OriginCue::neutral); // No wrap to "old".
        c.clock = 10000;
        c.lastPress = 1000;
        c.aimHeading = NAN;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.aimHeading = 0.3f;
        c.combat = false;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        c.combat = true;
        c.owned = false;
        c.motion = 6;
        assert(fp::originCue(c) == fp::OriginCue::neutral);
        assert(fp::cueSide(fp::CueSide::right, 0) == fp::CueSide::left &&
               fp::cueSide(fp::CueSide::left, 0.2f) == fp::CueSide::right);
        assert(fp::cueSide(fp::CueSide::top, -0.4f) == fp::CueSide::top &&
               fp::cueSide(fp::CueSide::none, 0) == fp::CueSide::none);
        assert(fp::cueSide(fp::CueSide::right, 0.6f) == fp::CueSide::none &&
               fp::cueSide(fp::CueSide::left, NAN) == fp::CueSide::none);
        // Hand mapping: the origin follows the hand of the weapon that swings.
        {
            using fp::CueSide;
            using fp::OriginCue;
            fp::SwingHands hands;
            const auto side = [&](OriginCue c) {
                return fp::originSide(c, hands);
            };
            assert(side(OriginCue::overhead) == CueSide::top &&
                   side(OriginCue::neutral) == CueSide::none);     // Overhead needs no hand.
            assert(side(OriginCue::defaultSide) == CueSide::none); // Hand unknown: no opinion.
            hands.slot = 0;
            hands.hand = 0;
            hands.expectedHand = 0;
            hands.handSide = 2; // Right-handed one-hander.
            assert(side(OriginCue::defaultSide) == CueSide::right &&
                   side(OriginCue::oppositeSide) == CueSide::left); // Forehand / backhand.
            hands.hand = 1;
            hands.expectedHand = 1;
            hands.handSide = 1; // Left lead (two-handed class 0x30, entry 3).
            assert(side(OriginCue::defaultSide) == CueSide::left &&
                   side(OriginCue::oppositeSide) == CueSide::right);
            hands.expectedHand = 0; // Weapon in the left hand, native grip entry expects the right: disagree.
            assert(side(OriginCue::defaultSide) == CueSide::none &&
                   side(OriginCue::oppositeSide) == CueSide::none);
            hands.slot = 0;
            hands.hand = 0;
            hands.expectedHand = 0;
            hands.handSide = 2;
            hands.alternate = true; // Dual wield / fists: slot 0 forehand.
            assert(side(OriginCue::defaultSide) == CueSide::right);
            assert(side(OriginCue::oppositeSide) ==
                   CueSide::none); // Opposite with alternate swings slot 1, never slot 0.
            hands.slot = 1;
            hands.hand = 1;
            hands.expectedHand = 1;
            hands.handSide = 1;
            assert(side(OriginCue::oppositeSide) == CueSide::left); // Offhand forehand.
            assert(side(OriginCue::defaultSide) ==
                   CueSide::none); // Default family never swings slot 1 by prediction.
            hands.alternate = false;
            assert(side(OriginCue::oppositeSide) == CueSide::none);
            hands.slot = 0;
            hands.hand = 0;
            hands.expectedHand = 0;
            hands.handSide = 0;
            assert(side(OriginCue::defaultSide) == CueSide::none); // Unnamed hand.
            assert(fp::swingSlot(false, true) == 0 && fp::swingSlot(true, true) == 1 &&
                   fp::swingSlot(true, false) == 0);
            const uintptr_t held[2] = {0, 0}, parts[2] = {11, 22};
            const uintptr_t both[2] = {7, 7}, one[2] = {0, 7};
            assert(fp::weaponHand(11, held, parts, 0) == 0 &&
                   fp::weaponHand(22, held, parts, 0) == 1); // Fists are the hand parts.
            assert(fp::weaponHand(7, both, parts, 1) == 1 && fp::weaponHand(7, both, parts, 0) == 0 &&
                   fp::weaponHand(7, both, parts, -1) == -1); // Two-handed: lead.
            assert(fp::weaponHand(7, one, parts, 0) == 1 && fp::weaponHand(9, one, parts, 0) == -1 &&
                   fp::weaponHand(0, one, parts, 0) == -1);
            assert(fp::handSideFromName("17RHand") == 2 && fp::handSideFromName("18LHand") == 1);
            assert(fp::handSideFromName("10Head") == 0 && fp::handSideFromName("Hand") == 0 &&
                   fp::handSideFromName("17XHand") == 0 && fp::handSideFromName(nullptr) == 0);
            const char unterminated[16] = {'1', '7', 'R', 'H', 'a', 'n', 'd', 'x',
                                           'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
            assert(fp::handSideFromName(unterminated) == 0);
            puts("PASS: cue origin follows the swinging weapon's hand: right/left one-hander, left-lead "
                 "two-hander, dual wield, fists; mismatch/unknown neutral");
        }
        { // The wind-up arc's geometry (x, y, alpha strips) and its state from the native swing.
            float v[3 * 3 * 68];
            int counts[3];
            const int cap = int(sizeof v / sizeof(float));
            assert(!fp::cueArc(fp::CueSide::none, 500, 300, 24, 100, 0.5f, v, cap, counts) &&
                   !fp::cueArc(fp::CueSide::left, 500, 300, 0, 100, 0.5f, v, cap, counts));
            assert(fp::cueArc(fp::CueSide::left, 500, 300, 24, 100, 0, v, cap, counts) && counts[0] == 66 &&
                   counts[1] == 0 && counts[2] == 0); // track only at fill 0
            for (int i = 0; i < counts[0]; ++i)
                assert(v[i * 3] <= 500.01f &&
                       fabsf(v[i * 3 + 2] - 0.3f) < 1e-5f); // all on the left half, a faint outline
            assert(fp::cueArc(fp::CueSide::left, 500, 300, 24, 100, 0.5f, v, cap, counts) && counts[1] > 0 &&
                   counts[2] == counts[1]);
            {
                const float* s = v + (counts[0] + counts[1]) * 3;
                float minA = 1, maxA = 0, maxDy = 0;
                for (int i = 0; i < counts[2]; ++i) {
                    minA = fminf(minA, s[i * 3 + 2]);
                    maxA = fmaxf(maxA, s[i * 3 + 2]);
                    maxDy = fmaxf(maxDy, fabsf(s[i * 3 + 1] - 300));
                }
                assert(fabsf(maxA - 1.f) < 0.02f && fabsf(minA - 0.7f) < 0.02f &&
                       maxDy < 13.34f * 0.7072f + 0.5f);
            } // half fill: +-45 degrees about the middle, brightest there
            {
                const float* g = v + counts[0] * 3;
                for (int i = 0; i < counts[1]; i += 2)
                    assert(g[i * 3 + 2] == 0.f && g[i * 3 + 5] > 0.24f &&
                           fabsf(g[i * 3] - 500) < fabsf(g[i * 3 + 3] - 500));
            } // the glow fades toward the crosshair (inner edge 0)
            assert(fp::cueArc(fp::CueSide::right, 500, 300, 24, 100, 1, v, cap, counts) &&
                   fabsf(v[32 * 3] - (500 + 12.84f - 0.5f)) < 0.05f &&
                   fabsf(v[33 * 3] - (500 + 12.84f + 0.5f)) <
                       0.05f); // the track's middle (angle 0), inner edge, right of the crosshair
            assert(fp::cueArc(fp::CueSide::top, 500, 300, 24, 100, 1, v, cap, counts));
            for (int i = 0; i < counts[0]; ++i) assert(v[i * 3 + 1] <= 300.01f);
            assert(fp::cueArc(fp::CueSide::bottom, 500, 300, 24, 100, 1, v, cap, counts));
            for (int i = 0; i < counts[0]; ++i) assert(v[i * 3 + 1] >= 299.99f);
            assert(fp::cueArc(fp::CueSide::right, 0, 0, 24, 50, 1, v, cap, counts) &&
                   fabsf(v[33 * 3] - (9.84f + 1 + 0.5f)) < 0.05f); // 50%: never inside the ring (ring + 1 px)
            assert(fp::cueArc(fp::CueSide::right, 0, 0, 24, 200, 1, v, cap, counts) &&
                   fabsf(v[33 * 3] - (25.68f + 0.744f)) < 0.05f); // 200%: radius and line scale
            fp::Windup w;
            unsigned long long now = 1000;
            fp::windupStep(w, true, 6, -0.2f, 0, now);
            assert(w.phase == 1 && w.fill == 0 && w.p0 == -0.2f);
            fp::windupStep(w, true, 6, 0.05f, 0, now += 50);
            assert(w.phase == 1 && fabsf(w.fill - 0.5f) < 1e-4f); // (0.05+0.2)/(0.30+0.2)
            fp::windupStep(w, true, 6, 0.31f, 0, now += 50);
            assert(w.phase == 2 && w.fill == 1 && w.fadeFrom == now); // struck
            assert(fp::windupAlpha(w.phase, w.fadeFrom, now) == 1.f &&
                   fabsf(fp::windupAlpha(w.phase, w.fadeFrom, now + 125) - 0.5f) < 1e-4f &&
                   fp::windupAlpha(w.phase, w.fadeFrom, now + 250) == 0.f);
            fp::windupStep(w, true, 6, 0.6f, 0, now += 50);
            assert(w.phase == 2 && w.fill == 1); // the same swing does not restart
            fp::windupStep(w, true, 9, 0.0f, 0x10000, now += 50);
            assert(w.phase == 1 && w.fill == 0); // a chained swing starts over
            fp::windupStep(w, true, 9, 0.1f, 0x10000 | 0x2, now += 50);
            assert(w.phase == 2 && w.fill == 0); // bounced: fades from where it was (the last fill)
            fp::windupStep(w, false, 0, 0, 0, now += 300);
            assert(w.phase == 0 && fp::windupAlpha(w.phase, w.fadeFrom, now) == 0.f);
            fp::windupStep(w, true, 11, 0.2f, 0x2, now += 10);
            assert(w.phase == 1 &&
                   fabsf(w.fill - 0.5f) <
                       1e-4f); // thrust strikes at 0.40; an old interrupt bit is not a new one
            fp::windupStep(w, true, 11, NAN, 0, now += 10);
            assert(w.phase == 2); // unreadable progress: fade
            fp::windupStep(w, false, 0, 0, 0, now += 10);
            assert(w.phase == 2);
            fp::windupStep(w, false, 0, 0, 0, now += 300);
            assert(w.phase == 0);
            puts("PASS: wind-up arc: sides (left/right/top, thrust below), fill from the middle, gradient "
                 "stroke, size, scale bound; fill from progress, strike at 0.30/0.40, 250 ms fade, chain "
                 "restart, interrupt");
        }
        // Runtime publication reads the native fields read-only, player behaviour only.
        stopping = 0;
        active = 1;
        cameraReady = 1;
        cursorFree = 0;
        attackSideKey = 0;
        attackCue = true;
        *(uintptr_t*)(base + 0x49aa60) = view;
        *(int*)(base + 0x499860) = 0;
        *(unsigned char*)(root + 0x12fe) = 1;
        *(uintptr_t*)(root + 0x11e0) = head;
        aim.valid = true;
        aim.root = root;
        aim.head = head;
        *(unsigned*)(root + 0xdf4) = 0;
        *(unsigned*)(base + 0x49cdd0) = 0;
        *(unsigned*)(base + 0x49cdc0) = 0;
        *(unsigned char*)(bh + 0x96e) = 0;
        *(int*)(root + 0x1068) = 0;
        *(int*)(root + 0x9b8) = 0;
        *(unsigned*)(base + 0x49b970) = 10000;
        *(unsigned*)(bh + 0x8e4) = 1000;
        *(float*)(bh + 0x1b0) = 0.3f + 16 * d;
        *(float*)(root + 0xad4) = 0.3f;
        // Synthetic actor: grip table entries 1 (one-hander, hand 0), 3 (left-lead
        // two-hander, hand 1) and 5 (second weapon, hand 1); hand parts with meshes.
        const uintptr_t table = base + 0x2bac50, weapon = base + 0x560000, weapon2 = base + 0x560800;
        *(int*)(table + 0xb8 + 8) = 0;
        *(int*)(table + 3 * 0xb8 + 8) = 1;
        *(int*)(table + 5 * 0xb8 + 8) = 1;
        const uintptr_t partR = base + 0x561000, partL = base + 0x562000, meshR = base + 0x563000,
                        meshL = base + 0x564000;
        *(uintptr_t*)(partR + 0x18) = meshR;
        *(uintptr_t*)(meshR + 0x160) = partR;
        strcpy_s((char*)(meshR + 0x140), 16, "17RHand");
        *(uintptr_t*)(partL + 0x18) = meshL;
        *(uintptr_t*)(meshL + 0x160) = partL;
        strcpy_s((char*)(meshL + 0x140), 16, "18LHand");
        *(uintptr_t*)(root + 0xe78) = partR;
        *(uintptr_t*)(root + 0xe80) = partL;
        const auto arm = [&](int count, uintptr_t w0, uintptr_t w1, uintptr_t h0, uintptr_t h1, unsigned grip,
                             int entry) {
            *(int*)(root + 0x920) = count;
            *(uintptr_t*)(root + 0x928) = w0;
            *(uintptr_t*)(root + 0x930) = w1;
            *(uintptr_t*)(root + 0xe88) = h0;
            *(uintptr_t*)(root + 0xe90) = h1;
            *(unsigned*)(root + 0x1068) = grip;
            *(uintptr_t*)(root + 0x1070) = table + entry * 0xb8;
            *(int*)(root + 0x9b8) = 0;
            *(uintptr_t*)(root + 0x988) = 0;
        };
        const auto origin = [&] {
            return fp::CueSide(cueOrigin.load());
        };
        arm(1, weapon, 0, weapon, 0, 0,
            1); // Right-handed one-hander (grip word 0 keeps the 15-degree profile).
        const unsigned before = *(unsigned*)(bh + 0x1b0);
        publishCue(bh);
        assert(cueState == int(fp::OriginCue::oppositeSide) && *(unsigned*)(bh + 0x1b0) == before);
        assert(origin() == fp::CueSide::left && handIndex == 0 && handExpected == 0 && handSideSeen == 2 &&
               !strcmp(handNames[0], "17RHand") && !strcmp(handNames[1], "18LHand"));
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = weapon;
        publishCue(bh);
        assert(cueState == int(fp::OriginCue::defaultSide) && origin() == fp::CueSide::right);
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = base + 0x565000;
        publishCue(bh);
        assert(origin() == fp::CueSide::none);   // Unknown accepted weapon.
        arm(1, weapon, 0, weapon, weapon, 0, 3); // Left-lead two-hander: mirrored.
        publishCue(bh);
        assert(origin() == fp::CueSide::right && handIndex == 1);
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = weapon;
        publishCue(bh);
        assert(origin() == fp::CueSide::left);
        arm(1, weapon, 0, weapon, weapon, 0, 1); // Right-lead two-hander.
        *(int*)(root + 0x9b8) = 9;
        *(uintptr_t*)(root + 0x988) = weapon;
        publishCue(bh);
        assert(origin() == fp::CueSide::left);
        arm(1, weapon, 0, 0, weapon, 0,
            1); // Offhand-only: weapon moved to slot 0 while still in the left hand.
        publishCue(bh);
        assert(origin() == fp::CueSide::none && handIndex == 1 && handExpected == 0);
        arm(2, weapon, weapon2, weapon, weapon2, 0x90,
            1);                                // Dual wield: opposite family swings the left weapon.
        *(float*)(bh + 0x1b0) = 0.3f + 25 * d; // Grip word 0x90: the 20-degree profile.
        publishCue(bh);
        *(unsigned*)(bh + 0x1b0) = before;
        assert(cueState == int(fp::OriginCue::oppositeSide) && origin() == fp::CueSide::left &&
               handSlot == 1);
        *(int*)(root + 0x9b8) = 9;
        *(uintptr_t*)(root + 0x988) = weapon2;
        publishCue(bh);
        assert(origin() == fp::CueSide::left);
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = weapon;
        publishCue(bh);
        assert(origin() == fp::CueSide::right);
        arm(2, partR, partL, 0, 0, 0x80, 0); // Unarmed: fists are the hand parts.
        *(float*)(bh + 0x1b0) = 0.3f + 25 * d;
        publishCue(bh);
        *(unsigned*)(bh + 0x1b0) = before;
        assert(origin() == fp::CueSide::left);
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = partR;
        publishCue(bh);
        assert(origin() == fp::CueSide::right);
        arm(0, 0, 0, 0, 0, 0, 0);
        publishCue(bh);
        assert(origin() == fp::CueSide::none); // No weapon record.
        arm(1, weapon, 0, weapon, 0, 0, 1);
        *(uintptr_t*)(root + 0x1070) = table + 0xb8 + 4;
        publishCue(bh);
        assert(origin() == fp::CueSide::none); // Not a table entry.
        arm(1, weapon, 0, weapon, 0, 0, 1);
        strcpy_s((char*)(meshR + 0x140), 16, "17Rhnd");
        publishCue(bh);
        assert(origin() == fp::CueSide::none); // Unknown hand name.
        strcpy_s((char*)(meshR + 0x140), 16, "17RHand");
        *(uintptr_t*)(meshR + 0x160) = 0;
        publishCue(bh);
        assert(origin() == fp::CueSide::none); // Mesh not that part's.
        *(uintptr_t*)(meshR + 0x160) = partR;
        *(int*)(root + 0x9b8) = 6;
        *(uintptr_t*)(root + 0x988) = weapon;
        publishCue(bh);
        assert(cueState == int(fp::OriginCue::defaultSide) && origin() == fp::CueSide::right);
        const uintptr_t other = base + 0x568000;
        *(uintptr_t*)(other + 0x28) = base + 0x569000;
        publishCue(other);
        assert(cueState == int(fp::OriginCue::defaultSide)); // Another actor's behaviour never writes it.
        { // publishCue feeds the arc from the swing (motion, progress) and keeps its side through the fade.
            *(int*)(root + 0x9b8) = 6;
            *(uintptr_t*)(root + 0x988) = weapon;
            *(float*)(root + 0xa28) = 0.15f;
            *(unsigned*)(root + 0x1018) = 0;
            publishCue(bh);
            assert(cueArcPhase == 1 && fabsf(cueArcFill.load() - 0.5f) < 1e-4f &&
                   cueArcSide == int(fp::CueSide::right));
            *(float*)(root + 0xa28) = 0.35f;
            publishCue(bh);
            assert(cueArcPhase == 2 && cueArcFill.load() == 1.f);
            *(int*)(root + 0x9b8) = 0;
            *(uintptr_t*)(root + 0x988) = 0;
            publishCue(bh);
            assert(cueArcSide == int(fp::CueSide::right)); // side kept while it fades
            *(int*)(root + 0x9b8) = 11;
            *(float*)(root + 0xa28) = 0.1f;
            publishCue(bh);
            assert(cueArcPhase == 1 && cueArcSide == int(fp::CueSide::bottom));
            *(int*)(root + 0x9b8) = 0;
            publishCue(bh);
            cueThrust = false;
            *(int*)(root + 0x9b8) = 11;
            *(float*)(root + 0xa28) = -0.1f;
            publishCue(bh);
            assert(cueArcPhase != 1);
            cueThrust = true; // thrust arc off
            *(int*)(root + 0x9b8) = 0;
            *(float*)(root + 0xa28) = 0;
            publishCue(bh);
        }
        { // Right mouse frees the cursor: the state machine, then through the window procedure.
            fp::RightMouseFree s;
            bool ch = false;
            assert(fp::rmbStep(s, 1, fp::RmbEvent::down, true, ch) && ch && s.on); // hold: the press frees it
            assert(fp::rmbStep(s, 1, fp::RmbEvent::up, true, ch) && ch &&
                   !s.on); // the release locks it again (and is taken)
            assert(!fp::rmbStep(s, 1, fp::RmbEvent::down, false, ch) && !ch &&
                   !s.on); // not allowed (combat, overhead, F8...): the game gets the click
            assert(!fp::rmbStep(s, 1, fp::RmbEvent::up, true, ch) && !ch); // and its release
            assert(fp::rmbStep(s, 1, fp::RmbEvent::down, true, ch) && s.on);
            fp::rmbStep(s, 1, fp::RmbEvent::lost, false, ch);
            assert(ch && !s.on); // view switch / F8 mid-hold
            assert(fp::rmbStep(s, 1, fp::RmbEvent::up, true, ch) &&
                   !ch); // its release is still taken (the game never saw the press)
            assert(fp::rmbStep(s, 1, fp::RmbEvent::down, true, ch) && s.on);
            fp::rmbStep(s, 1, fp::RmbEvent::focusLost, false, ch);
            assert(ch && !s.on && !s.swallowUp); // alt-tab
            assert(!fp::rmbStep(s, 1, fp::RmbEvent::up, true, ch));
            assert(fp::rmbStep(s, 2, fp::RmbEvent::down, true, ch) && s.on &&
                   fp::rmbStep(s, 2, fp::RmbEvent::up, true, ch) &&
                   s.on); // toggle: stays free after the release
            assert(fp::rmbStep(s, 2, fp::RmbEvent::down, false, ch) && !s.on &&
                   fp::rmbStep(s, 2, fp::RmbEvent::up, false, ch) &&
                   !s.on); // the next click locks it (always allowed)
            assert(!fp::rmbStep(s, 0, fp::RmbEvent::down, true, ch) && !s.on); // off
            assert(fp::rmbStep(s, 1, fp::RmbEvent::down, true, ch) && s.on &&
                   fp::rmbStep(s, 1, fp::RmbEvent::down, false, ch) && !ch && s.on &&
                   s.swallowUp); // a release lost outside the window: the next press is still taken
            fp::rmbStep(s, 1, fp::RmbEvent::up, true, ch);
            assert(!s.on);
            assert(fp::windupShown(1, 0, 1000, 1200) == 1.f &&
                   fp::windupShown(1, 0, 1000, 1300) ==
                       0.f); // a swing cut off by a pause or a load is not left on screen
            assert(fp::rmbModeFromText(L"Toggle") == 2 && fp::rmbModeFromText(L"off") == 0 &&
                   fp::rmbModeFromText(L"hold") == 1 && fp::rmbModeFromText(L"x") == 1 &&
                   !wcscmp(fp::rmbModeText(2), L"toggle"));
            const int keptView = viewMode.load();
            viewMode = 0;
            rmbMode = 1;
            rmbFree = 0;
            cursorFree = 0;
            *(unsigned char*)(root + 0x12fe) = 0;
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 1 && cursorFreeNow() && !cursorFree);
            windowProc(nullptr, WM_RBUTTONUP, 0, 0);
            assert(rmbFree == 0 && !cursorFreeNow());
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 1);
            windowProc(nullptr, WM_KEYDOWN, VK_F8, 1);
            assert(rmbFree == 0 && cursorFree == 1);
            windowProc(nullptr, WM_RBUTTONUP, 0, 0);
            assert(rmbFree == 0); // F8 mid-hold: F8 takes over
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 0);
            windowProc(nullptr, WM_RBUTTONUP, 0, 0);
            cursorFree = 0; // F8 open: right mouse is the game's
            viewMode = 2;
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 0);
            windowProc(nullptr, WM_RBUTTONUP, 0, 0);
            viewMode = 0; // overhead: the game's walking
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 1);
            windowProc(nullptr, WM_KILLFOCUS, 0, 0);
            assert(rmbFree == 0); // alt-tab mid-hold
            *(unsigned char*)(root + 0x12fe) = 1;
            windowProc(nullptr, WM_RBUTTONDOWN, MK_RBUTTON, 0);
            assert(rmbFree == 0);
            windowProc(nullptr, WM_RBUTTONUP, 0, 0); // combat: holding it still walks
            viewMode = keptView;
            puts("PASS: right mouse frees the cursor: hold/toggle/off, release taken with its press, ends on "
                 "F6/F8/Esc/alt-tab, never in combat, overhead or with F8 open");
        }
        attackCue = false;
        publishCue(bh);
        assert(cueState == 0 && cueOrigin == 0);
        attackCue = true;
        cursorFree = 1;
        publishCue(bh);
        assert(cueState == 0 && cueOrigin == 0);
        cursorFree = 0; // F8: UI owns the cursor.
        puts("PASS: attack-origin cue follows accepted families 5-10/12, predicts with native classifier "
             "inputs and the held binding, neutral when "
             "held-unaccepted/thrust/rapid/alternate/blocked/fallen/rolled/free-cursor");
    }
    {
        // World hover/click/drag picks read the on-screen reticle only while first
        // person owns gameplay input and the reticle is current.
        stopping = 0;
        active = 1;
        inputEnabled = 1;
        cursorFree = 0;
        *(int*)(base + 0x499860) = 0;
        *(int*)(base + 0x4996b0) = 0;
        LONG x = -1, y = -1;
        reticleFrame = -1;
        assert(!reticleForUi(&x, &y) && x == -1);
        reticleX = 640;
        reticleY = 355;
        reticleFrame = 3;
        reticleTick = GetTickCount64();
        const unsigned reads = uiReticleReads;
        assert(reticleForUi(&x, &y) && x == 640 && y == 355 && uiReticleReads == reads + 1);
        { // Something grabbed with the cursor freed keeps its screen offset after right mouse is let go.
            std::vector<unsigned char> dragMem(0x40, 0), objMem(0x200, 0);
            const uintptr_t keptDrag = *(uintptr_t*)(base + 0x4efdd0);
            *(uintptr_t*)(base + 0x4efdd0) = (uintptr_t)dragMem.data();
            *(unsigned*)(dragMem.data() + 8) = 0x2000;
            *(uintptr_t*)(dragMem.data() + 0x10) = (uintptr_t)objMem.data();
            heldOffsetObject = (uintptr_t)objMem.data();
            heldOffsetX = 30;
            heldOffsetY = -20;
            assert(reticleForUi(&x, &y) && x == 670 && y == 335);
            *(unsigned*)(dragMem.data() + 8) = 0;
            assert(reticleForUi(&x, &y) && x == 640 && y == 355 &&
                   heldOffsetObject.load() == 0); // let go: the crosshair again
            *(uintptr_t*)(base + 0x4efdd0) = keptDrag;
        }
        cursorFree = 1;
        x = -1;
        assert(!reticleForUi(&x, &y) && x == -1);
        cursorFree = 0;
        inputEnabled = 0;
        assert(!reticleForUi(&x, &y));
        inputEnabled = 1;
        *(int*)(base + 0x4996b0) = 1;
        assert(!reticleForUi(&x, &y));
        *(int*)(base + 0x4996b0) = 0;
        *(int*)(base + 0x499860) = 1;
        assert(!reticleForUi(&x, &y));
        *(int*)(base + 0x499860) = 0;
        active = 0;
        assert(!reticleForUi(&x, &y));
        active = 1;
        reticleTick = GetTickCount64() - reticleFreshMs - 50;
        assert(!reticleForUi(&x, &y)); // Stale.
        // Clearance: a blocked reticle reports a pointer outside the viewport so the
        // native picks (bounds first) find nothing; undetermined keeps the reticle.
        reticleTick = GetTickCount64();
        reticleClear = 0;
        const unsigned blocked = pickBlocked;
        assert(reticleForUi(&x, &y) && x == -1 && y == -1 && pickBlocked == blocked + 1);
        reticleClear = -1;
        assert(reticleForUi(&x, &y) && x == 640 && y == 355);
        reticleClear = 1;
        assert(reticleForUi(&x, &y) && x == 640 && y == 355);
        puts("PASS: native UI pointer poll reads the fresh on-screen reticle only under first-person "
             "gameplay ownership; blocked clearance hides it from picks");
    }
    {
        // Pick clearance against synthetic level geometry (a one-sided wall quad,
        // front face toward the room, like the native ray-triangle test 0x602D0).
        static float wallZ = -0.5f;
        static unsigned queries = 0;
        levelRay = [](uintptr_t, const float* o, const float* d, float len) -> bool {
            ++queries;
            if (!(d[2] < 0)) return false; // Back faces are not hit.
            const float t = (wallZ - o[2]) / d[2];
            if (!(t > 0) || !(t < len)) return false;
            const float hx = o[0] + d[0] * t, hy = o[1] + d[1] * t;
            return std::fabs(hx) <= 100 && std::fabs(hy) <= 100;
        };
        levelRay2 = [](uintptr_t, const float*, const float*, float) -> bool {
            return false;
        };
        // Pure helper first.
        const auto query = [](const float* o, const float* d, float len) {
            return levelRay(0, o, d, len);
        };
        const float headPoint[3] = {0, -5, 8}, nearPt[3] = {0, 0, -1}, inside[3] = {0, 0, -0.2f};
        assert(fp::pickClear(headPoint, nearPt, query) == 0); // Eye pressed through the wall.
        assert(fp::pickClear(headPoint, inside, query) == 1); // Pick origin still in front of it.
        wallZ = -3;
        assert(fp::pickClear(headPoint, nearPt, query) == 1);
        wallZ = 12;
        assert(fp::pickClear(headPoint, nearPt, query) == 1); // Behind the head.
        const float nan3[3] = {NAN, 0, 0};
        assert(fp::pickClear(headPoint, nan3, query) == -1);
        assert(fp::pickClear(headPoint, headPoint, query) == 1);
        float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, o[3];
        assert(fp::pickOrigin(pose, 640, 360, 1280, 720, 360, 1, o) && o[0] == 0 && o[1] == 0 && o[2] == -1);
        assert(fp::pickOrigin(pose, 1000, 0, 1280, 720, 360, 1, o) && o[0] == 1 && o[1] == 1 && o[2] == -1);
        float o2[3];
        float d2[3] = {o[0], o[1], o[2]};
        float px, py; // Round-trips through the cursor projection.
        assert(fp::projectCursor(d2, pose, 1280, 720, 90, px, py) && std::fabs(px - 1000) < 0.01f &&
               std::fabs(py - 0) < 0.01f);
        assert(fp::pickOrigin(pose, 1000, 0, 1280, 720, fp::projectionScale(720, 90), 1, o2) &&
               std::fabs(o2[0] - 1) < 1e-5f && std::fabs(o2[1] - 1) < 1e-5f);
        assert(!fp::pickOrigin(pose, 1, 1, 0, 720, 360, 1, o));
        // Runtime path: pick camera must be ours and the scene must carry level collision.
        const uintptr_t vp = base + 0x570000, mgr = base + 0x571000, grid = base + 0x572000,
                        cam = base + 0x573000;
        *(uintptr_t*)(base + 0x4efc90) = vp;
        *(uintptr_t*)(vp + 0x1c0) = cam;
        *(uintptr_t*)(vp + 0x1f0) = mgr;
        *(unsigned*)(mgr + 0xc) = 0x2000000;
        *(uintptr_t*)(mgr + 0x320) = grid;
        const uintptr_t priorSaved = savedCamera;
        savedCamera = cam;
        renderedFov = 90;
        memcpy(lastCamera, pose, 48);
        lastHeadGlobal[0] = 0;
        lastHeadGlobal[1] = -5;
        lastHeadGlobal[2] = 8;
        wallZ = -0.5f;
        queries = 0;
        assert(pickClearance(640, 360, 1280, 720) == 0 && queries == 1);
        wallZ = -3;
        assert(pickClearance(640, 360, 1280, 720) == 1);
        wallZ = -0.5f;
        assert(pickClearance(1270, 10, 1280, 720) == 0); // Corner pixel: its own near point.
        lastCamera[11] = -5;
        lastHeadGlobal[2] = -4;
        assert(pickClearance(640, 360, 1280, 720) == 1); // Walked away from the wall.
        memcpy(lastCamera, pose, 48);
        lastHeadGlobal[2] = 8;
        *(uintptr_t*)(vp + 0x1c0) = cam + 0x100;
        assert(pickClearance(640, 360, 1280, 720) == -1); // Picks use another camera.
        *(uintptr_t*)(vp + 0x1c0) = cam;
        *(unsigned*)(mgr + 0xc) = 0;
        assert(pickClearance(640, 360, 1280, 720) == -1); // No level collision.
        *(unsigned*)(mgr + 0xc) = 0x2000000;
        *(uintptr_t*)(mgr + 0x320) = 0;
        assert(pickClearance(640, 360, 1280, 720) == -1);
        *(uintptr_t*)(mgr + 0x320) = grid;
        levelRay2 = [](uintptr_t, const float*, const float*, float) -> bool {
            return true;
        };
        wallZ = -3;
        assert(pickClearance(640, 360, 1280, 720) == 0); // Second level layer (0x1431C0) also blocks.
        levelRay = nullptr;
        assert(pickClearance(640, 360, 1280, 720) == -1);
        savedCamera = priorSaved;
        *(uintptr_t*)(base + 0x4efc90) = 0;
        levelRay = levelRay2 = nullptr;
        puts("PASS: pick clearance: head-to-near-plane segment through synthetic wall geometry blocks picks; "
             "clear/behind/moved-away pass; foreign camera or no level collision undetermined");
    }
    {
        // Camera owner change policy: every swap parks. Our camera
        // still current -> restore or nothing to restore; not current -> release.
        using fp::OwnerChange;
        assert(fp::classifyOwnerChange(true, true) == OwnerChange::parkRestore);
        assert(fp::classifyOwnerChange(true, false) == OwnerChange::parkClean);
        assert(fp::classifyOwnerChange(false, true) == OwnerChange::parkRelease &&
               fp::classifyOwnerChange(false, false) == OwnerChange::parkRelease);
        assert(int(OwnerChange::parkRestore) == 1 && int(OwnerChange::parkClean) == 2 &&
               int(OwnerChange::parkRelease) == 3); // status park_kind
        unsigned stable = 7;
        assert(fp::parkStep(true, false, false, false, stable) == fp::ParkStep::wait &&
               stable == 0); // Title: no controlled root.
        stable = 7;
        assert(fp::parkStep(true, true, true, false, stable) == fp::ParkStep::wait &&
               stable == 0); // Menu still open.
        assert(fp::parkStep(true, true, false, true, stable) ==
               fp::ParkStep::rearm); // New gameplay, even in a back-off.
        stable = 0;
        for (unsigned f = 1; f < fp::parkGraceFrames; ++f)
            assert(fp::parkStep(false, true, false, false, stable) == fp::ParkStep::wait);
        assert(fp::parkStep(false, true, false, false, stable) ==
               fp::ParkStep::rearm); // Same gameplay (any kind), after grace.
        stable = fp::parkGraceFrames - 1;
        assert(fp::parkStep(false, true, true, false, stable) == fp::ParkStep::wait &&
               stable == 0); // UI resets grace.
        stable = fp::parkGraceFrames - 1;
        assert(fp::parkStep(false, true, false, true, stable) == fp::ParkStep::wait &&
               stable == 0); // Back-off holds the park.
        // Live parks: the third within 10 s backs off; slower ones never do.
        fp::LiveParks lp;
        assert(!fp::recordLivePark(lp, 1000) && !fp::recordLivePark(lp, 2000) &&
               fp::recordLivePark(lp, 10999));
        assert(lp.count == 0 && !fp::recordLivePark(lp, 11000) &&
               !fp::recordLivePark(lp, 12000)); // Fresh window after a back-off.
        fp::LiveParks slow;
        for (uint64_t t = 0; t < 20; ++t) assert(!fp::recordLivePark(slow, t * 5000));
        puts("PASS: camera owner change always parks; same-gameplay re-arm after grace frames; repeated live "
             "parks back off instead of stopping");
    }
    {
        // Bad head frames: hold up to headHoldFrames, then park.
        fp::HeadHold hold;
        for (unsigned f = 1; f <= fp::headHoldFrames; ++f)
            assert(fp::holdStep(hold) == fp::HoldStep::hold && hold.run == f);
        assert(hold.holds == 1 && hold.heldFrames == fp::headHoldFrames &&
               hold.maxHold == fp::headHoldFrames);
        assert(fp::holdStep(hold) == fp::HoldStep::park && hold.parks == 1 && hold.run == 0);
        assert(fp::holdStep(hold) == fp::HoldStep::hold && hold.holds == 2);
        fp::holdEnd(hold); // A good frame ends the run.
        assert(fp::holdStep(hold) == fp::HoldStep::hold && hold.holds == 3 && hold.run == 1 &&
               hold.maxHold == fp::headHoldFrames && hold.parks == 1);
        puts("PASS: bad head frames hold the last view for a bounded run, then park");
    }
    {
        // F7 hold-to-stop.
        using fp::StopKey;
        assert(fp::stopKeyStep(0, 0, 5000, true) == StopKey::idle);
        assert(fp::stopKeyStep(1000, 0, 1500, true) == StopKey::holding);
        assert(fp::stopKeyStep(1000, 0, 1999, true) == StopKey::holding &&
               fp::stopKeyStep(1000, 0, 2000, true) == StopKey::stop);
        assert(fp::stopKeyStep(1000, 1300, 1400, true) == StopKey::tap);     // Released early: ignored.
        assert(fp::stopKeyStep(1000, 1000, 1400, true) == StopKey::tap);     // Same-tick tap.
        assert(fp::stopKeyStep(1000, 2100, 2150, true) == StopKey::stop);    // Released after the hold time.
        assert(fp::stopKeyStep(5000, 1300, 5200, true) == StopKey::holding); // Older release, new press.
        assert(fp::stopKeyStep(1000, 0, 1500, false) == StopKey::cancel &&
               fp::stopKeyStep(1000, 0, 3000, false) == StopKey::cancel);
        assert(fp::rearmAllowed(true, true, false));
        assert(!fp::rearmAllowed(false, true, false) && !fp::rearmAllowed(true, false, false) &&
               !fp::rearmAllowed(true, true, true));
        puts("PASS: F7 stops only after a one-second hold; taps ignored, focus loss cancels; F6 re-arm only "
             "after a clean stop");
    }
    {
        // Fault notice: immutable titles; banner never activates or takes clicks,
        // stays hidden unless the game is frontmost, and expires.
        const wchar_t* m = fp::faultMessage(true, true, false);
        assert(wcsncmp(m, L"Exanima - ", fp::faultTitlePrefix) == 0 && wcsstr(m, L"Overhead restored."));
        assert(wcsstr(fp::faultMessage(false, false, false), L"Restart Exanima.") &&
               wcsstr(fp::faultMessage(false, true, true), L"could not activate"));
        assert(wcsstr(fp::faultMessage(true, false, false), L"Restart Exanima."));
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExCameraNoticeTestGame";
        RegisterClassW(&wc);
        HWND game = CreateWindowExW(0, wc.lpszClassName, L"game", WS_POPUP, 100, 100, 1280, 720, nullptr,
                                    nullptr, wc.hInstance, nullptr);
        assert(game);
        hookedWindow = game;
        module = GetModuleHandleW(nullptr);
        showNotice(m + fp::faultTitlePrefix);
        assert(noticeWindow && notices == 1 &&
               wcscmp(noticeText, FP_SHOWN_NAME L" disabled: camera source lost. Overhead restored.") == 0);
        const LONG_PTR ex = GetWindowLongPtrW(noticeWindow, GWL_EXSTYLE);
        assert((ex & WS_EX_TOPMOST) && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_NOACTIVATE) &&
               (ex & WS_EX_LAYERED));
        RECT r;
        GetWindowRect(noticeWindow, &r);
        assert(r.left >= 100 && r.right <= 1380 && r.top >= 100 && r.bottom - r.top >= 40);
        assert(pumpNotice() && !IsWindowVisible(noticeWindow)); // Fake foreground is not the game: hidden.
        noticeStarted -= noticeLifetimeMs;
        assert(!pumpNotice() && !noticeWindow);
        showNotice(m + fp::faultTitlePrefix);
        const HWND first = noticeWindow;
        showNotice(m + fp::faultTitlePrefix);
        assert(noticeWindow && noticeWindow != first &&
               !IsWindow(first)); // Replacing never leaves a dead handle.
        noticeStarted -= noticeLifetimeMs;
        assert(!pumpNotice() && !noticeWindow);
        hookedWindow = nullptr;
        DestroyWindow(game);
        puts("PASS: fault notice text, non-activating click-through topmost banner over the game window, "
             "hidden off-front, bounded lifetime");
    }
    {
        // Cue drawing on an owned legacy GL context: draws into the default
        // framebuffer and restores every state it touches.
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExCameraCueGlTest";
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"cue", WS_POPUP, 0, 0, 128, 128, nullptr, nullptr,
                                 wc.hInstance, nullptr);
        if (w) ShowWindow(w, SW_SHOWNOACTIVATE); // Pixel ownership needs a visible window.
        HDC dc = w ? GetDC(w) : nullptr;
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        const int format = dc ? ChoosePixelFormat(dc, &pfd) : 0;
        HGLRC rc = (format && SetPixelFormat(dc, format, &pfd)) ? wglCreateContext(dc) : nullptr;
        if (!rc || !wglMakeCurrent(dc, rc)) {
            puts("SKIP: no OpenGL context for the cue draw test");
        } else {
            glViewport(1, 2, 3, 4);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 1, 1);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            glColorMask(1, 0, 1, 0);
            glClearColor(0, 0, 0, 1);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glColorMask(1, 0, 1, 0);
            *(uint32_t*)gameAt(fp::sig::combatCursorColour) =
                0xb44c4cffu; // the game's default combat colour (ff 4c 4c b4)
            const LONG beforeCueStop = InterlockedExchange(&stopping, 0);
            const bool drew = drawCue(fp::CueSide::left, 64, 64, 1.f, 1.f, 128,
                                      128); // the default 24 px cursor: radius 12.84
            if (!drew)
                puts("SKIP: context lacks the GL 2+ entry points the cue needs (cue stays neutral there)");
            else {
                GLint vp[4];
                glGetIntegerv(GL_VIEWPORT, vp);
                GLboolean m[4];
                glGetBooleanv(GL_COLOR_WRITEMASK, m);
                GLint program = -1, vao = -1, buffer = -1;
                glGetIntegerv(0x8B8D, &program);
                glGetIntegerv(0x85B5, &vao);
                glGetIntegerv(0x8894, &buffer);
                assert(vp[0] == 1 && vp[1] == 2 && vp[2] == 3 && vp[3] == 4 && m[0] && !m[1] && m[2] &&
                       !m[3]);
                assert(glIsEnabled(GL_SCISSOR_TEST) && !glIsEnabled(GL_BLEND) && glIsEnabled(GL_DEPTH_TEST) &&
                       program == 0 && vao == 0 && buffer == 0);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(1, 1, 1, 1);
                unsigned char strip[10 * 4]{};
                glReadBuffer(GL_BACK);
                glReadPixels(64 - 20, 128 - 64 - 1, 10, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             strip); // Across the left arc.
                unsigned char px[4]{};
                for (int i = 0; i < 10; ++i)
                    if (strip[i * 4] > px[0]) memcpy(px, strip + i * 4, 4);
                unsigned char clear[8 * 4]{};
                glReadPixels(64 + 12, 128 - 64 - 1, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             clear); // Right side stays clear.
                for (int i = 0; i < 8; ++i) assert(clear[i * 4] == 0);
                assert(px[0] > 150);
                assert(cueDraws >= 1 && glGetError() == GL_NO_ERROR);
                puts("PASS: attack-origin cue draws on the default framebuffer and restores viewport, "
                     "scissor, blend, depth, color mask, program, vertex array and buffer");
            }
            assert(retireCurrentCue());
            InterlockedExchange(&stopping, beforeCueStop);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
        }
        if (dc) ReleaseDC(w, dc);
        if (w) DestroyWindow(w);
    }
    {
        // walkScene keeps the old quadratic walk's exact output (order and parents) with a hash set: random
        // graphs with shared children, cycles, null entries and the root reached again, at many capacities.
        auto reference = [](uintptr_t scene, uintptr_t* out, unsigned* outParents, unsigned capacity) {
            if (!readable(scene, 0x2a0)) return 0u;
            unsigned count = 0, next = 0;
            out[count] = scene;
            outParents[count++] = ~0u;
            while (next < count) {
                uintptr_t node = out[next];
                unsigned index = next++;
                if (!readable(node, 0x2a0)) continue;
                int last = *(int*)(node + field::sceneNode::lastChild);
                uintptr_t list = *(uintptr_t*)(node + field::sceneNode::children);
                if (last < 0 || last >= 6000 || !readable(list, (last + 1) * 8)) continue;
                for (int i = 0; i <= last && count < capacity; ++i) {
                    uintptr_t child = *(uintptr_t*)(list + i * 8);
                    if (!child) continue;
                    bool seen = false;
                    for (unsigned j = 0; j < count; ++j)
                        if (out[j] == child) {
                            seen = true;
                            break;
                        }
                    if (!seen) {
                        out[count] = child;
                        outParents[count++] = index;
                    }
                }
            }
            return count;
        };
        std::mt19937 rng(12345);
        for (int trial = 0; trial < 60; ++trial) {
            const unsigned n = 1 + rng() % 300;
            std::vector<std::vector<unsigned char>> mem(n, std::vector<unsigned char>(0x2a0, 0));
            std::vector<std::vector<uintptr_t>> kids(n);
            for (unsigned k = 0; k < n; ++k) {
                const unsigned c = rng() % 6;
                for (unsigned j = 0; j < c; ++j)
                    kids[k].push_back(
                        rng() % 7 == 0 ? 0 : (uintptr_t)mem[rng() % n].data()); // nulls, repeats, cycles
                *(int*)(mem[k].data() + field::sceneNode::lastChild) = int(kids[k].size()) - 1;
                *(uintptr_t*)(mem[k].data() + field::sceneNode::children) =
                    kids[k].empty() ? 0 : (uintptr_t)kids[k].data();
            }
            for (unsigned capacity : {1u, 2u, 7u, 64u, 256u, 1024u}) {
                std::vector<uintptr_t> got(capacity), want(capacity);
                std::vector<unsigned> pa(capacity), pb(capacity);
                const unsigned ca = walkScene((uintptr_t)mem[0].data(), got.data(), pa.data(), capacity);
                const unsigned cb = reference((uintptr_t)mem[0].data(), want.data(), pb.data(), capacity);
                assert(ca == cb && ca <= capacity);
                for (unsigned k = 0; k < ca; ++k) assert(got[k] == want[k] && pa[k] == pb[k]);
            }
        }
        puts("PASS: walkScene: same nodes, order and parents as the quadratic walk on 60 random graphs x 6 "
             "capacities");
    }
    {
        // The settings folder, Documents\ExCamera: an ini kept under the old ExaniMate name is copied
        // over once, never over a newer one, and the old file stays (the ExaniMate editor uses that folder).
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring profile =
            std::wstring(tmp) + L"excamera-settings-test-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring oldDir = profile + L"\\Documents\\ExaniMate",
                           oldIni = oldDir + L"\\first-person.ini";
        SHCreateDirectoryExW(nullptr, oldDir.c_str(), nullptr);
        WritePrivateProfileStringW(L"Camera", L"FOV", L"95", oldIni.c_str());
        const std::wstring dir = userSettingsDir(profile);
        assert(dir == profile + L"\\Documents\\ExCamera");
        assert(GetPrivateProfileIntW(L"Camera", L"FOV", 0, (dir + L"\\first-person.ini").c_str()) ==
               95); // carried over
        WritePrivateProfileStringW(L"Camera", L"FOV", L"80", (dir + L"\\first-person.ini").c_str());
        WritePrivateProfileStringW(L"Camera", L"FOV", L"60", oldIni.c_str());
        userSettingsDir(profile);
        assert(GetPrivateProfileIntW(L"Camera", L"FOV", 0, (dir + L"\\first-person.ini").c_str()) ==
               80);                                                            // never overwritten
        assert(GetFileAttributesW(oldIni.c_str()) != INVALID_FILE_ATTRIBUTES); // the old one stays
        DeleteFileW((dir + L"\\first-person.ini").c_str());
        RemoveDirectoryW(dir.c_str());
        puts("PASS: settings move to Documents\\ExCamera: the old ExaniMate ini is copied once, never over a "
             "newer one");
        DeleteFileW(oldIni.c_str());
        RemoveDirectoryW(oldDir.c_str());
        RemoveDirectoryW((profile + L"\\Documents").c_str());
        RemoveDirectoryW(profile.c_str());
    }
    {
        // The game build check (activation): the layout comes from the game's file; a file without the game's
        // code refuses with every unresolved target listed, and changes no address; a detour already on a hooked
        // entry is refused by name.
        const uintptr_t savedBase = base;
        const fp::Layout known;
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        faultDetail[0] = 0;
        unsupportedNotice[0] = 0;
        assert(!resolveGame(self));
        assert(strstr(faultDetail, " of ") && strstr(faultDetail, "unresolved:") &&
               strstr(faultDetail, "navSampler (not found)"));
        assert(wcsstr(unsupportedNotice, L"not supported") &&
               wcsstr(unsupportedNotice, L"Nothing was changed"));
        assert(!layout.resolved && !memcmp(layout.rva, known.rva, sizeof known.rva));
        faultDetail[0] = 0;
        assert(!resolveGame(L"Z:\\missing-exanima-test.exe") && strstr(faultDetail, "could not be read"));
        puts("PASS: a file without the game's code: refused, unresolved targets listed, layout untouched");
        wchar_t gameExe[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"EXANIMA_EXE", gameExe, MAX_PATH)) {
            assert(resolveGame(gameExe) && layout.resolved && !layout.moved);
            std::vector<uint8_t> image;
            fp::ImageView fileView;
            assert(readGameFile(gameExe, image, fileView));
            base = (uintptr_t)image.data();
            // As loaded: the slots hold the game's functions at this base (the file has them at its preferred base).
            const fp::sig::Target slotPairs[3][2] = {{fp::sig::navSlot, fp::sig::navSampler},
                                                     {fp::sig::behaviorSlot, fp::sig::playerBehavior},
                                                     {fp::sig::pointSlot, fp::sig::aimPoint}};
            for (const auto& sp : slotPairs) *(uintptr_t*)gameAt(sp[0]) = gameAt(sp[1]);
            otherHooks = fp::OtherHooks::ask;
            chained = false;
            InterlockedExchange(&playerApproved, 0);
            assert(otherHooksCheck(gameExe) && !chained && adoptDispatch() &&
                   (uintptr_t)originalNav == gameAt(fp::sig::navSampler));
            puts("PASS: the real game resolves; its own code and slots are accepted as they are");
            // Another mod: a detour on a hooked entry and its own function in the camera slot.
            static unsigned char otherModCode[16];
            image[layout.rva[fp::sig::cursorAim]] = 0xe9;
            *(uintptr_t*)gameAt(fp::sig::navSlot) = (uintptr_t)otherModCode;
            faultDetail[0] = 0;
            otherHooksNotice[0] = 0;
            assert(!otherHooksCheck(gameExe) && !chained && strstr(faultDetail, "cursorAim") &&
                   strstr(faultDetail, "navSlot"));
            assert(wcsstr(otherHooksNotice, L"run ExCamera.exe to choose") &&
                   wcsstr(otherHooksNotice, L"cursorAim"));
            assert(!adoptDispatch()); // not chained: a taken slot is still a conflict
            otherHooks = fp::OtherHooks::refuse;
            InterlockedExchange(&playerApproved, 1);
            assert(!otherHooksCheck(gameExe) && !chained && wcsstr(otherHooksNotice, L"OtherHooks=refuse"));
            otherHooks = fp::OtherHooks::ask;
            assert(otherHooksCheck(gameExe) && chained && wcsstr(otherHooksNotice, L"running on top") &&
                   strstr(otherHooksNames, "cursorAim,navSlot"));
            assert(adoptDispatch() && (uintptr_t)originalNav == (uintptr_t)otherModCode &&
                   (uintptr_t)originalBehavior == gameAt(fp::sig::playerBehavior));
            InterlockedExchange(&playerApproved, 0);
            assert(otherHooksCheck(gameExe)); // chosen once: F6 re-arms stay chained
            *(uintptr_t*)gameAt(fp::sig::navSlot) = (uintptr_t)&nav;
            assert(!adoptDispatch()); // never its own replacement
            chained = false;
            otherHooks = fp::OtherHooks::allow;
            assert(otherHooksCheck(gameExe) && chained);
            puts("PASS: another mod's hooks: ask refuses without the player's yes and chains with it; refuse "
                 "wins; allow chains; slots call through to it");
            chained = false;
            otherHooks = fp::OtherHooks::ask;
            InterlockedExchange(&playerApproved, 0);
            otherHooksNotice[0] = 0;
            otherHooksNames[0] = 0;
            base = savedBase;
            layout = known;
            originalNav = nullptr;
            originalBehavior = nullptr;
            originalPoint = nullptr;
        } else
            puts(
                "SKIP: set EXANIMA_EXE to an Exanima.exe for the activation check against a real game build");
    }
}
