#include <windows.h>

#include <exanima/runtime.h>

#include <cstring>
#include <mutex>

#include "native_camera.hpp"
#include "session.hpp"

namespace {
using exanima::Result;
std::mutex runtime_mutex;
exanima::Session runtime_session;
} // namespace

ExaResult ExaRuntimeStart(void) {
    try {
        const std::lock_guard lock(runtime_mutex);
        Result result = runtime_session.start();
        if (result != EXA_RESULT_OK) return result;
        if (GetModuleHandleW(L"Exanima.exe")) {
            result = exanima::camera::start(*runtime_session.broker());
            if (result != EXA_RESULT_OK) {
                (void)runtime_session.stop();
                runtime_session.fail(result);
                return result;
            }
            runtime_session.activate();
        }
        return EXA_RESULT_OK;
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaRuntimeStop(void) {
    try {
        const std::lock_guard lock(runtime_mutex);
        const Result camera_result = exanima::camera::stop();
        if (camera_result != EXA_RESULT_OK) {
            runtime_session.fail(camera_result);
            return camera_result;
        }
        return runtime_session.stop();
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaRuntimeGetStatus(ExaRuntimeStatus* status) {
    if (!status || status->struct_size < sizeof(ExaRuntimeStatus)) return EXA_RESULT_INVALID_ARGUMENT;
    try {
        const std::lock_guard lock(runtime_mutex);
        *status = runtime_session.status();
        status->module_faults = exanima::camera::fault_count();
        status->restoration_failed = exanima::camera::restoration_failed() ? 1u : 0u;
        const Result camera_error = exanima::camera::last_error();
        if (camera_error != EXA_RESULT_OK) status->last_error = camera_error;
        return EXA_RESULT_OK;
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaCameraSetMode(uint32_t mode) {
    if (mode > 2u) return EXA_RESULT_INVALID_ARGUMENT;
    try {
        const std::lock_guard lock(runtime_mutex);
        return exanima::camera::set_mode(mode);
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaCameraSetLook(uint64_t packed_yaw_pitch) {
    const uint32_t yaw_bits = static_cast<uint32_t>(packed_yaw_pitch & 0xffffffffu);
    const uint32_t pitch_bits = static_cast<uint32_t>(packed_yaw_pitch >> 32);
    float yaw, pitch;
    std::memcpy(&yaw, &yaw_bits, sizeof(yaw));
    std::memcpy(&pitch, &pitch_bits, sizeof(pitch));
    try {
        const std::lock_guard lock(runtime_mutex);
        return exanima::camera::set_look(yaw, pitch);
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaCameraSetDistance(uint32_t packed_distance) {
    float distance;
    std::memcpy(&distance, &packed_distance, sizeof(distance));
    try {
        const std::lock_guard lock(runtime_mutex);
        return exanima::camera::set_distance(distance);
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}

ExaResult ExaCameraGetDiagnostics(ExaCameraDiagnostics* diagnostics) {
    if (!diagnostics || diagnostics->struct_size < EXA_CAMERA_DIAGNOSTICS_BASE_SIZE)
        return EXA_RESULT_INVALID_ARGUMENT;
    try {
        const std::lock_guard lock(runtime_mutex);
        return exanima::camera::diagnostics(*diagnostics);
    } catch (...) {
        return EXA_RESULT_BACKEND_FAILURE;
    }
}
