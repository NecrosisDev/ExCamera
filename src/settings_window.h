#pragma once
// F8's Settings window: the first-person settings and nothing else (no network, no other page),
// drawn through PanelView (panel_ui.h).
#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <functional>
#include <thread>
#include "panel_ui.h"

namespace panel {

struct SettingsChange {
    ModSettings settings;
    bool changed = false;
    bool rawUnavailable = false;
    bool saved = true;
};

class SettingsWindow {
public:
    ~SettingsWindow() { stop(); }
    // Starts the UI thread (once). owner: the game window.
    void start(HWND owner) {
        if (running.exchange(true)) return;
        ownerWindow = owner;
        try {
            thread = std::thread([this] {
                try {
                    run();
                } catch (...) {
                    if (HWND w = window.exchange(nullptr)) DestroyWindow(w);
                    if (menuCursor) DestroyCursor(menuCursor);
                    menuCursor = nullptr;
                    done = true;
                }
            });
        } catch (...) {
            running = false;
            done = true;
            throw;
        }
    }
    void show(bool on) {
        if (HWND w = window.load()) PostMessageW(w, WM_APP + 2, on ? 1 : 0, 0);
    }
    // The mod calls heartbeat every tick while it runs; without it for a second the window hides itself
    // (F7 stop, a stalled mod), so no window floats over a game the mod no longer drives.
    void heartbeat() { lastBeat = GetTickCount64(); }
    bool finished() const { return done.load(); }
    void join() {
        if (thread.joinable()) thread.join();
    }
    bool visible() const {
        HWND w = window.load();
        return w && IsWindowVisible(w);
    }
    void stop() {
        if (!running.exchange(false)) return;
        for (int i = 0; i < 200 && !window.load() && !done.load(); ++i)
            Sleep(10); // a window still being made: wait for it (or its failure)
        if (HWND w = window.load()) PostMessageW(w, WM_CLOSE, 0, 0);
        if (thread.joinable()) thread.join();
    }
    // Send the action to the settings owner. A snapshot read here could already
    // be stale when the callback runs (FOV keys stay live while this window opens).
    bool applyControl(int row, int delta) {
        if (!changeSetting) return false;
        const SettingsChange got = changeSetting(row, delta, view.settingsGroup);
        view.settings = got.settings;
        if (got.changed)
            view.page.status = got.rawUnavailable
                                   ? "Raw mouse input is not available here (another program "
                                     "holds it); the cursor is used."
                                   : (got.saved ? "Saved." : "Applied. Save failed; retrying automatically.");
        return got.changed;
    }
    // Set before start: snapshots for display; row/delta/group actions for edits.
    std::function<ModSettings()> getSettings;
    std::function<SettingsChange(int, int, int)> changeSetting;
    std::function<void()> onClosed; // its close box: the cursor goes back to the game
private:
    std::atomic<bool> running{false}, done{false};
    std::atomic<HWND> window{nullptr};
    HWND ownerWindow = nullptr;
    std::atomic<ULONGLONG> lastBeat{0};
    std::thread thread;
    PanelView view;
    HCURSOR menuCursor = nullptr;
    // The game hides the system cursor and draws its own under this window: over it the arrow is asked for
    // again, and if the cursor is still hidden the window draws the arrow itself, last.
    POINT pointerAt{-100, -100};
    bool pointerOwn = false;
    static bool systemCursorShown() {
        CURSORINFO ci{};
        ci.cbSize = sizeof ci;
        return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
    }
    void movePointer(HWND w, POINT p) {
        const bool own = !systemCursorShown();
        if (pointerOwn) {
            RECT o{pointerAt.x - 2, pointerAt.y - 2, pointerAt.x + 40, pointerAt.y + 40};
            InvalidateRect(w, &o, FALSE);
        }
        pointerOwn = own;
        pointerAt = p;
        if (own) {
            RECT n{p.x - 2, p.y - 2, p.x + 40, p.y + 40};
            InvalidateRect(w, &n, FALSE);
        }
    }
    void drawPointer(HDC dc) const {
        if (pointerOwn)
            DrawIconEx(dc, pointerAt.x, pointerAt.y, menuCursor ? menuCursor : LoadCursor(nullptr, IDC_ARROW),
                       0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
    }
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (m == WM_NCCREATE) {
            self = (SettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)self);
        }
        return self ? self->handle(w, m, wp, lp) : DefWindowProcW(w, m, wp, lp);
    }
    void openGroup(int g) {
        g = std::clamp(g, 0, ModSettings::groupCount - 1);
        if (g != view.settingsGroup) view.settingsScroll = 0;
        view.settingsGroup = g;
        view.settingControls.clear(); // Never act on the previous page before repaint.
        view.tip.clear();
        view.page = Page();
        view.page.kind = Page::table;
        view.page.settingsPage = true;
        view.page.title = ModSettings::group(view.settingsGroup).name;
        view.page.description = ModSettings::groupDescription(view.settingsGroup);
        if (getSettings) view.settings = getSettings();
        for (NavNode* n : view.visible)
            if (n->request == "settings:" + std::to_string(view.settingsGroup)) view.selected = n;
    }
    void run() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        menuCursor = uiart::gameCursor(GetSystemMetrics(SM_CYCURSOR));
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"ExCameraSettings";
        wc.hCursor =
            LoadCursor(nullptr, IDC_ARROW); // Shared class cursor; menuCursor belongs to this window.
        RegisterClassW(&wc);
        HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"ExCamera",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 680, ownerWindow,
                                 nullptr, inst, this);
        if (!w) {
            if (menuCursor) DestroyCursor(menuCursor);
            menuCursor = nullptr;
            done = true;
            return;
        }
        window = w;
        darkCaption(w);
        NavNode s;
        s.label = "Settings";
        s.depth = 0;
        s.expanded = true;
        for (int g = 0; g < ModSettings::groupCount; ++g) {
            NavNode n;
            n.depth = 1;
            n.label = ModSettings::group(g).name;
            n.request = "settings:" + std::to_string(g);
            s.kids.push_back(n);
        }
        view.roots = {s};
        view.flatten();
        openGroup(0);
        SetTimer(w, 1, 250, nullptr);
        if (!running.load()) PostMessageW(w, WM_CLOSE, 0, 0); // Stop may have raced slow window creation.
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        window = nullptr;
        if (menuCursor) DestroyCursor(menuCursor);
        menuCursor = nullptr;
        done = true;
    }
    void click(HWND w, POINT p) {
        if (p.x >= view.layout.nav.right) {
            int delta = 0;
            const int row = view.settingAt(p, delta);
            if (row >= 0 && changeSetting) {
                playMenuSound(true);
                applyControl(row, delta);
            }
            InvalidateRect(w, nullptr, FALSE);
            return;
        }
        const int row = view.rowAt(p.x, p.y);
        if (row < 0) return;
        playMenuSound(false);
        NavNode* n = view.visible[size_t(row)];
        if (!n->kids.empty()) n->expanded = !n->expanded;
        if (n->request.rfind("settings:", 0) == 0) openGroup(atoi(n->request.c_str() + 9));
        view.flatten();
        InvalidateRect(w, nullptr, FALSE);
    }
    LRESULT handle(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        try {
            return handleInner(w, m, wp, lp);
        } catch (...) {
            return DefWindowProcW(w, m, wp, lp);
        }
    }
    LRESULT handleInner(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // the keyboard stays the game's
        case WM_TIMER:
            if (IsWindowVisible(w) && GetTickCount64() - lastBeat.load() > 1000)
                ShowWindow(w, SW_HIDE);
            else if (IsWindowVisible(w) && getSettings) {
                const ModSettings now = getSettings();
                if (!(now == view.settings) ||
                    now.manualDefenseAvailable != view.settings.manualDefenseAvailable ||
                    now.rawMouseFallback != view.settings.rawMouseFallback ||
                    now.savePending != view.settings.savePending) {
                    if (view.settings.savePending && !now.savePending) view.page.status = "Saved.";
                    view.settings = now;
                    InvalidateRect(w, nullptr, FALSE);
                }
            }
            return 0;
        case WM_APP + 2:
            ShowWindow(w, wp ? SW_SHOWNOACTIVATE
                             : SW_HIDE); // Restore a minimized settings window, without taking keys.
            if (wp) openGroup(view.settingsGroup);
            InvalidateRect(w, nullptr, FALSE);
            return 0; // fresh values
        case WM_CLOSE:
            if (running.load()) {
                ShowWindow(w, SW_HIDE);
                if (onClosed) onClosed();
                return 0;
            }
            DestroyWindow(w);
            return 0; // closing hides; stop destroys
        case WM_DESTROY:
            KillTimer(w, 1);
            PostQuitMessage(0);
            return 0; // also when the owner (game window) is rebuilt: the mod makes a new one
        case WM_SIZE:
            view.settingControls.clear(); // Hitboxes are rebuilt for the new client size in WM_PAINT.
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize.x = 700;
            limits->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            RECT c;
            GetClientRect(w, &c);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            view.paint(mem, c.right, c.bottom);
            view.paintTip(mem, c.right, c.bottom);
            drawPointer(mem);
            BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { // the game's own menu cursor
                const int count = ShowCursor(TRUE);
                if (count > 0) ShowCursor(FALSE);
                SetCursor(menuCursor ? menuCursor : LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            movePointer(w, p);
            const int row = view.rowAt(p.x, p.y);
            if (row != view.hoverRow) {
                view.hoverRow = row;
                RECT n = view.layout.nav;
                InvalidateRect(w, &n, FALSE);
            }
            const wchar_t* tip = view.settingTipAt(p);
            const std::wstring t = tip ? tip : L"";
            if (t != view.tip ||
                (!t.empty() && (std::abs(p.x - view.tipAt.x) > 40 || std::abs(p.y - view.tipAt.y) > 20))) {
                view.tip = t;
                view.tipAt = p;
                InvalidateRect(w, nullptr, FALSE);
            }
            TRACKMOUSEEVENT e{sizeof(e), TME_LEAVE, w, 0};
            TrackMouseEvent(&e);
            return 0;
        }
        case WM_MOUSELEAVE:
            view.hoverRow = -1;
            view.tip.clear();
            pointerOwn = false;
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            click(w, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &p);
            RECT c;
            GetClientRect(w, &c);
            if (p.x < view.layout.nav.right) {
                view.scrollBy(-3 * (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA), c.bottom);
            } else
                view.scrollSettings(-3 * (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA));
            view.settingControls.clear();
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(w, m, wp, lp);
    }
};

} // namespace panel
