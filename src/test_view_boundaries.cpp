#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "camera_motion.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <cstring>
// the camera is exactly the head (fp::headPose). These were DirectionFilter/PositionFilter
// boundary tests; the boundaries that remain are the head's own: poles, continuity, the eye mount.
constexpr float pi = 3.14159265358979323846f;
static void headPitch(float pitch, float* h) {
    for (int i = 0; i < 12; ++i) h[i] = 0;
    h[0] = 1;
    h[4] = std::cos(pitch);
    h[5] = -std::sin(pitch);
    h[7] = std::sin(pitch);
    h[8] = std::cos(pitch);
}
static float angle(const float* a, const float* b) {
    float d = 0;
    for (int i = 0; i < 3; ++i) d += a[i] * b[i];
    return std::acos(std::max(-1.f, std::min(1.f, d))) * 180 / pi;
}
int main() {
    float h[12]{}, a[12]{}, b[12]{};
    const float eye[3]{};
    headPitch(89.99f * pi / 180, h);
    assert(fp::headPose(h, eye, a));
    headPitch(90.01f * pi / 180, h);
    assert(fp::headPose(h, eye, b));
    const float pole = std::max({angle(a, b), angle(a + 3, b + 3), angle(a + 6, b + 6)});
    assert(pole < .1f);
    printf("PASS: pole crossing 89.99 -> 90.01 degrees; largest camera axis change %.6f degrees\n", pole);
    float maxStep = 0;
    for (int hz : {30, 60, 120, 240}) {
        headPitch(0, h);
        assert(fp::headPose(h, eye, a));
        for (int n = 1; n <= hz * 8; ++n) {
            headPitch(float(n) / hz * pi / 2, h);
            assert(fp::headPose(h, eye, b));
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    float d = 0;
                    for (int k = 0; k < 3; ++k) d += b[i * 3 + k] * b[j * 3 + k];
                    assert(std::fabs(d - (i == j ? 1.f : 0.f)) < 2e-5f);
                }
            const float step = std::max({angle(a, b), angle(a + 3, b + 3), angle(a + 6, b + 6)});
            maxStep = std::max(maxStep, step);
            assert(step < 12.f);
            std::memcpy(a, b, sizeof(a));
        }
    }
    printf("PASS: continuous full pitch rotations at 30-240 Hz; largest axis step %.4f degrees (the head's "
           "own)\n",
           maxStep);
    // A head rolled onto its side: the view rolls with it.
    h[0] = 1;
    h[1] = h[2] = h[5] = h[6] = h[7] = 0;
    h[3] = -1;
    h[4] = 0;
    h[8] = 1;
    assert(fp::headPose(h, eye, b));
    assert(b[3] < -.999f && std::fabs(b[4]) < 1e-5f);
    // The eye is the mount the caller gives, unchanged: no lag, no offset.
    {
        float target[3] = {12.5f, 170.25f, -3}, p[12];
        headPitch(pi / 3, h);
        assert(fp::headPose(h, target, p));
        assert(p[9] == target[0] && p[10] == target[1] && p[11] == target[2]);
    }
    puts("PASS: roll follows the head; the eye mount is used exactly");
    puts("Boundary tests are numerical: world collision, other head geometry and helmets still need gameplay "
         "validation.");
}
