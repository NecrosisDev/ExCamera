#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include <exanima/result.h>
#include <exanima/runtime.h>

namespace exanima::launcher {

struct Process {
    DWORD id = 0;
    HANDLE handle = nullptr;
    ~Process();
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
};

DWORD last_error() noexcept;
Result open_game(Process& process) noexcept;
Result inject_runtime(Process& process, const std::wstring& runtime_path) noexcept;
Result call_runtime(Process& process, const std::wstring& runtime_path, const char* export_name,
    void* argument, DWORD& result) noexcept;
Result runtime_status(Process& process, const std::wstring& runtime_path, ExaRuntimeStatus& status) noexcept;
Result camera_diagnostics(Process& process, const std::wstring& runtime_path,
    ExaCameraDiagnostics& diagnostics) noexcept;

} // namespace exanima::launcher
