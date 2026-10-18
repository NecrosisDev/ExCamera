#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "camera_motion.h"
#include <cassert>
#include <cwchar>
#include <cstdio>
#include <algorithm>
// The smallest depth difference a depth buffer of `bits` can tell apart at distance z, for a perspective
// projection with these near and far planes (d = f/(f-n) * (1 - n/z), one step of 2^-bits): what the
// near-clip floor costs far away.
static float depthResolution(float nearPlane, float farPlane, int bits, float z) {
    return std::ldexp(1.f, -bits) * (farPlane - nearPlane) * z * z / (farPlane * nearPlane);
}
int main() {
    {
        for (float pitch : {-1.15f, 0.f, 1.15f}) {
            float pose[12]{}, direction[3];
            fp::basis(0.7f, pitch, pose);
            for (float x : {-200.f, 0.f, 200.f})
                for (float y : {-100.f, 0.f, 100.f}) {
                    fp::cursorRay(pose, x, y, 1080.f, 75.f, direction);
                    float px = 0, py = 0;
                    assert(fp::projectCursor(direction, pose, 1920, 1080, 75, px, py));
                    assert(std::fabs(px - 960.f - x) < 0.001f && std::fabs(py - 540.f - y) < 0.001f);
                }
        }
        for (float pitch : {-1.15f, 1.15f})
            for (float y : {-432.f, 432.f}) {
                float pose[12]{}, ray[3];
                fp::basis(.7f, pitch, pose);
                fp::cursorRay(pose, 0, y, 1080, 130, ray);
                assert(std::fabs(fp::wrapPi(fp::cursorHeading(pose, ray) - .7f)) < 0.01f);
            }
        fp::AimBox a;
        float x = 40.f, y = -20.f;
        fp::aimBoxStep(a, x, y, 100.f, 50.f);
        assert(x == 0.f && y == 0.f && a.x == 40.f && a.y == -20.f);
        x = 80.f;
        y = -60.f;
        fp::aimBoxStep(a, x, y, 100.f, 50.f);
        assert(x == 20.f && y == -30.f && a.x == 100.f && a.y == -50.f);
        x = 5.f;
        y = 8.f;
        fp::aimBoxStep(a, x, y, 0.f, 0.f);
        assert(x == 5.f && y == 8.f && a.x == 0.f && a.y == 0.f);
        puts("PASS: configurable aim box consumes interior motion, forwards edge motion, and unprojects at "
             "extreme pitch");
    }
    float cp[12]{}, px, py;
    fp::basis(0, 0, cp);
    float targetPoint[3] = {0, 0, 100};
    assert(fp::projectCursor(targetPoint, cp, 1920, 1080, 75, px, py));
    assert(px == 960 && py == 540);
    targetPoint[0] = -10;
    targetPoint[1] = 10;
    assert(fp::projectCursor(targetPoint, cp, 1920, 1080, 75, px, py));
    assert(px > 960 && py < 540);
    float oldX = px, oldY = py;
    assert(fp::projectCursor(targetPoint, cp, 1920, 1080, 110, px, py));
    assert(px < oldX && py > oldY);
    targetPoint[2] = -100;
    assert(!fp::projectCursor(targetPoint, cp, 1920, 1080, 75, px, py));
    printf("PASS: native cursor projection and FOV scaling\n");
    assert(fp::clampFov(-1) == 30 && fp::clampFov(75) == 75 && fp::clampFov(500) == 130 &&
           fp::clampFov(130) == 130);
    // At 90 degrees the vertical projection's focal distance is half its height;
    // widening FOV must move an off-axis marker toward the screen center.
    assert(std::fabs(fp::projectionScale(1080, 90) - 540) < .001f);
    for (int fov = 45; fov < fp::fovMax; fov++)
        assert(fp::projectionScale(1080, float(fov)) > fp::projectionScale(1080, float(fov + 1)));
    { // At the widest field of view (130 degrees, vertical): a point 65 degrees above the view axis lands on the
        // top edge, 64 degrees inside it, and a centre point stays at the centre (crosshair, aim marker, arc).
        const float up = std::tan(65.f * 3.14159265f / 180.f), in = std::tan(64.f * 3.14159265f / 180.f);
        float edge[3] = {0, up * 100.f, -100.f}, inside[3] = {0, in * 100.f, -100.f},
              centre[3] = {0, 0, -100.f};
        float q[12]{};
        fp::basis(3.14159265f, 0, q); // looking down -z
        assert(fp::projectCursor(edge, q, 1920, 1080, 130, px, py) && std::fabs(py) < 0.5f &&
               std::fabs(px - 960) < 0.01f);
        assert(fp::projectCursor(inside, q, 1920, 1080, 130, px, py) && py > 1.f && py < 40.f);
        assert(fp::projectCursor(centre, q, 1920, 1080, 130, px, py) && px == 960 && py == 540);
        printf("PASS: projection at the 130-degree field of view\n");
    }
    { // the near plane's floor: the dynamic near never goes under it, and halving it halves depth precision far away
        fp::NearPlane n;
        assert(std::fabs(n.update(0.2f, 0.016f, 0.5f, 3.f) - 0.5f) < 1e-6f);
        n.reset();
        assert(std::fabs(n.update(0.2f, 0.016f, 1.f, 3.f) - 1.f) < 1e-6f);
        n.reset();
        assert(std::fabs(n.update(2.f, 0.016f, 0.5f, 3.f) - 1.f) <
               1e-6f); // no level grid: the caller passes 2, today's 1 cm
        assert(fp::clampNearClip(0) == fp::nearClipMinTenths &&
               fp::clampNearClip(99) == fp::nearClipMaxTenths &&
               fp::nearClipDefaultTenths >= fp::nearClipMinTenths);
        const float r1 = depthResolution(1.f, 25000.f, 24, 5000.f),
                    r05 = depthResolution(0.5f, 25000.f, 24, 5000.f);
        assert(std::fabs(r05 / r1 - 2.f) < 0.01f);
        printf("MODEL depth step at 50 m, 24-bit: %.2f cm at near 1, %.2f cm at near 0.5; at 20 m %.2f / "
               "%.2f cm\n",
               r1, r05, depthResolution(1.f, 25000.f, 24, 2000.f),
               depthResolution(0.5f, 25000.f, 24, 2000.f));
        printf("PASS: near plane floor and depth precision\n");
    }
    {
        float p[12]{};
        for (int y = -30; y <= 30; y++)
            for (int x = -11; x <= 11; x++) {
                fp::basis(y * .1f, x * .1f, p);
                assert(p[1] == 0);
                assert(p[4] > 0);
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) {
                        float dot = 0;
                        for (int k = 0; k < 3; k++) dot += p[i * 3 + k] * p[j * 3 + k];
                        assert(std::fabs(dot - (i == j ? 1.f : 0.f)) < 1.e-5f);
                    }
            }
    }
    // the camera is exactly the head: forward = head forward, up = head up, even under roll.
    for (int n = 0; n < 100; n++) {
        float head[12]{}, camera[12]{};
        fp::basis(n * .06f, .8f * std::sin(n * .1f), head);
        for (int i = 6; i < 9; i++) head[i] = -head[i];
        const float eye[3] = {1, 2, 3};
        assert(fp::headPose(head, eye, camera));
        {
            const float y = n * .06f;
            assert(std::fabs(camera[0] + std::cos(y)) < 1e-5f && std::fabs(camera[1]) < 1e-5f &&
                   std::fabs(camera[2] - std::sin(y)) < 1e-5f);
        } // right row: the old basis's (-cos yaw, 0, sin yaw)
        for (int i = 0; i < 3; i++)
            assert(std::fabs(camera[6 + i] + head[6 + i]) < 1.e-5f &&
                   std::fabs(camera[3 + i] - head[3 + i]) < 1.e-5f && camera[9 + i] == eye[i]);
    }
    float angle = 3.13f;
    angle = fp::approachAngle(angle, -3.13f, .01f);
    assert(angle > 3.13f && angle < 3.16f);
    printf("PASS: camera = head (axes and eye), aim angle wrap\n");
    {
        float v[3];
        for (unsigned m = 0; m < 16; m++) {
            bool moves = fp::travel(m, 0, v);
            float n = v[0] * v[0] + v[2] * v[2];
            assert(std::fabs(n - (moves ? 1.f : 0.f)) < 1.e-5f);
        }
        assert(!fp::travel(15, 0, v));
        fp::travel(1, 0, v);
        assert(v[2] == 1);
        fp::travel(4, 0, v);
        assert(v[2] == -1);
    }
    printf("PASS: movement directions\n");
    { // heading hold: a small heading error (the head's, looking up or down) is turned out; a big one (a fast turn
        // still catching up) is left; pitch and orthonormality kept; nothing near straight down
        auto yawOf = [](const float* q) {
            return std::atan2(-q[6], -q[8]);
        };
        auto pitchOf = [](const float* q) {
            return std::asin(-q[7]);
        };
        auto ortho = [](const float* q) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    float d = 0;
                    for (int k = 0; k < 3; k++) d += q[i * 3 + k] * q[j * 3 + k];
                    if (std::fabs(d - (i == j ? 1.f : 0.f)) > 1e-4f) return false;
                }
            return true;
        };
        float q[12]{};
        fp::basis(0.3f, -0.6f, q);
        assert(std::fabs(fp::holdHeading(q, 0.35f) - 0.05f) < 1e-5f && std::fabs(yawOf(q) - 0.35f) < 1e-4f &&
               std::fabs(pitchOf(q) + 0.6f) < 1e-4f && ortho(q));
        fp::basis(3.1f, 0.4f, q);
        assert(fp::holdHeading(q, -3.13f) != 0.f && std::fabs(yawOf(q) + 3.13f) < 1e-4f); // across the wrap
        fp::basis(0.f, 0.f, q);
        assert(fp::holdHeading(q, 0.3f) == 0.f && yawOf(q) == 0.f); // 17 degrees: the head is still turning
        fp::basis(0.f, 0.f, q);
        const float half = fp::holdHeading(q, 0.5f * (fp::headingHoldFull + fp::headingHoldNone));
        assert(half > 0.f && half < 0.5f * (fp::headingHoldFull + fp::headingHoldNone));
        fp::basis(0.f, -1.4f, q);
        assert(fp::holdHeading(q, 0.05f) == 0.f); // near straight down
        fp::TurnLead lead;
        float led = 0;
        for (int k = 0; k < 10; ++k) {
            fp::basis(0.03f * float(k), 0.f, q);
            led = lead.step(q, 1.f / 60);
        }
        assert(std::fabs(led - 0.03f) < 2e-3f && std::fabs(yawOf(q) - (0.27f + led)) < 1e-4f &&
               ortho(q)); // one frame ahead
        fp::TurnLead slow;
        for (int k = 0; k < 10; ++k) {
            fp::basis(0.005f * float(k), 0.f, q);
            led = slow.step(q, 1.f / 60);
        }
        assert(led == 0.f); // slow: none
        fp::TurnLead fast;
        for (int k = 0; k < 10; ++k) {
            fp::basis(0.2f * float(k), 0.f, q);
            led = fast.step(q, 1.f / 60);
        }
        assert(std::fabs(led - 0.052f) < 1e-6f); // capped
        assert(std::fabs(fp::horizontalFov(90.f, 1.f) - 90.f) < 1e-3f &&
               std::fabs(fp::horizontalFov(75.f, 16.f / 9.f) - 107.5f) < 0.2f &&
               fp::horizontalFov(75.f, 21.f / 9.f) > fp::horizontalFov(75.f, 16.f / 9.f));
        printf("PASS: heading hold turns out a few degrees of head heading error only; turn lead leads one "
               "frame in fast turns, capped; horizontal FOV\n");
    }
    // third person: behind the pivot along the aim, over the right shoulder; walls pull it in at once,
    // it eases back out; a jump (teleport) or a reset starts it fresh
    {
        fp::ThirdPerson tp;
        float pose[12];
        const float eye[3] = {0, 170, 0};
        auto open = [](const float*, const float*, float len) {
            return len;
        };
        tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, open, pose);
        float fwd[3] = {-pose[6], -pose[7], -pose[8]};
        assert(std::fabs(fwd[2] - 1) < 1e-5f); // yaw 0: looks along +z
        float back = 0, right = 0;
        for (int i = 0; i < 3; ++i) {
            const float d = pose[9 + i] - (i == 1 ? 185.f : eye[i]);
            back += -d * fwd[i];
            right += d * pose[i];
        }
        assert(std::fabs(back - 250) < 0.5f && std::fabs(right - 40) < 0.5f &&
               std::fabs(pose[10] - 185) < 0.5f); // 250 behind, 40 right, pivot 15 above the eye
        auto wall = [](const float*, const float*, float) {
            return 80.f;
        };
        tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, wall, pose);
        float d = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = pose[9 + i] - (i == 1 ? 185.f : eye[i]);
            d += v * v;
        }
        assert(std::fabs(std::sqrt(d) - 80) < 0.5f); // pulled in at once
        tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, open, pose);
        d = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = pose[9 + i] - (i == 1 ? 185.f : eye[i]);
            d += v * v;
        }
        assert(std::sqrt(d) > 80.5f && std::sqrt(d) < 120.f); // eases back out, not a jump
        for (int k = 0; k < 200; ++k) tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, open, pose);
        d = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = pose[9 + i] - (i == 1 ? 185.f : eye[i]);
            d += v * v;
        }
        assert(std::fabs(std::sqrt(d) - std::sqrt(250.f * 250 + 40 * 40)) < 1.f);
        auto none = [](const float*, const float*, float) {
            return 0.f;
        };
        tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, none,
                  pose); // No room: stay at the reachable pivot rather than crossing geometry.
        d = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = pose[9 + i] - tp.pivot[i];
            d += v * v;
        }
        assert(d == 0.f);
        const float far[3] = {5000, 170, 0};
        tp.update(far, 0.5f, 0.2f, 250, -40, 15, 1.f / 60, open, pose);
        float back2 = 0;
        for (int i = 0; i < 3; ++i) back2 += -(pose[9 + i] - (i == 1 ? 185.f : far[i])) * (-pose[6 + i]);
        assert(std::fabs(back2 - 250) < 0.5f); // a teleport: fresh, no easing from the old place
        for (float v : pose) assert(std::isfinite(v));
    }
    printf("PASS: third person orbits behind the aim over a shoulder, pulls in at walls at once and eases "
           "out\n");
    // tight spaces: a wall on the right pins the right shoulder; the camera swaps to the open left,
    // and with the setting off nothing changes.
    {
        fp::ThirdPerson tp, plain;
        float pose[12], ref[12];
        const float eye[3] = {0, 170, 0};
        auto rightWall = [](const float*, const float* dir, float len) {
            return dir[0] < 0.f ? 60.f : len;
        }; // yaw 0: the right vector is -x
        for (int k = 0; k < 120; ++k) tp.update(eye, 0, 0, 250, 40, 15, 1.f / 60, rightWall, pose, true);
        float right = 0;
        for (int i = 0; i < 3; ++i) right += (pose[9 + i] - eye[i]) * pose[i];
        printf("tight: swaps %u, right %.1f, pinned %.2f\n", tp.swaps, double(right), double(tp.pinned));
        assert(tp.swaps >= 1 && right < -5.f); // over the left shoulder now
        {
            fp::ThirdPerson off;
            for (int k = 0; k < 10; ++k) {
                off.update(eye, 0, 0, 250, 40, 15, 1.f / 60, rightWall, pose, false);
                plain.update(eye, 0, 0, 250, 40, 15, 1.f / 60, rightWall, ref);
            }
        }
        for (int i = 0; i < 12; ++i) assert(std::fabs(pose[i] - ref[i]) < 1e-3f); // off: the camera
        fp::ThirdPerson box;
        auto tight = [](const float*, const float*, float) {
            return 60.f;
        }; // walls both sides: no swap, closer and higher
        for (int k = 0; k < 120; ++k) box.update(eye, 0, 0, 250, 40, 15, 1.f / 60, tight, pose, true);
        printf("box: swaps %u, pinned %.2f, y %.1f\n", box.swaps, double(box.pinned), double(pose[10]));
        assert(box.swaps == 0 && box.pinned > 0.8f);
        for (float v : pose) assert(std::isfinite(v));
    }
    printf("PASS: third person swaps to the open shoulder in tight spaces and frames closer; off = the plain "
           "shoulder camera\n");
    // the third -> overhead blend: the ends are the two views, the middle an orthonormal frame between
    {
        float a[12], b[12], m[12];
        fp::basis(0.3f, 0.1f, a);
        a[9] = 0;
        a[10] = 180;
        a[11] = -250;
        fp::basis(1.4f, -0.9f, b);
        b[9] = 300;
        b[10] = 900;
        b[11] = -400;
        fp::blendPose(a, b, 0.f, m);
        for (int i = 0; i < 12; ++i) assert(std::fabs(m[i] - a[i]) < 1e-4f);
        fp::blendPose(a, b, 1.f, m);
        for (int i = 0; i < 12; ++i) assert(std::fabs(m[i] - b[i]) < 1e-3f);
        fp::blendPose(a, b, 0.5f, m);
        for (int r = 0; r < 3; ++r) {
            float l = 0;
            for (int c = 0; c < 3; ++c) l += m[r * 3 + c] * m[r * 3 + c];
            assert(std::fabs(l - 1) < 1e-3f);
        }
        for (int r = 0; r < 3; ++r)
            for (int s = r + 1; s < 3; ++s) {
                float d = 0;
                for (int c = 0; c < 3; ++c) d += m[r * 3 + c] * m[s * 3 + c];
                assert(std::fabs(d) < 1e-3f);
            }
        assert(m[10] > 180.f && m[10] < 900.f);
    }
    printf("PASS: the third -> overhead blend starts at the third-person view, ends at the game's and stays "
           "a true frame between\n");
    { // A half-turn toward overhead must never collapse or flip its basis at t=0.5.
        constexpr float pi = 3.14159265358979323846f;
        for (float yaw : {pi - 0.0001f, pi, pi + 0.0001f})
            for (float pitch : {-1.5f, -1.04719755f, 0.f, 1.04719755f, 1.5f}) {
                float a[12]{}, b[12]{}, m[12], previous[12];
                fp::basis(0.f, 0.f, a);
                fp::basis(yaw, pitch, b);
                a[9] = -40.f; a[10] = 170.f; a[11] = 20.f;
                b[9] = 300.f; b[10] = 900.f; b[11] = -400.f;
                fp::blendPose(a, b, 0.f, m);
                assert(!memcmp(m, a, sizeof m));
                memcpy(previous, m, sizeof m);
                for (int tick = 1; tick <= 200; ++tick) {
                    const float t = float(tick) / 200.f;
                    fp::blendPose(a, b, t, m);
                    for (float v : m) assert(std::isfinite(v));
                    for (int r = 0; r < 3; ++r) {
                        float movement = 0.f;
                        for (int c = 0; c < 3; ++c) movement += m[r * 3 + c] * previous[r * 3 + c];
                        assert(movement > 0.998f); // no sudden axis reversal at the midpoint
                        for (int other = 0; other < 3; ++other) {
                            float dot = 0.f;
                            for (int c = 0; c < 3; ++c) dot += m[r * 3 + c] * m[other * 3 + c];
                            assert(std::fabs(dot - (r == other ? 1.f : 0.f)) < 2e-5f);
                        }
                    }
                    float normal[3];
                    fp::cross(m, m + 3, normal);
                    float determinant = 0.f;
                    for (int c = 0; c < 3; ++c) determinant += normal[c] * m[6 + c];
                    assert(std::fabs(determinant - 1.f) < 2e-5f);
                    const float smooth = t * t * (3.f - 2.f * t);
                    for (int c = 9; c < 12; ++c)
                        assert(std::fabs(m[c] - (a[c] + (b[c] - a[c]) * smooth)) < 1e-4f);
                    memcpy(previous, m, sizeof m);
                }
                assert(!memcmp(m, b, sizeof m));
            }
        puts("PASS: half-turn overhead blends remain continuous, finite and right-handed through steep pitch; "
             "exact endpoints and position timing preserved");
    }

    // lag 0 = the camera exactly; lag > 0 trails behind and leads toward travel; knocked down
    // the pivot follows the body point
    {
        auto open = [](const float*, const float*, float len) {
            return len;
        };
        fp::ThirdPerson a, b;
        a.lag = 0.f;
        float pa[12], pb[12];
        for (int k = 0; k < 60; ++k) {
            const float e[3] = {float(k) * 5.f, 170, 0};
            a.update(e, 0, 0, 250, 40, 15, 1.f / 60, open, pa);
            b.update(e, 0, 0, 250, 40, 15, 1.f / 60, open, pb);
        }
        for (int i = 0; i < 12; ++i) assert(pa[i] == pb[i]); // lag 0 == default
        fp::ThirdPerson c;
        c.lag = 100.f;
        float pc[12];
        for (int k = 0; k < 120; ++k) {
            const float e[3] = {float(k) * 2.5f, 170, 0};
            c.update(e, 0, 0, 250, 40, 15, 1.f / 60, open, pc);
        } // walking +x at 150 cm/s
        const float eyeX = 119 * 2.5f;
        printf("lag: pivot x %.1f vs eye x %.1f\n", double(c.pivot[0]), double(eyeX));
        assert(c.pivot[0] > eyeX + 5.f && c.pivot[0] < eyeX + 55.f); // led ahead of the eye, at most 50 cm
        fp::ThirdPerson d;
        d.down = true;
        d.downPoint[0] = 10;
        d.downPoint[1] = 80;
        d.downPoint[2] = 0;
        float pd[12];
        const float e[3] = {0, 40, 0};
        for (int k = 0; k < 120; ++k) d.update(e, 0, 0, 250, 40, 15, 1.f / 60, open, pd);
        assert(std::fabs(d.pivot[1] - 80.f) < 1.f && std::fabs(d.pivot[0] - 10.f) < 1.f);
    } // knocked down: the body point
    printf("PASS: third person lag 0 is the camera without lag; lag leads toward travel; knocked down it "
           "follows the body\n");
    // the height setting moves the camera by the same amount, with
    // tight spaces, lag and combat framing on or off
    {
        auto open = [](const float*, const float*, float len) {
            return len;
        };
        for (int mode = 0; mode < 4; ++mode) {
            fp::ThirdPerson low, high;
            low.lag = high.lag = (mode & 1) ? 40.f : 0.f;
            const bool tight = (mode & 2) != 0;
            float pl[12], ph[12];
            const float e[3] = {0, 170, 0};
            for (int k = 0; k < 240; ++k) {
                low.update(e, 0.4f, 0.2f, 250, 40, 15, 1.f / 60, open, pl, tight);
                high.update(e, 0.4f, 0.2f, 250, 40, 75, 1.f / 60, open, ph, tight);
            }
            printf("height mode %d: camera y %.1f -> %.1f\n", mode, double(pl[10]), double(ph[10]));
            assert(std::fabs((ph[10] - pl[10]) - 60.f) < 1.f && std::fabs(ph[9] - pl[9]) < 0.5f &&
                   std::fabs(ph[11] - pl[11]) < 0.5f);
        }
    }
    printf("PASS: third-person height moves the camera by exactly the change, with every third-person "
           "feature on or off\n");
    { // a ceiling 90 cm above the eye: the pivot stops 20 cm under it (it never starts the pull-in rays inside it); below the
        // eye the same with a floor; open, the full height
        auto ceiling = [](const float*, const float* dir, float len) {
            return dir[1] > 0.9f ? std::fmin(len, 90.f) : (dir[1] < -0.9f ? std::fmin(len, 40.f) : len);
        };
        const float e[3] = {0, 170, 0};
        float p[12];
        fp::ThirdPerson hi;
        hi.update(e, 0, 0, 250, 40, 200, 1.f / 60, ceiling, p);
        assert(std::fabs(hi.pivot[1] - 240.f) < 0.5f);
        // below the eye, a pivot asked for under the floor is pulled back out of it (the camera clipped through the floor):
        // it stops the camera radius above it
        fp::ThirdPerson lo;
        lo.update(e, 0, 0, 250, 40, -100, 1.f / 60, ceiling, p);
        assert(std::fabs(lo.pivot[1] - (170.f - (40.f - fp::tpCameraRadius))) < 0.5f &&
               std::fabs(lo.lastPivotPulled - (100.f - (40.f - fp::tpCameraRadius))) < 0.5f);
        // the height the ceiling took (200 asked, 70 allowed) goes to the boom: the camera sits higher than with no height
        assert(std::fabs(hi.lastLostHeight - 130.f) < 0.5f && std::fabs(hi.lastCeilingRoom - 90.f) < 0.5f);
        {
            auto plane = [](const float* o, const float* d, float len) {
                return d[1] > 1e-4f ? std::fmin(len, (260.f - o[1]) / d[1]) : len;
            }; // a real ceiling at y 260
            fp::ThirdPerson tall, flat;
            float t[12], q[12];
            tall.update(e, 0, 0, 250, 40, 200, 1.f / 60, plane, t);
            flat.update(e, 0, 0, 250, 40, 0, 1.f / 60, plane, q);
            printf("low ceiling: height 200 puts the camera at y %.1f, height 0 at %.1f\n", double(t[10]),
                   double(q[10]));
            assert(t[10] > q[10] + 30.f && t[10] <= 260.f - fp::tpCameraRadius + 0.5f);
        }
        fp::ThirdPerson ok;
        ok.update(e, 0, 0, 250, 40, 60, 1.f / 60, ceiling, p);
        assert(std::fabs(ok.pivot[1] - 230.f) < 0.5f);
        auto open = [](const float*, const float*, float len) {
            return len;
        };
        fp::ThirdPerson lintel;
        lintel.lag = 100.f;
        for (int k = 0; k < 60; ++k) lintel.update(e, 0, 0, 250, 40, 200, 1.f / 60, open, p);
        assert(std::fabs(lintel.pivot[1] - 370.f) < 0.5f);
        lintel.update(e, 0, 0, 250, 40, 200, 1.f / 60, ceiling, p);
        assert(lintel.pivot[1] <=
               240.f + 1e-3f); // walking under a lintel: the eased pivot is held under it too
        fp::ThirdPerson down;
        down.down = true;
        down.downPoint[1] = 300.f;
        down.update(e, 0, 0, 250, 40, 200, 1.f / 60, open, p);
        assert(std::fabs(down.pivot[1] - 300.f) < 0.5f); // knocked down: the body point
        fp::ThirdPerson buried;
        buried.down = true;
        buried.downPoint[1] = 300.f;
        buried.update(e, 0, 0, 250, 40, 200, 1.f / 60, ceiling, p);
        assert(std::fabs(buried.pivot[1] - (170.f + 90.f - fp::tpCameraRadius)) < 0.5f);
    } // ... but never inside the ceiling
    printf("PASS: the third-person pivot stays under a low ceiling (eased or not), else the full height\n");
    { // Walking into a wall with the lead on (lag 100: up to 50 cm ahead). The level, like the game's, misses a wall when the
        // ray starts inside it. The pivot is kept out of the wall, so the camera never ends up on the far side.
        const float W = 100.f;
        auto wallX = [W](const float* o, const float* d, float len) {
            if (o[0] > W) return len; // from inside the wall: nothing hit (the old failure)
            return d[0] > 1e-4f ? std::fmin(len, (W - o[0]) / d[0]) : len;
        };
        fp::ThirdPerson tp;
        tp.lag = 100.f;
        float pose[12];
        float worstPivot = -1e9f, worstCamera = -1e9f;
        for (int k = 0; k < 240; ++k) {
            const float e[3] = {std::fmin(float(k) * 2.5f, W - 20.f), 170, 0}; // walk +x and stop 20 cm short
            tp.update(e, 1.5707963f, 0, 250, 40, 15, 1.f / 60, wallX,
                      pose); // looking along +x: the camera is behind, the lead points into the wall
            worstPivot = std::fmax(worstPivot, tp.pivot[0]);
            worstCamera = std::fmax(worstCamera, pose[9]);
        }
        printf("walking into a wall: furthest pivot x %.1f, camera x %.1f (wall at %.0f)\n",
               double(worstPivot), double(worstCamera), double(W));
        assert(worstPivot < W - 5.f &&
               worstCamera <
                   W); // the radius is kept along the eye-to-pivot ray (it slopes up: less across the wall)
        puts("PASS: the pivot is kept out of a wall the lead pushes it into; the camera stays on the "
             "player's side");
    }
    { // A doorframe edge 6 cm beside the line from the pivot to the camera: solid to the camera's right of 6 cm, over 20 cm of
        // the line. The centre ray passes it; the near plane would cut into it. The box cast (rays through the corners of the
        // camera's box) stops in front of it; a centre ray alone does not.
        const float e[3] = {0, 170, 0};
        float open[12];
        {
            fp::ThirdPerson probe;
            auto none = [](const float*, const float*, float len) {
                return len;
            };
            probe.update(e, 0, 0, 250, 0, 15, 1.f / 60, none, open);
        }
        const float pv[3] = {0, 185, 0}; // the pivot: eye + height 15, no shoulder
        float back[3] = {open[9] - pv[0], open[10] - pv[1], open[11] - pv[2]};
        const float bl = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
        for (float& v : back) v /= bl;
        const float right[3] = {open[0], open[1], open[2]};
        auto edge = [=](const float* o, const float* d,
                        float len) { // in (right, along) coordinates: r > 6 and 150 < a < 170
            const float orr =
                            (o[0] - pv[0]) * right[0] + (o[1] - pv[1]) * right[1] + (o[2] - pv[2]) * right[2],
                        oa = (o[0] - pv[0]) * back[0] + (o[1] - pv[1]) * back[1] + (o[2] - pv[2]) * back[2];
            const float dr = d[0] * right[0] + d[1] * right[1] + d[2] * right[2],
                        da = d[0] * back[0] + d[1] * back[1] + d[2] * back[2];
            float t0 = 0, t1 = len;
            auto slab = [&](float oo, float dd, float lo, float hi) {
                if (std::fabs(dd) < 1e-6f) return oo >= lo && oo <= hi;
                float a = (lo - oo) / dd, z = (hi - oo) / dd;
                if (a > z) std::swap(a, z);
                t0 = std::fmax(t0, a);
                t1 = std::fmin(t1, z);
                return t0 <= t1;
            };
            if (!slab(orr, dr, 6.f, 1e4f) || !slab(oa, da, 150.f, 170.f)) return len;
            return t0;
        };
        fp::ThirdPerson boxed, thin;
        thin.radius = 0.f;
        float pb[12], pt[12];
        boxed.update(e, 0, 0, 250, 0, 15, 1.f / 60, edge, pb);
        thin.update(e, 0, 0, 250, 0, 15, 1.f / 60, edge, pt);
        printf("doorframe edge 6 cm off the centre line: box cast stops at %.1f, centre ray alone at %.1f\n",
               double(boxed.lastAllowed), double(thin.lastAllowed));
        fflush(stdout);
        assert(thin.lastAllowed >= 249.f && boxed.lastAllowed <= 150.5f);
        puts("PASS: the camera's box keeps it in front of an edge the centre ray misses (what showed through "
             "the near plane)");
    }
    { // Free combat aim: the crosshair takes the mouse inside the box; past the edge the rest turns the view; back again it
        // comes off the edge before the view turns the other way. A bad step re-centres.
        fp::AimBox box;
        float dx = 30, dy = -10;
        fp::aimBoxStep(box, dx, dy, 100, 60);
        assert(box.x == 30 && box.y == -10 && dx == 0 && dy == 0);
        dx = 90;
        dy = -70;
        fp::aimBoxStep(box, dx, dy, 100, 60);
        assert(box.x == 100 && box.y == -60 && dx == 20 && dy == -20); // 20 past each edge: the view
        dx = -40;
        dy = 0;
        fp::aimBoxStep(box, dx, dy, 100, 60);
        assert(box.x == 60 && dx == 0); // off the edge: no view turn
        dx = NAN;
        dy = 5;
        fp::aimBoxStep(box, dx, dy, 100, 60);
        assert(box.x == 0 && box.y == -55);
        const float body[3] = {0, 0, 0}, ahead[3] = {0, 0, 300}, beside[3] = {300, 0, 0},
                    under[3] = {10, -100, 10};
        float d[3];
        fp::headingToward(body, ahead, 1.f, d);
        assert(std::fabs(d[2] - 1.f) < 1e-6f && d[1] == 0.f);
        fp::headingToward(body, beside, 0.f, d);
        assert(std::fabs(d[0] - 1.f) < 1e-6f);
        fp::headingToward(body, under, 0.5f, d);
        assert(std::fabs(d[0] - std::sin(0.5f)) < 1e-6f); // too near: the mouse yaw
        assert(fp::blockersFromText(L"hide") == fp::blockersHide &&
               fp::blockersFromText(L" PUSH ") == fp::blockersPush &&
               fp::blockersFromText(L"smart") == fp::blockersSmart &&
               fp::blockersFromText(L"") == fp::blockersSmart &&
               fp::blockersFromText(nullptr) == fp::blockersSmart);
        assert(!wcscmp(fp::blockersText(fp::blockersPush), L"push"));
        puts("PASS: free combat aim: the crosshair moves in its box, the edge turns the view, the body turns "
             "toward what it is over");
    }
    { // Knockdown for the follow comes from the game's own state bits and is held 0.3 s after they clear (no flicker).
        fp::KnockdownFollow k;
        assert(!fp::knockdownStep(k, 0, 0, 1000));   // standing: never down, whatever the eye height
        assert(fp::knockdownStep(k, 0x40, 0, 1100)); // knocked down
        assert(fp::knockdownStep(k, 0, 0, 1200) && fp::knockdownStep(k, 0, 0, 1499)); // cleared: held
        assert(!fp::knockdownStep(k, 0, 0, 1500));                                    // 0.3 s later: up
        assert(fp::knockdownStep(k, 0, fp::deadLifeBit, 1600));                       // dead
        puts("PASS: third-person knockdown follows the game's state bits, not a height guess; getting up "
             "does not flicker");
    }
    { // Eye setback: the viewpoint moves back along the head's forward axis by exactly the setback, whatever the head's
        // orientation; never behind the head's origin; 0 is the eye mount as before.
        float pose[12];
        fp::basis(0.7f, -0.4f, pose);
        const float o[3] = {10.f, 160.f, -5.f};
        std::memcpy(pose + 9, o, 12);
        float at[3], back[3], deep[3];
        fp::eyePoint(pose, at);
        fp::eyePoint(pose, back, 10.f);
        fp::eyePoint(pose, deep, 100.f);
        float d = 0, dot = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = at[i] - back[i];
            d += v * v;
            dot += v * pose[6 + i];
        }
        assert(std::fabs(std::sqrt(d) - 10.f) < 1e-3f &&
               std::fabs(dot - 10.f) < 1e-3f); // 10 cm, straight along the forward axis
        float origin[3];
        for (int i = 0; i < 3; ++i) origin[i] = pose[9 + i] + pose[3 + i] * fp::eyeAlongAxis2;
        for (int i = 0; i < 3; ++i)
            assert(std::fabs(deep[i] - origin[i]) < 1e-3f); // clamped: at the origin's depth, no further
        float zero[3];
        fp::eyePoint(pose, zero, 0.f);
        for (int i = 0; i < 3; ++i) assert(zero[i] == at[i]);
        assert(fp::clampEyeSetback(-3) == 0 && fp::clampEyeSetback(9) == 9 &&
               fp::clampEyeSetback(99) == fp::eyeSetbackMax);
        puts("PASS: eye setback moves the viewpoint straight back along the head by the setting, clamped "
             "inside the head");
    }
    { // Collision results closer than the old minimum boom must remain authoritative.
        const float eye[3] = {0, 170, 0};
        for (float gap : {0.f, 5.f, 11.f, 12.f, 25.f}) {
            auto wall = [gap](const float* origin, const float* dir, float length, bool) {
                if (dir[2] >= 0.f) return length;
                const float hit = (-gap - origin[2]) / dir[2];
                return hit >= 0.f && hit <= length ? hit : length;
            };
            fp::ThirdPerson camera;
            float pose[12]{};
            camera.update(eye, 0, 0, 250, 0, 0, 1.f / 60.f, wall, pose);
            assert(pose[11] >= -gap && camera.lastAllowed <= gap);
        }
        fp::ThirdPerson camera;
        assert(fp::knockdownStep(camera.knock, 4, 0, 1000));
        camera.down = true;
        camera.reset();
        assert(!camera.down && !fp::knockdownStep(camera.knock, 0, 0, 1100));
        puts("PASS: sub-12cm camera clearance is respected and reset retires knockdown state");
    }
    // dynamic near plane
    {
        fp::NearPlane n;
        assert(n.update(-1, .016f) == 1.f); // unknown: tight
        fp::NearPlane w;
        assert(w.update(1e9f, .016f) == 4.f); // first: straight to the wanted value
        float v = w.update(1e9f, .016f);
        assert(v == 4.f);
        v = w.update(3.f, .016f);
        assert(v == 1.5f); // geometry 3 units deep: tightened at once
        float prev = v;
        for (int f = 0; f < 10; f++) {
            v = w.update(1e9f, .016f);
            assert(v >= prev && v < 4.f);
            prev = v;
        } // widened gently
        for (int f = 0; f < 200; f++) v = w.update(1e9f, .016f);
        assert(std::fabs(v - 4.f) < .01f);
        assert(w.update(0.5f, .016f) == 1.f); // never below 1
        printf("PASS: near plane tightens at once, widens eased, stays within 1..4\n");
    }
}
