#include "ground.hpp"

#include <cmath>
#include <iterator>
#include <limits>

#include "../test_support.hpp"

namespace {

bool near(float left, float right, float tolerance = 1e-3f) {
    return std::fabs(left - right) <= tolerance;
}

bool supported_contacts_choose_highest_valid_height() {
    const excamera::GroundContact contacts[]{
        {{8100.f, 10.f, 3900.f}, 2},
        {{8100.f, 12.f, 3900.f}, 2},
        {{8100.f, 90.f, 3900.f}, 0},
        {{8100.f, 1000.f, 3900.f}, 2},
        {{8100.f, std::numeric_limits<float>::quiet_NaN(), 3900.f}, 2},
        {{8100.f, 1000.f, 3900.f}, 2},
    };
    const auto ground = excamera::evaluate_ground_contacts(contacts, std::size(contacts), {8100.f, 196.f, 3900.f});
    CHECK(ground.available);
    CHECK(near(ground.support_height, 12.f));
    CHECK(near(excamera::ground_clearance({8100.f, 224.f, 3900.f}, {0.f, -1.f, 0.f}, 240.f, ground),
        224.f - 12.f - excamera::ground_margin_cm));
    return true;
}

bool missing_or_invalid_support_fails_only_downward_booms() {
    const excamera::Vec3 pivot{8100.f, 224.f, 3900.f};
    const excamera::Vec3 down{0.f, -1.f, 0.f};
    const excamera::Vec3 up{0.f, 1.f, 0.f};
    CHECK(near(excamera::ground_clearance(pivot, down, 240.f, {}), 0.f));
    CHECK(near(excamera::ground_clearance(pivot, up, 240.f, {}), 240.f));
    CHECK(near(excamera::ground_clearance(pivot, down, 240.f,
        {std::numeric_limits<float>::quiet_NaN(), true}), 0.f));
    CHECK(near(excamera::ground_clearance(pivot, down, 240.f, {1000.f, true}), 0.f));
    CHECK(near(excamera::ground_clearance(pivot, {0.f, 0.f, 0.f}, 240.f, {10.f, true}), 0.f));
    CHECK(near(excamera::ground_clearance(pivot, down, 0.f, {10.f, true}), 0.f));
    return true;
}

bool invalid_contact_sets_and_stale_positions_are_rejected() {
    const excamera::Vec3 eye{8100.f, 196.f, 3900.f};
    CHECK(!excamera::evaluate_ground_contacts(nullptr, 1, eye).available);
    CHECK(!excamera::evaluate_ground_contacts(nullptr, 0, eye).available);
    const excamera::GroundContact far[]{{{8100.f, 10.f, 3900.f}, 2}};
    CHECK(!excamera::evaluate_ground_contacts(far, 1, {0.f, 0.f, 0.f}).available);
    CHECK(!excamera::evaluate_ground_contacts(far, 1, {8100.f, 1000.f, 3900.f}).available);
    CHECK(!excamera::evaluate_ground_contacts(far, 1, {8301.f, 196.f, 3900.f}).available);
    const excamera::GroundContact unsupported[]{{{8100.f, 10.f, 3900.f}, 0}};
    CHECK(!excamera::evaluate_ground_contacts(unsupported, 1, eye).available);
    const excamera::GroundContact too_many[17]{};
    CHECK(!excamera::evaluate_ground_contacts(too_many, 17, eye).available);
    return true;
}

bool sloped_and_unbounded_inputs_remain_finite() {
    const excamera::Vec3 pivot{8100.f, 224.f, 3900.f};
    const excamera::GroundConstraint ground{10.f, true};
    const excamera::Vec3 back = excamera::normalized({0.f, -2.f, -1.f});
    const float expected = (224.f - 10.f - excamera::ground_margin_cm) / -back.y;
    CHECK(near(excamera::ground_clearance(pivot, back, 240.f, ground), expected));
    CHECK(near(excamera::ground_clearance(pivot, back, 80.f, ground), 80.f));
    CHECK(near(excamera::ground_clearance({std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f},
        back, 240.f, ground), 0.f));
    CHECK(near(excamera::ground_clearance(pivot, back,
        std::numeric_limits<float>::infinity(), ground), 0.f));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"supported contacts choose highest valid height", supported_contacts_choose_highest_valid_height},
        {"missing or invalid support fails only downward booms", missing_or_invalid_support_fails_only_downward_booms},
        {"invalid contact sets and stale positions are rejected", invalid_contact_sets_and_stale_positions_are_rejected},
        {"sloped and unbounded inputs remain finite", sloped_and_unbounded_inputs_remain_finite},
    });
}
