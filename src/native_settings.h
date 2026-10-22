#pragma once
#include "native_widget_contract.h"
#include "settings_draft.h"
#include "native_scroll_coordinates.h"
#include <atomic>
#include <vector>

namespace panel {
// One of ExCamera's Mod Settings sections: a category, its title and its rows.
struct OwnSection { const wchar_t* category; const wchar_t* title; int rows[12]; int count; };
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
    // A native form's frame in UI units; the panes copy the root's width when the game opens Settings.
    struct Frame { int x = 0, y = 0, width = 0, height = 0; };
    std::function<bool(uintptr_t, Frame&)> frame;
    std::function<void(uintptr_t, const Frame&)> setFrame;
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
    bool modsByMod = false;                     // Mod Settings listed by mod rather than by category
    std::function<void(bool)> saveModsByMod;    // remembers that choice
    std::function<void()> requestRedraw; // Admitted native redraw request, not geometry.
    // Tabs the Exanima Lua runtime (ScriptRuntime.dll, settings tabs version 1) asks this form to host, such as
    // its "Mod Settings" tab. Row must match the runtime's settingstabs::Row exactly. All calls stay on the GUI
    // thread; a missing runtime leaves these null and the form exactly as before.
    struct HostedRow { int id = -1, kind = 5; const wchar_t* label = nullptr; const wchar_t* value = nullptr;
                       int steps = 0; float position = 0; int checked = 0, inactive = 0; };
    struct RuntimeTabs {
        int (*open)() = nullptr;
        int (*title)(int tab, wchar_t* out, int room) = nullptr;
        int (*rows)(int tab, int page, HostedRow* out, int room) = nullptr;
        void (*change)(int tab, int page, int id, int kind, int delta, float position, int checked, int key) = nullptr;
        void (*event)(int tab, int event) = nullptr;
        int (*help)(int id, wchar_t* out, int capacity) = nullptr; // optional: Mod Settings rows only
        int (*category)(int id, wchar_t* out, int capacity) = nullptr; // optional: a mod section's category
        bool ready() const { return open && title && rows && change && event; }
    } runtimeTabs;

    uintptr_t handle() const;
    bool retired() const;
    bool failed() const;
    void fail();
    // Restore the game's Settings table after a refusal only if its current
    // form and root table are still verifiably owned by ExCamera.
    bool recoverFault();
    uintptr_t faultAt = 0;            // return address of the first fail(), 0 while healthy
    unsigned dispatches = 0, steps = 0, refusedFlags = 0; // ...and the kind<<24 | flags of the last refused one // ExCamera row callbacks reached, value-button steps taken
    int selectedPage() const;
    bool widenedFrame() const; // the Settings form currently carries the five-tab width
    bool vrTab = true; // false in a build without the headset code: the VR tab is not shown, its page never reached
    const std::array<Frame, 4>& seenFrames() const; // root, Graphics, Controls, Controller as last read at adoption
    int seenFrameScale() const;                     // hundredths of the factor the game holds the frames at
    int selectedSection() const; // no subtabs; historical tests must use the continuous scrollable pane
    int captureTarget() const;
    bool completeCapture(int key);
    const ModSettings& draftSettings() const;
    int actionId(int index, bool controls = false) const;
    void tick(bool stop = false, bool want = false);
    void observe();
    bool callbackActive() const; // nested native maintenance must not retire a live action

private:
    static constexpr int capacity = 160, none = -1, pageEvent = 1000;
    uintptr_t root = 0;
    Panes children;
    // Five tabs need more width than the game's two: the form and its panes grow by widthGain while owned.
    // Up to two runtime-hosted tabs follow VR (pages 5 and 6); each widens the form by 25 more units.
    static constexpr int nativeWidth = 434, nativeHeight = 536, ownTabs = 5, maxTabs = 7, hostedCaptureRow = 4000, sectionPicker = 9;
    int widthGain = 100, tabCount = ownTabs;
    std::array<int, maxTabs - ownTabs> hostedPages{};          // runtime tab index for page 5 + i
    std::array<std::wstring, maxTabs - ownTabs> hostedTitles{};
    int hostedCaptureTab = -1, hostedCaptureId = -1;
    int ownHelpId = -1; // ExCamera's last-used row: its help shows under the details
    int hostedHelpTab = -1, hostedHelpId = -1; // the hosted row last used: its help shows under the page
    std::array<int, 16> hostedSection{}; // per runtime tab: which mod's section its page shows
    void hostedEvent(int tab, int event);
    void hostedRows(std::vector<NativeElement>& out, int& lines);
    int ownSectionCount() const;
    std::vector<const wchar_t*> ownCategories() const;
    static constexpr int sectionTitle = -1000; // a details line that is section k's title: sectionTitle - k
    static constexpr int headerLine = -2000;   // a details line that is sub-header n: headerLine - n
    static const OwnSection& ownSection(int index);
    std::array<Frame, 4> originalFrames{}, frames{};
    int frameScale = 0;
    bool widened = false;
    DWORD thread = 0;
    bool fault = false, dirty = false, controlsOwned = false, wanted = false, openedFromKey = false,
         closing = false, applyQueued = false, deferUntilMouseRelease = false, updating = false, valueRedraw = false;
    unsigned callbackDepth = 0, handlerDepth = 0;
    std::atomic<uint64_t> correctedReleases{0}, zeroScrollReleases{0}, rejectedCoordinates{0}, nativeActions{0};
    ModSettings acceptedInitial, acceptedValue;
    int page = 0, captureRow = none, rootCount = 0, controlCount = 0, baseCount = 44,
        originalExtent = 0, extentScale = 4, builtChildPage = -1;
    bool baseSaved = false, scrollOwned = false, originalScrolling = false;
    std::array<NativeElement, 44> originalControls{};
    SettingsDraft transaction;
    std::array<void (*)(NativeElement*), 9> originals{};
    // hosted: the runtime tab a hosted row belongs to (row is then the runtime's row id), hostedKind its Row kind.
    struct Action { int row = none, delta = 0; bool slider = false, capture = false; int hosted = none, hostedKind = 0; };
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
    void captureRowWidget(std::vector<NativeElement>& out,int id,int y);
    // half: 0 the whole row; 1 and 2 the left and right halves, for two switches on one line. The row is laid
    // out in the column from columnX, columnWidth units wide (the Mod Settings detail pane).
    template<bool Controls> void row(std::vector<NativeElement>& out,int id,int y,int half=0);
    int columnX = 16, columnWidth = 354;
    static constexpr int layoutToggle = -2; // the Mod Settings list's by-category / by-mod switch
    static constexpr int groupHeader = -10;  // a collapsed mods header g in the list: groupHeader - g
    static constexpr int listLines = 14;     // list lines the pane shows without scrolling at the game's sizes
    int expandedModGroup = 0;
    void serviceTick(bool stop, bool want);
    void refreshStatusCaptions(const ModSettings& live);
    void liveValue(int slider, const std::wstring& value);
    void hoverHelp();
    void widen();
    void narrow();
    void holdWidened();
    void insetScrollPane(bool inset);
    Frame paneHome{};
    bool paneInset = false;
    std::array<Frame, 4> widenedFrames{}; // root, Graphics, Controls, Controller as widen() set them

    void rebuildRoot();
    void rebuildControls(Table live);
    void restore();
    void retire();
    int baselineExtent() const;
};
} // namespace panel
