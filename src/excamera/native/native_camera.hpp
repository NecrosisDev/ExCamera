#pragma once

#include <cstdint>

#include <exanima/result.h>
#include <exanima/runtime.h>
#include "hook_broker.hpp"

namespace exanima::camera {

Result start(HookBroker& broker) noexcept;
Result stop() noexcept;
Result set_mode(uint32_t mode) noexcept;
Result set_look(float yaw, float pitch) noexcept;
Result set_distance(float distance) noexcept;
Result diagnostics(ExaCameraDiagnostics& output) noexcept;
uint32_t fault_count() noexcept;
bool restoration_failed() noexcept;
Result last_error() noexcept;

} // namespace exanima::camera
