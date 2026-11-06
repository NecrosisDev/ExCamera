#include "balance_shadow_model.h"

#include <cmath>
#include <vector>

#include "../test_support.hpp"

namespace {

namespace b = fp::balance;

bool near_equal(float a, float b, float eps = 1e-3f) { return std::isfinite(a) && std::fabs(a - b) <= eps; }

// A still body: nodes spread around a centre, the same three steps running.
struct Body {
    std::vector<b::V3> now, before, before2;
    std::vector<b::V2> feet;
    b::Sample sample(float ground = 0) const {
        b::Sample s;
        s.now = now.data(), s.before = before.data(), s.before2 = before2.data();
        s.nodes = unsigned(now.size());
        s.footprint = feet.data(), s.footprintCount = unsigned(feet.size());
        s.ground = ground, s.grounded = !feet.empty();
        return s;
    }
};
Body standing(float comX = 0, float stepX = 0) {
    Body body;
    for (float y : {10.f, 50.f, 90.f, 130.f})
        for (float x : {-10.f, 10.f}) {
            body.now.push_back({x + comX, y, 0});
            body.before.push_back({x + comX - stepX, y, 0});
            body.before2.push_back({x + comX - 2 * stepX, y, 0});
        }
    // two feet, each a 25 x 10 cm rectangle, 20 cm apart side to side
    for (float x : {-15.f, 15.f})
        for (float dx : {-5.f, 5.f})
            for (float dz : {-10.f, 15.f}) body.feet.push_back({x + dx, dz});
    return body;
}

bool outline_drops_inner_and_collinear_points() {
    b::V2 p[] = {{0, 0}, {10, 0}, {5, 0}, {10, 10}, {0, 10}, {5, 5}};
    const auto o = b::outlineOf<8>(p, 6);
    CHECK(o.count == 4);
    CHECK(near_equal(b::signedDistance(o, {5, 5}), 5));
    CHECK(near_equal(b::signedDistance(o, {5, 2}), 2));
    CHECK(near_equal(b::signedDistance(o, {13, 5}), -3));
    b::V2 one[] = {{1, 1}, {1, 1}};
    const auto point = b::outlineOf<8>(one, 2);
    CHECK(point.count == 1 && near_equal(b::signedDistance(point, {4, 5}), -5));
    b::V2 two[] = {{0, 0}, {10, 0}};
    const auto segment = b::outlineOf<8>(two, 2);
    CHECK(segment.count == 2 && near_equal(b::signedDistance(segment, {5, 3}), -3));
    return true;
}

bool still_body_over_its_feet_is_inside() {
    const Body body = standing();
    const auto m = b::measure(body.sample());
    CHECK(m.valid && !m.nonFinite);
    CHECK(near_equal(m.com.y, 70) && near_equal(m.height, 70));
    CHECK(near_equal(m.velocity.x, 0) && near_equal(m.shake, 0));
    CHECK(near_equal(m.omega, std::sqrt(b::gravity / 70.f)));
    CHECK(m.margin > 9.9f && m.margin < 10.1f); // the feet span x -20..20, z -10..15: nearest edge 10 cm in z
    return true;
}

bool moving_body_heads_past_its_feet() {
    // 1 cm a step sideways is 83 cm/s; at 70 cm the capture point leads by 83 / sqrt(980.665 / 70) = 22 cm.
    const Body body = standing(10, 1);
    const auto m = b::measure(body.sample());
    CHECK(near_equal(m.velocity.x, 83, 1e-2f));
    CHECK(near_equal(m.xcom.x, 10 + 83 / std::sqrt(b::gravity / 70.f), 1e-2f));
    CHECK(m.margin < 0 && near_equal(m.margin, -(m.xcom.x - 20), 1e-2f));
    return true;
}

bool non_finite_nodes_are_reported_not_measured() {
    Body body = standing();
    body.now[3].y = NAN;
    const auto m = b::measure(body.sample());
    CHECK(!m.valid && m.nonFinite);
    b::Tracker t;
    CHECK(t.feed({}, m) & b::Tracker::bit(b::Event::nonFinite));
    return true;
}

bool shaking_counts_reversals_not_swings() {
    // One node accelerates 2 cm/step^2 against the rest (a swing).
    Body swing = standing();
    swing.before[0].x += 1, swing.before2[0].x += 4;
    std::vector<b::V3> first(swing.now.size()), second(swing.now.size());
    b::Sample s = swing.sample();
    s.accelerations = first.data();
    CHECK(near_equal(b::measure(s).shake, 0)); // no previous step yet
    s.previous = first.data(), s.accelerations = second.data();
    CHECK(near_equal(b::measure(s).shake, 0, 1e-1f)); // the same push again: a swing, not a shake
    // The node's acceleration reverses from one step to the next: jitter.
    for (auto& a : first) a = {-a.x, -a.y, -a.z};
    s.previous = first.data();
    CHECK(b::measure(s).shake > 1000);
    // The whole body accelerating counts for nothing.
    Body moving = standing(0, 1);
    for (auto& v : moving.before2) v.x -= 3;
    std::vector<b::V3> body(moving.now.size(), b::V3{-5, 0, 0});
    b::Sample whole = moving.sample();
    whole.previous = body.data();
    CHECK(near_equal(b::measure(whole).shake, 0, 1e-1f));
    return true;
}

uint32_t run(b::Tracker& t, const b::GameState& g, const b::Measure& m, unsigned steps) {
    uint32_t all = 0;
    for (unsigned i = 0; i < steps; ++i) all |= t.feed(g, m);
    return all;
}

bool conditions_report_once_after_their_duration() {
    b::Measure m = b::measure(standing().sample());
    b::GameState g;
    g.footDown = false; // nothing under the feet while standing and not dropping
    b::Tracker t;
    CHECK(!(run(t, g, m, b::floatingSteps - 1) & b::Tracker::bit(b::Event::floating)));
    CHECK(t.feed(g, m) & b::Tracker::bit(b::Event::floating));
    CHECK(!(run(t, g, m, 200) & b::Tracker::bit(b::Event::floating)));
    g.motion = b::jumpMotion; // a jump clears it
    CHECK(!(run(t, g, m, 200) & b::Tracker::bit(b::Event::floating)));
    b::Tracker modJump;
    b::GameState pushed;
    pushed.jumping = true; // and so does the mod's own jump, which plays no clip
    CHECK(!(run(modJump, pushed, m, 200) & b::Tracker::bit(b::Event::floating)));
    b::Tracker hang; // the push ends at take-off; the flight after it is still the jump
    run(hang, pushed, m, 3);
    b::GameState air; // no foot down, no clip
    CHECK(!(run(hang, air, m, 200) & b::Tracker::bit(b::Event::floating)));
    b::GameState landed;
    landed.footDown = true;
    hang.feed(landed, m); // back on the ground: floating counts again
    CHECK(run(hang, air, m, b::floatingSteps) & b::Tracker::bit(b::Event::floating));
    return true;
}

bool held_up_needs_feet_down_and_no_step() {
    b::Measure m = b::measure(standing().sample());
    m.margin = b::heldUpMargin - 1;
    b::GameState g;
    g.footDown = true;
    b::Tracker t;
    CHECK(run(t, g, m, b::heldUpSteps) & b::Tracker::bit(b::Event::heldUp));
    b::Tracker stepping;
    g.stepping = true;
    CHECK(!(run(stepping, g, m, 200) & b::Tracker::bit(b::Event::heldUp)));
    return true;
}

bool falls_are_split_by_the_capture_point() {
    b::GameState up, fell;
    up.footDown = true;
    fell.state = 0x200;
    b::Measure inside = b::measure(standing().sample()), outside = inside;
    outside.margin = -5;
    b::Tracker a;
    run(a, up, inside, 10);
    CHECK(a.feed(fell, inside) & b::Tracker::bit(b::Event::fallByRule));
    b::Tracker c;
    run(c, up, outside, 5);
    run(c, up, inside, 10);
    CHECK(c.feed(fell, inside) & b::Tracker::bit(b::Event::fallPhysical));
    CHECK(!(c.feed(up, inside) & b::Tracker::bit(b::Event::recovered))); // a fall ends the stumble
    return true;
}

bool a_fall_from_the_air_is_by_rule() {
    b::Measure outside = b::measure(standing().sample());
    outside.margin = -5; // even with the capture point past the feet just before
    b::GameState up, air, fell;
    up.footDown = true;
    fell.state = 0x200;
    b::Tracker t;
    run(t, up, outside, 5);
    run(t, air, outside, b::airborneSteps);
    CHECK(t.feed(fell, outside) & b::Tracker::bit(b::Event::fallByRule));
    CHECK(t.lastFallAirborne);
    b::Tracker ground;
    run(ground, up, outside, 5);
    CHECK(ground.feed(fell, outside) & b::Tracker::bit(b::Event::fallPhysical));
    CHECK(!ground.lastFallAirborne);
    return true;
}

bool a_stumble_that_comes_back_inside_is_a_recovery() {
    b::GameState up;
    up.footDown = true;
    b::Measure inside = b::measure(standing().sample()), outside = inside;
    outside.margin = -12;
    b::Tracker t;
    run(t, up, outside, 30);
    CHECK(!(run(t, up, inside, b::recoverSteps - 1) & b::Tracker::bit(b::Event::recovered)));
    CHECK(t.feed(up, inside) & b::Tracker::bit(b::Event::recovered));
    CHECK(t.lastStumbleSteps == 30 && near_equal(t.lastStumbleDepth, -12));
    CHECK(!(run(t, up, inside, 100) & b::Tracker::bit(b::Event::recovered)));
    return true;
}

bool getting_up_back_to_lying_is_a_failed_get_up_and_long_down_is_stuck() {
    b::Measure m = b::measure(standing().sample());
    b::GameState lying, rising;
    lying.state = 0x200, rising.state = 0x300;
    b::Tracker t;
    t.feed(lying, m);
    t.feed(rising, m);
    CHECK(t.feed(lying, m) & b::Tracker::bit(b::Event::failedGetUp));
    CHECK(run(t, lying, m, b::stuckSteps) & b::Tracker::bit(b::Event::stuckDown));
    CHECK(!(run(t, lying, m, 100) & b::Tracker::bit(b::Event::stuckDown)));
    b::GameState deadState;
    deadState.state = 0xA00;
    b::Tracker corpse;
    CHECK(!(run(corpse, deadState, m, b::stuckSteps + 5) & b::Tracker::bit(b::Event::stuckDown)));
    return true;
}

bool kick_window_covers_the_kick_and_its_tail() {
    b::KickWindow k;
    CHECK(!k.feed(0, 0));
    CHECK(k.feed(b::kickMotion, 100) && k.feed(b::kickMotion, 900));
    CHECK(k.feed(0, 1000) && k.feed(0, 900 + b::kickTailMs - 1));
    CHECK(!k.feed(0, 900 + b::kickTailMs));
    CHECK(!k.feed(5, 5000));
    CHECK(k.feed(b::kickMotion, 6000)); // a second kick opens a new window
    return true;
}

bool spread_quantiles_are_bin_edges() {
    b::Spread s;
    s.low = 0, s.high = 32;
    for (int i = 0; i < 32; ++i) s.add(float(i) + .5f);
    CHECK(s.count == 32 && near_equal(s.mean(), 16) && near_equal(s.peak, 31.5f));
    CHECK(near_equal(s.quantile(.5f), 16) && near_equal(s.quantile(.95f), 31));
    s.add(1000);
    CHECK(near_equal(s.quantile(1), 32));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"outline drops inner and collinear points", outline_drops_inner_and_collinear_points},
        {"still body over its feet is inside", still_body_over_its_feet_is_inside},
        {"moving body heads past its feet", moving_body_heads_past_its_feet},
        {"non-finite nodes are reported, not measured", non_finite_nodes_are_reported_not_measured},
        {"shaking counts reversals, not swings", shaking_counts_reversals_not_swings},
        {"a fall from the air is a fall by rule", a_fall_from_the_air_is_by_rule},
        {"conditions report once after their duration", conditions_report_once_after_their_duration},
        {"held up needs feet down and no step", held_up_needs_feet_down_and_no_step},
        {"falls are split by the capture point", falls_are_split_by_the_capture_point},
        {"a stumble that comes back inside is a recovery", a_stumble_that_comes_back_inside_is_a_recovery},
        {"failed get-up and stuck down", getting_up_back_to_lying_is_a_failed_get_up_and_long_down_is_stuck},
        {"kick window covers the kick and its tail", kick_window_covers_the_kick_and_its_tail},
        {"spread quantiles are bin edges", spread_quantiles_are_bin_edges},
    });
}
