// Settings controls, migration and owned Win32 window regressions. No game is launched or inspected.
#include "first_person.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

static HWND testSettingsHandle() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM out) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(w, &process);
            wchar_t name[64]{};
            if (process == GetCurrentProcessId() && GetClassNameW(w, name, 64) &&
                !wcscmp(name, L"ExCameraSettings")) {
                *reinterpret_cast<HWND*>(out) = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

static void ownedCursorArchive(const fs::path& folder) {
    fs::create_directories(folder);
    const uint32_t header[2]{0xAFBF0C01u, 32};
    const uint32_t fields[8]{0x1D2D3DC6u, 128, 128, 1, 0x0100E608u, 0, 0, 128 * 128 * 4};
    std::vector<uint8_t> data(32 + 128 * 128 * 4, 255);
    memcpy(data.data(), fields, 32);
    uint8_t entry[32]{};
    constexpr char memberName[] = "menucursor.rfi";
    static_assert(sizeof(memberName) <= 16); // RPK member names occupy the first 16 bytes.
    memcpy(entry, memberName, sizeof(memberName) - 1);
    const uint32_t size = static_cast<uint32_t>(data.size());
    memcpy(entry + 20, &size, sizeof(size));
    std::ofstream out(folder / L"Resource.rpk", std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(entry), sizeof(entry));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    assert(out.good());
}

int main() {
    wchar_t temporary[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary));
    const fs::path work =
        fs::path(temporary) / (L"excamera-ui-test-" + std::to_wstring(GetCurrentProcessId()));
    assert(!fs::exists(work));
    fs::create_directories(work);
    ownedCursorArchive(work / L"assets");
    uiart::gameFolderOverride() = (work / L"assets").wstring();
    std::vector<uint8_t> cursorData;
    assert(uiart::readMember((work / L"assets" / L"Resource.rpk").wstring(), "menucursor.rfi", cursorData));
    uiart::Image cursorImage;
    assert(uiart::decodeRfi(cursorData, cursorImage));
    assert(cursorImage.ok() && cursorImage.w == 128 && cursorImage.h == 128);
    const HCURSOR fixtureCursor = uiart::gameCursor(GetSystemMetrics(SM_CYCURSOR));
    assert(fixtureCursor); // The lifetime test must exercise an owned cursor, not the shared-arrow fallback.
    DestroyCursor(fixtureCursor);

    {
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 1200;
        info.bmiHeader.biHeight = -900;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        uint32_t* pixels = nullptr;
        HBITMAP bitmap =
            CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        assert(bitmap && pixels);
        HGDIOBJ old = SelectObject(dc, bitmap);
        panel::PanelView view;
        view.page.kind = panel::Page::table;
        view.page.settingsPage = true;
        view.page.title = "Settings";
        for (const POINT size : {POINT{974, 609}, POINT{700, 420}, POINT{1180, 820}})
            for (int group = 0; group < panel::ModSettings::groupCount; ++group) {
                view.settingsGroup = group;
                view.page.title = panel::ModSettings::group(group).name;
                view.page.description = panel::ModSettings::groupDescription(group);
                view.settingsScroll = 0;
                std::set<int> reachable;
                for (;;) {
                    view.paint(dc, size.x, size.y);
                    GdiFlush();
                    assert(view.settingsPitch ==
                           (view.layout.stats.right - view.layout.stats.left < 620 ? 76 : 44));
                    assert(view.settingsViewport.top > view.layout.header.bottom);
                    const uint32_t* footerPixels = pixels + view.layout.actions.top * 1200;
                    const size_t footerCount = size_t(size.y - view.layout.actions.top) * 1200;
                    const std::vector<uint32_t> footer(footerPixels, footerPixels + footerCount);
                    for (const auto& control : view.settingControls) {
                        const RECT& r = control.r;
                        assert(r.left >= 0 && r.top >= 0 && r.right <= size.x && r.bottom <= size.y);
                        assert(r.right > r.left && r.bottom > r.top);
                        reachable.insert(control.row);
                        const POINT cursor{r.right - 1, (r.top + r.bottom) / 2};
                        if (control.row != panel::ModSettings::resetControl) {
                            const wchar_t* expected = view.settings.inactiveReason(control.row);
                            if (!expected) expected = panel::ModSettings::tip(control.row);
                            assert(view.settingTipAt(cursor) == expected);
                        }
                        if (const wchar_t* help = view.settingTipAt(cursor)) {
                            view.tip = help;
                            view.tipAt = cursor;
                            view.paintTip(dc, size.x, size.y);
                            GdiFlush();
                            // Help must not paint over Reset or the save status, even on the last row.
                            assert(!memcmp(footerPixels, footer.data(), footerCount * sizeof(uint32_t)));
                        }
                    }
                    const int previous = view.settingsScroll;
                    view.scrollSettings(1);
                    if (view.settingsScroll == previous) break;
                }
                view.settingsScroll = 0;
                view.paint(dc, size.x, size.y);
                int delta = 0;
                const POINT heading{view.settingsViewport.left + 10, view.settingsViewport.top + 10};
                assert(view.settingAt(heading, delta) == -1 && !view.settingTipAt(heading));
                const auto& rows = panel::ModSettings::group(group);
                for (int i = 0; i < rows.count; ++i) assert(reachable.count(rows.rows[i]));
                assert(reachable.count(panel::ModSettings::resetControl));
            }
        view.settingsGroup = 0;
        view.settings.tpCombatAimFree = false;
        view.settingsScroll = 999;
        view.paint(dc, 974, 609);
        for (const auto& control : view.settingControls)
            if (control.row == panel::setting::tpDeadzoneX || control.row == panel::setting::tpDeadzoneY) {
                assert(!control.enabled);
                int delta = 0;
                assert(view.settingAt({control.r.left + 1, control.r.top + 1}, delta) == -1);
            }
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        puts("PASS: all settings and Reset remain reachable; tooltips preserve the footer; inactive rows "
             "cannot edit");
    }
    {
        panel::ModSettings edited, defaults;
        for (int row = 0; row < panel::ModSettings::rows; ++row) edited.step(row, 1);
        for (int page = 0; page < panel::ModSettings::groupCount; ++page) {
            auto reset = edited;
            reset.resetGroup(page);
            std::set<int> pageRows;
            const auto& group = panel::ModSettings::group(page);
            for (int i = 0; i < group.count; ++i) pageRows.insert(group.rows[i]);
            for (int row = 0; row < panel::ModSettings::rows; ++row)
                assert(reset.value(row) == (pageRows.count(row) ? defaults.value(row) : edited.value(row)));
        }
        puts("PASS: Reset page restores exactly the consolidated page rows");
    }
    {
        const fs::path ini = work / L"settings.ini";
        wcscpy_s(settingsPath, ini.c_str());
        panel::ModSettings s;
        s.fov = 90;
        s.tpHeight = -20;
        assert(s.value(panel::setting::tpHeight) == L"20 cm below the eyes");
        assert(s.step(panel::setting::tpFov, -1) && s.tpFov == 85);
        s.tpFov = 0;
        assert(s.step(panel::setting::tpFov, 1) && s.tpFov == 95);
        makeLive(s);
        const auto changed = applySetting(panel::setting::fpSwingSelection, 0, 2);
        assert(changed.changed && changed.saved && fpSwingSelection.load() == 0 &&
               tpSwingSelection.load() == 1);
        assert(readSettings(ini.c_str()).fpSwingSelection == 0);
        rawMouse = true;
        rawWanted = false;
        assert(currentSettings().rawMouseFallback);
        applySetting(panel::setting::sensitivity, 1, 2);
        assert(rawMouse && !rawWanted.load() && currentSettings().rawMouseFallback);
        WritePrivateProfileStringW(L"Input", L"SensitivityPercent", L"200junk", ini.c_str());
        WritePrivateProfileStringW(L"View", L"SwingCrosshair", L"bogus", ini.c_str());
        assert(readSettings(ini.c_str()).sensitivity == 100 && !readSettings(ini.c_str()).hideSwingCrosshair);
        assert(panel::iniFov(L"   ") == panel::badText);
        assert(panel::ModSettings::nextKey(VK_SHIFT, 1) == VK_RBUTTON);
        const fs::path release = work / L"empty-release", user = work / L"first-user";
        fs::create_directories(release);
        assert(userSettingsFile((release / L"first-person.ini").wstring(), user.wstring()) ==
               (user / L"first-person.ini").wstring());
        assert(fs::is_regular_file(user / L"first-person.ini"));
        const fs::path profile = work / L"profile", redirected = work / L"redirected-documents";
        fs::create_directories(profile / L"Documents/ExCamera");
        WritePrivateProfileStringW(L"Camera", L"FOV", L"95",
                                   (profile / L"Documents/ExCamera/first-person.ini").c_str());
        const auto folder = userSettingsDir(profile.wstring(), redirected.wstring());
        assert(folder == (redirected / L"ExCamera").wstring());
        assert(GetPrivateProfileIntW(L"Camera", L"FOV", 0,
                                     (fs::path(folder) / L"first-person.ini").c_str()) == 95);
        puts("PASS: per-view swing settings, raw fallback preference, parsing and fresh/redirected Documents "
             "persistence");
    }
    {
        const fs::path folder = work / L"retry", ini = folder / L"first-person.ini";
        wcscpy_s(settingsPath, ini.c_str()); // Parent deliberately absent: Win32 profile writes fail.
        makeLive(panel::ModSettings{});
        persistedFov = selectedFov.load();
        const auto first = applySetting(panel::setting::sensitivity, 1, 2);
        assert(first.changed && !first.saved && sensitivity.load() > 0.0025f);
        assert(settingsSavePending() &&
               persistedFov.load() == selectedFov.load()); // Status includes non-FOV failures.
        const auto newer = applySetting(panel::setting::sensitivity, 1, 2);
        assert(newer.changed && !newer.saved && currentSettings().sensitivity == 120);
        applySetting(panel::setting::headRoll, -1, 1);
        applySetting(panel::setting::headPitch, -1, 1);
        applySetting(panel::setting::headYaw, -1, 1);
        applySetting(panel::setting::headBob, -1, 1);
        applySetting(panel::setting::spineFollow, -1, 1);
        assert(currentSettings().savePending);
        fs::create_directories(folder);
        saveFov(); // Existing worker entry retries only four dirty rows this tick.
        assert(currentSettings().savePending);
        saveFov();
        assert(!currentSettings().savePending);
        assert(!settingsSavePending());
        assert(readSettings(ini.c_str()).sensitivity == 120);
        assert(readSettings(ini.c_str()).headRoll == 70 && readSettings(ini.c_str()).headPitch == 60 &&
               readSettings(ini.c_str()).headYaw == 40 && readSettings(ini.c_str()).headBob == 20 &&
               readSettings(ini.c_str()).spineFollow == 50);
        puts("PASS: failed non-FOV saves retry in bounded batches and persist the newer edit, never the "
             "stale value");
    }
    {
        DWORD stableObjects = 0;
        for (int cycle = 0; cycle < 3; ++cycle) {
            {
                panel::SettingsWindow settings;
                std::atomic<int> clickedGroup{-1};
                settings.changeSetting = [&](int row, int, int group) {
                    assert(row == panel::ModSettings::resetControl);
                    clickedGroup = group;
                    return panel::SettingsChange{};
                };
                settings.getSettings = [] {
                    return panel::ModSettings{};
                };
                settings.start(nullptr);
                HWND w = nullptr;
                const ULONGLONG until = GetTickCount64() + 5000;
                while (!(w = testSettingsHandle()) && GetTickCount64() < until) {
                    settings.heartbeat();
                    Sleep(5);
                }
                assert(w);
                SetWindowPos(w, nullptr, -2000, -2000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(w, SW_SHOWMINNOACTIVE);
                assert(IsIconic(w));
                SendMessageW(w, WM_APP + 2, 0, 0);
                SendMessageW(w, WM_APP + 2, 1, 0);
                assert(IsWindowVisible(w) && !IsIconic(w));
                RECT client{};
                assert(GetClientRect(w, &client));
                // Exercise the real navigation and Reset callback, not just a copied order array.
                // Root row is 0; the three pages must map to their actual reset scope.
                const int expectedGroups[]{0, 1, 2};
                for (int nav = 0; nav < panel::ModSettings::groupCount; ++nav) {
                    SendMessageW(w, WM_PAINT, 0, 0);
                    const int y = panel::Layout::navTop + (nav + 1) * panel::Layout::rowHeight +
                                  panel::Layout::rowHeight / 2;
                    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, y));
                    SendMessageW(w, WM_PAINT, 0, 0);
                    clickedGroup = -1;
                    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON,
                                 MAKELPARAM(client.right - 60, client.bottom - 30));
                    assert(clickedGroup.load() == expectedGroups[nav]);
                }
                settings.stop();
            }
            const DWORD objects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            if (cycle) assert(objects <= stableObjects);
            stableObjects = objects;
        }
        puts("PASS: three-page navigation preserves group/reset mapping; F8 restore and cursor lifetime");
    }
    settingsPath[0] = 0;
    uiart::gameFolderOverride().clear();
    fs::remove_all(work);
}
