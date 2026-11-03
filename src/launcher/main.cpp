#include <windows.h>

#include <exanima/runtime.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "remote.hpp"
#include "settings_store.hpp"

namespace {

using exanima::Result;
using Start = ExaResult(*)();
using Stop = ExaResult(*)();
using GetStatus = ExaResult(*)(ExaRuntimeStatus*);
using GetCameraDiagnostics = ExaResult(*)(ExaCameraDiagnostics*);

std::wstring runtime_path() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    path += L"ExanimaRuntime.dll";
    return path;
}

const char* state_name(ExaRuntimeState state) {
    switch (state) {
        case EXA_RUNTIME_UNLOADED: return "unloaded";
        case EXA_RUNTIME_ATTACHING: return "attaching";
        case EXA_RUNTIME_DORMANT: return "dormant";
        case EXA_RUNTIME_ACTIVE: return "active";
        case EXA_RUNTIME_STOPPING: return "stopping";
        case EXA_RUNTIME_STOPPED: return "stopped";
        case EXA_RUNTIME_REFUSED: return "refused";
        case EXA_RUNTIME_FAULTED: return "faulted";
        default: return "unknown";
    }
}

int report(exanima::launcher::Process& process, Result result) {
    if (result == EXA_RESULT_OK) return 0;
    std::cerr << "Failed: " << ExaResultString(result) << " (Win32 " << exanima::launcher::last_error()
              << ", PID " << process.id << ")\n";
    return 1;
}

int print_status(exanima::launcher::Process& process, const std::wstring& path) {
    ExaRuntimeStatus status{};
    const Result result = exanima::launcher::runtime_status(process, path, status);
    if (result != EXA_RESULT_OK) return report(process, result);
    std::cout << "PID " << process.id << ": state=" << state_name(status.state)
              << " generation=" << status.generation << " installed_hooks=" << status.installed_hooks
              << " active_hooks=" << status.active_hooks << " last_error=" << ExaResultString(status.last_error)
              << " module_faults=" << status.module_faults << " restoration_failed=" << status.restoration_failed
              << " abi=" << status.abi_version << '\n';
    return 0;
}

int camera_status() {
    exanima::launcher::Process process;
    Result result = exanima::launcher::open_game(process);
    if (result != EXA_RESULT_OK) return report(process, result);
    ExaCameraDiagnostics diagnostics{};
    result = exanima::launcher::camera_diagnostics(process, runtime_path(), diagnostics);
    if (result != EXA_RESULT_OK) return report(process, result);
    static constexpr const char* modes[]{"first", "third", "overhead"};
    const auto mode = static_cast<uint32_t>(diagnostics.mode);
    const auto flag = [&](uint32_t bit) { return (diagnostics.flags & bit) ? 1u : 0u; };
    std::cout << "PID " << process.id << ": mode=" << (mode < 3 ? modes[mode] : "unknown")
              << " level_available=" << flag(EXA_CAMERA_DIAGNOSTICS_LEVEL_AVAILABLE)
              << " query_failed=" << flag(EXA_CAMERA_DIAGNOSTICS_QUERY_FAILED)
              << " cursor_owned=" << flag(EXA_CAMERA_DIAGNOSTICS_CURSOR_OWNED)
              << " custom_presented=" << flag(EXA_CAMERA_DIAGNOSTICS_CUSTOM_PRESENTED)
              << " collision_queries=" << diagnostics.collision_queries
              << " collision_failures=" << diagnostics.collision_failures
              << " camera_samples=" << diagnostics.camera_samples
              << " collision_mesh_traces=" << diagnostics.collision_mesh_traces
              << " collision_ns=" << diagnostics.collision_nanoseconds
              << " clearance=" << diagnostics.clearance
              << " applied_distance=" << diagnostics.applied_distance
              << " ground_available=" << flag(EXA_CAMERA_DIAGNOSTICS_GROUND_AVAILABLE)
              << " support_height=" << diagnostics.support_height
              << " minimum_camera_y=" << diagnostics.minimum_camera_y
              << " camera_y=" << diagnostics.pivot[1] + diagnostics.back[1] * diagnostics.applied_distance
              << " pivot=" << diagnostics.pivot[0] << ',' << diagnostics.pivot[1] << ',' << diagnostics.pivot[2]
              << " back=" << diagnostics.back[0] << ',' << diagnostics.back[1] << ',' << diagnostics.back[2]
              << " abi=" << diagnostics.abi_version << '\n';
    return 0;
}

int attach() {
    exanima::launcher::Process process;
    Result result = exanima::launcher::open_game(process);
    if (result != EXA_RESULT_OK) return report(process, result);
    const std::wstring path = runtime_path();
    result = exanima::launcher::inject_runtime(process, path);
    if (result != EXA_RESULT_OK) return report(process, result);
    DWORD remote_result = 0;
    result = exanima::launcher::call_runtime(process, path, "ExaRuntimeStart", nullptr, remote_result);
    if (result != EXA_RESULT_OK) return report(process, result);
    if (remote_result != EXA_RESULT_OK) {
        std::cerr << "Runtime start refused: " << ExaResultString(static_cast<ExaResult>(remote_result)) << '\n';
        return 1;
    }
    return print_status(process, path);
}

int stop() {
    exanima::launcher::Process process;
    Result result = exanima::launcher::open_game(process);
    if (result != EXA_RESULT_OK) return report(process, result);
    const std::wstring path = runtime_path();
    DWORD remote_result = 0;
    result = exanima::launcher::call_runtime(process, path, "ExaRuntimeStop", nullptr, remote_result);
    if (result != EXA_RESULT_OK) return report(process, result);
    if (remote_result != EXA_RESULT_OK) {
        std::cerr << "Runtime stop failed: " << ExaResultString(static_cast<ExaResult>(remote_result)) << '\n';
        return 1;
    }
    return print_status(process, path);
}

int status() {
    exanima::launcher::Process process;
    const Result result = exanima::launcher::open_game(process);
    if (result != EXA_RESULT_OK) return report(process, result);
    return print_status(process, runtime_path());
}

int control_call(const char* export_name, void* argument) {
    exanima::launcher::Process process;
    Result result = exanima::launcher::open_game(process);
    if (result != EXA_RESULT_OK) return report(process, result);
    const std::wstring path = runtime_path();
    DWORD remote_result = 0;
    result = exanima::launcher::call_runtime(process, path, export_name, argument, remote_result);
    if (result != EXA_RESULT_OK) return report(process, result);
    if (remote_result != EXA_RESULT_OK) {
        std::cerr << export_name << " refused: " << ExaResultString(static_cast<ExaResult>(remote_result)) << '\n';
        return 1;
    }
    return print_status(process, path);
}

int set_mode(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string_view mode = argv[2];
    uint32_t value;
    if (mode == "first") value = 0;
    else if (mode == "third") value = 1;
    else if (mode == "overhead") value = 2;
    else {
        std::cerr << "Mode must be first, third, or overhead.\n";
        return 2;
    }
    return control_call("ExaCameraSetMode", reinterpret_cast<void*>(static_cast<uintptr_t>(value)));
}

bool parse_float(const char* text, float& value) {
    try {
        size_t consumed = 0;
        value = std::stof(text, &consumed);
        return consumed == std::char_traits<char>::length(text) && std::isfinite(value);
    } catch (...) {
        return false;
    }
}

int set_look(int argc, char** argv) {
    if (argc < 4) return 2;
    float yaw, pitch;
    if (!parse_float(argv[2], yaw) || !parse_float(argv[3], pitch)) {
        std::cerr << "Look requires finite yaw and pitch values in radians.\n";
        return 2;
    }
    uint32_t yaw_bits, pitch_bits;
    std::memcpy(&yaw_bits, &yaw, sizeof(yaw));
    std::memcpy(&pitch_bits, &pitch, sizeof(pitch));
    const uint64_t packed = static_cast<uint64_t>(yaw_bits) | (static_cast<uint64_t>(pitch_bits) << 32);
    return control_call("ExaCameraSetLook", reinterpret_cast<void*>(static_cast<uintptr_t>(packed)));
}

int set_distance(int argc, char** argv) {
    if (argc < 3) return 2;
    float distance;
    if (!parse_float(argv[2], distance) || distance < 0.f || distance > 1200.f) {
        std::cerr << "Distance must be between 0 and 1200 centimetres.\n";
        return 2;
    }
    uint32_t packed;
    std::memcpy(&packed, &distance, sizeof(packed));
    return control_call("ExaCameraSetDistance", reinterpret_cast<void*>(static_cast<uintptr_t>(packed)));
}

int self_test() {
    const std::wstring path = runtime_path();
    HMODULE module = path.empty() ? nullptr : LoadLibraryW(path.c_str());
    if (!module) {
        std::cerr << "Could not load ExanimaRuntime.dll beside the launcher; Win32 error " << GetLastError() << '\n';
        return 1;
    }
    const auto start = reinterpret_cast<Start>(GetProcAddress(module, "ExaRuntimeStart"));
    const auto stop_export = reinterpret_cast<Stop>(GetProcAddress(module, "ExaRuntimeStop"));
    const auto get_status = reinterpret_cast<GetStatus>(GetProcAddress(module, "ExaRuntimeGetStatus"));
    const auto get_camera_diagnostics = reinterpret_cast<GetCameraDiagnostics>(
        GetProcAddress(module, "ExaCameraGetDiagnostics"));
    if (!start || !stop_export || !get_status || !get_camera_diagnostics ||
        !GetProcAddress(module, "ExaCameraSetDistance")) {
        std::cerr << "The runtime exports are incomplete.\n";
        FreeLibrary(module);
        return 1;
    }
    ExaCameraDiagnostics diagnostics{};
    diagnostics.struct_size = sizeof(diagnostics);
    if (get_camera_diagnostics(nullptr) != EXA_RESULT_INVALID_ARGUMENT ||
        get_camera_diagnostics(&diagnostics) != EXA_RESULT_INVALID_STATE) {
        std::cerr << "Camera diagnostics rejected invalid calls incorrectly.\n";
        FreeLibrary(module);
        return 1;
    }
    diagnostics.struct_size = EXA_CAMERA_DIAGNOSTICS_BASE_SIZE;
    if (get_camera_diagnostics(&diagnostics) != EXA_RESULT_INVALID_STATE) {
        std::cerr << "Legacy camera diagnostics size was not accepted.\n";
        FreeLibrary(module);
        return 1;
    }

    int exit_code = 0;
    for (uint32_t cycle = 1; cycle <= 8; ++cycle) {
        ExaResult result = start();
        ExaRuntimeStatus status_value{};
        status_value.struct_size = sizeof(status_value);
        if (result != EXA_RESULT_OK || get_status(&status_value) != EXA_RESULT_OK ||
            status_value.state != EXA_RUNTIME_DORMANT || status_value.generation != cycle ||
            status_value.installed_hooks != 0 || status_value.active_hooks != 0) {
            std::cerr << "Runtime start self-test failed at cycle " << cycle << ": " << ExaResultString(result) << '\n';
            exit_code = 1;
            break;
        }
        result = stop_export();
        status_value = ExaRuntimeStatus{};
        status_value.struct_size = sizeof(status_value);
        if (result != EXA_RESULT_OK || get_status(&status_value) != EXA_RESULT_OK ||
            status_value.state != EXA_RUNTIME_STOPPED || status_value.installed_hooks != 0 ||
            status_value.active_hooks != 0) {
            std::cerr << "Runtime stop self-test failed at cycle " << cycle << ": " << ExaResultString(result) << '\n';
            exit_code = 1;
            break;
        }
    }
    if (stop_export() != EXA_RESULT_OK) exit_code = 1;
    FreeLibrary(module);
    if (!exit_code) std::cout << "Runtime lifecycle self-test passed: 8 generations, zero game hooks.\n";
    return exit_code;
}

int show_settings() {
    const excamera::CameraSettings settings = excamera::settings::load(excamera::settings::path());
    std::cout << "first_fov=" << settings.first_fov << " third_fov=" << settings.third_fov
              << " sensitivity=" << settings.look_sensitivity << " third_distance=" << settings.third_distance
              << " third_shoulder=" << settings.third_shoulder << " third_height=" << settings.third_height
              << " near_plane=" << settings.near_plane << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view argument = argc > 1 ? argv[1] : "";
    if (argument == "--attach") return attach();
    if (argument == "--status") return status();
    if (argument == "--camera-status") return camera_status();
    if (argument == "--stop") return stop();
    if (argument == "--mode") return set_mode(argc, argv);
    if (argument == "--look") return set_look(argc, argv);
    if (argument == "--distance") return set_distance(argc, argv);
    if (argument == "--settings") return show_settings();
    if (argument == "--self-test") return self_test();
    if (argument == "--version") {
        std::cout << "ExCamera 0.1.0-research\n";
        return 0;
    }
    std::cerr << "Usage: ExCamera.exe --attach | --status | --camera-status | --stop | --mode first|third|overhead | --look YAW PITCH | --distance CENTIMETRES | --settings | --self-test | --version\n";
    return 2;
}
