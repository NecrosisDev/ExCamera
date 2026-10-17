// Calls the real windowProc in an owned test process; never attaches to Exanima.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "first_person.cpp"
static int forwarded = 0, checks = 0, failures = 0;
static LRESULT CALLBACK fallback(HWND, UINT, WPARAM, LPARAM) {
    ++forwarded;
    return 1234;
}
static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) ++failures;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
}
static LPARAM bits(unsigned flags) {
    return static_cast<LPARAM>(static_cast<unsigned long long>(flags));
}
static void reset(int fov = 75) {
    active = 1;
    viewMode = 0;
    selectedFov.store(fov);
    forwarded = 0;
    originalWindowProc = fallback;
    InterlockedExchange(&inputEnabled, 0);
    inputOwner.clearTransient();
}
int main() {
    reset();
    active = 0;
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 75 && forwarded == 1, "inactive/settled overhead keeps native F9");
    reset();
    viewMode = 2;
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 75 && forwarded == 1, "overhead blend keeps native F9");
    reset();
    auto r = windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 80 && forwarded == 0 && r == 0, "F10 system key widens 75 to 80");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 80 && forwarded == 0, "ordinary F10 also widens once");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 70 && forwarded == 0, "F9 narrows 75 to 70");
    reset(100);
    windowProc(nullptr, WM_KEYDOWN, VK_F11, 1);
    check(selectedFov.load() == 75 && forwarded == 0, "F11 resets to 75");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, bits(0x40000001u));
    windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xc0000001u));
    check(selectedFov.load() == 80 && forwarded == 0,
          "system F10 repeat/up consumed without another change or menu activation");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
    windowProc(nullptr, WM_KEYDOWN, VK_F10, bits(0x40000001u));
    windowProc(nullptr, WM_KEYUP, VK_F10, bits(0xc0000001u));
    check(selectedFov.load() == 80 && forwarded == 0, "ordinary F10 repeat/up consumed");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, bits(0x20000001u));
    windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xe0000001u));
    check(selectedFov.load() == 75 && forwarded == 2, "Alt+F10 remains native");
    reset();
    r = windowProc(nullptr, WM_SYSKEYDOWN, VK_F4, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 1 && r == 1234, "Alt+F4 remains native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_TAB, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 1, "Alt+Tab remains native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F9, bits(0x20000001u));
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F11, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 2, "Alt+F9 and Alt+F11 remain native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F6, 1);
    check(selectedFov.load() == 75 && forwarded == 1,
          "unrelated system key is not promoted to normal hotkey");
    reset(130);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 130, "upper FOV bound remains 130");
    reset(30);
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 30, "lower FOV bound remains 30");
    reset(128);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 130, "near-upper FOV value clamps to 130");
    reset(32);
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 30, "near-lower FOV value clamps to 30");
    reset(30);
    for (int i = 0; i < 20; ++i) {
        windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
        windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xc0000001u));
    }
    check(selectedFov.load() == 130 && forwarded == 0, "20 discrete F10 presses span the full 30-130 range");
    reset();
    InterlockedExchange(&inputEnabled, 1);
    windowProc(nullptr, WM_KEYDOWN, 'W', 1);
    check(inputOwner.heldMovement() == 1 && inputOwner.pendingMovement() == 1 && forwarded == 1,
          "movement press is unchanged");
    windowProc(nullptr, WM_KEYUP, 'W', bits(0xc0000001u));
    check(inputOwner.heldMovement() == 0 && forwarded == 2, "movement release is unchanged");
    windowProc(nullptr, WM_KEYDOWN, 'W', 1);
    windowProc(nullptr, WM_KILLFOCUS, 0, 0);
    check(inputEnabled == 0 && inputOwner.heldMovement() == 0 && inputOwner.pendingMovement() == 0,
          "focus loss still clears movement capture");
    reset();
    windowProc(nullptr, WM_LBUTTONDOWN, VK_F10, 0);
    check(selectedFov.load() == 75 && forwarded == 1, "non-key message cannot change FOV");
    {
        // FOV keys remain available with F8 open. A later panel click must edit the
        // current live values, even when its displayed snapshot predates the hotkey.
        makeLive(panel::ModSettings{});
        reset();
        cursorFree = 1;
        panel::SettingsWindow settings;
        settings.getSettings = currentSettings;
        settings.changeSetting = applySetting;
        windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
        check(selectedFov.load() == 80, "F10 updates live FOV while the settings pointer is free");
        settings.applyControl(panel::setting::sensitivity, 1);
        check(selectedFov.load() == 80 && currentSettings().sensitivity == 110,
              "an unrelated settings click preserves a newer FOV hotkey value");
        selectedFov = 100;
        settings.applyControl(panel::setting::fov, 1);
        check(selectedFov.load() == 105, "the panel FOV control steps from the latest live value");
        headSteady = 20;
        settings.applyControl(panel::ModSettings::resetControl, 0);
        check(selectedFov.load() == 75 && headSteady.load() == 20 && currentSettings().sensitivity == 110,
              "Reset page preserves newer settings outside the displayed group");
        cursorFree = 0;
    }
    {
        // Even a just-refreshed UI snapshot can go stale before its callback.
        // Inject updates at that boundary: the owner must combine each action
        // with the state that exists when it accepts it.
        makeLive(panel::ModSettings{});
        reset();
        panel::SettingsWindow settings;
        settings.getSettings = currentSettings;
        settings.changeSetting = [](int row, int delta, int group) {
            windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
            return applySetting(row, delta, group);
        };
        settings.applyControl(panel::setting::sensitivity, 1);
        check(selectedFov.load() == 80 && currentSettings().sensitivity == 110,
              "a hotkey at the settings callback boundary survives an unrelated row action");
        selectedFov = 100;
        settings.applyControl(panel::setting::fov, 1);
        check(selectedFov.load() == 110,
              "a hotkey and panel FOV step each contribute at the callback boundary");
        selectedFov = 100;
        headSteady = 70;
        settings.changeSetting = [](int row, int delta, int group) {
            panel::ModSettings newer = currentSettings();
            newer.headSteady = 20;
            makeLive(newer);
            return applySetting(row, delta, group);
        };
        settings.applyControl(panel::ModSettings::resetControl, 0);
        check(selectedFov.load() == 75 && headSteady.load() == 20,
              "Reset page preserves another group's update at the callback boundary");
    }
    {
        // Real INI persistence in an owned temporary file. The worker and panel
        // may arrive in either order; the final live value must be the saved one.
        wchar_t keptPath[MAX_PATH], temp[MAX_PATH], file[MAX_PATH];
        wcscpy_s(keptPath, settingsPath);
        GetTempPathW(MAX_PATH, temp);
        swprintf_s(file, L"%sfp-fov-owner-%lu.ini", temp, GetCurrentProcessId());
        DeleteFileW(file);
        wcscpy_s(settingsPath, file);
        bool consistent = true;
        for (int i = 0; i < 16; ++i) {
            makeLive(panel::ModSettings{});
            persistedFov = 75;
            adjustFov(5);
            HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!go) { consistent = false; break; }
            std::thread worker([&] { WaitForSingleObject(go, 2000); saveFov(); });
            std::thread editor([&] { WaitForSingleObject(go, 2000); applySetting(panel::setting::fov, 1, 0); });
            SetEvent(go);
            worker.join();
            editor.join();
            CloseHandle(go);
            consistent = consistent && selectedFov.load() == 85 && persistedFov.load() == 85 &&
                         GetPrivateProfileIntW(L"Camera", L"FOV", 0, file) == 85;
        }
        check(consistent, "concurrent worker persistence and panel FOV edits save the final live value");
        // A failed panel write must leave FOV pending so the worker can retry.
        swprintf_s(settingsPath, L"%s\\first-person.ini", file); // a file cannot contain a child INI
        makeLive(panel::ModSettings{});
        persistedFov = 75;
        const panel::SettingsChange result = applySetting(panel::setting::fov, 1, 0);
        check(result.changed && !result.saved && selectedFov.load() == 80 && persistedFov.load() == 75,
              "failed INI writes do not claim FOV was saved or suppress its retry");
        wcscpy_s(settingsPath, file);
        saveFov();
        check(persistedFov.load() == 80 && GetPrivateProfileIntW(L"Camera", L"FOV", 0, file) == 80,
              "worker retries a previously failed panel FOV write");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, file); // release profile cache before cleanup
        DeleteFileW(file);
        wcscpy_s(settingsPath, keptPath);
    }
    std::printf("SUMMARY: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
