#pragma once

#include <filesystem>

#include "controller.hpp"

namespace excamera::settings {

std::filesystem::path path();
CameraSettings load(const std::filesystem::path& file);
bool save(const std::filesystem::path& file, const CameraSettings& settings) noexcept;

} // namespace excamera::settings
