// Holding an object (held_object.h): distance from the eye along any ray, rotation, the rotate key. Portable.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "held_object.h"
#include <cassert>
#include <cstdio>

using namespace fp;

static bool near(float a, float b, float e = 1e-3f) {
    return std::fabs(a - b) < e;
}

static void orthonormal(const float* a) {
    for (int r = 0; r < 3; ++r)
        for (int q = 0; q < 3; ++q) {
            const float d = a[r * 3] * a[q * 3] + a[r * 3 + 1] * a[q * 3 + 1] + a[r * 3 + 2] * a[q * 3 + 2];
            assert(near(d, r == q ? 1.f : 0.f, 1e-4f));
        }
}

int main() {
    {
        const float eye[3] = {0, 170, 0}, fwd[3] = {0, 0, 1};
        assert(near(holdAlongRay(eye, fwd, eye, 150.f), 150.f)); // first person: the ray starts at the eye
        const float cam[3] = {40, 185, -250};                    // third person: behind and over the shoulder
        float d[3] = {-40, -15, 400};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& v : d) v /= l;
        const float t = holdAlongRay(cam, d, eye, 150.f);
        assert(t > 250.f && near(eyeDistance(cam, d, t, eye), 150.f)); // 150 from the eye, in front of it
        const float away[3] = {1, 0, 0};
        assert(holdAlongRay(cam, away, eye, 150.f) < 0.f); // pointing away from the character
        assert(holdAlongRay(eye, fwd, eye, 0.f) < 0.f);
        puts("PASS: the hold distance is from the eye, in first person and from a camera behind it");
    }
    {
        float a[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const float up[3] = {0, 1, 0};
        rotateAxes(a, up, 1.5707963f);
        assert(near(a[0], 0) && near(a[2], -1) && near(a[4], 1) && near(a[6], 1));
        for (int k = 0; k < 1000; ++k) rotateAxes(a, up, 0.37f);
        orthonormal(a);
        puts("PASS: axes turn about an axis and stay orthonormal");
    }
    {
        PushRate r; // the wheel pushing out 150 cm/s for half a second, then stopping
        float d = 100.f;
        for (unsigned long long ms = 0; ms <= 500; ms += 16) {
            r.step(d, 1000 + ms);
            d += 150.f * 0.016f;
        }
        assert(std::fabs(r.pushing(1500) - 150.f) < 15.f && r.pushing(1500 + flingFreshMs + 1) == 0.f);
        for (unsigned long long ms = 516; ms <= 800; ms += 16) r.step(d, 1000 + ms); // held still
        assert(r.pushing(1800) == 0.f);
        PushRate in; // pulling in is never a fling
        for (unsigned long long ms = 0; ms <= 300; ms += 16) in.step(300.f - float(ms) * 0.2f, ms);
        assert(in.pushing(300) == 0.f);

        const float now[4][3] = {{0, 100, 0}, {10, 100, 0}, {0, 110, 0}, {0, 100, 10}};
        float prev[4][3], prev2[4][3];
        const float v[3] = {0, 0, 300}; // 3 m/s forward, a 1/60 s step
        assert(flingHistory(v, 1.f / 60, now, prev, prev2));
        for (int k = 0; k < 4; ++k)
            assert(near(prev[k][2], now[k][2] - 5.f) && near(prev2[k][2], now[k][2] - 10.f));
        const float fast[3] = {0, 0, 9000}; // capped
        flingHistory(fast, 1.f / 60, now, prev, prev2);
        assert(near(now[0][2] - prev[0][2], flingMaxSpeed / 60.f));
        const float none[3] = {0, 0, 0};
        assert(!flingHistory(none, 1.f / 60, now, prev, prev2) && !flingHistory(v, 0.f, now, prev, prev2));
        puts("PASS: a push under way flings at its speed (capped); a stopped push, pulling in or an unknown "
             "step does not");
    }
    {
        assert(heldKeyFromText(L"rmb") == 0x02 && heldKeyFromText(L" RMB ") == 0x02 &&
               heldKeyFromText(L"mouse5") == 0x06);
        assert(heldKeyFromText(L"r") == 'R' && heldKeyFromText(L"7") == 7 && heldKeyFromText(L"alt") == 0x12);
        assert(heldKeyFromText(L"82") == 82 && heldKeyFromText(L"0") == 0 &&
               heldKeyFromText(L"nonsense") == 0 && heldKeyFromText(nullptr) == 0);
        assert(heldKeyName(0x02) == L"right mouse" && heldKeyName('R') == L"R" &&
               heldKeyName(0) == L"unbound");
        puts("PASS: the rotate key reads names, letters and codes (0 unbound); unknown text leaves it "
             "unbound");
    }
    puts("ALL HELD OBJECT TESTS PASSED");
}
