#include "settings_model.h"
#include "settings_presentation.h"

#include <algorithm>
#include <climits>
#include <cwchar>
#include <type_traits>

#include "../test_support.hpp"

namespace {

using panel::ModSettings;
using panel::setting::count;

namespace short_label_probe {
struct Fallback {};
template <typename... Args>
inline Fallback settingShortLabel(Args&&...) {
    return {};
}

inline const wchar_t* get(int row) {
    using namespace panel;
    auto result = settingShortLabel(row);
    if constexpr (std::is_convertible_v<decltype(result), const wchar_t*>) {
        return result;
    } else {
        return panel::settingLabel(row);
    }
}
} // namespace short_label_probe

bool valid_ui_text(const wchar_t* text) {
    return text != nullptr && text[0] != L'\0' && std::wcscmp(text, L"(null)") != 0;
}

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

bool normalize_sanitizes_invalid_lean_bindings() {
    const int invalidKeys[] = {VK_LBUTTON, VK_F7, -5, 999};
    for (int bad : invalidKeys) {
        ModSettings settings;
        settings.leanKey = bad;
        settings.leanLeftKey = bad;
        settings.leanRightKey = bad;
        settings.rotateHeldKey = bad;
        settings.kickKey = bad;
        settings.jumpKey = bad;
        settings.trackingKey = bad;
        settings.normalize();
        CHECK(settings.leanKey == 0);
        CHECK(settings.leanLeftKey == 0);
        CHECK(settings.leanRightKey == 0);
        CHECK(settings.rotateHeldKey == 0);
        CHECK(settings.kickKey == 0);
        CHECK(settings.jumpKey == 0);
        CHECK(settings.trackingKey == 0);
        CHECK(!settings.leanKeysMode);
    }

    ModSettings valid;
    valid.leanKey = VK_MENU;
    valid.leanLeftKey = 'Q';
    valid.leanRightKey = 'E';
    valid.normalize();
    CHECK(valid.leanKey == VK_MENU);
    CHECK(valid.leanLeftKey == 'Q');
    CHECK(valid.leanRightKey == 'E');
    CHECK(valid.leanKeysMode);
    return true;
}

bool every_row_has_valid_presentation_labels_and_tips() {
    const ModSettings defaults;
    for (int row = 0; row < count; ++row) {
        CHECK(valid_ui_text(ModSettings::label(row)));
        CHECK(valid_ui_text(panel::settingLabel(row)));
        CHECK(valid_ui_text(short_label_probe::get(row)));
        CHECK(valid_ui_text(panel::settingTip(row)));
        const std::wstring rendered = panel::settingValue(defaults, row);
        CHECK(rendered.find(L"(null)") == std::wstring::npos);
    }
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
        {"normalize sanitizes invalid lean bindings", normalize_sanitizes_invalid_lean_bindings},
        {"every row has valid presentation labels and tips", every_row_has_valid_presentation_labels_and_tips},
    });
}
