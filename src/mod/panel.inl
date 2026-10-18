// The F8 window: the Settings window and what its rows change.
// Part of first_person.cpp (one translation unit): included there, in order.
// ---- F8's Settings window (settings_window.h), made the first time F8 is pressed ----
static std::atomic<panel::SettingsWindow*> f8Window{nullptr};
static ULONGLONG f8CreatedAt = 0;
static void f8WindowTick(bool show, bool) {
    panel::SettingsWindow* p = f8Window.load();
    if (p && p->finished()) {
        p->join();
        f8Window = nullptr;
        delete p;
        p = nullptr;
    } // its owner window was rebuilt: a new one is made
    if (!p) {
        if (!show) return;
        HWND w = gameWindow();
        if (!w || GetTickCount64() - f8CreatedAt < 5000) return;
        f8CreatedAt = GetTickCount64();
        try {
            p = new panel::SettingsWindow();
            p->getSettings = currentSettings;
            p->changeSetting = applySetting;
            p->onClosed = [] {
                InterlockedExchange(&cursorFree, 0);
            }; // its close box: the cursor goes back to the game
            p->start(w);
        } catch (...) {
            delete p;
            InterlockedExchange(&cursorFree, 0);
            showNotice(L"Settings could not open. Try F8 again.");
            return;
        }
        f8Window = p;
    }
    p->heartbeat();
    if (show != p->visible()) p->show(show);
}
