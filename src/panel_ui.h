#pragma once
// The F8 Settings view: the mod's settings (ModSettings) on sub-pages, custom drawn (GDI) in the game's
// own fonts and stone, so the same paint code serves the live window and offline checks into a bitmap.
#include <set>
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <map>
#include <array>
#include <cstdio>
#include <locale.h>
#include "input_policy.h"
#include "ui_art.h"
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace panel {

// the game's own menu sounds (Sound.rpk menuclick / menusel, RIFF WAV as shipped), read from the
// game's archive (works without the service), played with PlaySound. The game's own sound path would
// need a game call that is not reviewed; this plays beside it.
inline void playMenuSound(bool click) {
    static std::vector<uint8_t> clickWav, selectWav;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::wstring a = uiart::gameFolder() + L"\\Sound.rpk";
        uiart::readMember(a, "menuclick", clickWav);
        uiart::readMember(a, "menusel", selectWav);
    }
    const std::vector<uint8_t>& w = click ? clickWav : selectWav;
    if (w.size() > 44 && !memcmp(w.data(), "RIFF", 4))
        PlaySoundW((LPCWSTR)w.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
// Windows' own dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 2004, 19 before),
// a dark grey with light text; loaded at run time, so an older Windows just keeps its own.
inline void darkCaption(HWND w) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
        const BOOL on = TRUE;
        if (FAILED(set(w, 20, &on, sizeof on))) set(w, 19, &on, sizeof on);
    }
    SetWindowPos(w, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

inline std::string narrowText(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// The first-person mod's own settings: shown on the F8 panel's Settings page, applied
// live and saved by the mod to first-person.ini.
struct ModSettings {
    int fov = 75, sensitivity = 100,
        headSteady = 70; // degrees, percent, percent (head stabilisation, 0 = the game's)
    bool rawMouse = false, hideHelmet = false, hideSwingCrosshair = false;
    int grabRange = fp::grabRangeDefault; // cm from the eye: the farthest grab and hold
    int tpDistance = 250, tpShoulder = 40,
        tpHeight = 15;   // third person: cm behind, cm to the right (negative: left), cm above the eye
    bool tpTight = true; // in tight spaces the shoulder swaps to the open side and the framing closes in
    bool tpCombat = true;
    int tpFov = 0;
    bool attackCue = true, cueThrust = true;
    int cueSize = 100, cueOpacity = 100; // the attack wind-up arc
    int rmbMode = 1;                     // right mouse frees the cursor: 0 off, 1 hold, 2 toggle
    bool idleCrosshairAlways = false;    // out of combat the crosshair shows only with right mouse
    bool freezeWindows = true;           // the game's windows free the cursor and hold look and movement
    int spineFollow = 60; // how far the spine bends with the look (forward looking down, back looking up)
    int crouchMode = 2; // the game's duck key out of combat: 0 off (the game's short duck), 1 hold, 2 toggle
    int nearClip = fp::nearClipDefaultTenths; // the near plane's floor, tenths of a centimetre
    int headRoll = 80, headPitch = 70, headYaw = 50,
        headBob = 30; // how much wobble on each axis is held off, and how much of the head's bob
    bool aimMarker = true;
    int flickCatchup =
        50; // the crosshair while the head catches up; how much a flick's lead is eased // how much head wobble on each axis stiffens the neck (0 = not at all)
    int tpLag = 40;
    bool
        tpHide = true,
        tpDown =
            true; // camera lag, hide what is in the way, knockdown follow // closer and centred in combat; third person's own field of view (0 = as first person)
    bool operator==(const ModSettings& o) const {
        return fov == o.fov && sensitivity == o.sensitivity && headSteady == o.headSteady &&
               rawMouse == o.rawMouse && hideHelmet == o.hideHelmet && grabRange == o.grabRange &&
               hideSwingCrosshair == o.hideSwingCrosshair && tpDistance == o.tpDistance &&
               tpShoulder == o.tpShoulder && tpHeight == o.tpHeight && tpTight == o.tpTight &&
               tpCombat == o.tpCombat && tpFov == o.tpFov && tpLag == o.tpLag && tpHide == o.tpHide &&
               tpDown == o.tpDown && attackCue == o.attackCue && cueThrust == o.cueThrust &&
               cueSize == o.cueSize && cueOpacity == o.cueOpacity && rmbMode == o.rmbMode &&
               idleCrosshairAlways == o.idleCrosshairAlways && headRoll == o.headRoll &&
               headPitch == o.headPitch && headYaw == o.headYaw && headBob == o.headBob &&
               freezeWindows == o.freezeWindows && spineFollow == o.spineFollow &&
               crouchMode == o.crouchMode && nearClip == o.nearClip && aimMarker == o.aimMarker &&
               flickCatchup == o.flickCatchup;
    }
    // One click on a row's control: numbers step by delta (within range), switches flip.
    bool step(int row, int delta) {
        auto clamp = [](int v, int lo, int hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        const ModSettings before = *this;
        switch (row) {
        case 0:
            fov = fp::clampFov(fov + delta * 5);
            break;
        case 1:
            sensitivity = clamp(sensitivity + delta * 10, 10, 400);
            break;
        case 2:
            headSteady = clamp(headSteady + delta * 10, 0, 100);
            break;
        case 3:
            rawMouse = !rawMouse;
            break;
        case 4:
            hideHelmet = !hideHelmet;
            break;
        case 5:
            grabRange = fp::clampGrabRange(grabRange + delta * 10);
            break;
        case 6:
            hideSwingCrosshair = !hideSwingCrosshair;
            break;
        case 7:
            tpDistance = clamp(tpDistance + delta * 25, fp::tpDistanceMin, fp::tpDistanceMax);
            break;
        case 8:
            tpShoulder = clamp(tpShoulder + delta * 10, -fp::tpShoulderMax, fp::tpShoulderMax);
            break;
        case 9:
            tpHeight = clamp(tpHeight + delta * 10, fp::tpHeightMin, fp::tpHeightMax);
            break;
        case 10:
            tpTight = !tpTight;
            break;
        case 11:
            tpCombat = !tpCombat;
            break;
        case 13:
            tpLag = clamp(tpLag + delta * 10, 0, 100);
            break;
        case 14:
            tpHide = !tpHide;
            break;
        case 15:
            tpDown = !tpDown;
            break;
        case 16:
            attackCue = !attackCue;
            break;
        case 17:
            cueSize = clamp(cueSize + delta * 10, 50, 200);
            break;
        case 18:
            cueOpacity = clamp(cueOpacity + delta * 10, 20, 100);
            break;
        case 19:
            cueThrust = !cueThrust;
            break;
        case 20:
            rmbMode = (rmbMode + (delta < 0 ? 2 : 1)) % 3;
            break; // hold -> toggle -> off, or back
        case 21:
            idleCrosshairAlways = !idleCrosshairAlways;
            break;
        case 22:
            headRoll = clamp(headRoll + delta * 10, 0, 100);
            break;
        case 23:
            headPitch = clamp(headPitch + delta * 10, 0, 100);
            break;
        case 24:
            headYaw = clamp(headYaw + delta * 10, 0, 100);
            break;
        case 25:
            aimMarker = !aimMarker;
            break;
        case 26:
            flickCatchup = clamp(flickCatchup + delta * 10, 0, 100);
            break;
        case 27:
            headBob = clamp(headBob + delta * 10, 0, 100);
            break;
        case 28:
            freezeWindows = !freezeWindows;
            break;
        case 29:
            spineFollow = clamp(spineFollow + delta * 10, 0, 100);
            break;
        case 30:
            crouchMode = crouchMode == 2 ? 1 : (crouchMode == 1 ? 0 : 2);
            break; // toggle -> hold -> off
        case 31:
            nearClip = fp::clampNearClip(nearClip + delta);
            break;
        case 12:
            tpFov = tpFov == 0 ? (delta > 0 ? 80 : 0)
                               : (tpFov + delta * 5 < fp::fovMin ? 0 : fp::clampFov(tpFov + delta * 5));
            break; // below the minimum: back to "as first person"
        default:
            return false;
        }
        return !(*this == before);
    }
    static constexpr int rows = 32;
    static const wchar_t* label(int row) {
        static const wchar_t* names[rows] = {
            L"Field of view",
            L"Mouse sensitivity",
            L"Head stabilisation",
            L"Raw mouse input",
            L"Helmet in view",
            L"Grab range",
            L"Crosshair during a swing",
            L"Third person: distance",
            L"Third person: shoulder",
            L"Third person: height",
            L"Third person: tight spaces",
            L"Third person: combat framing",
            L"Third person: field of view",
            L"Third person: camera lag",
            L"Third person: in the way",
            L"Third person: knocked down",
            L"Attack wind-up arc",
            L"Wind-up arc: size",
            L"Wind-up arc: opacity",
            L"Wind-up arc: thrusts",
            L"Right mouse frees the cursor",
            L"Crosshair out of combat",
            L"Head steadiness: roll",
            L"Head steadiness: pitch",
            L"Head steadiness: yaw",
            L"Aim marker",
            L"Flick catch-up",
            L"Head bob steadiness",
            L"Freeze while windows are open",
            L"Spine follows aim",
            L"Crouch",
            L"Near clip",
        };
        return row >= 0 && row < rows ? names[row] : L"";
    }
    // The Settings sub-pages: every row belongs to exactly one (test_controls checks it). The last group has
    // no rows: the save backup and restore.
    struct Group {
        const char* name;
        int rows[10];
        int count;
    };
    static constexpr int groupCount = 7, savesGroup = -1; // no save backup page
    static const Group& group(int g) {
        static const Group groups[groupCount] = {
            {"Head", {2, 22, 23, 24, 27, 29}, 6},
            {"View", {0, 4, 31}, 3},
            {"Controls", {1, 3, 20, 28, 30}, 5},
            {"Aim and crosshair", {6, 21, 25, 26}, 4},
            {"Attack wind-up arc", {16, 17, 18, 19}, 4},
            {"Grab", {5}, 1},
            {"Third person", {7, 8, 9, 10, 11, 12, 13, 14, 15}, 9},
        };
        return groups[g < 0 || g >= groupCount ? 0 : g];
    }
    std::wstring value(int row) const {
        switch (row) {
        case 0:
            return std::to_wstring(fov) + L"\x00B0";
        case 1:
            return std::to_wstring(sensitivity) + L"%";
        case 2:
            return headSteady ? std::to_wstring(headSteady) + L"%" : std::wstring(L"off (the game's)");
        case 3:
            return rawMouse ? L"on" : L"off";
        case 4:
            return hideHelmet ? L"hidden" : L"shown";
        case 5:
            return std::to_wstring(grabRange) + L" cm";
        case 6:
            return hideSwingCrosshair ? L"hidden" : L"shown";
        case 7:
            return std::to_wstring(tpDistance) + L" cm";
        case 8:
            return tpShoulder == 0 ? std::wstring(L"centre")
                                   : std::to_wstring(tpShoulder < 0 ? -tpShoulder : tpShoulder) +
                                         (tpShoulder < 0 ? L" cm left" : L" cm right");
        case 9:
            return std::to_wstring(tpHeight) + L" cm above the eye";
        case 10:
            return tpTight ? L"swap shoulder, frame closer" : L"off";
        case 11:
            return tpCombat ? L"closer and centred" : L"off";
        case 13:
            return tpLag == 0 ? std::wstring(L"off (as before)") : std::to_wstring(tpLag) + L"%";
        case 14:
            return tpHide ? L"hidden (walls still pull in)" : L"the camera pulls in";
        case 15:
            return tpDown ? L"follow the body" : L"follow the head";
        case 16:
            return attackCue ? L"shown while a swing winds up" : L"off";
        case 17:
            return std::to_wstring(cueSize) + L"%";
        case 18:
            return std::to_wstring(cueOpacity) + L"%";
        case 19:
            return cueThrust ? L"below the crosshair" : L"off";
        case 20:
            return rmbMode == 0 ? L"off"
                                : (rmbMode == 2 ? L"toggle (click on, click off)" : L"hold (out of combat)");
        case 21:
            return idleCrosshairAlways ? L"always" : L"only with right mouse";
        case 22:
            return headRoll ? std::to_wstring(headRoll) + L"%" : std::wstring(L"off");
        case 23:
            return headPitch ? std::to_wstring(headPitch) + L"%" : std::wstring(L"off");
        case 24:
            return headYaw ? std::to_wstring(headYaw) + L"%" : std::wstring(L"off");
        case 25:
            return aimMarker ? L"while the head catches up" : L"off";
        case 26:
            return flickCatchup ? std::to_wstring(flickCatchup) + L"%" : std::wstring(L"off");
        case 27:
            return headBob ? std::to_wstring(headBob) + L"%" : std::wstring(L"off");
        case 28:
            return freezeWindows ? L"on" : L"off";
        case 29:
            return spineFollow ? std::to_wstring(spineFollow) + L"%" : std::wstring(L"off");
        case 31: {
            wchar_t b[24];
            swprintf_s(b, L"%d.%d cm", nearClip / 10, nearClip % 10);
            return b;
        }
        case 30:
            return crouchMode == 2
                       ? L"toggle (the game's duck key)"
                       : (crouchMode == 1 ? L"hold (the game's duck key)" : L"off (the game's short duck)");
        case 12:
            return tpFov == 0 ? std::wstring(L"as first person") : std::to_wstring(tpFov) + L"\x00B0";
        default:
            return L"";
        }
    }
    static bool isNumber(int row) {
        return (row >= 0 && row <= 2) || row == 5 || (row >= 7 && row <= 9) || row == 12 || row == 13 ||
               row == 17 || row == 18 || (row >= 22 && row <= 24) || row == 26 || row == 27 || row == 29 ||
               row == 31 || row == 34 || row == 35;
    }
};

struct NavNode {
    std::string label, right, request; // request: service path that opens this row ("" = folder only)
    std::string full;                  // the whole name when the row shows a shorter one (tooltip)
    int depth = 0;
    bool expanded = false;
    std::vector<NavNode> kids;
};

// What the right side shows.
struct Page {
    enum Kind { none, item, table } kind = none;
    std::string title, tag, description, status;
    std::vector<std::pair<std::string, std::string>> stats; // item stats, or generic rows
    bool settingsPage = false;                              // the first-person settings (no service needed)
};

// ---- drawing ----
// restyle: Exanima's own palette (docs/INGAME.md "Look and feel", uistyle.style): steel dark
// #30302c, steel mid #4e5757, steel light #a9cce2 for what is lit or chosen; text is the game font's
// white tinted light grey (the game's exact menu text colour is not found in its code yet).
struct Theme {
    COLORREF bg = RGB(48, 48, 44), pane = RGB(40, 41, 38), line = RGB(78, 87, 87), text = RGB(242, 240, 232),
             dim = RGB(196, 202, 200), accent = RGB(169, 204, 226), select = RGB(70, 82, 86),
             hover = RGB(58, 66, 68), button = RGB(60, 68, 68), buttonOff = RGB(46, 50, 50);
};
struct Fonts {
    HFONT body = nullptr, note = nullptr, title = nullptr, heading = nullptr;
    Fonts() {
        auto make = [](int h, int w) {
            return CreateFontW(-h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Segoe UI");
        };
        body = make(19, FW_NORMAL);
        note = make(16, FW_NORMAL);
        title = make(32, FW_SEMIBOLD);
        heading = make(16, FW_BOLD); // a step larger
    }
    ~Fonts() {
        DeleteObject(body);
        DeleteObject(note);
        DeleteObject(title);
        DeleteObject(heading);
    }
};

struct Layout {
    RECT nav{}, content{}, header{}, model{}, stats{}, parts{}, actions{};
    static constexpr int navWidth = 380, rowHeight = 30, navTop = 122,
                         line = 30; // larger text, larger rows // the search box sits above the list
    void compute(int w, int h, const Page& page) {
        nav = {0, 0, navWidth, h};
        content = {navWidth, 0, w, h};
        const int pad = 20, x0 = navWidth + pad, x1 = w - pad;
        const int headerH = page.description.empty() ? 58 : 84;
        header = {x0, pad, x1, pad + headerH};
        actions = {x0, h - 52, x1, h - 12};
        const int top = header.bottom + 8, bottom = actions.top - 12;
        if (page.kind == Page::table) {
            model = {0, 0, 0, 0};
            const int summaryH =
                page.stats.empty() ? 0 : 34 + Layout::line * int((std::min)(page.stats.size(), size_t(6)));
            stats = {x0, top, x1, top + summaryH};
            parts = {x0, stats.bottom + (summaryH ? 14 : 0), x1, bottom}; // the table
        } else {
            model = {0, 0, 0, 0};
            stats = {x0, top, x1, bottom};
            parts = {0, 0, 0, 0};
        }
    }
};

class PanelView {
public:
    Theme theme;
    Fonts fonts;
    Layout layout;
    std::vector<NavNode> roots;
    std::vector<NavNode*> visible; // flattened rows
    int scroll = 0, hoverRow = -1;
    std::wstring tip;
    POINT tipAt{};        // a tooltip, drawn last
    unsigned clipped = 0; // strings too long even when shrunk and wrapped (the render test wants 0)
    void setGuiScale(int s) {
        art();
        guiScale = s;
    }                      // offline checks
    ModSettings settings;  // as the mod has them (refreshed when the page opens)
    int settingsGroup = 0; // the Settings sub-page last opened (ModSettings::group)
    struct SettingControl {
        RECT r;
        int row, delta;
    }; // delta 0 = a switch
    std::vector<SettingControl> settingControls;
    int settingAt(POINT p, int& delta) const {
        for (const SettingControl& c : settingControls)
            if (PtInRect(&c.r, p)) {
                delta = c.delta;
                return c.row;
            }
        return -1;
    }
    NavNode* selected = nullptr;
    Page page;
    std::string message = "Loading the catalog...";
    void flatten() {
        visible.clear();
        for (NavNode& n : roots) addVisible(n);
    }
    int rowAt(int x, int y) const {
        if (x >= layout.nav.right || y < Layout::navTop) return -1;
        const int r = (y - Layout::navTop) / Layout::rowHeight + scroll;
        return r >= 0 && r < int(visible.size()) ? r : -1;
    }
    int visibleRows(int h) const { return (std::max)(1, (h - Layout::navTop) / Layout::rowHeight); }
    void scrollBy(int rows, int h) {
        scroll =
            (std::max)(0, (std::min)(scroll + rows, (std::max)(0, int(visible.size()) - visibleRows(h))));
    }

    // Paints everything except the model picture (drawn into layout.model by the caller).
    void paint(HDC dc, int w, int h) {
        layout.compute(w, h, page);
        surface(dc, {0, 0, w, h}, false);
        surface(dc, layout.nav, true);
        fill(dc, {layout.nav.right - 1, 0, layout.nav.right, h}, theme.line);
        SetBkMode(dc, TRANSPARENT);
        // Settings only: a plain title, no search and no service light
        text(dc, fonts.heading, theme.accent, {16, 14, layout.nav.right - 12, 30}, L"FIRST PERSON", DT_LEFT);
        const int rows = visibleRows(h);
        for (int i = 0; i < rows && scroll + i < int(visible.size()); ++i) {
            const NavNode* n = visible[size_t(scroll + i)];
            const int y = Layout::navTop + i * Layout::rowHeight;
            RECT r{0, y, layout.nav.right - 1, y + Layout::rowHeight};
            const bool band = n->depth == 0 || !n->kids.empty(); // each category its own band and border
            if (band) {
                fill(dc, r,
                     n->depth == 0   ? RGB(22, 24, 24)
                     : n->depth == 1 ? RGB(28, 31, 31)
                                     : RGB(34, 37, 37));
                fill(dc, {r.left, r.top, r.right, r.top + 1}, theme.line);
                fill(dc, {r.left, r.bottom - 1, r.right, r.bottom}, RGB(16, 17, 17));
                fill(dc, {r.left, r.top, r.left + (n->depth == 0 ? 4 : 2), r.bottom}, theme.accent);
            }
            if (n == selected)
                fill(dc, r, theme.select);
            else if (scroll + i == hoverRow)
                fill(dc, r, theme.hover);
            for (int d = 1; d < n->depth; ++d)
                fill(dc, {8 + d * 14, r.top, 9 + d * 14, r.bottom},
                     RGB(70, 78, 78)); // guide lines down each open category
            const int indent = 14 + n->depth * 14;
            if (!n->kids.empty())
                text(dc, fonts.note, theme.dim, {indent - 13, y, indent, y + Layout::rowHeight},
                     n->expanded ? L"\x25BE" : L"\x25B8", DT_LEFT);
            HFONT f = n->depth == 0 ? fonts.heading : fonts.body;
            COLORREF c =
                n->depth == 0 ? theme.text : (n->request.empty() && n->kids.empty() ? theme.dim : theme.text);
            text(dc, f, c, {indent, y, layout.nav.right - 52, y + Layout::rowHeight},
                 wide(n->depth == 0 ? upper(n->label) : n->label).c_str(), DT_LEFT | DT_END_ELLIPSIS);
            text(dc, fonts.note, theme.dim,
                 {layout.nav.right - 56, y, layout.nav.right - 12, y + Layout::rowHeight},
                 wide(n->right).c_str(), DT_RIGHT);
        }
        if (int(visible.size()) > rows) { // scroll bar
            const int track = h - Layout::navTop, thumb = (std::max)(24, track * rows / int(visible.size()));
            const int top =
                Layout::navTop + (track - thumb) * scroll / (std::max)(1, int(visible.size()) - rows);
            fill(dc, {layout.nav.right - 5, top, layout.nav.right - 2, top + thumb}, theme.line);
        }
        // content
        if (page.kind == Page::none) {
            text(dc, fonts.body, theme.dim, layout.header, wide(message).c_str(), DT_LEFT);
            return;
        }
        RECT t = layout.header;
        t.bottom = t.top + 34;
        text(dc, fonts.title, theme.text, {t.left, t.top, t.right - 180, t.bottom}, wide(page.title).c_str(),
             DT_LEFT | DT_END_ELLIPSIS);
        fill(dc, {layout.header.left, layout.header.bottom, layout.header.right, layout.header.bottom + 1},
             theme.line);
        if (page.settingsPage) settingsView(dc);
    }
    // a tooltip near the mouse, over everything (the caller paints it after paint).
    void paintTip(HDC dc, int w, int h) {
        if (tip.empty()) return;
        const int width = 380, lines = (std::max)(1, (art() ? gameFont.width(tip, fontSize(fonts.note))
                                                            : int(tip.size()) * 7) /
                                                             (width - 20) +
                                                         1);
        const int height =
            (std::min)(200, 12 + lines * (art() ? gameFont.lineHeight(fontSize(fonts.note)) : 16) + 4);
        int x = int(tipAt.x) + 16, y = int(tipAt.y) + 18;
        if (x + width > w) x = (std::max)(0, w - width - 4);
        if (y + height > h) y = (std::max)(0, int(tipAt.y) - height - 6);
        RECT r{x, y, x + width, y + height};
        fill(dc, r, RGB(36, 38, 36));
        frame(dc, r, theme.accent);
        text(dc, fonts.note, theme.text, {r.left + 10, r.top + 6, r.right - 10, r.bottom - 4}, tip.c_str(),
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

private:
    void addVisible(NavNode& n) {
        visible.push_back(&n);
        if (n.expanded)
            for (NavNode& k : n.kids) addVisible(k);
    }
    static std::string upper(std::string s) {
        for (char& c : s) c = char(toupper((unsigned char)c));
        return s;
    }
    static void fill(HDC dc, RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    static void frame(HDC dc, RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &r, b);
        DeleteObject(b);
    }
    static void gdiText(HDC dc, HFONT f, COLORREF c, RECT r, const wchar_t* s, UINT flags) {
        HGDIOBJ old = SelectObject(dc, f);
        const UINT how = flags | ((flags & DT_WORDBREAK) ? 0 : DT_SINGLELINE | DT_VCENTER) | DT_NOPREFIX;
        SetTextColor(dc, RGB(0, 0, 0));
        RECT sr = r;
        OffsetRect(&sr, 2, 2);
        DrawTextW(dc, s, -1, &sr, how); // the drop shadow here too
        SetTextColor(dc, c);
        DrawTextW(dc, s, -1, &r, how);
        SelectObject(dc, old);
    }
    // the game's own UI art, read from the game's Resource.rpk once (works without the service).
    uiart::GameFont gameFont;
    uiart::Bitmap stone, stoneDark, logo;
    bool artTried = false, artOk = false;
    bool art() {
        if (artTried) return artOk;
        artTried = true;
        if (!gameFont.load()) return false;
        uiart::Image s;
        if (uiart::loadTexture("guitex.rfi", s)) {
            stone.make(s, RGB(78, 80, 78));
            stoneDark.make(s, RGB(60, 62, 60));
        } // darker, calmer stone
        guiScale = gameGuiScale();
        artOk = true;
        return true;
    }
    // The GDI font sizes the layout was made for, as pixel sizes for the game font (GUISize scales them).
    int fontSize(HFONT f) const {
        const int base = f == fonts.title ? 32 : f == fonts.body ? 19 : f == fonts.heading ? 17 : 16;
        return (base * guiScale + 50) / 100;
    }                   // +25 %
    int guiScale = 100; // percent; from Exanima.ini "GUISize" when the game has one
    // The game registers GUISize (0x9C820, default 2) and saves settings to %APPDATA%\Exanima\Exanima.ini
    // as "Key = value" lines; with no GUISize line the game's default is in use.
    // Provisional: the size steps are not read from the game's code (1 = 75 %, 2 = 100 %, 3 = 125 %).
    static int gameGuiScale() {
        wchar_t app[MAX_PATH];
        if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return 100;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (std::wstring(app) + L"\\Exanima\\Exanima.ini").c_str(), L"rb") || !f) return 100;
        char line[256];
        int scale = 100;
        while (fgets(line, sizeof line, f)) {
            if (_strnicmp(line, "GUISize", 7)) continue;
            const char* v = line + 7;
            while (*v == ' ' || *v == '=' || *v == ':' || *v == '\t') ++v;
            const int n = atoi(v);
            if (n >= 1 && n <= 4) scale = 50 + 25 * n;
            break;
        }
        fclose(f);
        return scale;
    }
    void surface(HDC dc, RECT r, bool darker) {
        const uiart::Bitmap& b = darker ? stoneDark : stone;
        if (!art() || !b.dc) {
            fill(dc, r, darker ? theme.pane : theme.bg);
            return;
        }
        for (int y = r.top - (r.top % b.h); y < r.bottom; y += b.h)
            for (int x = r.left - (r.left % b.w); x < r.right; x += b.w) {
                const int x0 = (std::max)(x, int(r.left)), y0 = (std::max)(y, int(r.top)),
                          x1 = (std::min)(x + b.w, int(r.right)), y1 = (std::min)(y + b.h, int(r.bottom));
                if (x1 > x0 && y1 > y0) BitBlt(dc, x0, y0, x1 - x0, y1 - y0, b.dc, x0 - x, y0 - y, SRCCOPY);
            }
    }
    // nothing is ever cut with dots. A line that does not fit
    // is drawn smaller (down to 72 %), then wrapped onto a second line when the box has room; only a box too
    // small for even that clips, and clipped counts it (the render test fails on any).
    static std::wstring noDots(std::wstring s) {
        while (s.size() >= 3 && s.compare(s.size() - 3, 3, L"...") == 0) s.resize(s.size() - 3);
        if (!s.empty() && s.back() == L'\x2026') s.pop_back();
        return s;
    }
    std::vector<std::wstring> wrapLines(const std::wstring& str, int size, int width) const {
        std::vector<std::wstring> lines;
        std::wstring lineNow, word;
        auto flush = [&] {
            lines.push_back(lineNow);
            lineNow.clear();
        };
        for (size_t i = 0; i <= str.size(); ++i) {
            const wchar_t ch = i < str.size() ? str[i] : L' ';
            if (ch == L'\n') {
                if (!word.empty()) {
                    lineNow += (lineNow.empty() ? L"" : L" ") + word;
                    word.clear();
                }
                flush();
                continue;
            }
            if (ch != L' ') {
                word += ch;
                continue;
            }
            if (word.empty()) continue;
            const std::wstring tryLine = lineNow.empty() ? word : lineNow + L" " + word;
            if (gameFont.width(tryLine, size) <= width || lineNow.empty())
                lineNow = tryLine;
            else {
                flush();
                lineNow = word;
            }
            word.clear();
        }
        if (!lineNow.empty()) flush();
        return lines;
    }
    void text(HDC dc, HFONT f, COLORREF c, RECT r, const wchar_t* s, UINT flags) {
        const std::wstring str = noDots(s ? s : L"");
        if (!art() || !gameFont.drawable(str)) {
            gdiText(dc, f, c, r, str.c_str(), flags & ~UINT(DT_END_ELLIPSIS));
            return;
        }
        const int full = fontSize(f), smallest = (std::max)(9, full * 72 / 100), width = r.right - r.left,
                  height = r.bottom - r.top;
        if (width <= 0 || str.empty()) return;
        int size = full;
        std::vector<std::wstring> lines;
        bool wrap = (flags & DT_WORDBREAK) != 0;
        for (;; --size) {
            const int lh = gameFont.lineHeight(size);
            if (wrap) {
                lines = wrapLines(str, size, width);
                bool fits = int(lines.size()) * lh <= height + 2;
                for (const auto& l : lines)
                    if (gameFont.width(l, size) > width) fits = false;
                if (fits) break;
            } else {
                lines = {str};
                if (gameFont.width(str, size) <= width) break;
            }
            if (size <= smallest) {
                if (!wrap) {
                    lines = wrapLines(str, size, width);
                    bool fits = int(lines.size()) * gameFont.lineHeight(size) <= height + 4;
                    for (const auto& l : lines)
                        if (gameFont.width(l, size) > width) fits = false;
                    if (fits && lines.size() > 1) {
                        wrap = true;
                        break;
                    }
                    lines = {str};
                }
                ++clipped;
                break;
            }
        }
        const int lh = gameFont.lineHeight(size);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, r.left - 2, r.top - 2, r.right + 2,
                          r.bottom + 5); // descenders and _ may reach just below the box
        int y = wrap && !(flags & DT_WORDBREAK) ? int(r.top) + (height - int(lines.size()) * lh) / 2
                : (flags & DT_WORDBREAK)        ? int(r.top)
                                                : int(r.top) + (height - lh) / 2;
        for (const std::wstring& l : lines) {
            const int lw = gameFont.width(l, size);
            const int x = (flags & DT_CENTER)  ? int(r.left) + (width - lw) / 2
                          : (flags & DT_RIGHT) ? int(r.right) - lw
                                               : int(r.left);
            for (const POINT o : {POINT{-1, 0}, POINT{1, 0}, POINT{0, -1}, POINT{0, 1}, POINT{2, 2}})
                gameFont.line(dc, x + o.x, y + o.y, l, size, RGB(0, 0, 0)); // outline + drop shadow
            gameFont.line(dc, x, y, l, size, c);
            y += lh;
        }
        RestoreDC(dc, saved);
    }
    void settingsView(HDC dc) {
        settingControls.clear();
        RECT r = layout.stats;
        const ModSettings::Group& g = ModSettings::group(settingsGroup);
        std::string heading = g.name;
        for (char& c : heading) c = char(toupper((unsigned char)c));
        text(dc, fonts.heading, theme.accent, {r.left, r.top, r.right, r.top + 20},
             wide(heading + "  (applied at once, saved by the mod)").c_str(), DT_LEFT);
        int y = r.top + 34;
        const int pitch = 38, ch = pitch - 8;
        for (int i = 0; i < g.count; ++i, y += pitch) {
            const int row = g.rows[i];
            const int x0 = r.left, x1 = r.right;
            text(dc, fonts.body, theme.text, {x0, y, x0 + 260, y + ch}, ModSettings::label(row), DT_LEFT);
            text(dc, fonts.body, theme.accent, {x0 + 270, y, x0 + 470, y + ch}, settings.value(row).c_str(),
                 DT_LEFT);
            auto button = [&](RECT b, const wchar_t* t, int delta) {
                fill(dc, b, theme.button);
                frame(dc, b, theme.line);
                text(dc, fonts.body, theme.text, b, t, DT_CENTER);
                settingControls.push_back({b, row, delta});
            };
            if (ModSettings::isNumber(row)) {
                button({x0 + 480, y, x0 + 520, y + ch}, L"\x2212", -1);
                button({x0 + 528, y, x0 + 568, y + ch}, L"+", 1);
            } else
                button({x0 + 480, y, x0 + 568, y + ch}, L"Switch", 0);
            fill(dc, {x0, y + ch + 4, x1, y + ch + 5}, RGB(44, 47, 51));
        }
        if (!page.status.empty())
            text(dc, fonts.note, theme.dim, {r.left, y + 8, r.right, y + 60}, wide(page.status).c_str(),
                 DT_LEFT | DT_WORDBREAK);
    }
    // What a settings row's tooltip says (the rows whose effect is not plain from the label).
    static const wchar_t* settingTip(int row) {
        switch (row) {
        case 22:
        case 23:
        case 24:
            return L"How much head wobble on this axis is held off. At 100% the head is held almost still "
                   L"while you walk. Mouse turns are never slowed.";
        case 27:
            return L"How much of the head's up-and-down bob while walking is held off.";
        case 31:
            return L"How close to your eye the view starts. Lower lets you lean into walls without them "
                   L"cutting into view; far-away surfaces may then shimmer slightly where they meet. First "
                   L"and third person.";
        case 30:
            return L"Out of combat, in first and third person, the game's duck key (X unless you changed it) "
                   L"crouches, also while you walk. Combat, a knockdown or talking ends it; you stay down "
                   L"while something is just above your head.";
        case 29:
            return L"In first and third person, combat included, your spine bends forward when you look down "
                   L"and back when you look up, so swings go where you look. Your view still points where "
                   L"you aim.";
        case 28:
            return L"While the inventory, an item's description, a container, a body's loot or the arena's "
                   L"end-of-match menu is open, the cursor shows and look and movement hold still.";
        case 2:
            return L"How steadily the neck holds the head, and (out of combat) how much the chest and neck "
                   L"sway is calmed. The view is always exactly what your head sees.";
        case 3:
            return L"Reads the mouse directly, without Windows pointer acceleration.";
        case 25:
            return L"Shows the crosshair where you aim while the head is still turning to it.";
        case 26:
            return L"Eases a fast flick so the head can catch up and you can see where it will land.";
        case 13:
            return L"How softly the third-person camera follows the character.";
        case 14:
            return L"Hides what stands between the camera and you; walls still pull the camera in.";
        }
        return nullptr;
    }

public:
    const wchar_t* settingTipAt(POINT p) const {
        if (!page.settingsPage) return nullptr;
        const RECT& r = layout.stats;
        if (p.x < r.left || p.x > r.left + 470 || p.y < r.top + 34) return nullptr;
        const ModSettings::Group& g = ModSettings::group(settingsGroup);
        const int i = (p.y - r.top - 34) / 38;
        return i >= 0 && i < g.count ? settingTip(g.rows[i]) : nullptr;
    }
};

} // namespace panel
