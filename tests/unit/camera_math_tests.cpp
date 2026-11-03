#include "math.hpp"

#include <cmath>
#include <limits>

#include "../test_support.hpp"

namespace {

bool near(float left, float right, float tolerance = 1e-4f) {
    return std::fabs(left - right) <= tolerance;
}

bool near(excamera::Vec3 value, float tolerance = 1e-3f) {
    return excamera::length(value) <= tolerance;
}

bool orthonormal(const excamera::Pose& pose) {
    return near(excamera::length(pose.right), 1.f) && near(excamera::length(pose.up), 1.f) &&
        near(excamera::length(pose.back), 1.f) && near(excamera::dot(pose.right, pose.up), 0.f) &&
        near(excamera::dot(pose.right, pose.back), 0.f) && near(excamera::dot(pose.up, pose.back), 0.f);
}

bool look_basis_has_expected_orientation() {
    const excamera::Pose identity = excamera::look_pose({});
    CHECK(near(identity.right - excamera::Vec3{-1.f, 0.f, 0.f}));
    CHECK(near(identity.up - excamera::Vec3{0.f, 1.f, 0.f}));
    CHECK(near(identity.back - excamera::Vec3{0.f, 0.f, -1.f}));
    CHECK(orthonormal(identity));
    const excamera::Pose turned = excamera::look_pose({excamera::pi * 0.5f, 0.2f});
    CHECK(orthonormal(turned));
    CHECK(near(-turned.back.x, 1.f, 0.02f));
    const excamera::Look recovered = excamera::look_from_pose(turned);
    CHECK(near(recovered.yaw, excamera::pi * 0.5f));
    CHECK(near(recovered.pitch, 0.2f));
    return true;
}

bool look_input_is_sanitized() {
    const excamera::Look wrapped = excamera::sanitize_look({7.f, 100.f});
    CHECK(near(wrapped.yaw, 7.f - excamera::two_pi));
    CHECK(near(wrapped.pitch, excamera::max_look_pitch));
    const excamera::Look invalid = excamera::sanitize_look({std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::quiet_NaN()});
    CHECK(near(invalid.yaw, 0.f));
    CHECK(near(invalid.pitch, 0.f));
    return true;
}

bool eye_mount_matches_native_convention() {
    const excamera::Pose head{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {10.f, 20.f, 30.f}};
    const excamera::Vec3 eye = excamera::eye_position(head, 8.f);
    CHECK(near(eye.x, 10.f));
    CHECK(near(eye.y, 29.60687065f));
    CHECK(near(eye.z, 38.5f));
    excamera::Pose camera;
    CHECK(excamera::eye_pose(head, 8.f, camera));
    CHECK(near(camera.origin - eye));
    CHECK(near(camera.right - excamera::Vec3{-1.f, 0.f, 0.f}));
    CHECK(near(camera.up - excamera::Vec3{0.f, 1.f, 0.f}));
    CHECK(near(camera.back - excamera::Vec3{0.f, 0.f, -1.f}));
    CHECK(orthonormal(camera));
    return true;
}

bool degenerate_or_nonfinite_heads_fail() {
    excamera::Pose output;
    const excamera::Pose parallel{{1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {}};
    CHECK(!excamera::eye_pose(parallel, 8.f, output));
    excamera::Pose invalid{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}};
    invalid.origin.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!excamera::eye_pose(invalid, 8.f, output));
    return true;
}

bool affine_expansion_matches_native_layout() {
    const excamera::Pose camera = excamera::look_pose({});
    const excamera::Vec3 expected_eye{10.f, 20.f, 30.f};
    excamera::Pose posed = camera;
    posed.origin = expected_eye;
    const excamera::Matrix4 matrix = excamera::expand_affine(posed);
    const std::array<float, 16> expected{-1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, -1.f, 0.f, 10.f, 20.f, 30.f, 1.f};
    for (size_t index = 0; index < expected.size(); ++index) CHECK(near(matrix.rows[index], expected[index]));
    return true;
}

bool projection_uses_vertical_fov_and_native_y() {
    excamera::Pose camera = excamera::look_pose({});
    excamera::Vec2 projected;
    CHECK(excamera::project_point(camera, {0.f, 0.f, 100.f}, 800.f, 600.f, 75.f, projected));
    CHECK(near(projected.x, 400.f));
    CHECK(near(projected.y, 300.f));
    CHECK(excamera::project_point(camera, {10.f, 0.f, 100.f}, 800.f, 600.f, 75.f, projected));
    CHECK(projected.x < 400.f);
    CHECK(near(projected.y, 300.f));
    CHECK(!excamera::project_point(camera, {0.f, 0.f, -100.f}, 800.f, 600.f, 75.f, projected));
    CHECK(!excamera::project_point(camera, {0.f, 0.f, 100.f}, 0.f, 600.f, 75.f, projected));
    return true;
}

bool strided_render_pose_ignores_row_padding() {
    const float native[16]{1.f, 2.f, 3.f, 99.f, 4.f, 5.f, 6.f, 98.f,
        7.f, 8.f, 9.f, 97.f, 10.f, 11.f, 12.f, 96.f};
    const excamera::Pose pose = excamera::strided_pose(native);
    CHECK(near(pose.right - excamera::Vec3{1.f, 2.f, 3.f}));
    CHECK(near(pose.up - excamera::Vec3{4.f, 5.f, 6.f}));
    CHECK(near(pose.back - excamera::Vec3{7.f, 8.f, 9.f}));
    CHECK(near(pose.origin - excamera::Vec3{10.f, 11.f, 12.f}));
    return true;
}

bool transitions_are_finite_at_half_turn() {
    excamera::Pose from = excamera::look_pose({0.f, 0.f});
    from.origin = {0.f, 10.f, 0.f};
    excamera::Pose to = excamera::look_pose({excamera::pi, 0.f});
    to.origin = {100.f, 30.f, 50.f};
    const excamera::Pose midpoint = excamera::blend_pose(from, to, 0.5f);
    CHECK(excamera::finite(midpoint));
    CHECK(orthonormal(midpoint));
    CHECK(near(midpoint.origin.x, 50.f));
    CHECK(near(midpoint.origin.y, 20.f));
    CHECK(near(midpoint.origin.z, 25.f));
    CHECK(near(excamera::blend_pose(from, to, 0.f).origin.x, from.origin.x));
    CHECK(near(excamera::blend_pose(from, to, 1.f).origin.x, to.origin.x));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"look basis has expected orientation", look_basis_has_expected_orientation},
        {"look input is sanitized", look_input_is_sanitized},
        {"eye mount matches native convention", eye_mount_matches_native_convention},
        {"degenerate or nonfinite heads fail", degenerate_or_nonfinite_heads_fail},
        {"affine expansion matches native layout", affine_expansion_matches_native_layout},
        {"projection uses vertical fov and native y", projection_uses_vertical_fov_and_native_y},
        {"strided render pose ignores row padding", strided_render_pose_ignores_row_padding},
        {"transitions are finite at half turn", transitions_are_finite_at_half_turn},
    });
}
