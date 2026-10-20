#pragma once
#include "settings_model.h"
namespace panel {
std::wstring settingValue(const ModSettings& settings,int id);
const wchar_t* settingLabel(int id);
} // namespace panel
