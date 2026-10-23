#include "native_settings.h"
#include "settings_presentation.h"
#include <cstring>
#include <intrin.h>

namespace panel {
// ExCamera's own settings, by category and subcategory, ahead of the runtime's mods on the Mod Settings page.
// The game's Graphics and Controls tabs carry nothing of ExCamera's.
// A row id at or below subHeader is a dimmed sub-header line: ownHeaders[subHeader - id].
constexpr int subHeader = -100;
static const wchar_t* const ownHeaders[] = {L"Framing", L"Behaviour", L"Light", L"Image", L"Damage", L"Mouse", L"Movement"};
constexpr int framing = subHeader, behaviour = subHeader - 1, light = subHeader - 2, image = subHeader - 3,
              damage = subHeader - 4, mouse = subHeader - 5, movement = subHeader - 6;
static const OwnSection ownSections[] = {
    {L"Camera", L"First person", {setting::fov, setting::hideHelmet, setting::helmetVisor, setting::fingerPoses}, 4},
    {L"Camera", L"Third person", {framing, setting::tpFov, setting::tpDistance, setting::tpShoulder, setting::tpHeight,
                                  setting::tpCombat, behaviour, setting::tpLag, setting::tpTight, setting::tpDown,
                                  setting::playerGhost}, 11},
    {L"Graphics", L"Screen effects", {light, setting::ambientOcclusion, setting::screenSpaceReflections, setting::lightSmoothing,
                                      image, setting::depthOfField, setting::edgeSmoothing, damage,
                                      setting::conditionEffects, setting::woundMarks}, 10},
    {L"Graphics", L"Performance", {setting::drawDistance, setting::objectLights}, 2},
    {L"Graphics", L"Surface detail", {setting::heldDetail, setting::armourDetail, setting::objectDetail, setting::levelDetail}, 4},
    {L"Combat", L"Blocking and aim", {setting::fpDefense, setting::fpSwingSelection, setting::tpCombatAim, setting::indicator}, 4},
    {L"Combat", L"Wind-up arc", {setting::attackCue, setting::cueThrust, setting::cueSize, setting::cueOpacity}, 4},
    {L"Combat", L"Blades", {setting::bladeDrag, setting::stabSink, setting::bladeGain}, 3},
    {L"Controls", L"Keys", {setting::rotateHeldKey, setting::leanKey, setting::kickKey, setting::jumpKey}, 4},
    {L"Controls", L"Mouse and movement", {mouse, setting::sensitivity, setting::invertY, setting::rawMouse, movement,
                                          setting::crouchMode}, 6},
    {L"VR", L"Headset", {setting::vrHeadset, setting::vrIpd, setting::vrScale, setting::vrRenderMode, setting::vrHands,
                         setting::vrPosture, setting::vrTurning, setting::vrEffects, setting::vrGripAngle, setting::vrHaptics,
                         setting::vrPanelWidth, setting::vrPanelDistance}, 12},
    {L"VR", L"Body and comfort", {setting::vrComfort, setting::vrBody, setting::vrHeight, setting::vrGrabTurn, setting::vrEyeScale,
                                  setting::vrGripSlide, setting::vrKick, setting::vrFingerBones}, 8},
    {L"VR", L"Head tracking", {setting::tracking, setting::trackingKey}, 2},
};

// Adapt only owned mod checkboxes. The native handler performs the hit test,
// toggles selection and calls the action. No selection flags are synthesized.
template<int Index>
int NativeSettingsIntegration::checkboxEvent(NativeElement* e,unsigned event,uint32_t first,uint32_t second) {
    auto* self=callbackOwner;
    if (!self || self->thread!=GetCurrentThreadId() || !self->checkboxHandler) return 0;
    ++self->handlerDepth;
    int result=0;
#ifdef _MSC_VER
    __try {
        __try { result=checkboxEventGuarded<Index>(self,e,event,first,second); }
        __except (EXCEPTION_EXECUTE_HANDLER) { self->fail(); }
    } __finally { --self->handlerDepth; }
#else
    result=checkboxEventGuarded<Index>(self,e,event,first,second);
    --self->handlerDepth;
#endif
    return result;
}

template<int Index>
int NativeSettingsIntegration::checkboxEventGuarded(NativeSettingsIntegration* self,NativeElement* e,
                                                    unsigned event,uint32_t first,uint32_t second) {
    try {
        const auto& owned=self->controlActions[Index];
        const int row=owned.row;
        const bool hosted=owned.hosted!=none && owned.hostedKind==1;
        if (!hosted && (row<0 || row>=setting::count)) return 0;
        if (!e || !self->controlsOwned || !self->children.controls ||
            e->owner!=self->children.controls || e->action!=activated<Index,true> ||
            e->handler!=reinterpret_cast<uintptr_t>(checkboxEvent<Index>) ||
            !(hosted ? e->kind==2 && e->group==0 : widget::checkboxAction(*e,row))) return 0;
        const auto live=self->table(self->children.controls);
        if (!self->ownedControls(live) || Index>=live.count || &live.data[Index]!=e) return 0;
        // Event40 forwards its unchanged coordinates through handler(event30).
        // Correcting40 here too would apply the correction twice.
        if (event==0x30 || event==0x50) {
            NativeScrollGeometry geometry;
            uint32_t& point=event==0x30 ? first : second;
            bool changed=false;
            if (!self->scrollGeometry || !self->scrollGeometry(e->owner,geometry) ||
                !correctNativeCheckboxPoint(point,geometry,changed)) {
                self->rejectedCoordinates.fetch_add(1,std::memory_order_relaxed);
                return 0; // Never wrap/clamp a failed coordinate into another row.
            }
            if (event==0x50) {
                auto& counter=changed ? self->correctedReleases : self->zeroScrollReleases;
                counter.fetch_add(1,std::memory_order_relaxed);
            }
        }
        return self->checkboxHandler(e,event,first,second);
    } catch (...) { self->fail(); return 0; }
}

template<size_t... I>
auto NativeSettingsIntegration::checkboxHandlers(std::index_sequence<I...>) {
    return std::array<NativeHandler,sizeof...(I)>{checkboxEvent<int(I)>...};
}

NativeSettingsIntegration::CheckboxDiagnostics NativeSettingsIntegration::checkboxDiagnostics() const {
    return {correctedReleases.load(std::memory_order_relaxed),zeroScrollReleases.load(std::memory_order_relaxed),
            rejectedCoordinates.load(std::memory_order_relaxed),nativeActions.load(std::memory_order_relaxed)};
}

// The native handler still accesses its element after returning from action.
// Keep this SEH wrapper free of destructible locals (MSVC /EHsc, C2712).
template<int Index, bool Controls>
void NativeSettingsIntegration::activated(NativeElement* e) {
    auto* self = callbackOwner;
    // After a refusal the game's own buttons (tabs, Defaults, Apply, Cancel) still do what they did.
    if constexpr (!Controls && Index < 9)
        if (self && self->fault && self->originals[Index] && self->thread == GetCurrentThreadId()) {
            self->originals[Index](e);
            return;
        }
    if (!self || self->callbackDepth || self->updating || self->fault || self->closing ||
        self->thread != GetCurrentThreadId()) return;
    ++self->callbackDepth;
#ifdef _MSC_VER
    __try {
        __try { guardedDispatch<Index, Controls>(self,e); }
        __except (EXCEPTION_EXECUTE_HANDLER) { self->fail(); }
    } __finally { --self->callbackDepth; }
#else
    guardedDispatch<Index, Controls>(self,e);
    --self->callbackDepth;
#endif
}

template<int Index, bool Controls>
void NativeSettingsIntegration::guardedDispatch(NativeSettingsIntegration* self, NativeElement* e) {
    try { dispatch<Index, Controls>(self,e); }
    catch (...) { self->fail(); } // Never unwind a C++ exception into the game.
}

template<int Index, bool Controls>
void NativeSettingsIntegration::dispatch(NativeSettingsIntegration* self, NativeElement* e) {
    if (!e) return;
    ++self->dispatches;
    const uintptr_t owner = Controls ? self->children.controls : self->root;
    if (!owner || e->owner != owner || e->action != activated<Index, Controls>) return;
    const auto live = self->table(owner);
    if (!live.data || Index >= live.count || &live.data[Index] != e) return;
    const auto action = Controls ? self->controlActions[Index] : self->rootActions[Index];
    if constexpr (Controls) if (action.hostedKind == sectionPicker) {
        const int key = action.hosted - ownTabs; // the page's index among the Mod Settings pages
        if (e->kind != 1 || key < 0 || key >= int(self->hostedSection.size())) return;
        if (action.row <= groupHeader) { // a collapsed category header: open it
            self->expandedModGroup = groupHeader - action.row;
            self->dirty = true;
            return;
        }
        if (action.row == layoutToggle) { // the list's own switch: by category or by mod
            self->modsByMod = !self->modsByMod;
            if (self->saveModsByMod) self->saveModsByMod(self->modsByMod);
            self->dirty = true;
            return;
        }
        self->hostedSection[size_t(key)] = action.row;
        self->captureRow = none;
        self->dirty = true;
        return;
    }
    if constexpr (Controls) if (action.hosted != none) {
        // A runtime-hosted row: the same widget admission as ExCamera's own rows, by the runtime's row kind.
        if ((e->flags & widget::inactive) || !self->runtimeTabs.ready()) return;
        const int tab = action.hosted;
        if (action.row != none && (self->hostedHelpTab != tab || self->hostedHelpId != action.row)) {
            self->hostedHelpTab = tab; self->hostedHelpId = action.row;
            if (self->runtimeTabs.help) self->dirty = true;
        }
        if (action.capture) {
            if (e->kind != 5) return;
            self->captureRow = hostedCaptureRow; self->hostedCaptureTab = tab; self->hostedCaptureId = action.row;
            self->dirty = true;
        } else if (action.hostedKind == 1) {
            if (e->kind != 2 || e->group != 0) return;
            self->runtimeTabs.change(tab, 0, action.row, 1, 0, 0.f, (e->flags & 0x20) != 0, 0);
        } else if (action.hostedKind == 2) {
            if (e->kind != 4 || !std::isfinite(e->position)) return;
            self->runtimeTabs.change(tab, 0, action.row, 2, 0, e->position, 0, 0);
            std::array<HostedRow, 64> rows{};
            const int count = (std::min)(self->runtimeTabs.rows(tab, 0, rows.data(), int(rows.size())), int(rows.size()));
            for (int i = 0; i < count; ++i)
                if (rows[size_t(i)].id == action.row && rows[size_t(i)].kind == 2 && rows[size_t(i)].value)
                    self->liveValue(Index, std::wstring(rows[size_t(i)].value).substr(0, 10));
            self->dirty = true;
        } else if (action.hostedKind == 3) {
            if (e->kind != 1) return;
            self->runtimeTabs.change(tab, 0, action.row, 3, 1, 0.f, 0, 0);
            self->dirty = true;
        }
        return;
    }
    if (!widget::activation(*e,action.row,action.capture,Controls)) { self->refusedFlags = e->flags | (unsigned(e->kind) << 24); return; }
    if constexpr (Controls)
        if (action.row >= 0 && action.row < setting::count && self->ownHelpId != action.row) {
            self->ownHelpId = action.row;
            self->dirty = true;
        }
    if constexpr (!Controls && Index < 9) {
        if constexpr (Index == 0 || Index == 1) {
            self->originals[Index](e);
            self->page=Index;
            self->captureRow=none;
            self->dirty=true;
        } else if constexpr (Index == 7) {
            // Freeze before native Apply can pump messages or mutate/close the form.
            self->acceptedInitial=self->transaction.initial;
            self->acceptedValue=self->transaction.value;
            self->closing=true;
            self->captureRow=none;
            self->originals[Index](e);
            // The native close is normally deferred, but never read a retired owner.
            if (self->contains(owner)!=1) { self->fail(); return; }
            if (self->accepted(owner)) self->applyQueued=true;
            else self->closing=false; // Native validation refusal leaves the draft editable.
        } else if constexpr (Index == 8) {
            self->closing=true;
            self->captureRow=none;
            self->transaction.value=self->transaction.initial;
            self->hostedEvent(-1,2);
            self->originals[Index](e);
        } else if constexpr (Index == 6) {
            if (self->page < 2) self->originals[Index](e);
            if (self->page>=ownTabs) {
                // The shown section only: one of ExCamera's, or the runtime's tab.
                const int key=self->page-ownTabs, shown=self->hostedSection[size_t(key)];
                const auto categories=self->ownCategories();
                if (shown < int(categories.size())) {
                    for (int k=0;k<self->ownSectionCount();++k)
                        if (!wcscmp(ownSection(k).category,categories[size_t(shown)]))
                            self->transaction.resetRows(ownSection(k).rows,ownSection(k).count);
                } else self->hostedEvent(self->hostedPages[size_t(key)],3);
            }
            self->dirty=true;
        }
    } else if (action.row >= pageEvent && action.row < pageEvent+self->tabCount) {
        self->page=action.row-pageEvent;
        self->captureRow=none;
        self->dirty=true;
    } else if (action.capture) {
        if (action.row==setting::rotateHeldKey || action.row==setting::leanKey || action.row==setting::kickKey ||
            action.row==setting::jumpKey ||
            (action.row==setting::trackingKey && self->transaction.value.tracking)) {
            self->captureRow=action.row;
            self->dirty=true;
        }
    } else if (action.row >= 0 && action.row < setting::count) {
        if (self->transaction.value.inactiveReason(action.row)) return;
        const auto& info=rowInfo(action.row);
        const ModSettings before=self->transaction.value;
        if (action.slider) {
            if (!self->transaction.slider(action.row,e->position)) { self->fail(); return; }
            if constexpr (Controls) self->liveValue(Index, settingValue(self->transaction.value, action.row));
        } else if (widget::checkboxAction(*e,action.row)) {
            // 0x40 is also hover preview. Only the native selected bit is state.
            const bool selected=(e->flags & 0x20)!=0;
            if (self->handlerDepth && (e->flags & widget::buttonTransient))
                self->nativeActions.fetch_add(1,std::memory_order_relaxed);
            if (info.flag) self->transaction.value.*(info.flag)=selected;
            else self->transaction.value.crouchMode=selected ? 2 : 1;
            self->transaction.value.normalize();
        } else { self->transaction.step(action.row,action.delta); ++self->steps; }
        if (!sameRow(action.row,before,self->transaction.value) &&
            (!info.checkbox() || action.row==setting::tracking || action.row==setting::attackCue))
            self->dirty=true;
        // Draft-only updates are synchronous: a second queued edit or Apply sees
        // every accepted action. Rebuilding/repainting waits for outer maintenance.
    }
}

template<bool Controls, size_t... I>
auto NativeSettingsIntegration::callbacks(std::index_sequence<I...>) {
    return std::array<void (*)(NativeElement*), sizeof...(I)>{activated<int(I), Controls>...};
}

void NativeSettingsIntegration::installCheckboxHandlers() {
    if (!controlsOwned || callbackActive()) return;
    const auto live=table(children.controls);
    if (!ownedControls(live)) return; // A native mode switch is reconciled by serviceTick.
    if (!checkboxHandler || !scrollGeometry) { fail(); return; }
    static const auto handlers=checkboxHandlers(std::make_index_sequence<capacity>{});
    static const auto actions=callbacks<true>(std::make_index_sequence<capacity>{});
    for (int i=baseCount;i<live.count;++i) {
        auto& e=live.data[i];
        const auto& action=controlActions[size_t(i)];
        const bool hosted=action.hosted!=none && action.hostedKind==1;
        if (!hosted && (action.hosted!=none || action.row<0 || action.row>=setting::count || !rowInfo(action.row).checkbox())) continue;
        if (e.owner!=children.controls || e.action!=actions[size_t(i)] ||
            !(hosted ? e.kind==2 && e.group==0 : widget::checkboxAction(e,action.row))) { fail(); return; }
        const uintptr_t adapter=reinterpret_cast<uintptr_t>(handlers[size_t(i)]);
        if (e.handler!=reinterpret_cast<uintptr_t>(checkboxHandler) && e.handler!=adapter) { fail(); return; }
        e.handler=adapter;
    }
}

template<bool Controls>
void NativeSettingsIntegration::add(std::vector<NativeElement>& out, int id, int delta,
                                 int x, int y, int width, const std::wstring& text,
                                 unsigned kind, unsigned flags,
                                 bool slider, float position, int steps) {
    const size_t at = out.size();
    if (at >= capacity || width <= 0 ||
        (Controls && (x < 0 || x + width > 376 + (widened ? widthGain : 0)))) { fail(); return; }
    auto& words = Controls ? controlText : rootText;
    auto& actions = Controls ? controlActions : rootActions;
    words[at].set(text);
    actions[at] = {id, delta, slider, false};
    NativeElement e;
    // +0x48 is an optional pointer to native text color, not a widget
    // identity! The native kind-five renderer dereferences it when nonzero.
    // The former 10000+at value was the Settings form exception source.
    e.identity = 0;
    e.kind = uint16_t(kind); e.x = int16_t(x); e.y = int16_t(y); e.width = int16_t(width);
    e.height = 0; e.text = words[at].bytes; e.flags = flags;
    e.appearance = widget::appearance(kind);
    e.owner = Controls ? children.controls : root;
    if (slider) {
        if (kind != 4 || steps < 1 || !std::isfinite(position)) { fail(); return; }
        e.position = position;
        e.data[0] = uint64_t(steps); // native slider's discrete step count at +0x50
    }
    static const auto functions = callbacks<Controls>(std::make_index_sequence<capacity>{});
    e.action = id == none ? nullptr : functions[at];
    out.push_back(e);
}

// The label column takes half of the widened pane's gain; a label still too long for it is cut
// with an ellipsis rather than run under its control (about 7 units per character of the native font).
static std::wstring fitLabel(const std::wstring& text, int width) {
    const size_t most = size_t((std::max)(width / 7, 4));
    return text.size() <= most ? text : text.substr(0, most - 3) + L"...";
}
// Button captions use a wider face: about 10 units per character plus 12 of padding.
// A Lua mod's slider value written as a fraction (0.35) reads as a percent (35%); anything else stays.
static std::wstring fractionAsPercent(const std::wstring& value) {
    if (value.find(L'.') == std::wstring::npos) return value;
    wchar_t* end = nullptr;
    const double v = wcstod(value.c_str(), &end);
    if (!end || *end || !(v >= 0.0 && v <= 1.0)) return value;
    return std::to_wstring(int(v * 100.0 + 0.5)) + L"%";
}
static std::wstring fitButton(const std::wstring& text, int width) {
    const size_t most = size_t((std::max)((width - 12) / 10, 4));
    return text.size() <= most ? text : text.substr(0, most - 2) + L"..";
}

template<bool Controls>
void NativeSettingsIntegration::row(std::vector<NativeElement>& out,int id,int y,int half) {
    const RowInfo& info=rowInfo(id);
    const unsigned flags=transaction.value.inactiveReason(id) ? 0x10 : 0;
    const wchar_t* label=settingLabel(id);
    const int x=columnX, whole=columnWidth, labelWidth=whole*2/5, control=x+labelWidth+4, valueWidth=56;
    if (info.kind==RowInfo::keyList) {
        if constexpr (Controls) captureRowWidget(out,id,y);
        return;
    }
    if (info.checkbox()) {
        const bool selected = info.flag ? transaction.value.*(info.flag) : transaction.value.crouchMode==2;
        const int width=half ? whole/2-4 : whole;
        const std::wstring text=id==setting::crouchMode ? L"Toggle crouch" : label;
        add<Controls>(out,id,1,half==2 ? x+whole/2+4 : x,half==2 ? -32768 : y,width,fitLabel(text,width-24),2,
                      flags|(selected ? widget::checked : 0));
        return;
    }
    add<Controls>(out,none,0,x,y,labelWidth,fitLabel(label,labelWidth),5,flags);
    const SettingRange& range=info.range;
    if (range.count) {
        const std::wstring value=settingValue(transaction.value,id);
        if (value.size()>10) { fail(); return; }
        add<Controls>(out,id,1,control,-32768,whole-labelWidth-valueWidth-8,L"",4,flags,true,
                      SettingsDraft::sliderPosition(transaction.value,id),range.count);
        add<Controls>(out,none,0,x+whole-valueWidth,-32768,valueWidth,value,5,flags);
        // Centred like the dividers: the right-aligned type-five appearance (0x4000) draws its text half
        // a row above its line in the native layout; the centred one sits on the row.
        out.back().appearance=0x3300;
    } else {
        add<Controls>(out,id,1,control,-32768,whole-labelWidth-4,fitButton(settingValue(transaction.value,id),whole-labelWidth-4),1,flags);
    }
}

uintptr_t NativeSettingsIntegration::handle() const { return root; }

bool NativeSettingsIntegration::retired() const { return root == 0 && !applyQueued; }

bool NativeSettingsIntegration::failed() const { return fault; }

__declspec(noinline) void NativeSettingsIntegration::fail() {
    if (!fault) faultAt = reinterpret_cast<uintptr_t>(_ReturnAddress()); // the first refusal, for the status line
    fault = true;
}

int NativeSettingsIntegration::selectedPage() const { return page; }
bool NativeSettingsIntegration::widenedFrame() const { return widened; }
const std::array<NativeSettingsIntegration::Frame, 4>& NativeSettingsIntegration::seenFrames() const { return frames; }
int NativeSettingsIntegration::seenFrameScale() const { return frameScale; }

int NativeSettingsIntegration::selectedSection() const { return 0; }

int NativeSettingsIntegration::captureTarget() const { return root && !closing && !fault ? captureRow : none; }

bool NativeSettingsIntegration::completeCapture(int key) {
    if (callbackActive() || captureRow == none || closing || fault || thread != GetCurrentThreadId()) return false;
    const int id=captureRow;
    captureRow=none;
    if (key < 0) { dirty=true; return true; } // Escape cancels
    if (id == hostedCaptureRow) {
        if (runtimeTabs.ready() && hostedCaptureTab >= 0)
            runtimeTabs.change(hostedCaptureTab, 0, hostedCaptureId, 4, 0, 0.f, 0, key);
        hostedCaptureTab = hostedCaptureId = -1;
        dirty=true;
        return true;
    }
    const bool captured = transaction.capture(id,key);
    dirty=true;
    return captured;
}

const ModSettings& NativeSettingsIntegration::draftSettings() const { return transaction.value; }

int NativeSettingsIntegration::actionId(int index, bool controls) const {
    return index >= 0 && index < capacity ?
        (controls ? controlActions[size_t(index)].row : rootActions[size_t(index)].row) : none;
}

void NativeSettingsIntegration::tick(bool stop, bool want) {
    if (updating || callbackActive()) return;
    updating=true; // Suppress feedback from native table/layout programmatic events.
    try { serviceTick(stop,want); }
    catch (...) { updating=false; throw; }
    updating=false;
}

void NativeSettingsIntegration::serviceTick(bool stop, bool want) {
    if (fault || callbackActive()) return;
    if (!thread) thread = GetCurrentThreadId();
    if (thread != GetCurrentThreadId()) { fail(); return; }
    // Native Apply can close/delete its form before the next GUI tick. Its callback
    // records acceptance while native deferred-close still owns the record.
    if (applyQueued) { applyQueued = false; apply(acceptedInitial, acceptedValue); hostedEvent(-1,1); }
    if (root) {
        const int member = contains(root);
        if (member < 0) { fail(); return; }
        if (!member) retire();
    }
    if (stop) {
        captureRow=none;
        if (root) restore();
        return;
    }
    if (root && closing) return;
    if (want && !wanted) {
        if (!current()) openNative();
        openedFromKey = true;
    } else if (!want && wanted && openedFromKey && root) {
        captureRow=none; // F8 dismissal must stop any pending binding capture
        closing=true;
        transaction.value=transaction.initial;
        hostedEvent(-1,2);
        closeNative(root);
        openedFromKey = false;
        wanted=want;
        return; // Close may retire the native owner immediately.
    }
    wanted = want;
    if (!root) {
        const uintptr_t candidate = current();
        if (!candidate) return;
        const int member = contains(candidate);
        if (member < 0) { fail(); return; }
        if (!member) return;
        const auto t = table(candidate), basis = rootBasis();
        if (!nativeRoot(t, basis) || t.data[0].owner != candidate) return;
        root = candidate;
        children = panes(root);
        if (!children.graphics || !children.controls) { fail(); return; }
        // Runtime tabs are snapshotted per opened form: up to two pages after VR.
        tabCount = ownTabs;
        if (runtimeTabs.ready()) {
            const int count = runtimeTabs.open();
            for (int hosted = 0; hosted < count && tabCount < maxTabs; ++hosted) {
                wchar_t title[32]{};
                if (runtimeTabs.title(hosted, title, 32) != 0) continue; // 1 = rows under Controls (not hosted here)
                hostedPages[size_t(tabCount - ownTabs)] = hosted;
                hostedTitles[size_t(tabCount - ownTabs)] = title;
                ++tabCount;
            }
        }
        if (tabCount == ownTabs) { // no runtime: ExCamera's own sections still get the page
            hostedPages[0] = none;
            hostedTitles[0] = L"Mod Settings";
            ++tabCount;
        }
        widthGain = 100; // room for the Mod Settings list beside its detail pane
        // A form just taken carries none of our frames. A form the game released while widened (retire, not
        // narrow) left the flag set: widen() then did nothing, holdWidened() put back the old root frame alone,
        // and every row was laid out for panes 100 units wider than they were (sliders reduced to their
        // arrows, headers pushed into the scroll bar), on every later open.
        widened = false;
        if (widthGain) widen();
        transaction.begin(settings());
        closing = false;
        for (int i = 0; i < 9; ++i) originals[size_t(i)] = t.data[i].action;
        page = (t.data[1].flags & 0x20) ? 1 : 0;
        callbackOwner = this;
        dirty = true;
    }
    if (!ownedRoot(table(root))) { fail(); return; }
    holdWidened();
    const ModSettings liveSettings = settings();
    transaction.refreshStatus(liveSettings);
    auto ct=table(children.controls);
    // Game keyboard/controller changes can replace the native child table,
    // invalidating our old contents. Never claim an unrelated replacement.
    if (controlsOwned && !ownedControls(ct)) {
        if (!nativeKeyboard(ct) && !nativeController(ct)) { fail(); return; }
        controlsOwned=false;
        scrollOwned=false; // Native mode change already restored its own frame flags.
        baseSaved=false;
        dirty=true;
    }
    if (!controlsOwned && ((nativeKeyboard(ct) && page!=0) || (nativeController(ct) && page>=2))) dirty=true;
    // Type-four native sliders keep a live element pointer while dragging.
    // Never replace that backing table until its physical drag ends.
    if (dirty && mouseHeld && mouseHeld()) {
        deferUntilMouseRelease=true;
        refreshStatusCaptions(liveSettings);
        return;
    }
    if (dirty && deferUntilMouseRelease) {
        deferUntilMouseRelease=false;
        refreshStatusCaptions(liveSettings);
        return;
    }
    if (dirty) {
        rebuildRoot();
        if (fault) return;
        if (nativeKeyboard(ct) || nativeController(ct) || controlsOwned) rebuildControls(ct);
        dirty = false;
    }
    hoverHelp();
    refreshStatusCaptions(liveSettings);
}

// The row under the cursor names the help shown under the details. The game marks the hovered element in
// its flags (bits 16-19, set by the hover pass at 0x2D7AA after clearing them on every element); a label's
// hover counts for the control after it on the same row.
void NativeSettingsIntegration::hoverHelp() {
    if (fault || closing || !controlsOwned || page < ownTabs || callbackActive()) return;
    const auto live = table(children.controls);
    if (!ownedControls(live)) return;
    for (int i = baseCount; i < live.count && i < capacity; ++i) {
        if (!(live.data[i].flags & 0xF0000u)) continue;
        int at = i;
        if (controlActions[size_t(at)].row == none && at + 1 < live.count && controlActions[size_t(at + 1)].row != none) ++at;
        const Action& a = controlActions[size_t(at)];
        if (a.hostedKind == sectionPicker || a.row == none) return;
        if (a.hosted == none) {
            if (a.row >= 0 && a.row < setting::count && ownHelpId != a.row) { ownHelpId = a.row; dirty = true; }
        } else if (hostedHelpTab != a.hosted || hostedHelpId != a.row) {
            hostedHelpTab = a.hosted; hostedHelpId = a.row; dirty = true;
        }
        return;
    }
}

// A slider's value caption follows the drag: the caption element after the slider is rewritten in place,
// since the table itself cannot be rebuilt until the drag ends.
void NativeSettingsIntegration::liveValue(int slider, const std::wstring& value) {
    if (fault || closing || !controlsOwned) return;
    const auto live = table(children.controls);
    const int at = slider + 1;
    if (!live.data || at >= live.count || at >= capacity) return;
    const auto& element = live.data[at];
    auto& text = controlText[size_t(at)];
    if (element.owner != children.controls || element.kind != 5 || element.text != text.bytes ||
        controlActions[size_t(at)].row != none) return;
    NativeText desired;
    desired.set(value);
    if (std::strcmp(text.bytes, desired.bytes) == 0) return;
    text.set(value);
    valueRedraw = true; // requested from maintenance, not from inside the slider's callback
}

void NativeSettingsIntegration::refreshStatusCaptions(const ModSettings& liveSettings) {
    // Private GUI-maintenance work inside tick's existing non-reentrancy guard.
    // A status update must not rebuild descriptors, move focus, or change a draft.
    if (valueRedraw && !fault && !closing && !callbackActive() && thread == GetCurrentThreadId() && requestRedraw) {
        valueRedraw = false;
        requestRedraw();
    }
    if (fault || closing || callbackActive() || thread != GetCurrentThreadId() ||
        !controlsOwned || (builtChildPage != 1 && builtChildPage != 4) || !requestRedraw) return;
    const auto live = table(children.controls);
    if (!ownedControls(live)) return;
    static const auto actions = callbacks<true>(std::make_index_sequence<capacity>{});
    // Admit every target before changing any buffer. A foreign second row
    // must not leave a partially refreshed first caption behind.
    for (int i = baseCount; i < live.count; ++i) {
        const int id = controlActions[size_t(i)].row;
        if (controlActions[size_t(i)].hosted != none) continue; // runtime rows reuse small ids
        if (id != setting::rawMouse && id != setting::tracking) continue;
        const auto& element = live.data[i];
        const auto& text = controlText[size_t(i)];
        if (element.owner != children.controls || element.kind != 2 ||
            element.action != actions[size_t(i)] || element.text != text.bytes) { fail(); return; }
    }
    bool changed = false;
    for (int i = baseCount; i < live.count; ++i) {
        const int id = controlActions[size_t(i)].row;
        if (controlActions[size_t(i)].hosted != none) continue; // runtime rows reuse small ids
        if (id != setting::rawMouse && id != setting::tracking) continue;
        auto& text = controlText[size_t(i)];
        const wchar_t* caption = settingLabel(id);
        if (id == setting::rawMouse && transaction.value.rawMouse && liveSettings.rawMouse &&
            liveSettings.rawMouseFallback) caption = L"Raw: fallback";
        if (id == setting::tracking && transaction.value.tracking && liveSettings.tracking) {
            if (liveSettings.trackingStatus == 1) caption = L"Tracking: waiting";
            if (liveSettings.trackingStatus == 3) caption = L"Tracking: error";
        }
        NativeText desired;
        desired.set(caption);
        if (std::strcmp(text.bytes, desired.bytes) != 0) {
            text.set(caption);
            changed = true;
        }
    }
    if (changed) requestRedraw();
}

bool NativeSettingsIntegration::callbackActive() const { return callbackDepth != 0 || handlerDepth != 0; }

void NativeSettingsIntegration::observe() {
    if (!root || fault || callbackActive() || updating) return;
    const int member = contains(root);
    if (member < 0) fail();
    else if (!member) retire();
    else if (!closing) installCheckboxHandlers();
}

NativeElement NativeSettingsIntegration::unscaled(NativeElement live, const NativeElement& canonical) {
    live.x = canonical.x; live.y = canonical.y; live.width = canonical.width;
    live.height = canonical.height; live.style = canonical.style;
    live.flags &= ~0x1000u;
    return live;
}

bool NativeSettingsIntegration::nativeRoot(Table live, Table source) const {
    if (!live.data || !source.data || live.count != 9 || source.count != 9) return false;
    for (int i=0;i<9;++i)
        if (live.data[i].action != source.data[i].action || live.data[i].kind != source.data[i].kind ||
            live.data[i].owner != live.data[0].owner) return false;
    return live.data[0].width > 0 && (live.data[0].flags & 0x1000);
}

bool NativeSettingsIntegration::nativeKeyboard(Table t) const {
    const auto source = controlsBasis();
    if (!t.data || !source.data || t.count != 44 || source.count != 44) return false;
    for (int i=0;i<44;++i)
        if (t.data[i].kind != source.data[i].kind || t.data[i].action != source.data[i].action) return false;
    return true;
}

bool NativeSettingsIntegration::ownedRoot(Table t) const {
    // The first tick admits the stock table before installing callbacks.
    if (dirty && t.count == 9) return nativeRoot(t, rootBasis());
    // The 9 native elements and at least the Mod Settings tab.
    return t.data && t.count == rootCount && t.count >= 10 &&
           t.data[0].action == activated<0, false> &&
           t.data[7].action == activated<7, false> &&
           t.data[9].text == rootText[9].bytes && t.data[9].owner == root;
}

bool NativeSettingsIntegration::ownedControls(Table t) const {
    return t.data && t.count == controlCount && t.count > baseCount &&
           t.data[baseCount].text == controlText[baseCount].bytes && t.data[baseCount].owner == children.controls;
}

bool NativeSettingsIntegration::nativeController(Table t) const {
    const auto source = controllerBasis();
    if (!t.data || !source.data || t.count != 5 || source.count != 5) return false;
    for (int i=0;i<5;++i)
        if (t.data[i].kind != source.data[i].kind || t.data[i].action != source.data[i].action) return false;
    return true;
}

void NativeSettingsIntegration::divider(std::vector<NativeElement>& out,const wchar_t* title,bool first) {
    add<true>(out,none,0,52,first ? 2 : -32760,324+(widened ? widthGain : 0),title,5);
    out.back().appearance=0x3300; // font metrics center in native type-five label
}

void NativeSettingsIntegration::captureRowWidget(std::vector<NativeElement>& out,int id,int y) {
    const int flags = transaction.value.inactiveReason(id) ? 0x10 : 0;
    const wchar_t* title = id==setting::rotateHeldKey ? L"Rotate held item" : id==setting::trackingKey ? L"Recenter tracking" : settingLabel(id);
    const int labelWidth = columnWidth*2/5;
    add<true>(out,none,0,columnX,y,labelWidth,fitLabel(title,labelWidth),5,flags);
    const auto text=captureRow==id ? L"Press key..." : settingValue(transaction.value,id);
    add<true>(out,id,0,columnX+labelWidth+4,-32768,columnWidth-labelWidth-4,text,5,flags);
    auto& key=out.back();
    key.appearance=0x20003380; // same native clickable binding-value presentation as Controls
    controlActions[out.size()-1].capture=true;
}

void NativeSettingsIntegration::rebuildRoot() {
    auto t = table(root), canonical = rootBasis();
    if (!ownedRoot(t) || canonical.count != 9) { fail(); return; }
    std::vector<NativeElement> records;
    for (int i = 0; i < 9; ++i) records.push_back(unscaled(t.data[i], canonical.data[i]));
    static const auto functions = callbacks<false>(std::make_index_sequence<capacity>{});
    for (int i : {0, 1, 6, 7, 8}) records[size_t(i)].action = functions[size_t(i)];
    const int x = canonical.data[0].x, gain = widened ? widthGain : 0;
    const int gap = canonical.data[1].x - x - canonical.data[0].width;
    const int span = canonical.data[1].x + canonical.data[1].width - x + gain;
    // The game's two tabs, then the Mod Settings page(s); every ExCamera setting lives there.
    std::vector<int> shown = {0, 1};
    for (int i = ownTabs; i < tabCount; ++i) shown.push_back(i);
    const int slots = int(shown.size());
    const int width = ((span - (slots - 1) * gap) / slots / 4) * 4;
    if (gap < 0 || width < 32) { fail(); return; }
    rootText[0].set(L"Graphics"); records[0].text = rootText[0].bytes;
    rootText[1].set(L"Controls"); records[1].text = rootText[1].bytes; // the game's own tab is the one in capitals
    for (int i : {4, 5}) records[size_t(i)].width = int16_t(records[size_t(i)].width + gain); // the rules under the tabs
    records[6].x = int16_t(records[6].x + gain / 2); // Defaults; Apply and Cancel flow after it
    for (int i = 0; i < 2; ++i) {
        records[size_t(i)].x = int16_t(x + i * (width + gap));
        records[size_t(i)].width = int16_t(width);
        records[size_t(i)].flags = (records[size_t(i)].flags & ~0x60u) | (page == i ? 0x60 : 0);
    }
    for (int i = 2; i < 6; ++i)
        if (page >= 2) records[size_t(i)].flags |= 0x110;
    for (int slot = 2; slot < slots; ++slot) {
        const int i = shown[size_t(slot)];
        const std::wstring& title = hostedTitles[size_t(i - ownTabs)];
        add<false>(records, pageEvent + i, 0, x + slot * (width + gap), canonical.data[0].y,
                   width, fitButton(title, width), 1, page == i ? 0x60 : 0);
        auto& e = records.back(); e.height = canonical.data[0].height;
        e.group = 1; e.appearance = canonical.data[0].appearance;
    }
    // The Mod Settings rows live in the existing scrollable native Controls pane.
    if (fault || records.size() > capacity) { fail(); return; }
    rootCount = int(records.size());
    replace(root, records.data(), rootCount);
    if (page >= 2) {
        visible(children.graphics,false); visible(children.controls,true);
        if (children.controller) visible(children.controller,false);
    }
}

void NativeSettingsIntegration::rebuildControls(Table live) {
    if (!controlsOwned) baseCount=nativeController(live) ? 5 : 44;
    const auto canonical=baseCount==5 ? controllerBasis() : controlsBasis();
    if (canonical.count!=baseCount ||
        (!controlsOwned && !nativeKeyboard(live) && !nativeController(live)) ||
        (controlsOwned && !ownedControls(live))) { fail(); return; }
    if (!controlsOwned) {
        originalExtent=contentHeight(children.controls);
        extentScale=scale();
    }
    // Preserve native keybind text and callbacks *before* hiding the table,
    // and refresh only when last presentation was genuinely Controls.
    if (!baseSaved || !controlsOwned || builtChildPage==1) {
        for(int i=0;i<baseCount;++i)
            originalControls[size_t(i)]=unscaled(live.data[i],canonical.data[i]);
        baseSaved=true;
    }
    std::vector<NativeElement> records(originalControls.begin(),originalControls.begin()+baseCount);
    insetScrollPane(page >= ownTabs);
    if (page < ownTabs) {
        // Graphics and Controls are the game's own, keyboard and controller alike.
        if (controlsOwned) {
            replace(children.controls,records.data(),baseCount);
            setContentHeight(children.controls,baselineExtent());
        }
        if (scrollOwned) setScrolling(children.controls,originalScrolling);
        scrollOwned=false;
        controlsOwned=false; builtChildPage=page;
        return;
    }
    // Do not rely on the game's initial scrolling flag: the page's rows must stay reachable.
    if (!scrollOwned) { originalScrolling=scrolling(children.controls); scrollOwned=true; }
    setScrolling(children.controls,true);
    for(auto& e:records) e.flags|=0x110; // the vanilla binds stay in the table, invisible and inert
    int lines=0;
    hostedRows(records,lines);
    const int pendingExtent=(lines+4)*12*scale();
    if(fault || records.size()>capacity) {fail();return;}
    controlCount=int(records.size());
    replace(children.controls,records.data(),controlCount);
    setContentHeight(children.controls,pendingExtent);
    builtChildPage=page;
    controlsOwned=true;
    installCheckboxHandlers();
}

// A hosted page's rows as the same native widgets ExCamera's own rows use (row(), divider(), captureRowWidget()).
// Rows beyond the element capacity are left out.
// ExCamera's own settings, by category and subcategory, ahead of the runtime's mods on the Mod Settings page.
// The game's Graphics and Controls tabs carry nothing of ExCamera's.
int NativeSettingsIntegration::ownSectionCount() const {
    const int all = int(std::size(ownSections));
    return vrTab ? all : all - 3; // no headset code in this build: its rows would do nothing
}
const OwnSection& NativeSettingsIntegration::ownSection(int index) { return ownSections[index]; }
// ExCamera's categories in table order; each is one entry in the list and one page of details.
std::vector<const wchar_t*> NativeSettingsIntegration::ownCategories() const {
    std::vector<const wchar_t*> out;
    for (int k = 0; k < ownSectionCount(); ++k)
        if (out.empty() || wcscmp(out.back(), ownSections[k].category)) out.push_back(ownSections[k].category);
    return out;
}

// The Mod Settings page. The left third lists the sections under their categories, ExCamera's first; the right
// two thirds show the chosen section's rows. Native rows flow by line, so line i carries left entry i (when
// there is one) and then right line i on the same line.
void NativeSettingsIntegration::hostedRows(std::vector<NativeElement>& out, int& lines) {
    if (page < ownTabs || page >= tabCount) return;
    const int key = page - ownTabs, tab = hostedPages[size_t(key)], gain = widened ? widthGain : 0;
    std::array<HostedRow, 64> rows{};
    int count = 0;
    if (tab >= 0 && runtimeTabs.ready())
        count = (std::min)(runtimeTabs.rows(tab, 0, rows.data(), int(rows.size())), int(rows.size()));
    auto mark = [&](int id, int kind, bool capture = false) {
        auto& a = controlActions[out.size() - 1];
        a.row = id; a.hosted = tab; a.hostedKind = kind; a.capture = capture;
    };
    std::array<int, 64> starts{};
    int runtimeSections = 0;
    for (int i = 0; i < count; ++i)
        if (rows[size_t(i)].kind == 0) starts[size_t(runtimeSections++)] = i;
    // Entries 0 .. own-1 are ExCamera's categories (all their sections on one details page, so the list stays
    // short enough not to scroll); the runtime's mods follow.
    const std::vector<const wchar_t*> categories = ownCategories();
    const int own = int(categories.size()), sections = own + runtimeSections;
    int& chosen = hostedSection[size_t(key)];
    chosen = sections ? ((chosen % sections) + sections) % sections : 0;
    const int shown = chosen;

    // Left: the layout switch, then headers and section buttons, by category (the default) or by mod.
    struct Left { int section; std::wstring text; };
    std::vector<Left> left;
    left.push_back({layoutToggle, modsByMod ? L"By mod" : L"By category"});
    if (modsByMod) left.push_back({-1, L"ExCamera"});
    for (int k = 0; k < own; ++k) left.push_back({k, categories[size_t(k)]});
    if (modsByMod) {
        if (runtimeSections) left.push_back({-1, L"Mods"});
        for (int k = 0; k < runtimeSections; ++k) {
            const wchar_t* title = rows[size_t(starts[size_t(k)])].label;
            left.push_back({own + k, title ? title : L""});
        }
    }
    if (!modsByMod) {
        // PROVISIONAL(2026-10-22, categories: the runtime's mod_settings_category when it exports one, else
        // the runtime's own pages under Utility and every mod under Gameplay, ratify-by: 2026-10-29)
        static const wchar_t* const order[] = {L"Graphics", L"Camera", L"Gameplay", L"Content", L"Utility"};
        std::vector<std::pair<std::wstring, int>> tagged;
        for (int k = 0; k < runtimeSections; ++k) {
            const auto& d = rows[size_t(starts[size_t(k)])];
            wchar_t name[32]{};
            if (!runtimeTabs.category || runtimeTabs.category(d.id, name, 32) <= 0) {
                const std::wstring title = d.label ? d.label : L"";
                wcscpy_s(name, title == L"Profile" || title == L"Lua runtime" ? L"Utility" : L"Gameplay");
            }
            tagged.push_back({name, k});
        }
        auto rank = [&](const std::wstring& c) {
            for (int k = 0; k < int(std::size(order)); ++k) if (c == order[k]) return k;
            return int(std::size(order));
        };
        std::stable_sort(tagged.begin(), tagged.end(),
                         [&](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
        std::vector<std::pair<std::wstring, std::vector<int>>> groups;
        for (const auto& t : tagged) {
            if (groups.empty() || groups.back().first != t.first) groups.push_back({t.first, {}});
            groups.back().second.push_back(t.second);
        }
        // More mods than the pane shows at once: each category header becomes a button and only the open
        // one (the chosen mod's, else the last header clicked) lists its mods, so the list never scrolls.
        const bool collapse = left.size() + groups.size() + tagged.size() > size_t(listLines);
        int open = expandedModGroup;
        for (size_t g = 0; g < groups.size(); ++g)
            for (int k : groups[g].second)
                if (own + k == shown) open = int(g);
        for (size_t g = 0; g < groups.size(); ++g) {
            left.push_back({collapse ? groupHeader - int(g) : -1, L"Mods: " + groups[g].first});
            if (collapse && int(g) != open) continue;
            for (int k : groups[g].second) {
                const wchar_t* title = rows[size_t(starts[size_t(k)])].label;
                left.push_back({own + k, title ? title : L""});
            }
        }
    }
    // The list takes two fifths, enough for a section name in the button face; the details the rest.
    const int whole = 354 + gain, leftWidth = whole * 2 / 5 - 4;
    const int x = 16 + whole * 2 / 5 + 4, width = 370 + gain - x;
    const int labelWidth = width * 2 / 5, controlX = x + labelWidth + 4, valueWidth = 56;

    // Right lines: {first, second} indices, two switches side by side. ExCamera's rows are setting ids, the
    // runtime's are indices into rows.
    const bool ownShown = shown < own;
    std::vector<std::pair<int, int>> right;
    auto isSwitch = [&](int v) {
        return ownShown ? rowInfo(v).checkbox() : rows[size_t(v)].kind == 1;
    };
    if (ownShown) {
        // Every section of the chosen category, each under its subcategory title (first < sectionTitle).
        for (int k = 0; k < ownSectionCount(); ++k) {
            const OwnSection& o = ownSections[k];
            if (wcscmp(o.category, categories[size_t(shown)])) continue;
            right.push_back({sectionTitle - k, -1});
            for (int r = 0; r < o.count; ++r) {
                const int id = o.rows[r];
                if (id <= subHeader) { right.push_back({headerLine - (subHeader - id), -1}); continue; }
                if (id == setting::trackingKey && !transaction.value.tracking) continue;
                right.push_back({id, -1}); // one setting a line
            }
        }
    } else if (runtimeSections) {
        const int section = shown - own;
        const int from = starts[size_t(section)] + 1, to = section + 1 < runtimeSections ? starts[size_t(section + 1)] : count;
        for (int i = from; i < to; ++i) right.push_back({i, -1}); // one setting a line
    }
    std::vector<std::wstring> help;
    wchar_t text[512]{};
    bool helpFound = false;
    if (ownShown) {
        bool listed = false;
        for (const auto& r : right) listed = listed || (r.first >= 0 && r.first == ownHelpId);
        const wchar_t* tip = listed ? settingTip(ownHelpId) : nullptr;
        if (tip) helpFound = wcsncpy_s(text, tip, _TRUNCATE) != EINVAL;
    } else if (runtimeTabs.help && hostedHelpTab == tab) {
        bool listed = false;
        for (const auto& r : right)
            listed = listed || (r.first >= 0 && rows[size_t(r.first)].kind != 0 && rows[size_t(r.first)].id == hostedHelpId);
        helpFound = listed && runtimeTabs.help(hostedHelpId, text, int(std::size(text))) > 0;
    }
    {
        if (helpFound) {
            const size_t most = size_t((std::max)(width / 7, 8));
            std::wstring rest = text;
            while (!rest.empty() && help.size() < 3) {
                size_t cut = rest.size() <= most ? rest.size() : rest.rfind(L' ', most);
                if (cut == std::wstring::npos || cut == 0) cut = (std::min)(most, rest.size());
                std::wstring part = rest.substr(0, cut);
                rest.erase(0, (std::min)(rest.size(), cut + (cut < rest.size() && rest[cut] == L' ')));
                if (help.size() == 2 && !rest.empty()) part = fitLabel(part + L" " + rest, width);
                help.push_back(part);
            }
        }
    }

    const size_t total = (std::max)(left.size(), right.size() + help.size());
    columnX = x; columnWidth = width;
    for (size_t line = 0; line < total && !fault; ++line) {
        if (out.size() + 4 > size_t(capacity)) break;
        int y = line == 0 ? 2 : -32760; // the first element of a line starts it; the rest join it
        if (line < left.size()) {
            const Left& l = left[line];
            if (l.section == -1) {
                add<true>(out, none, 0, 16, y, leftWidth, fitLabel(l.text, leftWidth), 5, 0x10);
            } else {
                add<true>(out, 0, 1, 16, y, leftWidth, fitButton(l.text, leftWidth), 1, l.section == shown ? 0x60 : 0);
                auto& a = controlActions[out.size() - 1];
                a.row = l.section; a.hosted = page; a.hostedKind = sectionPicker;
            }
            y = -32768;
        }
        if (line >= right.size()) {
            if (line - right.size() < help.size())
                add<true>(out, none, 0, x, y, width, help[line - right.size()], 5, 0x10);
            continue;
        }
        const int first = right[line].first, second = right[line].second;
        if (ownShown && first <= headerLine) {
            add<true>(out, none, 0, x, y, width, ownHeaders[headerLine - first], 5, 0x10);
            continue;
        }
        if (ownShown && first <= sectionTitle) {
            add<true>(out, none, 0, x, y, width, ownSections[sectionTitle - first].title, 5);
            out.back().appearance = 0x3300;
            continue;
        }
        if (ownShown) {
            row<true>(out, first, y, second >= 0 ? 1 : 0);
            if (second >= 0) row<true>(out, second, y, 2);
            continue;
        }
        const auto& r = rows[size_t(first)];
        const std::wstring label = r.label ? r.label : L"", value = r.value ? r.value : L"";
        const unsigned flags = r.inactive ? 0x10 : 0;
        switch (r.kind) {
        case 0:
            add<true>(out, none, 0, x, y, width, label, 5);
            out.back().appearance = 0x3300;
            break;
        case 1: {
            const int w = second >= 0 ? width / 2 - 4 : width;
            add<true>(out, 0, 1, x, y, w, fitLabel(label, w - 24), 2, flags | (r.checked ? widget::checked : 0));
            mark(r.id, 1);
            if (second >= 0) {
                const auto& r2 = rows[size_t(second)];
                const std::wstring label2 = r2.label ? r2.label : L"";
                add<true>(out, 0, 1, x + width / 2 + 4, -32768, w, fitLabel(label2, w - 24), 2,
                          (r2.inactive ? 0x10u : 0u) | (r2.checked ? widget::checked : 0u));
                mark(r2.id, 1);
            }
            break;
        }
        case 2:
            add<true>(out, none, 0, x, y, labelWidth, fitLabel(label, labelWidth), 5, flags);
            add<true>(out, 0, 1, controlX, -32768, width - labelWidth - valueWidth - 8, L"", 4, flags, true, r.position,
                      (std::max)(r.steps, 1));
            mark(r.id, 2);
            add<true>(out, none, 0, x + width - valueWidth, -32768, valueWidth, fractionAsPercent(value).substr(0, 10), 5, flags);
            out.back().appearance = 0x3300;
            break;
        case 3:
            add<true>(out, none, 0, x, y, labelWidth, fitLabel(label, labelWidth), 5, flags);
            add<true>(out, 0, 1, controlX, -32768, width - labelWidth - 4, fitButton(value, width - labelWidth - 4), 1, flags);
            mark(r.id, 3);
            break;
        case 4: {
            add<true>(out, none, 0, x, y, labelWidth, fitLabel(label, labelWidth), 5, flags);
            const bool capturing = captureRow == hostedCaptureRow && hostedCaptureTab == tab && hostedCaptureId == r.id;
            add<true>(out, 0, 0, controlX, -32768, width - labelWidth - 4,
                      capturing ? std::wstring(L"Press key...") : value, 5, flags);
            out.back().appearance = 0x20003380;
            mark(r.id, 4, true);
            break;
        }
        default:
            add<true>(out, none, 0, x, y, width, value.empty() ? label : label + L"  " + value, 5, flags);
            break;
        }
    }
    columnX = 16; columnWidth = 354 + gain;
    lines += int(total);
}

void NativeSettingsIntegration::hostedEvent(int tab, int event) {
    if (runtimeTabs.ready()) runtimeTabs.event(tab, event);
}

void NativeSettingsIntegration::restore() {
    if (callbackActive()) return;
    if (!ownedRoot(table(root))) { fail(); return; }
    auto ct = table(children.controls);
    if (controlsOwned && ownedControls(ct)) {
        if (!baseSaved) {fail();return;}
        replace(children.controls,originalControls.data(),baseCount);
        setContentHeight(children.controls, baselineExtent());
        if (scrollOwned) setScrolling(children.controls,originalScrolling);
    } else if (controlsOwned) {
        // A foreign child-table replacement must remain untouched, but it
        // does not prevent restoring our still-owned root controls.
        if (!nativeKeyboard(ct) && !nativeController(ct)) {
            controlsOwned = false;
            scrollOwned = false;
            baseSaved = false;
        }
    }
    const auto live = table(root), canonical = rootBasis();
    std::array<NativeElement, 9> original;
    for (int i = 0; i < 9; ++i) {
        original[size_t(i)] = unscaled(live.data[i], canonical.data[i]);
        original[size_t(i)].action = originals[size_t(i)];
    }
    original[0].text = canonical.data[0].text;
    original[1].text = canonical.data[1].text;
    narrow();
    if (page >= 2) {
        original[0].flags |= 0x60;
        original[1].flags &= ~0x60u;
    }
    replace(root, original.data(), int(original.size()));
    if (page >= 2 && !closing) {
        auto stock = table(root);
        originals[0](&stock.data[0]);
    }
    root = 0; controlsOwned = false; paneInset = false; scrollOwned=false; baseSaved=false; captureRow=none;
    deferUntilMouseRelease=false;
    callbackOwner = nullptr;
    hostedEvent(-1,4);
    tabCount=ownTabs;
}

bool NativeSettingsIntegration::recoverFault() {
    if (!fault || callbackActive() || !thread || thread != GetCurrentThreadId()) return false;
    if (!root) return true;
    const int member = contains(root);
    if (member == 0) {
        retire(); // the native form is gone; never dereference its retired tables
        return true;
    }
    if (member != 1 || !ownedRoot(table(root))) return false;
    restore(); // restore() independently rechecks root ownership before replacement
    if (root != 0) return false;
    if (onClosed) onClosed();
    return true;
}

void NativeSettingsIntegration::retire() {
    if (callbackActive()) return;
    root=0; children={}; paneInset=false; widened=false; captureRow=none; baseSaved=false;
    controlsOwned=false; scrollOwned=false; dirty=false;
    deferUntilMouseRelease=false;
    callbackOwner = nullptr;
    hostedEvent(-1,4);
    tabCount=ownTabs;
    if (onClosed) onClosed();
}

int NativeSettingsIntegration::baselineExtent() const { return int(int64_t(originalExtent) * scale() / extentScale); }

// Grow the form and its panes by widthGain, only when every frame reads as the game built it:
// the root at its constructor size, the Graphics pane 8 units narrower, Controls 16, the
// Controller pane 16 narrower than Controls, and the root already placed by the desktop. The game may
// hold the frames at a scale; the Graphics inset gives the factor and the gain follows it. The
// root shifts left by half the gain to stay centred, and the Graphics pane, whose rows are the game's own,
// moves right by the same half instead of growing. The frames read are kept for the status line.
void NativeSettingsIntegration::widen() {
    if (widened || !frame || !setFrame) return;
    constexpr int unplaced = int(0xC521974Fu); // the game's marker for a form the desktop has not placed yet
    Frame r{}, g{}, c{}, k{};
    if (!frame(root, r) || !frame(children.graphics, g) || !frame(children.controls, c)) return;
    if (children.controller && !frame(children.controller, k)) return;
    frames = {r, g, c, k};
    const double s = (r.width - g.width) / 8.0;
    frameScale = int(s * 100 + (s < 0 ? -.5 : .5));
    // Hellmode's form reads 1066 high at scale 2, not 1072: allow a few units per scale step.
    if (s < .9 || s > 8 || std::abs(r.width - nativeWidth * s) > 4 * s || std::abs(r.height - nativeHeight * s) > 8 * s ||
        std::abs((r.width - c.width) - 16 * s) > 2 || r.x == unplaced || r.y == unplaced) return;
    if (children.controller && std::abs((c.width - k.width) - 16 * s) > 2) return;
    originalFrames = {r, g, c, k};
    const int gain = int(widthGain * s + .5);
    r.x -= gain / 2; r.width += gain; setFrame(root, r);
    g.x += gain / 2; setFrame(children.graphics, g); // the game's own rows keep their width: centre them
    c.width += gain; setFrame(children.controls, c);
    if (children.controller) { k.width += gain; setFrame(children.controller, k); }
    widenedFrames = {r, g, c, k};
    widened = true;
}

// The game can lay the form out again at its own size while it is open; the widened width is put back when
// it differs. Only the root's: the panes' frames follow their scroll bars, and setting them every tick reset
// the scroll position and left the thumb where the old frame had it.
// The Controls pane's scroll bar sits at its own right edge, past the game's frame rule; the vanilla list never
// scrolls, so it never shows. While the Mod Settings page scrolls, the pane is narrowed by the bar's width so the
// bar lands inside the frame; any other page gets the pane back as it was.
void NativeSettingsIntegration::insetScrollPane(bool inset) {
    if (!frame || !setFrame || !children.controls || inset == paneInset) return;
    if (inset) {
        if (!frame(children.controls, paneHome)) return;
        const int s = frameScale > 0 ? frameScale : 100;
        setFrame(children.controls, {paneHome.x, paneHome.y, paneHome.width - 24 * s / 100, paneHome.height});
    } else {
        setFrame(children.controls, paneHome);
    }
    paneInset = inset;
}
void NativeSettingsIntegration::holdWidened() {
    if (!widened || !frame || !setFrame) return;
    Frame now{};
    if (frame(root, now) && now.width != widenedFrames[0].width) setFrame(root, widenedFrames[0]);
}

void NativeSettingsIntegration::narrow() {
    insetScrollPane(false);
    if (!widened) return;
    widened = false;
    if (contains(root) != 1) return; // the game already released the form
    setFrame(root, originalFrames[0]);
    setFrame(children.graphics, originalFrames[1]);
    setFrame(children.controls, originalFrames[2]);
    if (children.controller) setFrame(children.controller, originalFrames[3]);
}
} // namespace panel
