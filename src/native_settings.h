#pragma once
#include "native_widget_contract.h"
#include "settings_draft.h"
#include "native_scroll_coordinates.h"
#include <atomic>
#include <vector>

namespace panel {
// Extends native Settings and its scrolling Controls pane. No extra form/renderer.
class NativeSettingsIntegration {
public:
    struct Table { NativeElement* data = nullptr; int count = 0; };
    using NativeHandler = int (*)(NativeElement*, unsigned, uint32_t, uint32_t);
    NativeHandler checkboxHandler = nullptr;
    std::function<bool(uintptr_t, NativeScrollGeometry&)> scrollGeometry;
    struct CheckboxDiagnostics { uint64_t correctedReleases, zeroScrollReleases, rejectedCoordinates, nativeActions; };
    CheckboxDiagnostics checkboxDiagnostics() const;
    struct Panes { uintptr_t graphics = 0, controls = 0, controller = 0; };
    std::function<uintptr_t()> current;
    std::function<int(uintptr_t)> contains;
    std::function<Table(uintptr_t)> table;
    std::function<Table()> rootBasis, controlsBasis, controllerBasis;
    std::function<void(uintptr_t, const NativeElement*, int)> replace;
    std::function<Panes(uintptr_t)> panes;
    std::function<void(uintptr_t, bool)> visible;
    std::function<bool(uintptr_t)> scrolling;
    std::function<void(uintptr_t, bool)> setScrolling;
    std::function<int(uintptr_t)> contentHeight;
    std::function<void(uintptr_t, int)> setContentHeight;
    std::function<int()> scale;
    std::function<bool()> mouseHeld; // active native-slider drag keeps its descriptor stable
    std::function<void()> openNative;
    std::function<void(uintptr_t)> closeNative;
    std::function<bool(uintptr_t)> accepted;
    std::function<ModSettings()> settings;
    std::function<void(const ModSettings&, const ModSettings&)> apply;
    std::function<void()> recenter, onClosed;

    uintptr_t handle() const;
    bool retired() const;
    bool failed() const;
    void fail();
    int selectedPage() const;
    int selectedSection() const; // no subtabs; historical tests must use the continuous scrollable pane
    int captureTarget() const;
    bool completeCapture(int key);
    const ModSettings& draftSettings() const;
    int actionId(int index, bool controls = false) const;
    void tick(bool stop = false, bool want = false);
    void observe();
    bool callbackActive() const; // nested native maintenance must not retire a live action

private:
    static constexpr int capacity = 96, none = -1, pageEvent = 1000;
    uintptr_t root = 0;
    Panes children;
    DWORD thread = 0;
    bool fault = false, dirty = false, controlsOwned = false, wanted = false, openedFromKey = false,
         closing = false, applyQueued = false, deferUntilMouseRelease = false, updating = false;
    unsigned callbackDepth = 0, handlerDepth = 0;
    std::atomic<uint64_t> correctedReleases{0}, zeroScrollReleases{0}, rejectedCoordinates{0}, nativeActions{0};
    ModSettings acceptedInitial, acceptedValue;
    int page = 0, captureRow = none, rootCount = 0, controlCount = 0, baseCount = 44,
        originalExtent = 0, extentScale = 4, builtChildPage = -1;
    bool baseSaved = false, scrollOwned = false, originalScrolling = false;
    std::array<NativeElement, 44> originalControls{};
    SettingsDraft transaction;
    std::array<void (*)(NativeElement*), 9> originals{};
    struct Action { int row = none, delta = 0; bool slider = false, capture = false; };
    std::array<Action, capacity> rootActions{}, controlActions{};
    std::array<NativeText, capacity> rootText{}, controlText{};
    inline static NativeSettingsIntegration* callbackOwner = nullptr;

    template<int Index> static int checkboxEvent(NativeElement* e, unsigned event, uint32_t first, uint32_t second);
    template<int Index> static int checkboxEventGuarded(NativeSettingsIntegration* self, NativeElement* e, unsigned event, uint32_t first, uint32_t second);
    template<size_t... I> static auto checkboxHandlers(std::index_sequence<I...>);
    void installCheckboxHandlers();
    template<int Index, bool Controls> static void activated(NativeElement* e);
    template<int Index, bool Controls> static void guardedDispatch(NativeSettingsIntegration* self, NativeElement* e);
    template<int Index, bool Controls> static void dispatch(NativeSettingsIntegration* self, NativeElement* e);
    template<bool Controls, size_t... I> static auto callbacks(std::index_sequence<I...>);
    static NativeElement unscaled(NativeElement live, const NativeElement& canonical);
    bool nativeRoot(Table live, Table source) const;
    bool nativeKeyboard(Table t) const;
    bool ownedRoot(Table t) const;
    bool ownedControls(Table t) const;
    bool nativeController(Table t) const;
    template<bool Controls> void add(std::vector<NativeElement>& out, int id, int delta,
                                     int x, int y, int width, const std::wstring& text,
                                     unsigned kind = 1, unsigned flags = 0,
                                     bool slider = false, float position = 0.f, int steps = 0);
    // Each native separator uses the game's text-centering appearance bits.
    // Keep text and controls inside the scrollable pane's content coordinates.
    void divider(std::vector<NativeElement>& out,const wchar_t* title,bool first=false);
    void captureRowWidget(std::vector<NativeElement>& out,int id);
    template<bool Controls> void row(std::vector<NativeElement>& out,int id,int y);
    void serviceTick(bool stop, bool want);
    void rebuildRoot();
    void rebuildControls(Table live);
    void restore();
    void retire();
    int baselineExtent() const;
};
} // namespace panel
