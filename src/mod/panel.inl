// Settings form changes run only at the existing GUI maintenance boundary.
#include "../native_settings.h"
// Cross WndProc/GUI-thread key-capture mailbox. -2=no result, -1=cancel,
// 0=unbind, positive=virtual-key code. All UI mutations remain on GUI thread.
static std::atomic<int> nativeBindingArmed{0}, nativeBindingResult{-2};
// WndProc owns each consumed press until its matching release, including repeats.
static std::array<std::atomic<bool>, 256> nativeBindingOwned{};
static void clearNativeBindingPresses() {
    for (auto& owned : nativeBindingOwned) owned = false;
}
static bool nativeSettingsBindingInput(UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || (message == WM_ACTIVATEAPP && !wp)) {
        clearNativeBindingPresses();
        if (nativeBindingArmed.exchange(0)) nativeBindingResult = -1;
        return false; // Native focus cleanup must run too.
    }
    const int key=bindingMessageKey(message,wp);
    if (key <= 0 || key >= static_cast<int>(nativeBindingOwned.size())) return false;
    const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN ||
                    message==WM_LBUTTONDOWN || message==WM_LBUTTONDBLCLK ||
                    message==WM_RBUTTONDOWN || message==WM_RBUTTONDBLCLK ||
                    message==WM_MBUTTONDOWN || message==WM_MBUTTONDBLCLK ||
                    message==WM_XBUTTONDOWN || message==WM_XBUTTONDBLCLK;
    const bool up=message==WM_KEYUP || message==WM_SYSKEYUP ||
                  message==WM_LBUTTONUP || message==WM_RBUTTONUP ||
                  message==WM_MBUTTONUP || message==WM_XBUTTONUP;
    const bool repeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (lp & (1LL << 30));
    if (up && nativeBindingOwned[static_cast<size_t>(key)].exchange(false)) return true;
    if (down && repeat && nativeBindingOwned[static_cast<size_t>(key)].load()) return true;
    // A fresh press retires a release lost while another window had focus.
    if (down && !repeat) nativeBindingOwned[static_cast<size_t>(key)] = false;
    if (!nativeBindingArmed.load() || !down) return false;
    // A key already held before capture is not a new binding. Leave its release
    // with the native owner that received its original down.
    if (repeat) return false;
    nativeBindingOwned[static_cast<size_t>(key)] = true;
    if ((key>=VK_F6 && key<=VK_F11)) return true;
    const int result=key==VK_ESCAPE ? -1 : key==VK_DELETE ? 0 : key;
    nativeBindingResult=result;
    nativeBindingArmed=0;
    return true;
}
static panel::NativeSettingsIntegration nativeSettings;
static void writeNativeCheckboxStatus(fp::JsonOut& j) {
    const auto d=nativeSettings.checkboxDiagnostics();
    // These count dispatched releases and admitted callbacks, not visual toggles.
    j.object("native_checkboxes")
        .unum("corrected_releases",d.correctedReleases)
        .unum("zero_scroll_releases",d.zeroScrollReleases)
        .unum("rejected_coordinates",d.rejectedCoordinates)
        .unum("action_callbacks",d.nativeActions)
        .end();
}
static std::atomic<bool> nativeSettingsWanted{false}, nativeSettingsRetired{true},
    nativeSettingsFaulted{false};
static bool nativeSettingsConfigured = false;
static thread_local bool inNativeSettingsMaintenance = false;
static volatile LONG nativeSettingsInFlight = 0, nativeSettingsThread = 0;
static uintptr_t nativeOptionsDesktop = 0;
static int nativeOptionsContains(uintptr_t form) {
    const uintptr_t parent = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiDesktop));
    if (parent != nativeOptionsDesktop || !readable(parent, field::guiTask::forms + sizeof(uintptr_t))) return -1;
    const int n = *reinterpret_cast<int*>(parent + field::guiTask::formCount);
    const uintptr_t list = *reinterpret_cast<uintptr_t*>(parent + field::guiTask::forms);
    if (n < 0 || n > 4096 || (n && !readable(list, static_cast<size_t>(n) * sizeof(uintptr_t)))) return -1;
    for (int i = 0; i < n; ++i) if (reinterpret_cast<uintptr_t*>(list)[i] == form) return 1;
    return 0;
}
static panel::NativeSettingsIntegration::Table nativeSettingsTable(uintptr_t form) {
    namespace F = field::nativeForm;
    if (!readable(form, F::elements + sizeof(uintptr_t))) return {};
    const int last = *reinterpret_cast<int*>(form + F::elementLast);
    if (last < 0 || last >= 96) return {};
    auto* e = *reinterpret_cast<panel::NativeElement**>(form + F::elements);
    if (!readable(reinterpret_cast<uintptr_t>(e), static_cast<size_t>(last + 1) * sizeof(*e))) return {};
    return {e, last + 1};
}
static void replaceNativeSettingsElements(uintptr_t form, const panel::NativeElement* source, int n) {
    using Copy = void (*)(const panel::NativeElement*, int, panel::NativeElement**, int*);
    using Free = void (*)(panel::NativeElement**, int);
    using Set = void (*)(uintptr_t, const panel::NativeElement*, int, unsigned char);
    panel::NativeElement* retained = nullptr;
    int last = -1;
    // Retain managed captions BEFORE Set releases the old table.
    ((Copy)gameAt(fp::sig::guiElementsCopy))(source, n - 1, &retained, &last);
    __try { ((Set)gameAt(fp::sig::guiFormElements))(form, retained, last, 0); }
    __finally { ((Free)gameAt(fp::sig::guiElementsFree))(&retained, last); }
    ((void (*)(uintptr_t))gameAt(fp::sig::guiFormInvalidate))(form);
}
static void configureNativeSettings() {
    using Table = panel::NativeSettingsIntegration::Table;
    nativeSettings.current = [] {
        nativeOptionsDesktop = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiDesktop));
        return *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::guiOptionsForm));
    };
    nativeSettings.contains = nativeOptionsContains;
    nativeSettings.table = nativeSettingsTable;
    nativeSettings.rootBasis = []() -> Table {
        auto* e = reinterpret_cast<panel::NativeElement*>(gameAt(fp::sig::guiOptionsElements));
        if (!readable(reinterpret_cast<uintptr_t>(e), 9 * sizeof(*e))) return {};
        if (uintptr_t(e[0].action) != gameAt(fp::sig::guiSettingsTab) ||
            uintptr_t(e[1].action) != gameAt(fp::sig::guiControlsTab) ||
            uintptr_t(e[6].action) != gameAt(fp::sig::guiOptionsDefaults) ||
            uintptr_t(e[7].action) != gameAt(fp::sig::guiOptionsApply) ||
            uintptr_t(e[8].action) != gameAt(fp::sig::guiOptionsCancel)) return {};
        return {e, 9};
    };
    nativeSettings.controlsBasis = []() -> Table {
        auto* e = reinterpret_cast<panel::NativeElement*>(gameAt(fp::sig::guiControlsElements));
        return readable(reinterpret_cast<uintptr_t>(e), 44 * sizeof(*e)) ? Table{e, 44} : Table{};
    };
    nativeSettings.controllerBasis = []() -> Table {
        auto* e = reinterpret_cast<panel::NativeElement*>(gameAt(fp::sig::guiControllerElements));
        return readable(reinterpret_cast<uintptr_t>(e), 5 * sizeof(*e)) ? Table{e, 5} : Table{};
    };
    nativeSettings.replace = replaceNativeSettingsElements;
    if (layout.optionalGroupAvailable("settingsRedraw"))
        nativeSettings.requestRedraw = [] {
            ((void (*)())gameAt(fp::sig::guiRequestRedraw))();
        };
    nativeSettings.checkboxHandler = reinterpret_cast<panel::NativeSettingsIntegration::NativeHandler>(gameAt(fp::sig::guiCheckboxHandler));
    nativeSettings.scrollGeometry = [](uintptr_t pane, panel::NativeScrollGeometry& g) {
        namespace F=field::nativeForm;
        if (!readable(pane,F::verticalScroll+sizeof(float))) return false;
        g.left=*reinterpret_cast<int32_t*>(pane+F::contentLeft); g.top=*reinterpret_cast<int32_t*>(pane+F::contentTop);
        g.right=*reinterpret_cast<int32_t*>(pane+F::contentRight); g.bottom=*reinterpret_cast<int32_t*>(pane+F::contentBottom);
        g.width=*reinterpret_cast<int32_t*>(pane+F::contentWidth); g.height=*reinterpret_cast<int32_t*>(pane+F::scrollExtent);
        g.horizontal=*reinterpret_cast<float*>(pane+F::horizontalScroll); g.vertical=*reinterpret_cast<float*>(pane+F::verticalScroll);
        return true;
    };
    nativeSettings.panes = [](uintptr_t root) -> panel::NativeSettingsIntegration::Panes {
        if (!readable(root, field::optionsForm::controllerPane + sizeof(uintptr_t))) return {};
        return {*reinterpret_cast<uintptr_t*>(root + field::optionsForm::graphicsPane),
                *reinterpret_cast<uintptr_t*>(root + field::optionsForm::controlsPane),
                *reinterpret_cast<uintptr_t*>(root + field::optionsForm::controllerPane)};
    };
    nativeSettings.visible = [](uintptr_t pane, bool show) {
        ((void (*)(uintptr_t, unsigned char))gameAt(fp::sig::guiFormShow))(pane, show ? 1 : 0);
    };
    nativeSettings.scrolling = [](uintptr_t pane) {
        return (*reinterpret_cast<unsigned*>(pane+field::nativeForm::frameFlags)&0x200)!=0;
    };
    nativeSettings.setScrolling = [](uintptr_t pane,bool enabled) {
        auto& flags=*reinterpret_cast<unsigned*>(pane+field::nativeForm::frameFlags);
        const unsigned updated=(flags&~0x200u)|(enabled ? 0x200u : 0u);
        if (updated!=flags) {
            flags=updated;
            ((void (*)(uintptr_t))gameAt(fp::sig::guiFormGeometry))(pane);
        }
    };
    nativeSettings.contentHeight = [](uintptr_t pane) {
        return *reinterpret_cast<int*>(pane + field::nativeForm::scrollExtent);
    };
    nativeSettings.setContentHeight = [](uintptr_t pane, int height) {
        *reinterpret_cast<int*>(pane + field::nativeForm::scrollExtent) = height;
        ((void (*)(uintptr_t))gameAt(fp::sig::guiFormGeometry))(pane);
    };
    nativeSettings.frame = [](uintptr_t form, panel::NativeSettingsIntegration::Frame& out) {
        namespace F = field::nativeForm;
        if (!readable(form, F::height + sizeof(int))) return false;
        out = {*reinterpret_cast<int*>(form + F::x), *reinterpret_cast<int*>(form + F::y),
               *reinterpret_cast<int*>(form + F::width), *reinterpret_cast<int*>(form + F::height)};
        return true;
    };
    nativeSettings.setFrame = [](uintptr_t form, const panel::NativeSettingsIntegration::Frame& in) {
        namespace F = field::nativeForm;
        *reinterpret_cast<int*>(form + F::x) = in.x; *reinterpret_cast<int*>(form + F::y) = in.y;
        *reinterpret_cast<int*>(form + F::width) = in.width; *reinterpret_cast<int*>(form + F::height) = in.height;
        ((void (*)(uintptr_t))gameAt(fp::sig::guiFormGeometry))(form); // content rect and scrollbars follow the frame
    };
    nativeSettings.scale = [] {
        const int value = *reinterpret_cast<int*>(gameAt(fp::sig::guiUiScale));
        if (value < 1 || value > 32) throw "Native UI scale outside admitted range";
        return value;
    };
    nativeSettings.mouseHeld = [] { return (GetAsyncKeyState(VK_LBUTTON)&0x8000) != 0; };

    nativeSettings.openNative = [] {
        if (InterlockedCompareExchange(&stopping, 0, 0)) return;
        releaseInput();
        InterlockedExchange(&cursorFree, 1);
        ((void (*)())gameAt(fp::sig::guiOptionsOpen))();
    };
    nativeSettings.closeNative = [](uintptr_t form) {
        ((uintptr_t (*)(uintptr_t, uintptr_t))gameAt(fp::sig::guiFormClose))(form, 0);
    };
    nativeSettings.accepted = [](uintptr_t form) {
        if (!readable(form,field::optionsForm::applied+sizeof(int)))
            throw "Native Settings Apply owner is unreadable";
        return *reinterpret_cast<int*>(form + field::optionsForm::applied) == 1;
    };
#ifdef FP_OPENXR_EMULATION
    nativeSettings.vrTab = true;
#else
    nativeSettings.vrTab = false; // no headset code in this build: the rows would do nothing
#endif
    nativeSettings.settings = currentSettings;
    nativeSettings.apply = commitSettingsDraft;
    nativeSettings.recenter = [] { ++trackingReset; };
    nativeSettings.modsByMod = settingsPath[0] && GetPrivateProfileIntW(L"Settings", L"ModSettingsByMod", 0, settingsPath) != 0;
    nativeSettings.saveModsByMod = [](bool byMod) {
        if (settingsPath[0]) WritePrivateProfileStringW(L"Settings", L"ModSettingsByMod", byMod ? L"1" : L"0", settingsPath);
    };
    // The Exanima Lua runtime's Settings tabs (its "Mod Settings" tab): this form already owns GUI maintenance,
    // so it hosts them instead of the runtime hooking the same step. Version 1 and every export must be present.
    if (HMODULE runtime = GetModuleHandleW(L"ScriptRuntime.dll")) {
        const auto version = reinterpret_cast<uint32_t (*)()>(GetProcAddress(runtime, "settings_tabs_version"));
        const auto claim = reinterpret_cast<bool (*)()>(GetProcAddress(runtime, "settings_tabs_claim_host"));
        panel::NativeSettingsIntegration::RuntimeTabs tabs;
        tabs.open = reinterpret_cast<decltype(tabs.open)>(GetProcAddress(runtime, "settings_tab_host_open"));
        tabs.title = reinterpret_cast<decltype(tabs.title)>(GetProcAddress(runtime, "settings_tab_host_title"));
        tabs.rows = reinterpret_cast<decltype(tabs.rows)>(GetProcAddress(runtime, "settings_tab_host_rows"));
        tabs.change = reinterpret_cast<decltype(tabs.change)>(GetProcAddress(runtime, "settings_tab_host_change"));
        tabs.event = reinterpret_cast<decltype(tabs.event)>(GetProcAddress(runtime, "settings_tab_host_event"));
        tabs.help = reinterpret_cast<decltype(tabs.help)>(GetProcAddress(runtime, "mod_settings_help"));
        tabs.category = reinterpret_cast<decltype(tabs.category)>(GetProcAddress(runtime, "mod_settings_category"));
        if (version && claim && tabs.ready() && version() == 1 && claim()) nativeSettings.runtimeTabs = tabs;
    }
    nativeSettings.onClosed = [] {
        nativeBindingArmed=0;
        nativeBindingResult=-2;
        nativeSettingsWanted = false;
        InterlockedExchange(&cursorFree, 0);
        releaseInput();
    };
    nativeSettingsConfigured = true;
}
extern "C" IMAGE_DOS_HEADER __ImageBase;
static void publishNativeSettingsRetirement() {
    if (nativeSettings.faultAt && !nativeSettingsFaultAt.load())
        nativeSettingsFaultAt = nativeSettings.faultAt - reinterpret_cast<uintptr_t>(&__ImageBase);
    if (nativeSettings.failed()) nativeSettingsFaulted = true;
    nativeSettingsRetired = nativeSettings.retired();
}
static void nativeSettingsService() {
    if (nativeSettingsFaulted.load()) {
        // Retry only guarded restoration after an owner refusal; unknown forms
        // remain untouched and cannot keep the native Settings table blocked.
        try { nativeSettings.recoverFault(); }
        catch (...) { nativeSettingsRetired = false; }
        publishNativeSettingsRetirement();
        return;
    }
    try {
        if (!nativeSettingsConfigured) configureNativeSettings();
        const int captured=nativeBindingResult.exchange(-2);
        if (captured!=-2) nativeSettings.completeCapture(captured);
        nativeSettings.tick(InterlockedCompareExchange(&stopping, 0, 0) != 0,
                            nativeSettingsWanted.load());
        nativeBindingArmed=nativeSettings.captureTarget()<0 ? 0 : nativeSettings.captureTarget()+1;
        nativeSettingsWidened=nativeSettings.widenedFrame();
        nativeSettingsDispatches=nativeSettings.dispatches;
        nativeSettingsSteps=nativeSettings.steps;
        nativeSettingsRefused=nativeSettings.refusedFlags;
        {
            const auto& f=nativeSettings.seenFrames();
            const int v[9]={f[0].x,f[0].y,f[0].width,f[0].height,f[1].width,f[2].width,f[3].width,nativeSettings.seenFrameScale(),
                            nativeSettings.scale ? nativeSettings.scale() : 0};
            for (unsigned i=0;i<9;++i) nativeSettingsFrames[i]=v[i];
        }
        publishNativeSettingsRetirement();
    } catch (...) {
        nativeBindingArmed=0;
        nativeSettings.fail();
        nativeSettingsFaulted = true;
        nativeSettingsRetired = false;
    }
}
static void nativeSettingsMaintenance(uintptr_t self) {
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    const LONG owner = InterlockedCompareExchange(&nativeSettingsThread, thread, 0);
    if (owner && owner != thread) {
        nativeSettingsFaulted = true;
        nativeSettingsRetired = false;
        originalGuiMaintenance(self);
        return;
    }
    // A native action still owns its descriptor until its handler returns.
    // Defer even native maintenance, which can retire forms, at this boundary.
    if (nativeSettings.callbackActive()) return;
    if (inNativeSettingsMaintenance) {
        originalGuiMaintenance(self);
        return;
    }
    InterlockedIncrement(&nativeSettingsInFlight);
    inNativeSettingsMaintenance = true;
    __try {
        __try {
            nativeSettingsService();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            nativeSettings.fail();
            nativeSettingsFaulted = true;
            nativeSettingsRetired = false;
        }
        originalGuiMaintenance(self);
        __try {
            nativeSettings.observe();
            publishNativeSettingsRetirement();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            nativeSettings.fail();
            nativeSettingsFaulted = true;
            nativeSettingsRetired = false;
        }
    } __finally {
        inNativeSettingsMaintenance = false;
        InterlockedDecrement(&nativeSettingsInFlight);
    }
}
static void f8WindowTick(bool requested, bool foreground) {
    // Focus loss releases input, not the settings transaction. Only begin an
    // opening while foreground; explicit F8 dismissal still closes off-focus.
    if (!requested || foreground) nativeSettingsWanted = requested;
    // A Settings refusal shows no desktop banner: the game's buttons keep working and the status file
    // carries settings_fault_at.
}
static void nativeSettingsBeforeStop() {
    clearNativeBindingPresses();
    nativeBindingArmed=0;
    nativeBindingResult=-2;
    nativeSettingsWanted = false;
}
static bool awaitNativeSettings(DWORD timeout) {
    const ULONGLONG start = GetTickCount64();
    while (!nativeSettingsRetired.load() || InterlockedCompareExchange(&nativeSettingsInFlight, 0, 0)) {
        if (GetTickCount64() - start >= timeout) return false;
        Sleep(5);
    }
    return true;
}
