#include "settings_model.h"

#include <algorithm>
#include <climits>

#include "../test_support.hpp"

namespace {

using panel::ModSettings;
using panel::setting::count;

bool defaults_are_already_normalized() {
    ModSettings defaults, normalized;
    normalized.normalize();
    CHECK(normalized == defaults);
    for (int row = 0; row < count; ++row) {
        const auto& info = panel::rowInfo(row);
        if (info.value) {
            const int value = defaults.*info.value;
            if (info.range.count) {
                CHECK(value >= info.range.min);
                CHECK(value <= info.range.max);
            }
        }
    }
    return true;
}

bool numbers_step_and_clamp_to_their_range() {
    ModSettings settings;
    CHECK(settings.step(panel::setting::fov, 1));
    CHECK(settings.fov == 80);
    CHECK(settings.step(panel::setting::fov, 1000));
    CHECK(settings.fov == 130);
    CHECK(settings.step(panel::setting::fov, -1000));
    CHECK(settings.fov == 30);

    CHECK(settings.step(panel::setting::sensitivity, 1));
    CHECK(settings.sensitivity == 110);
    CHECK(settings.step(panel::setting::sensitivity, -1000));
    CHECK(settings.sensitivity == 10);

    // Inherited third-person FOV: one step from 0 starts from the base field of view,
    // and stepping below the minimum returns to inheritance.
    settings.fov = 75;
    CHECK(settings.step(panel::setting::tpFov, 1));
    CHECK(settings.tpFov == 80);
    settings.tpFov = 30;
    CHECK(settings.step(panel::setting::tpFov, -1));
    CHECK(settings.tpFov == 0);
    return true;
}

bool switches_and_choices_step() {
    ModSettings settings;
    CHECK(settings.step(panel::setting::hideHelmet, 1));
    CHECK(settings.hideHelmet);
    CHECK(settings.step(panel::setting::hideHelmet, 1));
    CHECK(!settings.hideHelmet);

    // Everyone -> You -> Off -> You -> Everyone.
    CHECK(settings.step(panel::setting::fingerPoses, -1));
    CHECK(settings.fingerPoses);
    CHECK(!settings.fingerPosesOthers);
    CHECK(settings.step(panel::setting::fingerPoses, -1));
    CHECK(!settings.fingerPoses);
    CHECK(!settings.fingerPosesOthers);
    CHECK(settings.step(panel::setting::fingerPoses, 1));
    CHECK(settings.fingerPoses);
    CHECK(!settings.fingerPosesOthers);
    CHECK(settings.step(panel::setting::fingerPoses, 1));
    CHECK(settings.fingerPoses);
    CHECK(settings.fingerPosesOthers);

    const int oldKey = settings.rotateHeldKey;
    CHECK(settings.step(panel::setting::rotateHeldKey, 1));
    CHECK(settings.rotateHeldKey != oldKey);
    return true;
}

bool fixed_rows_do_not_step() {
    ModSettings settings, before = settings;
    CHECK(!settings.step(panel::setting::headSteady, 1));
    CHECK(!settings.step(panel::setting::grabRange, -1));
    CHECK(!settings.step(panel::setting::holdHeading, 1));
    CHECK(settings == before);
    return true;
}

bool normalize_clamps_and_repairs_values() {
    ModSettings settings;
    settings.fov = INT_MAX;
    settings.sensitivity = INT_MIN;
    settings.indicator = 7;
    settings.tpFov = 999;
    settings.crouchMode = 0;
    settings.trackingKey = VK_LBUTTON;
    settings.normalize();
    CHECK(settings.fov == 130);
    CHECK(settings.sensitivity == 10);
    CHECK(settings.indicator == 1);
    CHECK(settings.tpFov == 130);
    CHECK(settings.crouchMode == 1);
    CHECK(settings.trackingKey == 0);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"defaults are already normalized", defaults_are_already_normalized},
        {"numbers step and clamp to their range", numbers_step_and_clamp_to_their_range},
        {"switches and choices step", switches_and_choices_step},
        {"fixed rows do not step", fixed_rows_do_not_step},
        {"normalize clamps and repairs values", normalize_clamps_and_repairs_values},
    });
}
