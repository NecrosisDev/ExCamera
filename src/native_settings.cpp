#include "native_settings.h"
#include "settings_presentation.h"
#include <cstring>

namespace panel {
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
        const int row=self->controlActions[Index].row;
        if (row<0 || row>=setting::count) return 0;
        if (!e || !self->controlsOwned || !self->children.controls ||
            e->owner!=self->children.controls || e->action!=activated<Index,true> ||
            e->handler!=reinterpret_cast<uintptr_t>(checkboxEvent<Index>) ||
            !widget::checkboxAction(*e,row)) return 0;
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
    const uintptr_t owner = Controls ? self->children.controls : self->root;
    if (!owner || e->owner != owner || e->action != activated<Index, Controls>) return;
    const auto live = self->table(owner);
    if (!live.data || Index >= live.count || &live.data[Index] != e) return;
    const auto action = Controls ? self->controlActions[Index] : self->rootActions[Index];
    if (!widget::activation(*e,action.row,action.capture,Controls)) return;
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
            self->originals[Index](e);
        } else if constexpr (Index == 6) {
            // Controller Controls is entirely native. Its Defaults must not
            // reset the ExCamera rows that are only shown in keyboard mode.
            // Decide before native Defaults can rebuild the child table.
            bool resetControls=false;
            if (self->page==1) {
                const auto child=self->table(self->children.controls);
                resetControls=self->nativeKeyboard(child) ||
                    (self->controlsOwned && self->baseCount==44 && self->ownedControls(child));
            }
            if (self->page < 2) self->originals[Index](e);
            if (resetControls || self->page>=2)
                self->transaction.resetGroup(self->page == 1 ? 1 : self->page == 2 ? 0 : 2);
            self->dirty=true;
        }
    } else if (action.row >= pageEvent && action.row < pageEvent+4) {
        self->page=action.row-pageEvent;
        self->captureRow=none;
        self->dirty=true;
    } else if (action.capture) {
        if (action.row==setting::rotateHeldKey ||
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
        } else if (widget::checkboxAction(*e,action.row)) {
            // 0x40 is also hover preview. Only the native selected bit is state.
            const bool selected=(e->flags & 0x20)!=0;
            if (self->handlerDepth && (e->flags & widget::buttonTransient))
                self->nativeActions.fetch_add(1,std::memory_order_relaxed);
            if (info.flag) self->transaction.value.*(info.flag)=selected;
            else self->transaction.value.crouchMode=selected ? 2 : 1;
            self->transaction.value.normalize();
        } else self->transaction.step(action.row,action.delta);
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
        if (action.row<0 || action.row>=setting::count || !rowInfo(action.row).checkbox()) continue;
        if (e.owner!=children.controls || e.action!=actions[size_t(i)] || !widget::checkboxAction(e,action.row)) { fail(); return; }
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
        (Controls && (x < 0 || x + width > 376))) { fail(); return; }
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

template<bool Controls>
void NativeSettingsIntegration::row(std::vector<NativeElement>& out,int id,int y) {
    const RowInfo& info=rowInfo(id);
    const unsigned flags=transaction.value.inactiveReason(id) ? 0x10 : 0;
    const wchar_t* label=settingLabel(id);
    const int align=Controls ? -32768 : y;
    if (id==setting::rotateHeldKey || id==setting::trackingKey) {
        if constexpr (Controls) captureRowWidget(out,id);
        return;
    }
    if (info.checkbox()) {
        const bool selected = info.flag ? transaction.value.*(info.flag) : transaction.value.crouchMode==2;
        add<Controls>(out,id,1,80,y,292,id==setting::crouchMode ? L"Toggle crouch" : label,2,
                      flags|(selected ? widget::checked : 0));
        return;
    }
    add<Controls>(out,none,0,16,y,148,label,5,flags);
    const SettingRange& range=info.range;
    if (range.count) {
        const std::wstring value=settingValue(transaction.value,id);
        if (value.size()>10) { fail(); return; }
        add<Controls>(out,id,1,176,align,116,L"",4,flags,true,
                      SettingsDraft::sliderPosition(transaction.value,id),range.count);
        add<Controls>(out,none,0,300,align,70,value,5,flags);
        // The text is right-aligned within the shorter final field.
        out.back().appearance=0x4000;
    } else if (ModSettings::isNumber(id)) {
        // Direct binding rows are special-cased above. Other key lists are not shown.
        add<Controls>(out,id,1,176,align,194,settingValue(transaction.value,id),1,flags);
    } else {
        add<Controls>(out,id,1,176,align,194,settingValue(transaction.value,id),1,flags);
    }
}

uintptr_t NativeSettingsIntegration::handle() const { return root; }

bool NativeSettingsIntegration::retired() const { return root == 0 && !applyQueued; }

bool NativeSettingsIntegration::failed() const { return fault; }

void NativeSettingsIntegration::fail() { fault = true; }

int NativeSettingsIntegration::selectedPage() const { return page; }

int NativeSettingsIntegration::selectedSection() const { return 0; }

int NativeSettingsIntegration::captureTarget() const { return root && !closing && !fault ? captureRow : none; }

bool NativeSettingsIntegration::completeCapture(int key) {
    if (callbackActive() || captureRow == none || closing || fault || thread != GetCurrentThreadId()) return false;
    const int id=captureRow;
    captureRow=none;
    if (key < 0) { dirty=true; return true; } // Escape cancels
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
    if (applyQueued) { applyQueued = false; apply(acceptedInitial, acceptedValue); }
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
        transaction.begin(settings());
        closing = false;
        for (int i = 0; i < 9; ++i) originals[size_t(i)] = t.data[i].action;
        page = (t.data[1].flags & 0x20) ? 1 : 0;
        callbackOwner = this;
        dirty = true;
    }
    if (!ownedRoot(table(root))) { fail(); return; }
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
    refreshStatusCaptions(liveSettings);
}

void NativeSettingsIntegration::refreshStatusCaptions(const ModSettings& liveSettings) {
    // Private GUI-maintenance work inside tick's existing non-reentrancy guard.
    // A status update must not rebuild descriptors, move focus, or change a draft.
    if (fault || closing || callbackActive() || thread != GetCurrentThreadId() ||
        !controlsOwned || builtChildPage != 1 || !requestRedraw) return;
    const auto live = table(children.controls);
    if (!ownedControls(live)) return;
    static const auto actions = callbacks<true>(std::make_index_sequence<capacity>{});
    // Admit every target before changing any buffer. A foreign second row
    // must not leave a partially refreshed first caption behind.
    for (int i = baseCount; i < live.count; ++i) {
        const int id = controlActions[size_t(i)].row;
        if (id != setting::rawMouse && id != setting::tracking) continue;
        const auto& element = live.data[i];
        const auto& text = controlText[size_t(i)];
        if (element.owner != children.controls || element.kind != 2 ||
            element.action != actions[size_t(i)] || element.text != text.bytes) { fail(); return; }
    }
    bool changed = false;
    for (int i = baseCount; i < live.count; ++i) {
        const int id = controlActions[size_t(i)].row;
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
    return t.data && t.count == rootCount && t.count >= 11 &&
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
    add<true>(out,none,0,52,first ? 2 : -32760,324,title,5);
    out.back().appearance=0x3300; // font metrics center in native type-five label
}

void NativeSettingsIntegration::captureRowWidget(std::vector<NativeElement>& out,int id) {
    const int flags = transaction.value.inactiveReason(id) ? 0x10 : 0;
    const wchar_t* title = id==setting::rotateHeldKey ? L"Rotate held item" : L"Recenter tracking";
    add<true>(out,none,0,80,-32760,124,title,5,flags);
    const auto text=captureRow==id ? L"Press key..." : settingValue(transaction.value,id);
    add<true>(out,id,0,232,-32768,136,text,5,flags);
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
    const int x = canonical.data[0].x;
    const int gap = canonical.data[1].x - x - canonical.data[0].width;
    const int span = canonical.data[1].x + canonical.data[1].width - x;
    const int width = ((span - 3 * gap) / 4 / 4) * 4;
    if (gap < 0 || width < 32) { fail(); return; }
    rootText[0].set(L"Graphics"); records[0].text = rootText[0].bytes;
    for (int i = 0; i < 2; ++i) {
        records[size_t(i)].x = int16_t(x + i * (width + gap));
        records[size_t(i)].width = int16_t(width);
        records[size_t(i)].flags = (records[size_t(i)].flags & ~0x60u) | (page == i ? 0x60 : 0);
    }
    for (int i = 2; i < 6; ++i)
        if (page >= 2) records[size_t(i)].flags |= 0x110;
    for (int i = 2; i < 4; ++i) {
        add<false>(records, pageEvent + i, 0, x + i * (width + gap), canonical.data[0].y,
                   width, i == 2 ? L"Camera" : L"Combat", 1, page == i ? 0x60 : 0);
        auto& e = records.back(); e.height = canonical.data[0].height;
        e.group = 1; e.appearance = canonical.data[0].appearance;
    }
    // Camera and Combat content live in the existing scrollable native
    // Controls pane, not in an overflowing, non-scrolling root form.
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
    if (page==0 || (page==1 && baseCount==5)) {
        // The native controller list is a child at y=192, not another flow row.
        // Keep controller Controls entirely native rather than overlap that list.
        if (controlsOwned) {
            replace(children.controls,records.data(),baseCount);
            setContentHeight(children.controls,baselineExtent());
        }
        if (scrollOwned) setScrolling(children.controls,originalScrolling);
        scrollOwned=false;
        controlsOwned=false; builtChildPage=0;
        return;
    }
    // ExCamera extends the native keyboard Controls list too. Do not rely on
    // the game's initial scrolling flag: appended rows must remain reachable
    // even when the stock list currently fits the pane.
    const bool needsScrolling = page >= 2 || (page == 1 && baseCount == 44);
    if (needsScrolling) {
        if (!scrollOwned) { originalScrolling=scrolling(children.controls); scrollOwned=true; }
        setScrolling(children.controls,true);
    } else if (scrollOwned) {
        setScrolling(children.controls,originalScrolling);
        scrollOwned=false;
    }
    if (page>=2)
        for(auto& e:records) e.flags|=0x110; // vanilla binds invisible/inert on Camera and Combat

    int lines=0,pendingExtent=baselineExtent();
    if (page==1) {
        divider(records,L"ExCamera controls",false); ++lines;
        captureRowWidget(records,setting::rotateHeldKey); ++lines;
        row<true>(records,setting::sensitivity,-32760); ++lines;
        for (int id : {setting::invertY,setting::rawMouse,setting::crouchMode,
                       setting::freezeWindows,setting::tracking}) {
            row<true>(records,id,-32760); ++lines;
        }
        if(transaction.value.tracking) { captureRowWidget(records,setting::trackingKey); ++lines; }
        pendingExtent=baselineExtent()+(lines+2)*8*scale();
    } else if (page==2) {
        divider(records,L"Perspective",true); ++lines;
        for(int id: {setting::fov,setting::hideHelmet,setting::flickCatchup,
                     setting::tpFov,setting::tpDistance,setting::tpShoulder,
                     setting::tpHeight,setting::tpLag}) {
            row<true>(records,id,-32760); ++lines;
        }
        divider(records,L"Behavior"); ++lines;
        for(int id: {setting::tpTight,setting::tpCombat,
                     setting::tpDown,setting::tpCombatAim}) {
            row<true>(records,id,-32760); ++lines;
        }
        pendingExtent=(lines+3)*8*scale();
    } else if (page==3) {
        divider(records,L"Defense",true); ++lines;
        for(int id: {setting::fpDefense,setting::fpSwingSelection,setting::indicator}) {
            row<true>(records,id,-32760); ++lines;
        }
        divider(records,L"Attack indicators"); ++lines;
        for(int id: {setting::attackCue,setting::cueSize,setting::cueOpacity,setting::cueThrust}) {
            row<true>(records,id,-32760); ++lines;
        }
        pendingExtent=(lines+3)*8*scale();
    }
    if(fault || records.size()>capacity) {fail();return;}
    controlCount=int(records.size());
    replace(children.controls,records.data(),controlCount);
    setContentHeight(children.controls,pendingExtent);
    builtChildPage=page;
    controlsOwned=true;
    installCheckboxHandlers();
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
        if (!nativeKeyboard(ct) && !nativeController(ct)) { fail(); return; }
    }
    const auto live = table(root), canonical = rootBasis();
    std::array<NativeElement, 9> original;
    for (int i = 0; i < 9; ++i) {
        original[size_t(i)] = unscaled(live.data[i], canonical.data[i]);
        original[size_t(i)].action = originals[size_t(i)];
    }
    original[0].text = canonical.data[0].text;
    if (page >= 2) {
        original[0].flags |= 0x60;
        original[1].flags &= ~0x60u;
    }
    replace(root, original.data(), int(original.size()));
    if (page >= 2 && !closing) {
        auto stock = table(root);
        originals[0](&stock.data[0]);
    }
    root = 0; controlsOwned = false; scrollOwned=false; baseSaved=false; captureRow=none;
    deferUntilMouseRelease=false;
    callbackOwner = nullptr;
}

void NativeSettingsIntegration::retire() {
    if (callbackActive()) return;
    root=0; children={}; captureRow=none; baseSaved=false;
    controlsOwned=false; scrollOwned=false; dirty=false;
    deferUntilMouseRelease=false;
    callbackOwner = nullptr;
    if (onClosed) onClosed();
}

int NativeSettingsIntegration::baselineExtent() const { return int(int64_t(originalExtent) * scale() / extentScale); }
} // namespace panel
