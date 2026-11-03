#include "collision.hpp"

#include <cmath>
#include <iterator>
#include <limits>

#include "../test_support.hpp"

namespace {

bool near(float left, float right) {
    return std::fabs(left - right) <= 1e-4f;
}

const excamera::Vec3 origin{10.f, 20.f, 30.f};
const excamera::Vec3 direction{0.f, 0.f, 1.f};

bool clear_segment_uses_one_bounded_probe() {
    int queries = 0;
    const float result = excamera::segment_clearance(origin, direction, 240.f,
        [&](float length) {
            ++queries;
            if (!near(length, 240.f)) return excamera::SegmentHits{0.f, 0.f, true};
            return excamera::SegmentHits{};
        });
    CHECK(near(result, 240.f));
    CHECK(queries == 1);
    return true;
}

bool forward_hit_has_exact_distance_and_margin() {
    const float result = excamera::segment_clearance(origin, direction, 160.f,
        [](float) { return excamera::SegmentHits{80.f}; });
    CHECK(near(result, 72.f));
    return true;
}

bool reverse_hit_is_converted_to_origin_distance() {
    const float result = excamera::segment_clearance(origin, direction, 160.f,
        [](float) { return excamera::SegmentHits{std::numeric_limits<float>::infinity(), 80.f}; });
    CHECK(near(result, 72.f));
    return true;
}

bool nearest_surface_wins_across_multiple_walls() {
    const float result = excamera::segment_clearance(origin, direction, 200.f,
        [](float) { return excamera::SegmentHits{110.f, 160.f}; });
    CHECK(near(result, 32.f));
    CHECK(near(excamera::segment_clearance(origin, direction, 160.f,
        [](float) { return excamera::SegmentHits{80.f, 80.f}; }), 72.f));
    CHECK(near(excamera::segment_clearance(origin, direction, 6.f,
        [](float) { return excamera::SegmentHits{3.f}; }), 0.f));
    return true;
}

bool failures_and_invalid_hits_pull_camera_in() {
    CHECK(near(excamera::segment_clearance(origin, direction, 160.f,
        [](float) { return excamera::SegmentHits{{}, {}, true}; }), 0.f));
    const float invalid[]{-1.f, 161.f, std::numeric_limits<float>::quiet_NaN()};
    for (const float value : invalid) {
        CHECK(near(excamera::segment_clearance(origin, direction, 160.f,
            [&](float) { return excamera::SegmentHits{value}; }), 0.f));
        CHECK(near(excamera::segment_clearance(origin, direction, 160.f,
            [&](float) { return excamera::SegmentHits{std::numeric_limits<float>::infinity(), value}; }), 0.f));
    }
    return true;
}

bool invalid_geometry_never_reaches_native() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const excamera::Vec3 origins[]{origin, {nan, 0.f, 0.f}, {0.f, infinity, 0.f}};
    const excamera::Vec3 directions[]{direction, {}, {0.f, 0.f, 2.f}, {nan, 0.f, 0.f}};
    const float lengths[]{240.f, 0.f, -1.f, nan, infinity};
    int queries = 0;
    const auto probe = [&](float) {
        ++queries;
        return excamera::SegmentHits{};
    };
    CHECK(near(excamera::segment_clearance(origin, direction, 240.f, probe), 240.f));
    CHECK(queries == 1);
    for (std::size_t origin_index = 0; origin_index < std::size(origins); ++origin_index)
        for (std::size_t direction_index = 0; direction_index < std::size(directions); ++direction_index)
            for (std::size_t length_index = 0; length_index < std::size(lengths); ++length_index) {
                if (!origin_index && !direction_index && !length_index) continue;
                CHECK(near(excamera::segment_clearance(origins[origin_index], directions[direction_index],
                    lengths[length_index], probe), 0.f));
            }
    CHECK(queries == 1);
    CHECK(near(excamera::segment_clearance({0.f, 0.f, std::numeric_limits<float>::max()},
        direction, std::numeric_limits<float>::max(), probe), 0.f));
    CHECK(queries == 1);
    return true;
}

bool unit_tolerance_and_extreme_lengths() {
    int queries = 0;
    CHECK(near(excamera::segment_clearance(origin, {0.f, 0.f, 1.0005f}, 20.f,
        [&](float) {
            ++queries;
            return excamera::SegmentHits{};
        }), 20.f));
    CHECK(queries == 1);
    const float maximum = std::numeric_limits<float>::max();
    CHECK(excamera::segment_clearance(origin, direction, maximum,
        [](float) { return excamera::SegmentHits{}; }) == maximum);
    CHECK(near(excamera::segment_clearance(origin, direction, maximum,
        [](float) { return excamera::SegmentHits{maximum}; }), maximum - 8.f));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"clear segment uses one bounded probe", clear_segment_uses_one_bounded_probe},
        {"forward hit has exact distance and margin", forward_hit_has_exact_distance_and_margin},
        {"reverse hit is converted to origin distance", reverse_hit_is_converted_to_origin_distance},
        {"nearest surface wins across multiple walls", nearest_surface_wins_across_multiple_walls},
        {"failures and invalid hits pull camera in", failures_and_invalid_hits_pull_camera_in},
        {"invalid geometry never reaches native", invalid_geometry_never_reaches_native},
        {"unit tolerance and extreme lengths", unit_tolerance_and_extreme_lengths},
    });
}
