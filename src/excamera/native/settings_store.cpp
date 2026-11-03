#include "settings_store.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <cstdio>
#include <optional>
#include <string>

namespace excamera::settings {
namespace {

std::optional<float> number(const wchar_t* file, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[64]{};
    if (!GetPrivateProfileStringW(section, key, L"", buffer, 64, file)) return std::nullopt;
    wchar_t* end = nullptr;
    const float value = std::wcstof(buffer, &end);
    if (end == buffer || !end || *end) return std::nullopt;
    return value;
}

bool write_number(const wchar_t* file, const wchar_t* section, const wchar_t* key, float value) {
    wchar_t text[64];
    swprintf_s(text, L"%.9g", value);
    return WritePrivateProfileStringW(section, key, text, file) != 0;
}

} // namespace

std::filesystem::path path() {
    wchar_t documents[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, documents))) return {};
    return std::filesystem::path(documents) / L"ExCamera" / L"controller.ini";
}

CameraSettings load(const std::filesystem::path& file) {
    CameraSettings result;
    if (file.empty()) return sanitize_camera_settings(result);
    const wchar_t* name = file.c_str();
    if (const auto value = number(name, L"Camera", L"FirstFov")) result.first_fov = *value;
    if (const auto value = number(name, L"Camera", L"ThirdFov")) result.third_fov = *value;
    if (const auto value = number(name, L"Camera", L"NearPlane")) result.near_plane = *value;
    if (const auto value = number(name, L"Camera", L"EyeSetback")) result.eye_setback = *value;
    if (const auto value = number(name, L"ThirdPerson", L"Distance")) result.third_distance = *value;
    if (const auto value = number(name, L"ThirdPerson", L"Shoulder")) result.third_shoulder = *value;
    if (const auto value = number(name, L"ThirdPerson", L"Height")) result.third_height = *value;
    if (const auto value = number(name, L"Input", L"Sensitivity")) result.look_sensitivity = *value;
    if (const auto value = number(name, L"Timing", L"TransitionSeconds")) result.transition_seconds = *value;
    if (const auto value = number(name, L"Timing", L"ReleaseSeconds")) result.release_seconds = *value;
    if (const auto value = number(name, L"Timing", L"InvalidHoldSeconds")) result.invalid_hold_seconds = *value;
    return sanitize_camera_settings(result);
}

bool save(const std::filesystem::path& file, const CameraSettings& settings) noexcept {
    if (file.empty()) return false;
    const CameraSettings value = sanitize_camera_settings(settings);
    try {
        const std::filesystem::path parent = file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        const wchar_t* name = temporary.c_str();
        bool ok = write_number(name, L"Camera", L"FirstFov", value.first_fov) &&
            write_number(name, L"Camera", L"ThirdFov", value.third_fov) &&
            write_number(name, L"Camera", L"NearPlane", value.near_plane) &&
            write_number(name, L"Camera", L"EyeSetback", value.eye_setback) &&
            write_number(name, L"ThirdPerson", L"Distance", value.third_distance) &&
            write_number(name, L"ThirdPerson", L"Shoulder", value.third_shoulder) &&
            write_number(name, L"ThirdPerson", L"Height", value.third_height) &&
            write_number(name, L"Input", L"Sensitivity", value.look_sensitivity) &&
            write_number(name, L"Timing", L"TransitionSeconds", value.transition_seconds) &&
            write_number(name, L"Timing", L"ReleaseSeconds", value.release_seconds) &&
            write_number(name, L"Timing", L"InvalidHoldSeconds", value.invalid_hold_seconds);
        if (!ok || MoveFileExW(name, file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            DeleteFileW(name);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace excamera::settings
