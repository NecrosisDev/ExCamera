#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "camera_motion.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
int main() {
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
        const float r1 = fp::depthResolution(1.f, 25000.f, 24, 5000.f),
                    r05 = fp::depthResolution(0.5f, 25000.f, 24, 5000.f);
        assert(std::fabs(r05 / r1 - 2.f) < 0.01f);
        printf("MODEL depth step at 50 m, 24-bit: %.2f cm at near 1, %.2f cm at near 0.5; at 20 m %.2f / "
               "%.2f cm\n",
               r1, r05, fp::depthResolution(1.f, 25000.f, 24, 2000.f),
               fp::depthResolution(0.5f, 25000.f, 24, 2000.f));
        printf("PASS: near plane floor and depth precision\n");
    }
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
    printf("PASS: movement directions\n");
    // highlight: a box of 12 edges, two move handles and a 24-segment turn ring, placed by the node pose
    {
        float node[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 100, 0, -50}, centre[3] = {0, 10, 0},
              half[3] = {20, 10, 5}, pts[200 * 3];
        const int n = fp::highlightLines(node, centre, half, pts, 200);
        assert(n == 2 * (12 + 2 + 24));
        for (int i = 0; i < 24; ++i) {
            assert(std::fabs(std::fabs(pts[i * 3] - 100) - 20) < 1e-4f &&
                   std::fabs(std::fabs(pts[i * 3 + 2] + 50) - 5) < 1e-4f);
        } // every box corner at centre +- half
        float turned[12] = {0, 0, -1, 0, 1, 0, 1, 0, 0, 0, 0, 0};
        assert(fp::highlightLines(turned, centre, half, pts, 200) == n);
        assert(std::fabs(std::fabs(pts[2]) - 20) < 1e-4f &&
               std::fabs(std::fabs(pts[0]) - 5) < 1e-4f); // turned 90 degrees: x and z swap
        assert(fp::highlightLines(node, centre, half, pts, 10) == 10);
    } // never past the buffer
    printf("PASS: highlight box, handles and turn ring follow the object's pose\n");
    // terrain brush ring: 40 segments at the radius, 5 cm above the ground, and an upright tick
    {
        const float at[3] = {100, -20, 300};
        float pts[100 * 3];
        const int n = fp::ringLines(at, 250, pts, 100);
        assert(n == 2 * 41);
        for (int i = 0; i < 80; ++i) {
            const float dx = pts[i * 3] - 100, dz = pts[i * 3 + 2] - 300;
            assert(std::fabs(std::sqrt(dx * dx + dz * dz) - 250) < 1e-2f && pts[i * 3 + 1] == -15);
        }
        assert(pts[80 * 3 + 1] == -20 && pts[81 * 3 + 1] == 20);
        assert(fp::ringLines(at, 250, pts, 6) == 6);
    }
    printf("PASS: terrain brush ring at the radius on the ground\n");
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
                  pose); // never closer than 12 to the pivot (now easing down to the eye: no room above it)
        d = 0;
        for (int i = 0; i < 3; ++i) {
            const float v = pose[9 + i] - tp.pivot[i];
            d += v * v;
        }
        assert(std::fabs(std::sqrt(d) - 12) < 0.5f);
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
        fp::ThirdPerson lo;
        lo.update(e, 0, 0, 250, 40, -100, 1.f / 60, ceiling, p);
        assert(std::fabs(lo.pivot[1] - 70.f) < 0.5f); // below the eye: never asked
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
        down.update(e, 0, 0, 250, 40, 200, 1.f / 60, ceiling, p);
        assert(std::fabs(down.pivot[1] - 300.f) < 0.5f);
    } // knocked down: the body point, as before
    printf("PASS: the third-person pivot stays under a low ceiling (eased or not), else the full height\n");
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
