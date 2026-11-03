#pragma once

#include <string>

namespace exanima::platform {

inline constexpr const char* standard_exanima_sha256 =
    "af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70";

std::string file_sha256(const wchar_t* path);
bool supported_game_executable(const wchar_t* path);

} // namespace exanima::platform
