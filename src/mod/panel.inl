// Settings form changes run only at the existing GUI maintenance boundary.
#include "../native_settings.h"
// Cross WndProc/GUI-thread key-capture mailbox. -2=no result, -1=cancel,
 // 0=unbind, positive=virtual-key code. All UI mutations remain on GUI thread.
static std::atomic<int> nativeBindingArmed{0}, nativeBindingResult{-2},
                        nativeBindingOwedUp{0};
static bool nativeSettingsBindingInput(UINT message, WPARAM wp, LPARAM) {
    const int key=bindingMessageKey(message,wp);
    if (!key) return false;
    const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN ||
                    message==WM_LBUTTONDOWN || message==WM_LBUTTONDBLCLK ||
                    message==WM_RBUTTONDOWN || message==WM_RBUTTONDBLCLK ||
                    message==WM_MBUTTONDOWN || message==WM_MBUTTONDBLCLK ||
                    message==WM_XBUTTONDOWN || message==WM_XBUTTONDBLCLK;
    const bool up=message==WM_KEYUP || message==WM_SYSKEYUP ||
                  message==WM_LBUTTONUP || message==WM_RBUTTONUP ||
                  message==WM_MBUTTONUP || message==WM_XBUTTONUP;
    if (up && nativeBindingOwedUp.load()==key) {
        nativeBindingOwedUp=0;
        return true;
    }
    if (!nativeBindingArmed.load() || !down) return false;
    // An unmatched physical release must not become a new rebind. Only a
    // fresh down publishes a result, and the following up remains swallowed.
    nativeBindingOwedUp=key;
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
    const uintptr_t parent = *(uintptr_t*)gameAt(fp::sig::guiDesktop);
    if (parent != nativeOptionsDesktop || !readable(parent, field::guiTask::forms + sizeof(uintptr_t))) return -1;
    const int n = *(int*)(parent + field::guiTask::formCount);
    const uintptr_t list = *(uintptr_t*)(parent + field::guiTask::forms);
    if (n < 0 || n > 4096 || (n && !readable(list, size_t(n) * sizeof(uintptr_t)))) return -1;
    for (int i = 0; i < n; ++i) if (((uintptr_t*)list)[i] == form) return 1;
    return 0;
}
static panel::NativeSettingsIntegration::Table nativeSettingsTable(uintptr_t form) {
    namespace F = field::nativeForm;
    if (!readable(form, F::elements + sizeof(uintptr_t))) return {};
    const int last = *(int*)(form + F::elementLast);
    if (last < 0 || last >= 96) return {};
    auto* e = *(panel::NativeElement**)(form + F::elements);
    if (!readable(reinterpret_cast<uintptr_t>(e), size_t(last + 1) * sizeof(*e))) return {};
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
        nativeOptionsDesktop = *(uintptr_t*)gameAt(fp::sig::guiDesktop);
        return *(uintptr_t*)gameAt(fp::sig::guiOptionsForm);
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
        return readable(uintptr_t(e), 44 * sizeof(*e)) ? Table{e, 44} : Table{};
    };
    nativeSettings.controllerBasis = []() -> Table {
        auto* e = reinterpret_cast<panel::NativeElement*>(gameAt(fp::sig::guiControllerElements));
        return readable(uintptr_t(e), 5 * sizeof(*e)) ? Table{e, 5} : Table{};
    };
    nativeSettings.replace = replaceNativeSettingsElements;
    nativeSettings.checkboxHandler = reinterpret_cast<panel::NativeSettingsIntegration::NativeHandler>(gameAt(fp::sig::guiCheckboxHandler));
    nativeSettings.scrollGeometry = [](uintptr_t pane, panel::NativeScrollGeometry& g) {
        namespace F=field::nativeForm;
        if (!readable(pane,F::verticalScroll+sizeof(float))) return false;
        g.left=*(int32_t*)(pane+F::contentLeft); g.top=*(int32_t*)(pane+F::contentTop);
        g.right=*(int32_t*)(pane+F::contentRight); g.bottom=*(int32_t*)(pane+F::contentBottom);
        g.width=*(int32_t*)(pane+F::contentWidth); g.height=*(int32_t*)(pane+F::scrollExtent);
        g.horizontal=*(float*)(pane+F::horizontalScroll); g.vertical=*(float*)(pane+F::verticalScroll);
        return true;
    };
    nativeSettings.panes = [](uintptr_t root) -> panel::NativeSettingsIntegration::Panes {
        if (!readable(root, field::optionsForm::controllerPane + sizeof(uintptr_t))) return {};
        return {*(uintptr_t*)(root + field::optionsForm::graphicsPane),
                *(uintptr_t*)(root + field::optionsForm::controlsPane),
                *(uintptr_t*)(root + field::optionsForm::controllerPane)};
    };
    nativeSettings.visible = [](uintptr_t pane, bool show) {
        ((void (*)(uintptr_t, unsigned char))gameAt(fp::sig::guiFormShow))(pane, show ? 1 : 0);
    };
    nativeSettings.scrolling = [](uintptr_t pane) {
        return (*(unsigned*)(pane+field::nativeForm::frameFlags)&0x200)!=0;
    };
    nativeSettings.setScrolling = [](uintptr_t pane,bool enabled) {
        auto& flags=*(unsigned*)(pane+field::nativeForm::frameFlags);
        const unsigned updated=(flags&~0x200u)|(enabled ? 0x200u : 0u);
        if (updated!=flags) {
            flags=updated;
            ((void (*)(uintptr_t))gameAt(fp::sig::guiFormGeometry))(pane);
        }
    };
    nativeSettings.contentHeight = [](uintptr_t pane) {
        return *(int*)(pane + field::nativeForm::scrollExtent);
    };
    nativeSettings.setContentHeight = [](uintptr_t pane, int height) {
        *(int*)(pane + field::nativeForm::scrollExtent) = height;
        ((void (*)(uintptr_t))gameAt(fp::sig::guiFormGeometry))(pane);
    };
    nativeSettings.scale = [] {
        const int value = *(int*)gameAt(fp::sig::guiUiScale);
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
        return *(int*)(form + field::optionsForm::applied) == 1;
    };
    nativeSettings.settings = currentSettings;
    nativeSettings.apply = commitSettingsDraft;
    nativeSettings.recenter = [] { ++trackingReset; };
    nativeSettings.onClosed = [] {
        nativeBindingArmed=0;
        nativeBindingResult=-2;
        nativeSettingsWanted = false;
        InterlockedExchange(&cursorFree, 0);
        releaseInput();
    };
    nativeSettingsConfigured = true;
}
static void publishNativeSettingsRetirement() {
    if (nativeSettings.failed()) nativeSettingsFaulted = true;
    nativeSettingsRetired = !nativeSettingsFaulted.load() && nativeSettings.retired();
}
static void nativeSettingsService() {
    if (nativeSettingsFaulted.load()) return;
    try {
        if (!nativeSettingsConfigured) configureNativeSettings();
        const int captured=nativeBindingResult.exchange(-2);
        if (captured!=-2) nativeSettings.completeCapture(captured);
        nativeSettings.tick(InterlockedCompareExchange(&stopping, 0, 0) != 0,
                            nativeSettingsWanted.load());
        nativeBindingArmed=nativeSettings.captureTarget()<0 ? 0 : nativeSettings.captureTarget()+1;
        publishNativeSettingsRetirement();
    } catch (...) {
        nativeBindingArmed=0;
        nativeSettings.fail();
        nativeSettingsFaulted = true;
        nativeSettingsRetired = false;
    }
}
static void nativeSettingsMaintenance(uintptr_t self) {
    const LONG thread = LONG(GetCurrentThreadId());
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
static void f8WindowTick(bool show, bool) {
    nativeSettingsWanted =
        show;
    static bool noticed = false;
    if (nativeSettingsFaulted.load() && !noticed) {
        noticed = true;
        showNotice(L"Native Settings lost its verified UI owner. Settings are unavailable; restart Exanima "
                   L"before retrying.");
    }
}
static void nativeSettingsBeforeStop() {
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
