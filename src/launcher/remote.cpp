#include "remote.hpp"
#include "build_identity.hpp"

#include <tlhelp32.h>

#include <array>

namespace exanima::launcher {
namespace {

thread_local DWORD error_value = ERROR_SUCCESS;

void fail(DWORD error) noexcept {
    error_value = error ? error : ERROR_GEN_FAILURE;
}

bool module_base(Process& process, const wchar_t* name, uintptr_t& base) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.id);
        if (snapshot == INVALID_HANDLE_VALUE) {
            fail(GetLastError());
            return false;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL ok = Module32FirstW(snapshot, &entry);
        while (ok) {
            if (_wcsicmp(entry.szModule, name) == 0) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                CloseHandle(snapshot);
                return true;
            }
            ok = Module32NextW(snapshot, &entry);
        }
        const DWORD error = GetLastError();
        CloseHandle(snapshot);
        if (error == ERROR_BAD_LENGTH) continue;
        fail(ERROR_MOD_NOT_FOUND);
        return false;
    }
    fail(ERROR_RETRY);
    return false;
}

Result remote_export(Process& process, const wchar_t* module_name, const char* export_name,
    const std::wstring& local_module_path, void*& address) noexcept {
    address = nullptr;
    uintptr_t remote_base = 0;
    if (!module_base(process, module_name, remote_base)) return EXA_RESULT_NOT_FOUND;

    HMODULE local = nullptr;
    bool unload = false;
    if (local_module_path.empty()) local = GetModuleHandleW(module_name);
    else {
        local = LoadLibraryW(local_module_path.c_str());
        unload = local != nullptr;
    }
    if (!local) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    const FARPROC procedure = GetProcAddress(local, export_name);
    if (!procedure) {
        fail(GetLastError());
        if (unload) FreeLibrary(local);
        return EXA_RESULT_NOT_FOUND;
    }
    const uintptr_t offset = reinterpret_cast<uintptr_t>(procedure) - reinterpret_cast<uintptr_t>(local);
    address = reinterpret_cast<void*>(remote_base + offset);
    if (unload) FreeLibrary(local);
    return EXA_RESULT_OK;
}

Result call_address(Process& process, void* address, void* argument, DWORD& result) noexcept {
    result = EXA_RESULT_BACKEND_FAILURE;
    HANDLE thread = CreateRemoteThread(process.handle, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr);
    if (!thread) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    if (wait != WAIT_OBJECT_0) {
        fail(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        CloseHandle(thread);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    DWORD exit_code = 0;
    const BOOL ok = GetExitCodeThread(thread, &exit_code);
    const DWORD error = GetLastError();
    CloseHandle(thread);
    if (!ok) {
        fail(error);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    result = exit_code;
    return EXA_RESULT_OK;
}

Result remote_string_call(Process& process, void* address, const std::wstring& value, DWORD& result) noexcept {
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process.handle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process.handle, remote, value.c_str(), bytes, &written) || written != bytes) {
        fail(GetLastError());
        VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    const Result result_code = call_address(process, address, remote, result);
    VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
    return result_code;
}

} // namespace

Process::~Process() {
    if (handle) CloseHandle(handle);
}

DWORD last_error() noexcept {
    return error_value;
}

Result open_game(Process& process) noexcept {
    error_value = ERROR_SUCCESS;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    BOOL ok = Process32FirstW(snapshot, &entry);
    while (ok) {
        if (_wcsicmp(entry.szExeFile, L"Exanima.exe") == 0) {
            if (pid) {
                CloseHandle(snapshot);
                fail(ERROR_ALREADY_EXISTS);
                return EXA_RESULT_INVALID_STATE;
            }
            pid = entry.th32ProcessID;
        }
        ok = Process32NextW(snapshot, &entry);
    }
    CloseHandle(snapshot);
    if (!pid) {
        fail(ERROR_NOT_FOUND);
        return EXA_RESULT_NOT_FOUND;
    }
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!handle) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(handle, 0, path.data(), &length)) {
        fail(GetLastError());
        CloseHandle(handle);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    if (!platform::supported_game_executable(path.data())) {
        CloseHandle(handle);
        fail(ERROR_BAD_EXE_FORMAT);
        return EXA_RESULT_UNSUPPORTED;
    }
    process.id = pid;
    process.handle = handle;
    return EXA_RESULT_OK;
}

Result inject_runtime(Process& process, const std::wstring& runtime_path) noexcept {
    if (!process.handle || runtime_path.empty()) {
        fail(ERROR_INVALID_PARAMETER);
        return EXA_RESULT_INVALID_ARGUMENT;
    }
    uintptr_t existing = 0;
    if (module_base(process, L"ExanimaRuntime.dll", existing)) return EXA_RESULT_OK;
    error_value = ERROR_SUCCESS;

    void* load_library = nullptr;
    const Result resolved = remote_export(process, L"kernel32.dll", "LoadLibraryW", {}, load_library);
    if (resolved != EXA_RESULT_OK) return resolved;
    DWORD result = 0;
    const Result called = remote_string_call(process, load_library, runtime_path, result);
    if (called != EXA_RESULT_OK) return called;
    uintptr_t loaded = 0;
    if (!module_base(process, L"ExanimaRuntime.dll", loaded)) {
        if (!error_value) fail(ERROR_MOD_NOT_FOUND);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    return EXA_RESULT_OK;
}

Result call_runtime(Process& process, const std::wstring& runtime_path, const char* export_name,
    void* argument, DWORD& result) noexcept {
    if (!process.handle || !export_name) {
        fail(ERROR_INVALID_PARAMETER);
        return EXA_RESULT_INVALID_ARGUMENT;
    }
    void* address = nullptr;
    const Result resolved = remote_export(process, L"ExanimaRuntime.dll", export_name, runtime_path, address);
    if (resolved != EXA_RESULT_OK) return resolved;
    return call_address(process, address, argument, result);
}

template <typename Snapshot>
Result runtime_snapshot(Process& process, const std::wstring& runtime_path,
    const char* export_name, Snapshot& snapshot) noexcept {
    snapshot = Snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    void* remote = VirtualAllocEx(process.handle, nullptr, sizeof(snapshot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        fail(GetLastError());
        return EXA_RESULT_BACKEND_FAILURE;
    }
    SIZE_T copied = 0;
    if (!WriteProcessMemory(process.handle, remote, &snapshot, sizeof(snapshot), &copied) || copied != sizeof(snapshot)) {
        fail(GetLastError());
        VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
        return EXA_RESULT_BACKEND_FAILURE;
    }
    DWORD result = 0;
    Result result_code = call_runtime(process, runtime_path, export_name, remote, result);
    if (result_code == EXA_RESULT_OK && result == EXA_RESULT_OK) {
        copied = 0;
        if (!ReadProcessMemory(process.handle, remote, &snapshot, sizeof(snapshot), &copied) || copied != sizeof(snapshot)) {
            fail(GetLastError());
            result_code = EXA_RESULT_BACKEND_FAILURE;
        }
    } else {
        if (result_code == EXA_RESULT_OK) result_code = static_cast<Result>(result);
        snapshot = Snapshot{};
    }
    VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
    return result_code;
}

Result runtime_status(Process& process, const std::wstring& runtime_path, ExaRuntimeStatus& status) noexcept {
    return runtime_snapshot(process, runtime_path, "ExaRuntimeGetStatus", status);
}

Result camera_diagnostics(Process& process, const std::wstring& runtime_path,
    ExaCameraDiagnostics& diagnostics) noexcept {
    return runtime_snapshot(process, runtime_path, "ExaCameraGetDiagnostics", diagnostics);
}

} // namespace exanima::launcher
