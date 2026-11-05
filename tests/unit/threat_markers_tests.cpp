#include "threat_markers.h"

#include <cmath>

#include "../test_support.hpp"

namespace {

namespace t = fp::threat;

t::Sample guarding() {
    t::Sample s;
    s.motion = 3;
    s.targetsPlayer = true;
    s.distance = 300.f;
    return s;
}

// A camera at the origin looking down -z: right +x, up +y, back +z.
const float camera[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

bool a_guard_or_an_attack_aimed_at_you_is_a_threat() {
    t::Sample s = guarding();
    CHECK(t::isThreat(s));
    s.motion = 0;
    CHECK(!t::isThreat(s)); // neither on guard nor attacking
    s.stance = true;
    CHECK(t::isThreat(s));
    s.stance = false;
    s.motion = 7;
    CHECK(t::isThreat(s));
    return true;
}

bool the_dead_the_down_others_targets_yourself_and_the_far_are_not() {
    t::Sample s = guarding();
    s.state = 0x800;
    CHECK(!t::isThreat(s));
    s.state = 0x200;
    CHECK(!t::isThreat(s));
    s.state = 0x300;
    CHECK(!t::isThreat(s));
    s.state = 0;
    s.targetsPlayer = false;
    CHECK(!t::isThreat(s));
    s.targetsPlayer = true;
    s.self = true;
    CHECK(!t::isThreat(s));
    s.self = false;
    s.distance = t::rangeCm + 1.f;
    CHECK(!t::isThreat(s));
    s.distance = NAN;
    CHECK(!t::isThreat(s));
    return true;
}

bool winding_up_ends_at_the_commit_point() {
    t::Sample s = guarding();
    s.motion = 6;
    s.progress = .1f;
    CHECK(t::windingUp(s));
    s.progress = .3f;
    CHECK(!t::windingUp(s));
    s.motion = 11; // the Stab commits at 0.4
    s.progress = .35f;
    CHECK(t::windingUp(s));
    s.motion = 3;
    s.progress = .1f;
    CHECK(!t::windingUp(s));
    return true;
}

bool a_threat_ahead_gets_no_marker_and_one_aside_or_behind_does() {
    float angle = 0;
    const float ahead[3] = {0, 0, -500};
    CHECK(!t::markerAngle(ahead, camera, 1920, 1080, 900, angle));
    const float right[3] = {800, 0, -100};
    CHECK(t::markerAngle(right, camera, 1920, 1080, 900, angle));
    CHECK(std::fabs(angle) < .2f); // to the right
    const float behind[3] = {0, 0, 400};
    CHECK(t::markerAngle(behind, camera, 1920, 1080, 900, angle));
    CHECK(std::fabs(angle - 1.5707963f) < 1e-3f); // straight down
    const float left[3] = {-400, 0, 50};
    CHECK(t::markerAngle(left, camera, 1920, 1080, 900, angle));
    CHECK(std::fabs(std::fabs(angle) - 3.1415927f) < .2f);
    return true;
}

bool markers_are_two_bands_on_the_ring() {
    float out[128];
    int counts[2];
    CHECK(t::markerArc(960, 540, 1920, 1080, 0.f, out, 128, counts));
    CHECK(counts[0] > 0 && counts[1] > 0);
    const float r = 1080 * t::ringFraction;
    const float* v = out + counts[0] * 3; // the line band, centred on the ring
    for (int i = 0; i < counts[1]; ++i) {
        const float dx = v[i * 3] - 960, dy = v[i * 3 + 1] - 540;
        CHECK(std::fabs(std::sqrt(dx * dx + dy * dy) - r) < 3.f);
    }
    CHECK(!t::markerArc(960, 540, 1920, 1080, NAN, out, 128, counts));
    return true;
}

bool fades_are_short_and_reach_their_target() {
    CHECK(t::fade(0.f, 1.f, t::fadeInMs) == 1.f);
    CHECK(t::fade(1.f, 0.f, t::fadeOutMs) == 0.f);
    const float half = t::fade(0.f, 1.f, t::fadeInMs * .5f);
    CHECK(half > .4f && half < .6f);
    CHECK(t::fade(.5f, 1.f, 0.f) == .5f);
    return true;
}

bool the_setting_reads_its_words() {
    CHECK(t::modeFromText(L"off") == t::off);
    CHECK(t::modeFromText(L"Always") == t::always);
    CHECK(t::modeFromText(L"combat") == t::combatOnly);
    CHECK(t::modeFromText(nullptr) == t::combatOnly);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"a guard or an attack aimed at you is a threat", a_guard_or_an_attack_aimed_at_you_is_a_threat},
        {"the dead, the down, others' targets, yourself and the far are not",
         the_dead_the_down_others_targets_yourself_and_the_far_are_not},
        {"winding up ends at the commit point", winding_up_ends_at_the_commit_point},
        {"a threat ahead gets no marker, one aside or behind does",
         a_threat_ahead_gets_no_marker_and_one_aside_or_behind_does},
        {"markers are two bands on the ring", markers_are_two_bands_on_the_ring},
        {"fades are short and reach their target", fades_are_short_and_reach_their_target},
        {"the setting reads its words", the_setting_reads_its_words},
    });
}
