// Production WndProc + input ownership, with an owned keyboard and synthetic
// native memory. These tests establish event routing, not native combat timing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool fixtureKeys[256]{}, foreground = true;
static HWND testForeground() {
    return foreground ? (HWND)1 : nullptr;
}
static DWORD testWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
static SHORT WINAPI testKeyState(int key) {
    return key > 0 && key < 256 && fixtureKeys[key] ? SHORT(-32768) : 0;
}
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#define GetAsyncKeyState testKeyState
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#include "test_controlled_player.h"
#include <cstdio>
#include <vector>

static unsigned nativeKeys = 0, checks = 0, failures = 0, forwarded = 0;
static uintptr_t actor = 0, behaviorObject = 0, viewObject = 0, dragObject = 0, heldObject = 0;
static void expect(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
static void poll() {
    *(unsigned*)gameAt(fp::sig::keyWord) = nativeKeys;
    *(unsigned*)gameAt(fp::sig::buttonWord) =
        (fixtureKeys[VK_LBUTTON] ? 1u : 0u) | (fixtureKeys[VK_RBUTTON] ? 2u : 0u);
}
static LRESULT CALLBACK fallback(HWND, UINT, WPARAM, LPARAM) {
    ++forwarded;
    return 42;
}
static WPARAM mouseFlags() {
    return (fixtureKeys[VK_LBUTTON] ? MK_LBUTTON : 0) | (fixtureKeys[VK_RBUTTON] ? MK_RBUTTON : 0) |
           (fixtureKeys[VK_MBUTTON] ? MK_MBUTTON : 0);
}
static LRESULT message(int key, bool pressed, bool repeat = false) {
    fixtureKeys[key] = pressed;
    if (key == VK_LBUTTON)
        return windowProc(nullptr, pressed ? WM_LBUTTONDOWN : WM_LBUTTONUP, mouseFlags(), 0);
    if (key == VK_RBUTTON)
        return windowProc(nullptr, pressed ? WM_RBUTTONDOWN : WM_RBUTTONUP, mouseFlags(), 0);
    if (key == VK_MBUTTON)
        return windowProc(nullptr, pressed ? WM_MBUTTONDOWN : WM_MBUTTONUP, mouseFlags(), 0);
    if (key == VK_XBUTTON1 || key == VK_XBUTTON2)
        return windowProc(nullptr, pressed ? WM_XBUTTONDOWN : WM_XBUTTONUP,
                          MAKEWPARAM(mouseFlags(), key == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2), 0);
    return windowProc(nullptr, pressed ? WM_KEYDOWN : WM_KEYUP, WPARAM(key), repeat ? LPARAM(1LL << 30) : 1);
}
static void held(bool on) {
    *(unsigned*)(dragObject + field::drag::flags) = on ? 0x2000 : 0;
    *(uintptr_t*)(dragObject + field::drag::object) = on ? heldObject : 0;
}
static void reset() {
    for (bool& key : fixtureKeys) key = false;
    nativeKeys = 0;
    fixture::resetInput();
    foreground = true;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(uintptr_t*)gameAt(fp::sig::uiView) = viewObject;
    *(unsigned char*)(actor + field::character::combatStance) = 0;
    *(unsigned*)(actor + field::character::stateFlags) = 0;
    held(false);
    forwarded = 0;
}
#ifndef EXCAMERA_PRE_REVIEW
static void manual() {
    reset();
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    fpManualGuard = tpManualGuard = 1;
    manualDefenseReady = true;
    input(); // Enter this gameplay context with physically released buttons.
}
static void bindingWidthTests() {
    constexpr size_t interactIndex = field::bindings::interact / field::bindings::stride;
    auto interact = (uint32_t*)(gameAt(fp::sig::movementKeys) + field::bindings::interact);
    const uint32_t keptInteract = *interact;
    *interact = 0xbeef0046u; // Native VK_F is the low WORD; adjacent bytes are not part of its binding.
    manual();
    fixtureKeys['F'] = true;
    expect(nativeBinding(interactIndex) == 'F' && !ordinaryCombat(actor, behaviorObject),
           "native WORD interaction retains input even when its adjacent upper WORD is nonzero");
    fixtureKeys['F'] = false;
    input();
    fixtureKeys[VK_RBUTTON] = true;
    input();
    expect(defensePermission(actor) == DefensePermission::held,
           "released native interaction admits a fresh guard hold");
    *interact = 0xcafe0046u;
    input();
    expect(inputContext(actor, behaviorObject).bindings[interactIndex] == 'F' &&
               defensePermission(actor) == DefensePermission::held,
           "changing bytes beyond the WORD binding cannot create an ownership boundary");
    *interact = keptInteract;
    reset();
    auto forward = (uint32_t*)gameAt(fp::sig::movementKeys);
    const uint32_t keptForward = *forward;
    *forward = 0xbeef0057u;
    fixtureKeys['W'] = true;
    expect(nativeBinding(0) == 0xbeef0057u && keyMask('W') == 0 && physicalMovement() == 0 &&
               inputContext(actor, behaviorObject).bindings[0] == 0xbeef0057u,
           "native DWORD movement keeps its full value instead of manufacturing a low-WORD key");
    *forward = keptForward;
    reset();
}
static LRESULT altMessage(bool pressed) {
    fixtureKeys[VK_MENU] = fixtureKeys[VK_LMENU] = pressed;
    return windowProc(nullptr, pressed ? WM_SYSKEYDOWN : WM_SYSKEYUP, VK_MENU,
                      LPARAM(1LL << 29) | (pressed ? 0 : LPARAM(1LL << 31)));
}
static unsigned polledAlternate() {
    return *(unsigned*)gameAt(fp::sig::keyWord) & 0x80u;
}
static void alternateLeanTests() {
    auto alternate = (uint32_t*)(gameAt(fp::sig::movementKeys) + 6 * field::bindings::stride);
    const uint32_t keptAlternate = *alternate;
    for (int mode : {0, 1}) {
        reset();
        viewMode = mode;
        *alternate = VK_MENU;
        leanKey = VK_MENU;
        *(unsigned char*)(actor + field::character::combatStance) = 1;
        input();
        nativeKeys = 0x80;
        expect(altMessage(true) == 42 && !inputOwner.windowOwns(VK_MENU),
               "combat Alt system-key down stays native in both close views");
        input();
        expect(!leaning && polledAlternate(), "combat native alternate survives lean polling");
        *(unsigned char*)(behaviorObject + field::behaviour::pressBlocked) = 1;
        input();
        expect(!leaning && polledAlternate(), "temporary attack blocking cannot give native Alt to lean");
        *(unsigned char*)(behaviorObject + field::behaviour::pressBlocked) = 0;
        expect(altMessage(false) == 42, "native combat Alt keeps its matching system-key release");
    }

    reset();
    *alternate = VK_MENU;
    leanKey = VK_MENU;
    input();
    nativeKeys = 0x80;
    expect(altMessage(true) == 0, "exploration still owns its configured Alt lean press");
    input();
    expect(leaning && !polledAlternate(), "exploration Alt leans without a simultaneous native action");
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    expect(!leaning && !polledAlternate(), "entering combat cannot transfer a held lean press into an attack");
    expect(altMessage(false) == 0, "former lean press retains its release after entering combat");
    nativeKeys = 0;
    input();
    nativeKeys = 0x80;
    expect(altMessage(true) == 42, "fresh Alt after lean release belongs to native combat");
    input();
    expect(!leaning && polledAlternate(), "fresh combat Alt restores native alternate attack");
    *(unsigned char*)(actor + field::character::combatStance) = 0;
    input();
    expect(!leaning && polledAlternate(), "leaving combat cannot turn an existing native Alt hold into lean");
    expect(altMessage(false) == 42, "former native combat press retains its release in exploration");
    nativeKeys = 0;
    input();
    expect(altMessage(true) == 0, "a fresh exploration Alt press can lean again");
    input();
    expect(leaning, "lean resumes after the native press was physically released");

    reset();
    *alternate = 'K';
    leanKey = 'K';
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    nativeKeys = 0x80;
    expect(message('K', true) == 42, "rebound native alternate key takes priority over lean");
    input();
    expect(!leaning && polledAlternate(), "rebound native alternate bit survives polling");
    message('K', false);
    nativeKeys = 0;
    input();
    *alternate = VK_MENU;
    input();
    expect(message('K', true) == 0, "nonconflicting configured lean key remains usable in combat");
    input();
    expect(leaning, "native rebinding removes the former lean conflict");

    reset();
    *alternate = VK_MENU;
    leanKeysMode = true;
    leanLeftKey = VK_MENU;
    leanRightKey = 'J';
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    nativeKeys = 0x80;
    expect(altMessage(true) == 42, "two-key lean also leaves native alternate press untouched");
    input();
    expect(!leaning && polledAlternate(), "two-key lean cannot consume native alternate while polling");
    expect(message('J', true) == 0, "the other nonconflicting lean direction remains available");
    input();
    expect(leaning && leanKeySide == 1 && polledAlternate(),
           "independent lean direction does not erase the held native alternate key");

    const unsigned aliases[][2] = {{VK_MENU, VK_LMENU}, {VK_MENU, VK_RMENU},
                                   {VK_LMENU, VK_MENU}, {VK_RMENU, VK_MENU},
                                   {VK_CONTROL, VK_LCONTROL}, {VK_RCONTROL, VK_CONTROL},
                                   {VK_SHIFT, VK_RSHIFT}, {VK_LSHIFT, VK_SHIFT}};
    for (const auto& keys : aliases) {
        reset();
        *alternate = keys[0];
        leanKey = int(keys[1]);
        *(unsigned char*)(actor + field::character::combatStance) = 1;
        input();
        fixtureKeys[keys[0]] = fixtureKeys[keys[1]] = true;
        nativeKeys = 0x80;
        input();
        expect(!leaning && polledAlternate(), "generic and side-specific modifier aliases cannot bypass combat priority");
    }

    reset();
    *alternate = VK_LMENU;
    leanKey = VK_MENU;
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    expect(altMessage(true) == 42 && !inputOwner.windowOwns(VK_MENU),
           "generic Alt window event respects a side-specific native alternate binding");

    reset();
    *alternate = VK_MENU;
    leanKey = VK_LMENU;
    input();
    fixtureKeys[VK_MENU] = fixtureKeys[VK_LMENU] = true;
    nativeKeys = 0x80;
    input();
    expect(leaning && !polledAlternate(), "side-specific exploration lean owns its generic native modifier");
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    expect(!leaning && !polledAlternate(), "modifier alias cannot transfer a held lean into native alternate at a stance change");
    fixtureKeys[VK_MENU] = fixtureKeys[VK_LMENU] = false;
    nativeKeys = 0;
    input();
    fixtureKeys[VK_MENU] = fixtureKeys[VK_LMENU] = true;
    nativeKeys = 0x80;
    input();
    expect(!leaning && polledAlternate(), "physical alias release admits a later native alternate hold");

    reset();
    *alternate = VK_LMENU;
    leanKey = VK_RMENU;
    *(unsigned char*)(actor + field::character::combatStance) = 1;
    input();
    fixtureKeys[VK_RMENU] = true;
    input();
    expect(leaning, "separately assigned left and right modifiers remain independent");
    fixtureKeys[VK_LMENU] = true;
    nativeKeys = 0x80;
    input();
    expect(leaning && polledAlternate(), "right-modifier lean cannot mask a distinct left-modifier native attack");
    *alternate = keptAlternate;
    reset();
}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fixture::NativeImage storage(0x600000);
    expect(bool(storage), "owned input image allocated");
    if (!storage) return 1;
    actor = storage.object(0x5c0000);
    behaviorObject = storage.object(0x5c2000);
    viewObject = storage.object(0x5c4000);
    dragObject = storage.object(0x5c8000);
    heldObject = storage.object(0x5ca000);
    const fixture::ControlledPlayer player{actor, behaviorObject, storage.object(0x5c6000), viewObject};
    player.connect();
    *(uintptr_t*)gameAt(fp::sig::dragManager) = dragObject;
    fixture::bindings({'W', 'A', 'S', 'D', 'C', 'U', 'E'});
    originalInput = poll;
    originalWindowProc = fallback;
    keyState = testKeyState;

    reset();
    expect(controls(behaviorObject), "shared player fixture reaches the production input adapter");
#ifndef EXCAMERA_PRE_REVIEW
    bindingWidthTests();
    alternateLeanTests();
#endif

    expect(rotateKeyMessage(WM_LBUTTONDOWN, MK_LBUTTON, VK_LBUTTON) == 1,
           "LMB binding decodes physical down");
    expect(rotateKeyMessage(WM_LBUTTONDBLCLK, MK_LBUTTON, VK_LBUTTON) == 1,
           "LMB binding decodes double-click down");
    expect(rotateKeyMessage(WM_LBUTTONUP, 0, VK_LBUTTON) == 0, "LMB binding decodes physical up");

    reset();
    rotateHeldKey = VK_LBUTTON;
    held(true);
    input();
    expect(message(VK_LBUTTON, true) == 0 && !forwarded && !(inputOwner.pendingButtons() & 1),
           "LMB held rotation owns its down without queuing attack");
    input();
    expect(heldRotating && !(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "LMB held rotation removes polled native attack bit");
    expect(message(VK_LBUTTON, false) == 0, "LMB held rotation owns its release");
    input();

    reset();
    leanKey = VK_LBUTTON;
    input();
    expect(message(VK_LBUTTON, true) == 0 && !forwarded && !(inputOwner.pendingButtons() & 1),
           "LMB lean owns its down without queuing attack");
    input();
    expect(leaning && !(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "LMB lean removes polled native attack bit");
    expect(message(VK_LBUTTON, false) == 0, "LMB lean owns its release");
    input();

    reset();
    rotateHeldKey = 'R';
    held(true);
    input();
    expect(message('R', true) == 0, "rotate owns original keyboard press");
    rotateHeldKey = 'T';
    expect(message('R', false) == 0, "rotate rebind preserves original key release owner");
    // Retire the old pre-review implementation's single anonymous press flag.
    rotateHeldKey = 'R';
    message('R', false);
    rotateHeldKey = 'T';
    expect(message('T', false) == 42, "unpressed replacement rotate key has no owned release");

    reset();
    leanKey = 'U';
    input();
    expect(message('U', true) == 0, "lean owns original keyboard press");
    fixtureKeys['U'] = true;
    nativeKeys = 0x20;
    input();
    expect(leaning && !(*(unsigned*)gameAt(fp::sig::keyWord) & 0x20), "lean removes its native run binding");
    leanKey = 'I';
    input();
    expect(!leaning && !(*(unsigned*)gameAt(fp::sig::keyWord) & 0x20),
           "old consumed native key stays masked through lean rebind");
    expect(message('U', false) == 0, "lean rebind preserves original key release owner");
    leanKey = 'U';
    expect(message('U', false) == 42, "lean does not consume an up without its down");

#ifndef EXCAMERA_PRE_REVIEW
    reset();
    leanKey = 'U';
    input();
    message('U', true);
    releaseInput();
    expect(inputOwner.windowOwns('U') && message('U', false) == 0,
           "ordinary releaseInput preserves the consumed press's window release owner");
    inputEnabled = 1;
    message('U', true);
    inputOwner.attackDown(true);
    const uint32_t keptGeneration = inputOwner.generation();
    resetBindingWindowOwnership();
    expect(!inputOwner.windowOwns('U') && inputOwner.attackSuppressed() &&
               inputOwner.generation() == keptGeneration,
           "new window-hook session clears only its physical-press ledger");
    expect(message('U', false) == 42, "new window-hook session does not inherit an older release");

    reset();
    held(true);
    for (int key : {'R', 'T', 'Y'}) {
        rotateHeldKey = key;
        input();
        expect(message(key, true) == 0, "each rebound rotate key owns its own physical press");
    }
    rotateHeldKey = 0;
    cursorFree = 1;
    for (int key : {'T', 'R', 'Y'})
        expect(message(key, false) == 0, "several older rotate releases survive unbind and F8");

    reset();
    leanKeysMode = true;
    leanLeftKey = VK_XBUTTON1;
    leanRightKey = VK_XBUTTON2;
    input();
    expect(message(VK_XBUTTON1, true) == TRUE && message(VK_XBUTTON2, true) == TRUE,
           "two-key lean owns both X-button presses with Windows TRUE result");
    leanLeftKey = leanRightKey = 0;
    *(int*)gameAt(fp::sig::uiOpenCount) = 1;
    expect(message(VK_XBUTTON2, false) == TRUE && message(VK_XBUTTON1, false) == TRUE,
           "two-key lean keeps both releases across rebind and native UI");

    reset();
    leanKey = 'U';
    input();
    message('U', true);
    nativeKeys = 0x20;
    input();
    expect(leaning, "lean active before profile handoff");
    viewMode = 1;
    input();
    expect(!leaning && !(*(unsigned*)gameAt(fp::sig::keyWord) & 0x20),
           "FP to TP ends lean and does not transfer held key to native run");
    input();
    expect(!leaning, "held lean cannot rearm on the next profile sample");
    message('U', false);
    input();
    message('U', true);
    input();
    expect(leaning, "fresh physical press rearms lean in TP");
    foreground = false;
    input();
    foreground = true;
    input();
    expect(!leaning, "focus loss requires release before lean rearm");
    message('U', false);
    input();

    // Direct polling remains supported, but rebinding a currently held key
    // cannot reinterpret that old physical press as a new mod action.
    reset();
    fixtureKeys['I'] = true;
    input();
    leanKey = 'I';
    input();
    expect(!leaning, "rebinding an already held key waits for release");
    fixtureKeys['I'] = false;
    input();
    fixtureKeys['I'] = true;
    input();
    expect(leaning, "polled fresh key rearms without a window event");

    reset();
    expect(message('U', true) == 42, "unbound physical key begins with its native owner");
    leanKey = 'U';
    input();
    expect(!leaning && message('U', true, true) == 42,
           "a native autorepeat cannot acquire a newly assigned lean binding");
    expect(message('U', false) == 42, "native autorepeat keeps its matching release");

    reset();
    leanKey = 'U';
    input();
    *(uintptr_t*)gameAt(fp::sig::uiView) = viewObject + 8;
    expect(message('U', true) == 42 && !inputOwner.windowOwns('U'),
           "new native UI view owns lean-key down before windowFree publication");
    *(uintptr_t*)gameAt(fp::sig::uiView) = viewObject;
    expect(message('U', false) == 42, "a native UI press keeps its release after UI closes");

    manual();
    leanKey = VK_RBUTTON;
    input();
    message(VK_RBUTTON, true);
    input();
    expect(!leaning && guardRequest.held, "ordinary manual guard outranks RMB lean");
    message(VK_RBUTTON, false);
    input();
    held(true);
    rotateHeldKey = VK_RBUTTON;
    input();
    expect(message(VK_RBUTTON, true) == 0, "held-item RMB rotation keeps message priority");
    input();
    expect(heldRotating && !leaning && !guardRequest.held,
           "held-item RMB rotation keeps polling priority over guard");
    message(VK_RBUTTON, false);
    input();

    manual();
    message(VK_RBUTTON, true);
    input();
    expect(guardRequest.held, "guard held before event overlap");
    message(VK_LBUTTON, true);
    message(VK_RBUTTON, false);
    input();
    expect(!guardRequest.held && !(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "LMB down then guard up between polls cannot start attack");
    input();
    expect(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "suppressed LMB remains blocked after guard release");
    message(VK_LBUTTON, false);
    input();
    message(VK_LBUTTON, true);
    input();
    expect((*(unsigned*)gameAt(fp::sig::buttonWord) & 1) != 0, "fresh LMB after physical release can attack");

    manual();
    message(VK_RBUTTON, true);
    input();
    message(VK_LBUTTON, true);
    message(VK_LBUTTON, false);
    message(VK_RBUTTON, false);
    input();
    expect(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "fully released guard overlap cannot replay its buffered attack tap");
    fixtureKeys[VK_LBUTTON] = true; // a later fresh physical down whose window event is lost
    input();
    expect((*(unsigned*)gameAt(fp::sig::buttonWord) & 1) != 0,
           "observed release rearms a later polled LMB press without a window event");

    manual();
    message(VK_LBUTTON, true);
    message(VK_RBUTTON, true);
    message(VK_RBUTTON, false);
    input();
    expect(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1) && !guardRequest.held,
           "RMB down over held LMB suppresses attack even when guard releases before poll");

    manual();
    message(VK_LBUTTON, true);
    message(VK_LBUTTON, false);
    message(VK_RBUTTON, true);
    message(VK_RBUTTON, false);
    input();
    expect(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1),
           "guard down retires an LMB tap still waiting for its input sample");

    manual();
    message(VK_RBUTTON, true);
    input();
    message(VK_LBUTTON, true);
    input();
    message(VK_LBUTTON, false);
    message(VK_RBUTTON, false);
    message(VK_LBUTTON, true);
    input();
    expect((*(unsigned*)gameAt(fp::sig::buttonWord) & 1) != 0,
           "LMB up and fresh down after guard up rearm within one polling interval");

    manual();
    message(VK_RBUTTON, true);
    input();
    message(VK_LBUTTON, true);
    message(VK_LBUTTON, false);
    message(VK_RBUTTON, false);
    fixtureKeys[VK_LBUTTON] = true;
    windowProc(nullptr, WM_LBUTTONDBLCLK, mouseFlags(), 0);
    message(VK_LBUTTON, false);
    input();
    expect((*(unsigned*)gameAt(fp::sig::buttonWord) & 1) != 0,
           "a fresh released double-click after guard up retains its new attack tap");

    manual();
    message(VK_RBUTTON, true);
    input();
    message(VK_LBUTTON, true);
    message(VK_LBUTTON, false);
    message(VK_RBUTTON, false);
    viewMode = 1;
    input();
    input();
    fixtureKeys[VK_LBUTTON] = true;
    input();
    expect((*(unsigned*)gameAt(fp::sig::buttonWord) & 1) != 0,
           "old profile event suppression cannot poison a later polled press");

    manual();
    *(int*)gameAt(fp::sig::uiOpenCount) = 1;
    message(VK_RBUTTON, true);
    expect(message(VK_LBUTTON, true) == 42 && !inputOwner.attackSuppressed(),
           "native UI messages never publish a manual-guard overlap");
#endif

    std::printf("Binding ownership: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
