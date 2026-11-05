#include "settings_model.h"
#include "settings_presentation.h"
#include "settings_draft.h"
#include "settings_paths.h"

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

bool f6_cycle_row_steps_saves_and_normalizes() {
    ModSettings settings;
    CHECK(settings.f6Cycle == 0);
    CHECK(settings.step(panel::setting::f6Cycle, 1));
    CHECK(settings.f6Cycle == 1);
    CHECK(settings.step(panel::setting::f6Cycle, 1));
    CHECK(settings.f6Cycle == 0);
    const auto& info = panel::rowInfo(panel::setting::f6Cycle);
    CHECK(info.key != nullptr && info.fromText && info.toText);
    CHECK(info.fromText(L"two") == 1);
    CHECK(info.fromText(L" Three ") == 0);
    CHECK(info.fromText(L"four") == panel::badText);
    CHECK(info.fromText(info.toText(1)) == 1);
    settings.f6Cycle = 7;
    settings.normalize();
    CHECK(settings.f6Cycle == 0);
    return true;
}

bool hit_reaction_row_is_a_percent_slider() {
    ModSettings settings;
    CHECK(settings.hitReaction == 100);
    CHECK(!settings.step(panel::setting::hitReaction, 1));
    CHECK(settings.step(panel::setting::hitReaction, -3));
    CHECK(settings.hitReaction == 70);
    CHECK(settings.step(panel::setting::hitReaction, -100));
    CHECK(settings.hitReaction == 0);
    settings.hitReaction = 999;
    settings.normalize();
    CHECK(settings.hitReaction == 100);
    CHECK(panel::rowInfo(panel::setting::hitReaction).key != nullptr);
    return true;
}

bool normalize_drops_duplicate_mod_bindings() {
    ModSettings settings;
    settings.rotateHeldKey = 'R';
    settings.kickKey = 'R';
    settings.jumpKey = 'F';
    settings.swapShoulderKey = 'F';
    settings.trackingKey = 'G';
    settings.leanLeftKey = 'G';
    settings.leanRightKey = 'E';
    settings.normalize();
    CHECK(settings.rotateHeldKey == 'R');
    CHECK(settings.kickKey == 0);
    CHECK(settings.jumpKey == 'F');
    CHECK(settings.swapShoulderKey == 0);
    CHECK(settings.trackingKey == 'G');
    CHECK(settings.leanLeftKey == 0);
    CHECK(settings.leanRightKey == 'E');

    ModSettings swap;
    CHECK(swap.swapShoulderKey == 0);
    swap.swapShoulderKey = VK_F6; // F6-F11 belong to the mod
    swap.normalize();
    CHECK(swap.swapShoulderKey == 0);
    swap.swapShoulderKey = 'Q';
    swap.normalize();
    CHECK(swap.swapShoulderKey == 'Q');
    CHECK(panel::rowInfo(panel::setting::swapShoulderKey).kind == panel::RowInfo::keyList);
    return true;
}

bool unavailable_and_faulted_rows_say_why() {
    ModSettings settings;
    CHECK(settings.inactiveReason(panel::setting::farRooms) == nullptr);
    settings.unavailable = panel::capFarRooms | panel::capViewEffects;
    CHECK(std::wcsstr(settings.inactiveReason(panel::setting::farRooms), L"game version") != nullptr);
    CHECK(std::wcsstr(settings.inactiveReason(panel::setting::ambientOcclusion), L"game version") != nullptr);
    CHECK(settings.inactiveReason(panel::setting::fingerPoses) == nullptr);
    settings.faulted = panel::capViewEffects | panel::capFingers;
    // A fault outranks a missing feature: restarting is the thing to do.
    CHECK(std::wcsstr(settings.inactiveReason(panel::setting::depthOfField), L"Restart") != nullptr);
    CHECK(std::wcsstr(settings.inactiveReason(panel::setting::fingerPosesOthers), L"Restart") != nullptr);
    CHECK(settings.inactiveReason(panel::setting::fov) == nullptr);
    // Runtime status, not a saved value: it never makes two settings differ.
    CHECK(settings == ModSettings{});
    for (int row = 0; row < count; ++row)
        if (panel::rowCapability(row)) CHECK(panel::rowInfo(row).editable());
    panel::SettingsDraft draft;
    draft.begin(ModSettings{});
    CHECK(draft.refreshStatus(settings));
    CHECK(draft.value.faulted == settings.faulted && draft.value.unavailable == settings.unavailable);
    CHECK(!draft.refreshStatus(settings));
    return true;
}

bool look_follow_row_is_a_saved_switch() {
    ModSettings settings;
    CHECK(settings.tpLookFollow);
    const auto& info = panel::rowInfo(panel::setting::tpLookFollow);
    CHECK(info.checkbox() && info.key != nullptr);
    CHECK(settings.step(panel::setting::tpLookFollow, 1));
    CHECK(!settings.tpLookFollow);
    return true;
}

bool knockdown_view_row_steps_and_saves() {
    ModSettings settings;
    CHECK(settings.fpKnockdown == 1);
    CHECK(settings.step(panel::setting::fpKnockdown, 1));
    CHECK(settings.fpKnockdown == 0);
    const auto& info = panel::rowInfo(panel::setting::fpKnockdown);
    CHECK(info.fromText(L"first") == 0 && info.fromText(L"Third") == 1 && info.fromText(L"x") == panel::badText);
    settings.fpKnockdown = 3;
    settings.normalize();
    CHECK(settings.fpKnockdown == 1);
    return true;
}

bool kick_style_row_steps_and_saves() {
    ModSettings settings;
    CHECK(settings.kickStyle == 0);
    CHECK(settings.step(panel::setting::kickStyle, 1));
    CHECK(settings.kickStyle == 1);
    const auto& info = panel::rowInfo(panel::setting::kickStyle);
    CHECK(info.key != nullptr && info.fromText(L"Animated") == 1 && info.fromText(L"classic") == 0 && info.fromText(L"x") == panel::badText);
    CHECK(panel::rowCapability(panel::setting::kickStyle) == panel::capAnimatedKick);
    settings.kickStyle = 7;
    settings.normalize();
    CHECK(settings.kickStyle == 0);
    return true;
}

bool controller_rows_step_save_and_drop_duplicates() {
    ModSettings settings;
    CHECK(settings.padKick == fp::padY && settings.padJump == fp::padRS);
    CHECK(settings.padGuard == fp::padNone && settings.padShoulder == fp::padNone);
    CHECK(settings.step(panel::setting::padGuard, -1));
    CHECK(settings.padGuard == fp::padRight); // backwards from None wraps to the last button
    CHECK(settings.step(panel::setting::padGuard, 1));
    CHECK(settings.padGuard == fp::padNone);
    const auto& info = panel::rowInfo(panel::setting::padGuard);
    CHECK(info.kind == panel::RowInfo::choice && !wcscmp(info.section, L"Controller") && !wcscmp(info.key, L"Guard"));
    CHECK(info.fromText(L"LT") == fp::padLT && info.fromText(L"rs") == fp::padRS && info.fromText(L"x") == fp::padX);
    CHECK(info.fromText(L"lt2") == panel::badText && info.fromText(L"") == panel::badText);
    CHECK(info.fromText(info.toText(fp::padStart)) == fp::padStart);
    settings.padGuard = fp::padLB;
    settings.padShoulder = fp::padLB; // the later duplicate is unbound
    settings.padJump = 99;            // out of range
    settings.normalize();
    CHECK(settings.padGuard == fp::padLB && settings.padShoulder == fp::padNone && settings.padJump == fp::padNone);
    settings.padKick = fp::padRight; // above the other choices' 0..2 is still a button
    settings.normalize();
    CHECK(settings.padKick == fp::padRight);
    return true;
}

bool settings_file_candidates_follow_portable_mode() {
    // A portable copy: only the file beside it, which safe start then backs up and resets.
    const auto portable = fp::settingsCandidates(L"E:\\test\\", true, L"C:\\Docs", L"C:\\Users\\p\\Documents");
    CHECK(portable.paths[0] == L"E:\\test\\first-person.ini");
    CHECK(portable.paths[1].empty() && portable.paths[2].empty() && portable.paths[3].empty());
    CHECK(portable.target() == L"E:\\test\\first-person.ini");
    // Otherwise Documents\ExCamera first, its USERPROFILE twin, then the ExaniMate files; the target is Documents.
    const auto user = fp::settingsCandidates(L"E:\\test\\", false, L"C:\\Docs", L"C:\\Users\\p\\Documents");
    CHECK(user.paths[0] == L"C:\\Docs\\ExCamera\\first-person.ini");
    CHECK(user.paths[1] == L"C:\\Users\\p\\Documents\\ExCamera\\first-person.ini");
    CHECK(user.paths[2] == L"C:\\Docs\\ExaniMate\\first-person.ini");
    CHECK(user.target() == user.paths[0]);
    const auto noDocs = fp::settingsCandidates(L"E:\\test\\", false, L"", L"C:\\Users\\p\\Documents");
    CHECK(noDocs.paths[0].empty() && noDocs.target() == noDocs.paths[1]);
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
        {"F6 cycle row steps, saves and normalizes", f6_cycle_row_steps_saves_and_normalizes},
        {"hit reaction row is a percent slider", hit_reaction_row_is_a_percent_slider},
        {"normalize drops duplicate mod bindings", normalize_drops_duplicate_mod_bindings},
        {"unavailable and faulted rows say why", unavailable_and_faulted_rows_say_why},
        {"look-follow row is a saved switch", look_follow_row_is_a_saved_switch},
        {"knockdown view row steps and saves", knockdown_view_row_steps_and_saves},
        {"kick style row steps and saves", kick_style_row_steps_and_saves},
        {"controller rows step, save and drop duplicates", controller_rows_step_save_and_drop_duplicates},
        {"settings file candidates follow portable mode", settings_file_candidates_follow_portable_mode},
        {"every row has valid presentation labels and tips", every_row_has_valid_presentation_labels_and_tips},
    });
}
