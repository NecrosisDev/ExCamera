// Numerical/reference tests only. No Exanima process, memory or input access.
// the camera is
// exactly the head (fp::headPose). DirectionFilter, PositionFilter and YawActivity are retired; what
// was tested here as camera smoothing now lives on the head (Head stabilisation, test_controls).
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "camera_motion.h"
#include "input_policy.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>

static constexpr float pi = 3.14159265359f;
static void rotation(float yaw, float pitch, float roll, float* h) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch),
                cr = std::cos(roll), sr = std::sin(roll);
    const float Y[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy}, P[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp},
                R[9] = {cr, sr, 0, -sr, cr, 0, 0, 0, 1};
    float m[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            m[i * 3 + j] = 0;
            for (int k = 0; k < 3; ++k) m[i * 3 + j] += R[i * 3 + k] * P[k * 3 + j];
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            h[i * 3 + j] = 0;
            for (int k = 0; k < 3; ++k) h[i * 3 + j] += m[i * 3 + k] * Y[k * 3 + j];
        }
}
int main() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-pi, pi), up(-1.4f, 1.4f);
    float worst = 0;
    for (int n = 0; n < 20000; ++n) {
        float h[12], pose[12];
        rotation(any(rng), up(rng), any(rng), h);
        const float eye[3] = {any(rng) * 100, any(rng) * 100, any(rng) * 100};
        if (n % 3 == 0)
            for (int i = 0; i < 9; ++i) h[i] *= 1.25f; // a scaled head matrix: unit axes all the same
        assert(fp::headPose(h, eye, pose));
        float back[3] = {-h[6], -h[7], -h[8]}, upv[3] = {h[3], h[4], h[5]};
        fp::unit(back);
        fp::unit(upv);
        for (int i = 0; i < 3; ++i) {
            worst = std::max({worst, std::fabs(pose[6 + i] - back[i]), std::fabs(pose[3 + i] - upv[i])});
            assert(pose[9 + i] == eye[i]);
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float d = 0;
                for (int k = 0; k < 3; ++k) d += pose[i * 3 + k] * pose[j * 3 + k];
                assert(std::fabs(d - (i == j ? 1.f : 0.f)) < 2e-5f);
            }
        const float det = pose[0] * (pose[4] * pose[8] - pose[5] * pose[7]) -
                          pose[1] * (pose[3] * pose[8] - pose[5] * pose[6]) +
                          pose[2] * (pose[3] * pose[7] - pose[4] * pose[6]);
        assert(det > 0.9999f);
    }
    assert(worst < 2e-5f);
    std::printf("PASS: 20,000 head poses (yaw, pitch, roll, scaled): camera back = head forward reversed, up "
                "= head up, eye exact; worst %.2g\n",
                double(worst));
    // Roll follows the head: a rolled head rolls the view (the horizon is no longer levelled).
    {
        float h[12]{}, pose[12];
        const float eye[3]{};
        rotation(0, 0, 0, h);
        assert(fp::headPose(h, eye, pose));
        assert(pose[0] == -1.f && pose[4] == 1.f &&
               pose[8] == -1.f); // the old basis(0,0): right (-1,0,0), up (0,1,0), back (0,0,-1)
        rotation(0, 0, 0.3f, h);
        assert(fp::headPose(h, eye, pose));
        assert(
            std::fabs(pose[1] + std::sin(0.3f)) <
            1e-5f); // the head's roll (this helper's up = (-sin, cos, 0)): right = up x back = (-cos, -sin, 0)
        float flat[12] = {1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
        assert(!fp::headPose(flat, eye, pose));
    } // up parallel to forward: refused
    std::puts("PASS: roll follows the head");
    // No history: the same head always gives the same camera, whatever came before.
    {
        float a[12], b[12], h[12]{}, g[12]{};
        const float eye[3]{1, 2, 3};
        rotation(1, 0.2f, 0.1f, h);
        rotation(-2, 0.5f, -0.4f, g);
        assert(fp::headPose(h, eye, a) && fp::headPose(g, eye, b) && fp::headPose(h, eye, b));
        assert(!std::memcmp(a, b, 48));
    }
    std::puts("PASS: no camera history: a pose depends on this frame's head only");
    {
        float h[12]{};
        for (float deg : {0.f, 3.f, 4.9f, 5.f, 8.f, 12.f, 20.f, 40.f}) {
            rotation(0.7f, 0.2f, deg / 57.2957795f, h);
            const float r = fp::headRollDegrees(h);
            assert(std::fabs(r - deg) < 0.05f);
            const float s = fp::rollSlackScale(r);
            if (deg <= 5.f)
                assert(s == 1.f);
            else
                assert(s < 1.f && s >= fp::rollCapAtFull);
        }
        assert(fp::rollSlackScale(6) > fp::rollSlackScale(10) &&
               fp::rollSlackScale(10) > fp::rollSlackScale(15) &&
               fp::rollSlackScale(20) == fp::rollCapAtFull && fp::rollSlackScale(90) == fp::rollCapAtFull);
        rotation(0, 1.5f, 0.5f, h);
        assert(fp::headRollDegrees(h) == 0.f);
    } // looking nearly straight up: no level, no roll damping
    std::puts("PASS: roll comfort engages only beyond 5 degrees of head roll, rising softly to a quarter of "
              "the cap at 20");
    std::puts("ALL CAMERA = HEAD TESTS PASSED (synthetic, not live gameplay acceptance)");
}
