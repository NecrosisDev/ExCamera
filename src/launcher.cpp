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
#include <shlobj.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
// Windows 10 SDK flags; older and MinGW headers lack them.
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#ifndef RRF_SUBKEY_WOW6432KEY
#define RRF_SUBKEY_WOW6432KEY 0x00020000
#endif
#include "native_build.h"
#include "game_layout.h"
#include "native_contracts.h"
#include "hook_health.h"
#include "build_identity.h"
#include "module_export.h"
#include "activation_status.h"
#include "settings_paths.h"
static int fail(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"ExCamera", MB_OK | MB_ICONERROR);
    return 1;
}
static DWORD gamePid() {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W p{};
    p.dwSize = sizeof p;
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
// Ready to attach as soon as the game's own module and kernel32 are mapped: before it creates its GL context and
// links its first shader programs, which the mod's linker hook must see (mod/material_ids.inl: a program linked
// before it has no recoverable vertex source when the game deletes both shaders, as 0x1FACF0 and 0x1FAD60 do).
// The mod itself then waits for the game's own GL window (waitForGame: its DC global, set once its context is
// current) before anything else.
static bool ready(DWORD pid) {
    bool game = false, kernel = false;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W m{};
    m.dwSize = sizeof m;
    if (s != INVALID_HANDLE_VALUE) {
        if (Module32FirstW(s, &m)) do {
                if (!_wcsicmp(m.szModule, L"Exanima.exe")) game = true;
                if (!_wcsicmp(m.szModule, L"kernel32.dll")) kernel = true;
            } while (Module32NextW(s, &m));
        CloseHandle(s);
    }
    return game && kernel;
}
// The game build: every target the DLL uses must resolve by signature (game_layout.h). The DLL checks
// again in the game; this is so an unsupported version is reported before anything is loaded into it.
static fp::Layout layout;
static std::vector<uint8_t> gameImage; // the exe as mapped, for the comparison with the running game
static fp::ImageView gameView;
static BY_HANDLE_FILE_INFORMATION resolvedFileInfo{};
static bool hasResolvedFile = false;
static wchar_t gameProblem[512];
static bool sameFileIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
           a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime;
}
static bool resolveGameFile(const wchar_t* path) {
    gameProblem[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        swprintf_s(gameProblem, L"Could not read %s.", path);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    const bool haveInfo = GetFileInformationByHandle(f, &fileInfo) != FALSE;
    if (haveInfo && hasResolvedFile && gameView.data && sameFileIdentity(fileInfo, resolvedFileInfo)) {
        CloseHandle(f);
        return true;
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
    hasResolvedFile = false;
    fp::ResolveReport report;
    if (!ok || !fp::mapPeFile(file.data(), file.size(), gameImage, gameView)) {
        swprintf_s(gameProblem, L"%s is not a 64-bit Exanima executable.", path);
        return false;
    }
    fp::Layout candidate;
    if (fp::resolveLayout(gameView, candidate, report)) {
        fp::ContractReport contracts;
        if (!fp::validateNativeContracts(gameView, candidate, contracts)) {
            swprintf_s(gameProblem,
                       L"This Exanima version isn't supported by this ExCamera, so nothing was changed. If the game "
                       L"updated, look for a new ExCamera release. Details: %hs (%hs).",
                       contracts.firstRequired ? contracts.firstRequired : "field contract",
                       fp::contractFailureText(contracts.firstRequiredReason));
            return false;
        }
        layout = candidate;
        if (haveInfo) {
            resolvedFileInfo = fileInfo;
            hasResolvedFile = true;
        }
        return true;
    }
    int at = swprintf_s(gameProblem,
                        L"This Exanima version isn't supported by this ExCamera, so nothing was changed. If the game "
                        L"updated, look for a new ExCamera release. Details: %u of %u functions missing (",
                        report.failures, unsigned(fp::sig::targetCount));
    unsigned listed = 0;
    for (unsigned t = 0; t < fp::sig::targetCount && at > 0; ++t)
        if (report.why[t] != fp::Unresolved::none && listed++ < 4)
            at += swprintf_s(gameProblem + at, 512 - at, L"%s%hs", listed > 1 ? L", " : L"",
                             fp::sig::targetNames[t]);
    if (at > 0)
        swprintf_s(gameProblem + at, 512 - at, L"%s).", listed > 4 ? L", ..." : L"");
    return false;
}
static uintptr_t remoteModule(DWORD pid, const wchar_t* moduleName, uint32_t* outSize = nullptr) {
    uintptr_t remoteBase = 0;
    uint32_t remoteSize = 0;
    for (int tries = 0; tries < 5 && !remoteBase; ++tries) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W m{};
        m.dwSize = sizeof m;
        if (s == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }
        if (Module32FirstW(s, &m)) do {
                if (!_wcsicmp(m.szModule, moduleName)) {
                    if (remoteBase) { // two copies: do not guess which one to call
                        CloseHandle(s);
                        return 0;
                    }
                    remoteBase = (uintptr_t)m.modBaseAddr;
                    remoteSize = m.modBaseSize;
                }
            } while (Module32NextW(
                s, &m)); // by name, as the already-loaded check: a path can be spelled many ways
        CloseHandle(s);
        if (!remoteBase && tries + 1 < 5) Sleep(200);
    }
    if (outSize) *outSize = remoteSize;
    return remoteBase;
}
// Resolve against the module actually loaded: a newer DLL on disk can have
// different export RVAs from the version still mapped in the game.
static uintptr_t remoteExport(HANDLE process, DWORD pid, const char* name) {
    uint32_t remoteSize = 0;
    const uintptr_t remoteBase = remoteModule(pid, L"ExanimaFirstPerson.dll", &remoteSize);
    if (!remoteBase || remoteSize > UINTPTR_MAX - remoteBase) return 0;
    auto read = [&](uint32_t rva, void* out, size_t size) {
        SIZE_T got = 0;
        return ReadProcessMemory(process, (void*)(remoteBase + rva), out, size, &got) && got == size;
    };
    const uint32_t rva = fp::loadedExportRva(remoteSize, name, read);
    return rva ? remoteBase + rva : 0;
}
// Waiting for an export only confirms the call; activation completes on the worker.
static bool callExport(HANDLE process, DWORD pid, const char* name, void* argument = nullptr) {
    const uintptr_t at = remoteExport(process, pid, name);
    if (!at) return false;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)at, argument, 0, nullptr);
    if (!thread) return false;
    DWORD result = 0;
    const bool done =
        WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    return done && (result & 0xFF) != 0; // a bool: only the low byte is set
}
// Runs the DLL's `enable` in the game; first `allowOtherHooks` when the player chose to chain.
static bool chainChosen = false;
static bool callEnable(HANDLE process, DWORD pid, uint64_t request) {
    if (chainChosen && !callExport(process, pid, "allowOtherHooks")) return false;
    return callExport(process, pid, "enableWithStatus", (void*)uintptr_t(request));
}
static bool readActivationFile(const std::wstring& path, fp::ActivationStatus& status) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    DWORD got = 0;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64 * 1024;
    std::vector<char> data;
    if (ok) {
        data.resize(size_t(size.QuadPart));
        ok = ReadFile(file, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
    }
    CloseHandle(file);
    return ok && fp::readActivationStatus(data.data(), data.size(), status);
}
// Fresh loading and re-entry require the same acknowledgement from this process,
// this mapped build and this request. A timestamp or an old "installed" field cannot satisfy it.
static int activate(HANDLE process, DWORD pid, const std::wstring& directory) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return fail(L"Could not identify the current Exanima process. Activation was not requested.");
    const uint64_t processStarted = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    uint64_t request = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&request, sizeof request, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !request)
        return fail(L"Could not create an activation request. Activation was not requested.");
    if (!callEnable(process, pid, request))
        return fail(L"ExCamera could not confirm it started. Restart Exanima and try again.");
    const std::wstring path = directory + L"first-person-status.json";
    // The mod waits up to gameReadyWaitMs (5 minutes) for a slow title; give it that and a little more.
    for (unsigned i = 0; i < 3100; ++i) {
        if (!waitTick()) return 0;
        DWORD code = 0;
        if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE)
            return fail(L"Exanima closed before camera activation completed.");
        fp::ActivationStatus status;
        if (!readActivationFile(path, status)) continue;
        switch (fp::activationResult(status, pid, processStarted, request, FP_REVISION, FP_BUILD)) {
        case fp::ActivationResult::ready: return 0;
        case fp::ActivationResult::failed:
            return fail(L"ExCamera could not start, and the game was left unchanged where possible. Restart Exanima "
                        L"and try again. If it keeps failing, report it with first-person-status.json "
                        L"and fault-log.txt from this folder.");
        case fp::ActivationResult::versionMismatch:
            return fail(L"The camera module mapped in Exanima differs from this launcher. Restart Exanima "
                        L"to load the current build.");
        case fp::ActivationResult::waiting: break;
        }
    }
    return fail(L"ExCamera did not finish starting within five minutes. If you stopped it with F7, press F6 "
                L"in game to restart it. Otherwise restart Exanima and try again.");
}
// The settings files the DLL reads, in its order (fp::settingsCandidates): beside the launcher and DLL when
// portable.txt is there (a test copy; session.inl decides the same), else the per-user files.
struct UserIni {
    fp::SettingsCandidates files;
    std::wstring existing() const { // the first that exists, else empty
        for (const std::wstring& user : files.paths)
            if (!user.empty() && GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return user;
        return L"";
    }
    const std::wstring& target() const { return files.target(); }
};
static UserIni userIni(const std::wstring& directory) {
    const bool portable = GetFileAttributesW((directory + L"portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    PWSTR docsFolder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docsFolder);
    const std::wstring docs = SUCCEEDED(hr) && docsFolder ? docsFolder : L"";
    if (docsFolder) CoTaskMemFree(docsFolder);
    wchar_t profile[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    const std::wstring fallbackDocs = n && n < MAX_PATH ? std::wstring(profile) + L"\\Documents" : L"";
    return {fp::settingsCandidates(directory, portable, docs, fallbackDocs)};
}
// [Compat] OtherHooks from the player's settings (the per-user file, else the one beside the launcher).
static fp::OtherHooks otherHooksSetting(const std::wstring& directory) {
    std::wstring ini = userIni(directory).existing();
    if (ini.empty()) ini = directory + L"first-person.ini";
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
    // Optional defense entries have their own complete-group admission in
    // the DLL; a conflict there keeps Automatic effective for the camera session.
    fp::Layout required = layout;
    for (unsigned t = 0; t < fp::sig::targetCount; ++t)
        if (fp::sig::targetOptionalGroups[t]) required.rva[t] = 0;
    unsigned n = fp::patchedEntries(gameView, required, read, found, 8);
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
// Safe start. The last status file says whether that session saw an exception (faults_seen) or a refused
// Settings form (settings_fault_at); a missing or unreadable file says nothing.
static bool lastSessionFaulted(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char data[64 * 1024 + 1]{};
    DWORD got = 0;
    const bool read = ReadFile(file, data, sizeof data - 1, &got, nullptr) != FALSE;
    CloseHandle(file);
    if (!read) return false;
    data[got] = 0;
    auto value = [&](const char* key) -> unsigned long long { // a number, or a "0x.." string; 0 when absent
        const char* at = strstr(data, key);
        if (!at) return 0;
        at += strlen(key);
        while (*at == ' ' || *at == ':' || *at == '"') ++at;
        return strtoull(at, nullptr, 0);
    };
    return value("\"faults_seen\"") || value("\"settings_fault_at\"");
}
// The player's file is copied to first-person.backup-YYYYMMDD-HHMMSS.ini beside it, then the DLL's file is
// replaced by one holding only [Safe] Reset=1: the DLL reads built-in defaults from it, writes every row out
// and drops the marker (settings.inl applySafeReset). Never deleted outright: the DLL would copy a legacy or
// release ini back in its place. The controls notice already seen stays seen.
static bool resetUserSettings(const std::wstring& directory, std::wstring& problem) {
    const UserIni ini = userIni(directory); // a portable copy resets and backs up its own file
    const std::wstring source = ini.existing(), target = ini.target();
    if (target.empty()) {
        problem = L"Could not find your Documents folder, so nothing was changed.";
        return false;
    }
    if (!source.empty()) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf_s(name, L"first-person.backup-%04u%02u%02u-%02u%02u%02u.ini", t.wYear, t.wMonth, t.wDay, t.wHour,
                   t.wMinute, t.wSecond);
        const std::wstring backup = source.substr(0, source.find_last_of(L'\\') + 1) + name;
        if (!CopyFileW(source.c_str(), backup.c_str(), TRUE)) {
            problem = L"Could not save a backup of your settings to\n" + backup + L"\n\nNothing was changed.";
            return false;
        }
    }
    const std::wstring folder = target.substr(0, target.find_last_of(L'\\'));
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const std::wstring staged = target + L".safe.tmp";
    DeleteFileW(staged.c_str());
    const bool seen = !source.empty() && GetPrivateProfileIntW(L"Launcher", L"ControlsSeen", 0, source.c_str()) != 0;
    const bool written = WritePrivateProfileStringW(L"Safe", L"Reset", L"1", staged.c_str()) &&
                         (!seen || WritePrivateProfileStringW(L"Launcher", L"ControlsSeen", L"1", staged.c_str())) &&
                         MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!written) {
        DeleteFileW(staged.c_str());
        problem = L"Could not write default settings to\n" + target +
                  (source.empty() ? L"" : L"\n\nYour settings and their backup are unchanged.");
        return false;
    }
    return true;
}
// Hold Shift while starting ExCamera.exe, or pass --safe; also offered after a session that reported faults.
// Not while the DLL already runs in the game: it holds the settings it read and would write them back.
static void offerSafeStart(const std::wstring& directory) {
    bool asked = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!asked) {
        int count = 0;
        if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count)) {
            for (int i = 1; i < count && !asked; ++i) asked = !_wcsicmp(args[i], L"--safe");
            LocalFree(args);
        }
    }
    const bool faulted = !asked && lastSessionFaulted(directory + L"first-person-status.json");
    if (!asked && !faulted) return;
    const DWORD pid = gamePid();
    if (pid && pid != MAXDWORD && remoteModule(pid, L"ExanimaFirstPerson.dll")) {
        if (asked)
            MessageBoxW(nullptr,
                        L"ExCamera is already running in Exanima, so it can't start with default settings now. "
                        L"Close Exanima, then start ExCamera with Shift held again.",
                        L"ExCamera", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    const wchar_t* question =
        asked ? L"Start with default settings? Your current settings are kept as a backup."
              : L"ExCamera ran into an error last time. Starting with default settings may not help, but it rules "
                L"your settings out.\n\nStart with default settings? Your current settings are kept as a backup.";
    if (MessageBoxW(nullptr, question, L"ExCamera",
                    MB_YESNO | MB_ICONQUESTION | (asked ? MB_DEFBUTTON1 : MB_DEFBUTTON2) | MB_SETFOREGROUND) != IDYES)
        return;
    std::wstring problem;
    if (!resetUserSettings(directory, problem))
        MessageBoxW(nullptr, (problem + L"\n\nExCamera will start with your current settings.").c_str(), L"ExCamera",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE launcherMutex = CreateMutexW(nullptr, FALSE, L"Local\\ExanimaFirstPersonLauncher");
    if (!launcherMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (launcherMutex) CloseHandle(launcherMutex);
        MessageBoxW(nullptr, L"ExCamera is already running. Check its other window, or close it first.", L"ExCamera",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
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
    offerSafeStart(directory);
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
    unsigned loadedCopies = 0;
    std::wstring loadedPath;
    // the module list can fail briefly while the game loads modules; try a few times.
    for (int tries = 0; tries < 5; ++tries) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        Sleep(200);
    }
    MODULEENTRY32W m{};
    m.dwSize = sizeof m;
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Module32FirstW(snapshot, &m)) do {
                if (!_wcsicmp(m.szModule, name))
                    remoteLoad = (uintptr_t)m.modBaseAddr + (uintptr_t)load - (uintptr_t)owner;
                if (!_wcsicmp(m.szModule, L"Exanima.exe")) base = (uintptr_t)m.modBaseAddr;
                if (!_wcsicmp(m.szModule, L"ExanimaFirstPerson.dll")) {
                    ++loadedCopies;
                    loadedPath = m.szExePath;
                }
            } while (Module32NextW(snapshot, &m));
        CloseHandle(snapshot);
    }
    if (loadedCopies) {
        if (loadedCopies > 1) {
            CloseHandle(process);
            return fail(L"More than one camera module is loaded. Restart Exanima with one copy before retrying.");
        }
        if (_wcsicmp(loadedPath.c_str(), dll.c_str())) {
            CloseHandle(process);
            return fail((L"A camera version from another folder is already loaded. Restart Exanima to switch "
                         L"versions.\n\nLoaded from:\n" + loadedPath).c_str());
        }
        // Our active patches are expected. The module retains its own re-arm
        // admission and trampoline checks; no launcher-level foreign-hook prompt is needed.
        const int result = activate(process, pid, directory);
        CloseHandle(process);
        return result;
    }
    if (!base || !remoteLoad) {
        CloseHandle(process);
        return fail(L"Could not read the game's module list (it may still be starting). Try again in a few "
                    L"seconds. No changes were made.");
    }
    if (!otherHooksOk(process, base, directory)) {
        CloseHandle(process);
        return 0;
    }
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t), written = 0;
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) || written != bytes) {
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return fail(L"Could not prepare the module path.");
    }
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
        return fail(L"Exanima is taking too long to respond. Close it, then start it and ExCamera again.");
    }
    // GetExitCodeThread truncates LoadLibraryW's 64-bit HMODULE to 32 bits, which is 0 for 4 GB-aligned ASLR bases.
    if (!remoteModule(pid, L"ExanimaFirstPerson.dll") && !result) {
        CloseHandle(process);
        return fail(L"Windows could not load ExanimaFirstPerson.dll.");
    }
    const int activation = activate(process, pid, directory);
    CloseHandle(process);
    return activation;
}
