#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <shellapi.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
// Windows 10 SDK flags; older and MinGW headers lack them.
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#ifndef RRF_SUBKEY_WOW6432KEY
#define RRF_SUBKEY_WOW6432KEY 0x00020000
#endif
#include "native_build.h"
#include "game_layout.h"
#include "hook_health.h"
#include "build_identity.h"
static int fail(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"ExCamera", MB_OK | MB_ICONERROR);
    return 1;
}
static DWORD gamePid() {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W p{sizeof(p)};
    if (s != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(s, &p)) do {
                if (!_wcsicmp(p.szExeFile, L"Exanima.exe")) {
                    if (pid) {
                        pid = MAXDWORD;
                        break;
                    }
                    pid = p.th32ProcessID;
                }
            } while (Process32NextW(s, &p));
        CloseHandle(s);
    }
    return pid;
}
static bool cancelled = false;
static LRESULT CALLBACK progressProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == 1)) {
        cancelled = true;
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
static bool waitTick() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (cancelled) return false;
    Sleep(100);
    return true;
}
static BOOL CALLBACK findGameWindow(HWND w, LPARAM data) {
    auto values = (DWORD*)data;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t c[32]{};
    const bool console =
        GetClassNameW(w, c, 32) &&
        !wcscmp(c, L"ConsoleWindowClass"); // EMTK's framework console reports the game's process too
    if (pid == values[0] && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && !console) {
        values[1] = 1;
        return FALSE;
    }
    return TRUE;
}
static bool ready(DWORD pid) {
    bool gl = false;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W m{sizeof(m)};
    if (s != INVALID_HANDLE_VALUE) {
        if (Module32FirstW(s, &m)) do {
                if (!_wcsicmp(m.szModule, L"opengl32.dll")) gl = true;
            } while (Module32NextW(s, &m));
        CloseHandle(s);
    }
    DWORD values[2] = {pid, 0};
    EnumWindows(findGameWindow, (LPARAM)values);
    return gl && values[1];
}
// The game build: every target the DLL uses must resolve by signature (game_layout.h). The DLL checks
// again in the game; this is so an unsupported version is reported before anything is loaded into it.
static fp::Layout layout;
static std::vector<uint8_t> gameImage; // the exe as mapped, for the comparison with the running game
static fp::ImageView gameView;
static wchar_t gameProblem[512];
static bool resolveGameFile(const wchar_t* path) {
    gameProblem[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        swprintf_s(gameProblem, L"Could not read %s.", path);
        return false;
    }
    LARGE_INTEGER size{};
    std::vector<uint8_t> file;
    DWORD got = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64ll * 1024 * 1024;
    if (ok) {
        file.resize(size_t(size.QuadPart));
        ok = ReadFile(f, file.data(), DWORD(file.size()), &got, nullptr) && got == file.size();
    }
    CloseHandle(f);
    fp::ResolveReport report;
    if (!ok || !fp::mapPeFile(file.data(), file.size(), gameImage, gameView)) {
        swprintf_s(gameProblem, L"%s is not a 64-bit Exanima executable.", path);
        return false;
    }
    if (fp::resolveLayout(gameView, layout, report)) return true;
    int at = swprintf_s(gameProblem,
                        L"This Exanima version is not supported: %u of %u game functions were not found (",
                        report.failures, unsigned(fp::sig::targetCount));
    unsigned listed = 0;
    for (unsigned t = 0; t < fp::sig::targetCount && at > 0; ++t)
        if (report.why[t] != fp::Unresolved::none && listed++ < 4)
            at += swprintf_s(gameProblem + at, 512 - at, L"%s%hs", listed > 1 ? L", " : L"",
                             fp::sig::targetNames[t]);
    if (at > 0)
        swprintf_s(gameProblem + at, 512 - at, L"%s). No changes were made.", listed > 4 ? L", ..." : L"");
    return false;
}
// Where an export of our DLL sits in the game: the module's base there plus the export's offset, read
// from the file mapped here without running any of it. 0 if either is missing.
static uintptr_t remoteExport(DWORD pid, const std::wstring& dll, const char* name) {
    uintptr_t remoteBase = 0;
    for (int tries = 0; tries < 5 && !remoteBase; ++tries) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W m{sizeof(m)};
        if (s == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }
        if (Module32FirstW(s, &m)) do {
                if (!_wcsicmp(m.szModule, L"ExanimaFirstPerson.dll")) remoteBase = (uintptr_t)m.modBaseAddr;
            } while (Module32NextW(
                s, &m)); // by name, as the already-loaded check: a path can be spelled many ways
        CloseHandle(s);
        if (!remoteBase) Sleep(200);
    }
    HMODULE image = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return 0;
    const auto at = (uintptr_t)GetProcAddress(image, name);
    FreeLibrary(image);
    return remoteBase && at ? remoteBase + (at - (uintptr_t)image) : 0;
}
// Runs the DLL's `enable` in the game and waits for it (it only starts a thread and returns).
static bool callExport(HANDLE process, DWORD pid, const std::wstring& dll, const char* name) {
    const uintptr_t at = remoteExport(pid, dll, name);
    if (!at) return false;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)at, nullptr, 0, nullptr);
    if (!thread) return false;
    DWORD result = 0;
    const bool done =
        WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    return done && (result & 0xFF) != 0; // a bool: only the low byte is set
}
// Runs the DLL's `enable` in the game; first `allowOtherHooks` when the player chose to chain.
static bool chainChosen = false;
static bool callEnable(HANDLE process, DWORD pid, const std::wstring& dll) {
    if (chainChosen && !callExport(process, pid, dll, "allowOtherHooks")) return false;
    return callExport(process, pid, dll, "enable");
}
// [Compat] OtherHooks from the player's settings (the per-user file, else the one beside the launcher).
static fp::OtherHooks otherHooksSetting(const std::wstring& directory) {
    std::wstring ini = directory + L"first-person.ini";
    wchar_t profile[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n &&
        n < MAX_PATH) // the DLL's per-user file (it copies one kept under the old ExaniMate name over once)
        for (const wchar_t* folder : {L"\\Documents\\ExCamera", L"\\Documents\\ExaniMate"}) {
            const std::wstring user = std::wstring(profile) + folder + L"\\first-person.ini";
            if (GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ini = user;
                break;
            }
        }
    wchar_t value[16];
    GetPrivateProfileStringW(L"Compat", L"OtherHooks", L"ask", value, 16, ini.c_str());
    return fp::otherHooksFromText(value);
}
// Another program's hooks on what the DLL hooks or calls, or in its dispatch slots: allowed, refused or
// asked here, before anything is loaded. False: do not start (refused, or the player said no).
static bool otherHooksOk(HANDLE process, uintptr_t base, const std::wstring& directory) {
    auto read = [&](uint32_t rva, uint8_t* out) {
        SIZE_T got = 0;
        return ReadProcessMemory(process, (void*)(base + rva), out, 16, &got) && got == 16;
    };
    fp::sig::Target found[8];
    unsigned n = fp::patchedEntries(gameView, layout, read, found, 8);
    const fp::sig::Target slots[3][2] = {{fp::sig::navSlot, fp::sig::navSampler},
                                         {fp::sig::behaviorSlot, fp::sig::playerBehavior},
                                         {fp::sig::pointSlot, fp::sig::aimPoint}};
    for (const auto& sl : slots) {
        uint64_t held = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, (void*)(base + layout.rva[sl[0]]), &held, 8, &got) || got != 8 ||
            held != base + layout.rva[sl[1]]) {
            if (n < 8) found[n] = sl[0];
            ++n;
        }
    }
    if (!n) return true;
    std::wstring names;
    for (unsigned i = 0; i < n && i < 8; ++i) {
        wchar_t w[48];
        swprintf_s(w, L"%s%hs", i ? L", " : L"", fp::sig::targetNames[found[i]]);
        names += w;
    }
    if (n > 8) names += L", ...";
    switch (otherHooksSetting(directory)) {
    case fp::OtherHooks::allow:
        chainChosen = true;
        return true;
    case fp::OtherHooks::refuse:
        fail((L"Another mod has already hooked game functions ExCamera uses:\n  " + names +
              L"\n\nNot started, because OtherHooks=refuse in first-person.ini. No changes were made.")
                 .c_str());
        return false;
    default:
        break;
    }
    const std::wstring text =
        L"Another mod has already hooked game functions ExCamera uses:\n  " + names +
        L"\n\nIt can hook on top of that mod. This usually works, but if both mods change the same thing you "
        L"may see odd behaviour or a crash. It stops by itself and restores the game's camera if its own "
        L"hooks stop running."
        L"\n\nYes: start on top of the other mod (this time).\nNo: don't start; nothing is changed."
        L"\n\nTo stop this question, set OtherHooks=allow or OtherHooks=refuse under [Compat] in "
        L"first-person.ini.";
    if (MessageBoxW(nullptr, text.c_str(), L"ExCamera",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
        return false;
    chainChosen = true;
    return true;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE launcherMutex = CreateMutexW(nullptr, FALSE, L"Local\\ExanimaFirstPersonLauncher");
    if (!launcherMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (launcherMutex) CloseHandle(launcherMutex);
        return 0;
    }
    struct MutexGuard {
        HANDLE h;
        ~MutexGuard() { CloseHandle(h); }
    } mutexGuard{launcherMutex};
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    std::wstring directory(path);
    directory = directory.substr(0, directory.find_last_of(L"\\/") + 1);
    std::wstring dll = directory + L"ExanimaFirstPerson.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"ExanimaFirstPerson.dll must be beside this launcher.");
    DWORD pid = gamePid();
    if (pid == MAXDWORD) return fail(L"More than one Exanima process is running. Close the extra instance.");
    WNDCLASSW wc{};
    wc.lpfnWndProc = progressProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ExCameraLauncher";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND progress =
        CreateWindowW(wc.lpszClassName, L"ExCamera", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                      CW_USEDEFAULT, 440, 145, nullptr, nullptr, wc.hInstance, nullptr);
    HWND label = CreateWindowW(
        L"STATIC",
        pid ? L"Attaching first-person mode..." : L"Starting Exanima and waiting for its window...",
        WS_CHILD | WS_VISIBLE, 20, 20, 390, 35, progress, nullptr, wc.hInstance, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 320, 65, 85, 28, progress,
                  (HMENU)1, wc.hInstance, nullptr);
    ShowWindow(progress, SW_SHOWNOACTIVATE);
    struct WindowGuard {
        HWND w;
        ~WindowGuard() { DestroyWindow(w); }
    } windowGuard{progress};
    if (!pid) {
        wchar_t install[32768]{};
        DWORD bytes = sizeof(install);
        LSTATUS result = RegGetValueW(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 362490",
            L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, install, &bytes);
        if (result != ERROR_SUCCESS) {
            bytes = sizeof(install);
            result = RegGetValueW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 362490",
                L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, install, &bytes);
        }
        std::wstring executable = std::wstring(install) + L"\\Exanima.exe";
        if (result != ERROR_SUCCESS)
            return fail(L"Could not find Exanima's Steam installation. Start Exanima normally, then run this "
                        L"launcher; no character needs to be loaded first.");
        if (!resolveGameFile(executable.c_str())) return fail(gameProblem);
        // Steam owns launch prerequisites and the game process. No install-directory modifications.
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"steam://rungameid/362490", nullptr, nullptr,
                                   SW_SHOWNORMAL) <= 32)
            return fail(L"Steam could not start Exanima. Start it normally and run this launcher again.");
    }
    for (int i = 0; i < 1200; i++) {
        if (!waitTick()) return 0;
        pid = gamePid();
        if (pid == MAXDWORD) return fail(L"More than one Exanima process is running.");
        if (pid && ready(pid)) break;
        if (i == 1199)
            return fail(L"Exanima did not become ready within two minutes. The launcher stopped waiting; run "
                        L"it again once the game is open.");
    }
    SetWindowTextW(label, L"Checking the game build and attaching the camera...");
    HANDLE snapshot;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process)
        return fail(L"Could not open Exanima. Run the game and launcher with the same Windows permissions.");
    wchar_t exe[32768];
    DWORD length = 32768;
    if (!QueryFullProcessImageNameW(process, 0, exe, &length)) {
        CloseHandle(process);
        return fail(L"Could not read which executable Exanima is running from. No changes were made.");
    }
    if (!resolveGameFile(exe)) {
        CloseHandle(process);
        return fail(gameProblem);
    }
    auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)load, &owner);
    wchar_t ownerPath[MAX_PATH];
    GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    const wchar_t* name = wcsrchr(ownerPath, L'\\');
    name = name ? name + 1 : ownerPath;
    uintptr_t remoteLoad = 0, base = 0;
    bool already = false;
    std::wstring loadedPath;
    // the module list can fail briefly while the game loads modules; try a few times.
    for (int tries = 0; tries < 5; ++tries) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        Sleep(200);
    }
    MODULEENTRY32W m{sizeof(m)};
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Module32FirstW(snapshot, &m)) do {
                if (!_wcsicmp(m.szModule, name))
                    remoteLoad = (uintptr_t)m.modBaseAddr + (uintptr_t)load - (uintptr_t)owner;
                if (!_wcsicmp(m.szModule, L"Exanima.exe")) base = (uintptr_t)m.modBaseAddr;
                if (!_wcsicmp(m.szModule, L"ExanimaFirstPerson.dll")) {
                    already = true;
                    loadedPath = m.szExePath;
                }
            } while (Module32NextW(snapshot, &m));
        CloseHandle(snapshot);
    }
    if (base && !otherHooksOk(process, base, directory)) {
        CloseHandle(process);
        return 0;
    }
    if (already) {
        // Loaded by EMTK with the mod switched off, nothing runs yet: start it (enable starts a session once).
        if (!_wcsicmp(loadedPath.c_str(), dll.c_str()) && !callEnable(process, pid, dll)) {
            CloseHandle(process);
            return fail(L"ExanimaFirstPerson.dll is loaded, but it could not be started. Restart Exanima "
                        L"before retrying.");
        }
        CloseHandle(process);
        std::wstring message = _wcsicmp(loadedPath.c_str(), dll.c_str())
                                   ? L"A first-person version from another folder is already loaded. Close "
                                     L"Exanima, then run this launcher to switch versions.\n\nLoaded from:\n"
                                   : L"First-person mode is already loaded.\nF6: toggle mode   F8: free "
                                     L"cursor\nF9/F10: adjust FOV   F11: reset FOV\n\nAfter F7, restart "
                                     L"Exanima before loading again.\n\nLoaded from:\n";
        message += loadedPath;
        MessageBoxW(nullptr, message.c_str(), L"ExCamera", MB_OK);
        return 0;
    }
    if (!base || !remoteLoad) {
        CloseHandle(process);
        return fail(L"Could not read the game's module list (it may still be starting). Try again in a few "
                    L"seconds. No changes were made.");
    }
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t), written = 0;
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) || written != bytes) {
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return fail(L"Could not prepare the module path.");
    }
    FILETIME activationStarted;
    GetSystemTimeAsFileTime(&activationStarted);
    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)remoteLoad, remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return fail(L"Could not load the camera module.");
    }
    DWORD wait = WaitForSingleObject(thread, 10000), result = 0;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeThread(thread, &result);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    CloseHandle(thread);
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(process);
        return fail(L"Loading is still pending. Check the status file; do not run a second launcher.");
    }
    if (!result) {
        CloseHandle(process);
        return fail(L"Windows could not load ExanimaFirstPerson.dll.");
    }
    // Loading no longer starts the camera (so EMTK can load the DLL with the mod switched off): call its
    // `enable` export in the game.
    const bool started = callEnable(process, pid, dll);
    CloseHandle(process);
    if (!started)
        return fail(
            L"ExanimaFirstPerson.dll loaded, but it could not be started. Restart Exanima before retrying.");
    // Activation is reported by the module, not inferred from LoadLibrary success.
    std::wstring status = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"first-person-status.json";
    for (int i = 0; i < 50; i++) {
        Sleep(100);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(status.c_str(), GetFileExInfoStandard, &info) ||
            CompareFileTime(&info.ftLastWriteTime, &activationStarted) < 0)
            continue;
        FILE* f = nullptr;
        _wfopen_s(&f, status.c_str(), L"rb");
        if (f) {
            char data[2048]{};
            fread(data, 1, sizeof(data) - 1, f);
            fclose(f);
            char needle[64];
            sprintf_s(needle, "\"pid\":%lu,", pid);
            if (strstr(data, needle) && strstr(data, "\"revision\":" FP_REVISION_TEXT ",") &&
                strstr(data, "\"build\":\"" FP_BUILD "\"") && strstr(data, "\"installed\":true"))
                return 0;
        }
    }
    return fail(L"The DLL loaded but did not confirm activation. See first-person-status.json. Restart "
                L"Exanima before retrying.");
}
