// Production input-owner tests with synthetic native polling/storage.
// No game process or physics solver: the checks establish request ownership,
// not a measurement of stopping distance or gameplay controller behavior.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool foreground = true;
static HWND testForeground() {
    return foreground ? (HWND)1 : nullptr;
}
static DWORD testWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#include <cstdio>
#include <vector>

static unsigned keys = 0, buttons = 0;
static bool pollAnalog = true;
static float analog[3] = {0.6f, 0, 0.8f};
static unsigned checks = 0, failures = 0;
static void expect(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
static void poll() {
    *(unsigned*)gameAt(fp::sig::keyWord) = keys;
    *(unsigned*)gameAt(fp::sig::buttonWord) = buttons;
    if (pollAnalog) std::memcpy((void*)gameAt(fp::sig::analogStick), analog, sizeof analog);
}
static bool analogNeutral() {
    const float* v = (const float*)gameAt(fp::sig::analogStick);
    return v[0] == 0 && v[1] == 0 && v[2] == 0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<unsigned char> storage(0x600000);
    base = (uintptr_t)storage.data();
    const uintptr_t root = base + 0x5c0000, bh = base + 0x5c2000, view = base + 0x5c4000,
                    head = base + 0x5c6000;
    *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;
    *(uintptr_t*)(root + field::character::behaviour) = bh;
    *(uintptr_t*)(root + field::character::headPart) = head;
    *(uintptr_t*)(bh + field::behaviour::character) = root;
    *(uintptr_t*)(bh + field::behaviour::view) = view;
    *(uintptr_t*)gameAt(fp::sig::uiView) = view;
    originalInput = poll;
    keyState = [](int) -> SHORT {
        return 0;
    };
    active = cameraReady = 1;
    stopping = cursorFree = rmbFree = windowFree = 0;
    viewMode = 0;
    rotateHeldKey = leanKey = leanLeftKey = leanRightKey = 0;
    aim = {};
    aim.root = root;
    aim.head = head;
    aim.valid = true;

    // The pointer freeze clears both independent movement producers. The
    // GUI owner may already have written moveIntent before native polling.
    for (int mode : {0, 1}) {
        viewMode = mode;
        cursorFree = 1;
        windowFree = 0;
        inputEnabled = 1;
        keys = 15;
        buttons = 3;
        *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
        input();
        expect((*(unsigned*)gameAt(fp::sig::keyWord) & 15) == 0 &&
                   *(unsigned*)gameAt(fp::sig::buttonWord) == 0,
               mode ? "TP F8 digital and mouse intent neutral" : "FP F8 digital and mouse intent neutral");
        expect(analogNeutral(), mode ? "TP F8 analog movement neutral" : "FP F8 analog movement neutral");
        expect(!*(unsigned char*)(bh + field::behaviour::moveIntent),
               mode ? "TP F8 cached GUI movement neutral" : "FP F8 cached GUI movement neutral");

        cursorFree = 0;
        windowFree = 1;
        *(uintptr_t*)gameAt(fp::sig::uiView) = view + 8;
        *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
        input();
        expect(analogNeutral(), mode ? "TP native window analog freeze" : "FP native window analog freeze");
        expect(!*(unsigned char*)(bh + field::behaviour::moveIntent),
               mode ? "TP native window cached GUI freeze" : "FP native window cached GUI freeze");
        *(uintptr_t*)gameAt(fp::sig::uiView) = view;
        windowFree = 0;
    }

    // The first native UI sample may precede the render hook's windowFree
    // update. Freeze all supported UI ownership signals at the consumer.
    const uintptr_t manager = base + 0x5c8000;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = manager;
    windowsFreeze = true;
    for (int ui : {0, 1, 2}) {
        *(int*)gameAt(fp::sig::uiOpenCount) = ui == 0 ? 1 : 0;
        *(uintptr_t*)gameAt(fp::sig::uiView) = ui == 1 ? view + 8 : view;
        *(uintptr_t*)(manager + field::gui::capture) = ui == 2 ? view + 8 : 0;
        *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
        input();
        expect(analogNeutral() && !*(unsigned char*)(bh + field::behaviour::moveIntent),
               ui == 0   ? "new UI counter freezes before render"
               : ui == 1 ? "new UI view freezes before render"
                         : "new UI capture freezes before render");
    }
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(uintptr_t*)gameAt(fp::sig::uiView) = view;
    *(uintptr_t*)(manager + field::gui::capture) = 0;

    // The request freeze does not rewrite committed native motion or raw
    // device channels used by a GUI/held-object owner. These exact archive
    // offsets are deliberately fixture sentinels, not additional runtime
    // resolved targets introduced by the production change.
    cursorFree = 1;
    const unsigned gait = 0x257, pending = 8;
    *(unsigned*)(root + field::character::gaitFlags) = gait;
    *(unsigned char*)(root + field::character::walking) = 1;
    *(unsigned*)(bh + 0x8f8) = pending;
    *(float*)(base + 0x5b29e0) = 0.75f;
    *(float*)(base + 0x5b29e4) = -0.25f;
    *(unsigned char*)(base + 0x5b2a10) = 1;
    *(unsigned char*)(base + 0x5b2a20) = 1;
    *(unsigned*)(base + 0x5b2a50) = 0x2003;
    *(unsigned*)(base + 0x4efd50) = 0x102003;
    keys = 0xb0;
    input();
    expect(*(unsigned*)(root + field::character::gaitFlags) == gait &&
               *(unsigned char*)(root + field::character::walking) == 1 &&
               *(unsigned*)(bh + 0x8f8) == pending,
           "poll freeze preserves native gait and step execution state");
    expect(*(float*)(base + 0x5b29e0) == 0.75f && *(float*)(base + 0x5b29e4) == -0.25f &&
               *(unsigned char*)(base + 0x5b2a10) == 1 && *(unsigned char*)(base + 0x5b2a20) == 1 &&
               *(unsigned*)(base + 0x5b2a50) == 0x2003 && *(unsigned*)(base + 0x4efd50) == 0x102003,
           "poll freeze preserves raw controller UI and gait-request channels");
    expect(*(unsigned*)gameAt(fp::sig::keyWord) == 0xb0,
           "movement freeze preserves non-movement native key bits");
    cursorFree = 0;

    // The current native sample remains available in ordinary close-view
    // gameplay and when the owner explicitly disables native-window freeze.
    keys = buttons = 0;
    input();
    expect(!analogNeutral(), "ordinary close-view controller sample passes");
    windowsFreeze = false;
    *(int*)gameAt(fp::sig::uiOpenCount) = 1;
    *(uintptr_t*)gameAt(fp::sig::uiView) = view + 8;
    *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
    input();
    expect(!analogNeutral() && *(unsigned char*)(bh + field::behaviour::moveIntent) == 1,
           "disabled native-window freeze retains legacy input");
    windowsFreeze = true;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(uintptr_t*)gameAt(fp::sig::uiView) = view;

    // Native AA080 leaves the last controller vector when its own focus gate
    // does not poll the controller. ExCamera must not interpret it as fresh
    // close-view movement while the application has lost focus.
    viewMode = 0;
    foreground = false;
    keys = buttons = 0;
    pollAnalog = false;
    std::memcpy((void*)gameAt(fp::sig::analogStick), analog, sizeof analog);
    *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
    input();
    expect(analogNeutral(), "lost-focus stale analog sample neutral");
    expect(!*(unsigned char*)(bh + field::behaviour::moveIntent), "lost-focus cached GUI movement neutral");
    foreground = true;
    pollAnalog = true;

    // A stale or unrelated behavior pointer must not establish ownership.
    cursorFree = 1;
    *(uintptr_t*)(bh + field::behaviour::character) = root + 0x1000;
    *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
    input();
    expect(!analogNeutral() && *(unsigned char*)(bh + field::behaviour::moveIntent) == 1,
           "unrelated behavior retains native sample");
    *(uintptr_t*)(bh + field::behaviour::character) = root;
    cameraReady = 0;
    input();
    expect(!analogNeutral() && *(unsigned char*)(bh + field::behaviour::moveIntent) == 1,
           "unready camera cannot freeze native input");
    cameraReady = 1;

    // A former freeze can outlive the frame that handed the view to native.
    // It must not erase native input while stopping or fully inactive.
    for (int state : {0, 1, 2}) {
        active = state != 0;
        stopping = state == 1;
        viewMode = state == 2 ? 2 : 0;
        cursorFree = windowFree = 1;
        keys = 15;
        buttons = 3;
        *(unsigned char*)(bh + field::behaviour::moveIntent) = 1;
        input();
        expect(*(unsigned*)gameAt(fp::sig::keyWord) == keys &&
                   *(unsigned*)gameAt(fp::sig::buttonWord) == buttons && !analogNeutral() &&
                   *(unsigned char*)(bh + field::behaviour::moveIntent) == 1,
               state == 0   ? "inactive view preserves native sample"
               : state == 1 ? "stopping preserves native sample"
                            : "overhead blend preserves native sample");
    }
    std::printf("RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
