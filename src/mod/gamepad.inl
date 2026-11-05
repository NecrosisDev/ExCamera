// Controller buttons for the mod's own actions: kick, jump, Manual defense's guard (with the right stick picking
// the parry) and the third-person shoulder swap. gamepad.h says how the game itself reads a controller (Steam
// Input only) and why the mod reads the pad through XInput instead. Part of first_person.cpp (one translation
// unit): included there, in order. Runs on the game's input thread from input().
#include <xinput.h>

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetStateFn padGetState = nullptr;
static const char* padLibrary = "none";          // which XInput answered (status file)
static std::atomic<int> padIndex{-1};             // the pad in use, -1 none
static std::atomic<unsigned> padButtonsSeen{0};   // every XInput button bit seen down this session
static std::atomic<unsigned> padKicks{0}, padJumps{0}, padGuardPolls{0}, padShoulders{0}, padParryPicks{0};

// The first pad that answers. A missing pad is looked for again every two seconds, since asking XInput about an
// empty slot is slow.
static bool padRead(fp::PadState& out) {
    static bool loaded = false;
    static ULONGLONG nextScan = 0;
    static int index = -1;
    if (!loaded) {
        loaded = true;
        for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            const HMODULE library = LoadLibraryA(name);
            if (!library) continue;
            padGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(library, "XInputGetState"));
            if (padGetState) {
                padLibrary = name;
                break;
            }
            FreeLibrary(library);
        }
    }
    if (!padGetState) return false;
    XINPUT_STATE state{};
    if (index >= 0 && padGetState(DWORD(index), &state) != ERROR_SUCCESS) index = -1;
    if (index < 0) {
        const ULONGLONG now = GetTickCount64();
        if (now < nextScan) return false;
        nextScan = now + 2000;
        for (int i = 0; i < XUSER_MAX_COUNT && index < 0; ++i)
            if (padGetState(DWORD(i), &state) == ERROR_SUCCESS) index = i;
        padIndex = index;
        if (index < 0) return false;
    }
    out.buttons = state.Gamepad.wButtons;
    out.leftTrigger = state.Gamepad.bLeftTrigger;
    out.rightTrigger = state.Gamepad.bRightTrigger;
    out.rightX = state.Gamepad.sThumbRX;
    out.rightY = state.Gamepad.sThumbRY;
    return true;
}

static void padTick(uintptr_t root, uintptr_t bh, bool owned) {
    (void)root;
    (void)bh;
    static fp::PadState last{};
    static bool parryWriting = false;
    fp::PadState pad{};
    if (!padRead(pad)) pad = {};
    if (pad.buttons) padButtonsSeen.fetch_or(pad.buttons);
    // A press is taken where the matching key's would be (gameplay owns the pointer, no close view); a button
    // already down when that starts waits for its next press.
    auto pressed = [&](int button) { return fp::padHeld(button, pad) && !fp::padHeld(button, last); };
    if (owned && bindingContextNow()) {
        if (pressed(padKickButton.load())) {
            padKickRequested = true;
            padKicks.fetch_add(1);
        }
        if (pressed(padJumpButton.load())) {
            padJumpRequested = true;
            padJumps.fetch_add(1);
        }
        if (pressed(padShoulderButton.load()) && viewMode.load() == 1 && tpShoulder.load() != 0 &&
            !cursorFreeBitSet(cursorFreeBit)) {
            swapShoulder();
            padShoulders.fetch_add(1);
        }
    }
    const bool guard = owned && fp::padHeld(padGuardButton.load(), pad);
    padGuardDown = guard;
    if (guard) padGuardPolls.fetch_add(1);
    // parryDragHeld is combatInput's answer for the previous poll: a guard is held under a Parry direction.
    if (guard && parryDragHeld.load()) {
        const int pose = fp::padParryPose(pad.rightX, pad.rightY, parryDragMirror.load());
        if (pose && pose != parryDragPose.load()) padParryPicks.fetch_add(1);
        parryDragPose = pose;
        parryWriting = true;
    } else if (parryWriting) {
        parryDragPose = 0;
        parryWriting = false;
    }
    last = pad;
}
