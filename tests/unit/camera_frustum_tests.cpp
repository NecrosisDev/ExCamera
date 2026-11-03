#include "frustum.hpp"

#include <cmath>
#include <limits>

#include "../test_support.hpp"

namespace {

bool near(float left, float right, float tolerance = 1e-3f) {
    return std::fabs(left - right) <= tolerance;
}

excamera::CameraFrustum frustum() {
    return {{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}}, 90.f, 1.f, 10.f};
}

auto plane_probe(float intercept, float slope, int& queries) {
    return [intercept, slope, &queries](excamera::Vec3 origin, excamera::Vec3 direction, float length) {
        ++queries;
        const float denominator = direction.z - slope * direction.x;
        if (std::fabs(denominator) < 1e-8f) return excamera::SegmentHits{};
        const float distance = (intercept + slope * origin.x - origin.z) / denominator;
        return distance >= 0.f && distance <= length
            ? excamera::SegmentHits{distance} : excamera::SegmentHits{};
    };
}

bool clear_frustum_uses_center_and_four_corners() {
    int queries = 0;
    const float result = excamera::frustum_clearance({}, frustum(), 240.f, plane_probe(1000.f, 0.f, queries));
    CHECK(near(result, 240.f));
    CHECK(queries == 5);
    return true;
}

bool central_obstruction_limits_the_boom() {
    int queries = 0;
    const float result = excamera::frustum_clearance({}, frustum(), 240.f, plane_probe(100.f, 0.f, queries));
    CHECK(near(result, 92.f));
    CHECK(queries == 5);
    return true;
}

bool near_plane_obstruction_fails_inward() {
    int queries = 0;
    const float result = excamera::frustum_clearance({}, frustum(), 240.f, plane_probe(12.f, 0.f, queries));
    CHECK(near(result, 0.f));
    CHECK(queries == 1);
    return true;
}

bool oblique_corner_hit_is_refined_and_rechecked() {
    int queries = 0;
    const float result = excamera::frustum_clearance({}, frustum(), 240.f, plane_probe(208.f, -1.8f, queries));
    const float corner_length = std::sqrt(190.f * 190.f + 200.f);
    const float expected = 10.f + 190.f * (corner_length - 8.f) / corner_length;
    CHECK(near(result, expected, 0.002f));
    CHECK(queries == 9);
    return true;
}

bool persistent_obstruction_has_a_bounded_failure() {
    int queries = 0;
    const auto probe = [&](excamera::Vec3, excamera::Vec3, float length) {
        ++queries;
        return excamera::SegmentHits{length * 0.99f};
    };
    CHECK(near(excamera::frustum_clearance({}, frustum(), 240.f, probe), 0.f));
    CHECK(queries == 1 + 4 * (excamera::frustum_refinement_steps + 1));
    return true;
}

bool zero_near_plane_only_needs_center_query() {
    int queries = 0;
    auto input = frustum();
    input.near_plane = 0.f;
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, plane_probe(1000.f, 0.f, queries)), 240.f));
    CHECK(queries == 1);
    return true;
}

bool invalid_inputs_fail_before_native_queries() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    int queries = 0;
    const auto probe = [&](excamera::Vec3, excamera::Vec3, float) {
        ++queries;
        return excamera::SegmentHits{};
    };
    auto input = frustum();
    CHECK(near(excamera::frustum_clearance({}, input, 0.f, probe), 0.f));
    CHECK(near(excamera::frustum_clearance({}, input, nan, probe), 0.f));
    input.vertical_fov = 0.f;
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, probe), 0.f));
    input = frustum();
    input.aspect = 0.f;
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, probe), 0.f));
    input = frustum();
    input.near_plane = -1.f;
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, probe), 0.f));
    input = frustum();
    input.orientation.back = {};
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, probe), 0.f));
    input = frustum();
    input.orientation.right = {};
    CHECK(near(excamera::frustum_clearance({}, input, 240.f, probe), 0.f));
    input = frustum();
    CHECK(near(excamera::frustum_clearance({nan, 0.f, 0.f}, input, 240.f, probe), 0.f));
    CHECK(queries == 0);
    return true;
}

bool failed_and_nan_hits_pull_in_immediately() {
    int queries = 0;
    const auto failed = [&](excamera::Vec3, excamera::Vec3, float) {
        ++queries;
        return excamera::SegmentHits{{}, {}, true};
    };
    CHECK(near(excamera::frustum_clearance({}, frustum(), 240.f, failed), 0.f));
    CHECK(queries == 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(near(excamera::frustum_clearance({}, frustum(), 240.f,
        [&](excamera::Vec3, excamera::Vec3, float) { return excamera::SegmentHits{nan}; }), 0.f));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"clear frustum uses center and four corners", clear_frustum_uses_center_and_four_corners},
        {"central obstruction limits the boom", central_obstruction_limits_the_boom},
        {"near plane obstruction fails inward", near_plane_obstruction_fails_inward},
        {"oblique corner hit is refined and rechecked", oblique_corner_hit_is_refined_and_rechecked},
        {"persistent obstruction has a bounded failure", persistent_obstruction_has_a_bounded_failure},
        {"zero near plane only needs center query", zero_near_plane_only_needs_center_query},
        {"invalid inputs fail before native queries", invalid_inputs_fail_before_native_queries},
        {"failed and NaN hits pull in immediately", failed_and_nan_hits_pull_in_immediately},
    });
}
