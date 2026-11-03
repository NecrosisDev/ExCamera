#include "lifecycle.hpp"

#include "../test_support.hpp"

namespace {

bool first_identity_is_a_change_and_repeat_is_stable() {
    excamera::LifecycleTracker tracker;
    const excamera::CameraIdentity identity{1, 2, 3, 4, 5};
    CHECK(tracker.changed(identity));
    CHECK(!tracker.changed(identity));
    return true;
}

bool player_and_scene_changes_are_detected() {
    excamera::LifecycleTracker tracker;
    CHECK(tracker.changed({1, 2, 3, 4, 5}));
    CHECK(tracker.changed({9, 2, 3, 4, 5}));
    CHECK(tracker.changed({9, 8, 3, 4, 5}));
    CHECK(tracker.changed({9, 8, 7, 4, 5}));
    CHECK(tracker.changed({9, 8, 7, 6, 5}));
    CHECK(tracker.changed({9, 8, 7, 6, 10}));
    return true;
}

bool reset_requires_fresh_initialization() {
    excamera::LifecycleTracker tracker;
    const excamera::CameraIdentity identity{1, 2, 3, 4, 5};
    CHECK(tracker.changed(identity));
    tracker.reset();
    CHECK(tracker.changed(identity));
    CHECK(!tracker.changed(identity));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"first identity is a change and repeat is stable", first_identity_is_a_change_and_repeat_is_stable},
        {"player and scene changes are detected", player_and_scene_changes_are_detected},
        {"reset requires fresh initialization", reset_requires_fresh_initialization},
    });
}
