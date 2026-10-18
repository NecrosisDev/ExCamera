// Real mouse-look, polling, behavior and cue adapters with owned native storage.
// The native classifier spy follows C1CF0's new-press branch; no game is launched.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool swingKeys[256]{};
static POINT swingCursor{800, 540};
static int swingWidth = 1600, swingHeight = 1080;
static HWND fakeForeground() { return (HWND)1; }
static DWORD fakeWindowPid(HWND, DWORD* pid) { if (pid) *pid = GetCurrentProcessId(); return 1; }
static BOOL fakeClient(HWND, RECT* r) { *r = RECT{0, 0, swingWidth, swingHeight}; return TRUE; }
static BOOL fakeScreen(HWND, POINT*) { return TRUE; }
static BOOL fakeCursor(POINT* p) { *p = swingCursor; return TRUE; }
static BOOL fakeSetCursor(int x, int y) { swingCursor = POINT{x, y}; return TRUE; }
static BOOL fakeClip(const RECT*) { return TRUE; }
static HMONITOR fakeMonitor(POINT, DWORD) { return (HMONITOR)1; }
static SHORT WINAPI fakeKey(int vk) { return vk > 0 && vk < 256 && swingKeys[vk] ? SHORT(-32768) : 0; }
#define GetForegroundWindow fakeForeground
#define GetWindowThreadProcessId fakeWindowPid
#define GetClientRect fakeClient
#define ClientToScreen fakeScreen
#define GetCursorPos fakeCursor
#define SetCursorPos fakeSetCursor
#define ClipCursor fakeClip
#define MonitorFromPoint fakeMonitor
#define GetAsyncKeyState fakeKey
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetClientRect
#undef ClientToScreen
#undef GetCursorPos
#undef SetCursorPos
#undef ClipCursor
#undef MonitorFromPoint
#undef GetAsyncKeyState
#include "test_controlled_player.h"

static uintptr_t actor, bh, head, rightHand, leftHand, weapon, secondWeapon;
static unsigned nativeButtons = 0, classified = 0;
static float filterHeading = 0;
static int checks = 0, failures = 0;
template <typename T> static T& fieldAt(uintptr_t address, size_t offset) { return *(T*)(address + offset); }
static void check(bool value, const char* what) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", what); }
}
static void nativePoll() {
    *(unsigned*)gameAt(fp::sig::buttonWord) = nativeButtons;
    *(unsigned*)gameAt(fp::sig::keyWord) = 0;
}
static float nativeHeading(float* d) { return std::atan2(d[0], d[2]); }
static void filterPush(uintptr_t, float* d) { filterHeading = nativeHeading(d); }
static void filterUpdate(uintptr_t) {}
static void nativeBehavior(uintptr_t self) {
    input();
    const unsigned buttons = *(unsigned*)gameAt(fp::sig::buttonWord);
    unsigned& press = fieldAt<unsigned>(self, field::behaviour::pressFlags);
    if ((buttons & 1u) && !(press & 1u) && !fieldAt<unsigned char>(self, field::behaviour::pressBlocked)) {
        // C1CF0 reads aimHeading before cursorAim, with a strict 15/20 degree threshold.
        const float relative = std::remainder(fieldAt<float>(self, field::behaviour::aimHeading) -
                                                  fieldAt<float>(actor, field::character::bodyHeading),
                                              6.28318530718f);
        const float threshold = (fieldAt<unsigned>(actor, field::character::gripFlags) ? 20.f : 15.f) *
                                0.0174532925f;
        classified = relative > threshold ? 2u : 0u;
        const unsigned grip = fieldAt<unsigned>(actor, field::character::gripFlags);
        fieldAt<int>(actor, field::character::motion) = classified ? 8 : 5;
        fieldAt<uintptr_t>(actor, field::character::swingWeapon) = classified && (grip & 0x80u) ? secondWeapon : weapon;
        fieldAt<float>(actor, field::character::motionProgress) = .15f;
    }
    press = buttons & 1u;
    cursorAim(self);
}
static LRESULT CALLBACK nativeWindow(HWND, UINT, WPARAM, LPARAM) { return 0; }
static void thunk(fp::sig::Target target, const void* destination) {
    unsigned char bytes[12]{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(bytes + 2, &destination, sizeof destination);
    // The native aim-filter entries share a page. A preceding thunk already
    // made that page executable/read-only, so acquire it before writing again.
    DWORD old = 0;
    const bool writable = VirtualProtect((void*)gameAt(target), sizeof bytes, PAGE_READWRITE, &old) != FALSE;
    check(writable, "owned native-call thunk page is writable before installation");
    if (!writable) return;
    std::memcpy((void*)gameAt(target), bytes, sizeof bytes);
    check(VirtualProtect((void*)gameAt(target), sizeof bytes, PAGE_EXECUTE_READ, &old) != FALSE,
          "owned native-call thunk is executable");
}
static void frame(float dx = 0, float dy = 0) {
    swingCursor = POINT{swingWidth / 2 + LONG(dx), swingHeight / 2 + LONG(dy)};
    *(unsigned*)gameAt(fp::sig::gameClock) += 16;
    behavior(bh);
}
static void setup(int mode, int box, int fov, int gripKind) {
    fixture::resetInput(mode);
    std::memset(swingKeys, 0, sizeof swingKeys);
    std::memset((void*)(base + 0x600000), 0, 0x180000);
    fixture::ControlledPlayer{actor, bh, head, base + 0x670000}.connect();
    *(uintptr_t*)gameAt(fp::sig::currentScene) = base + 0x680000;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = 0;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = 0;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(unsigned*)gameAt(fp::sig::gameClock) = 10000;
    fieldAt<unsigned char>(actor, field::character::combatStance) = 1;
    fieldAt<float>(actor, field::character::facing + 8) = 1.f;
    float* pose = (float*)(head + field::bodyPart::pose);
    pose[0] = pose[4] = pose[8] = 1.f;
    originalInput = nativePoll;
    originalBehavior = nativeBehavior;
    originalWindowProc = nativeWindow;
    selectedFov = fov;
    tpFov = 0;
    fpDeadzoneX = tpDeadzoneX = box;
    fpDeadzoneY = tpDeadzoneY = 1;
    tpCombatAimFree = box > 0;
    attackCue = true;
    cueThrust = true;
    attackSideKey = 0;
    rawRegistered = 0;
    flickCatchup = 100;
    sensitivity = .0025f;
    inputClock = ~0u;
    nativeButtons = classified = 0;
    fieldAt<int>(actor, field::character::weaponCount) = gripKind == 2 ? 2 : 1;
    fieldAt<uintptr_t>(actor, field::character::weapons) = weapon;
    fieldAt<uintptr_t>(actor, field::character::weapons + 8) = gripKind == 2 ? secondWeapon : 0;
    fieldAt<unsigned>(actor, field::character::gripFlags) = gripKind == 2 ? 0x90 : (gripKind == 1 ? 0x30 : 0);
    const uintptr_t table = gameAt(fp::sig::gripTable);
    fieldAt<int>(table + 0xb8, 8) = 0;
    fieldAt<int>(table + 3 * 0xb8, 8) = 1;
    fieldAt<int>(table + 5 * 0xb8, 8) = 1;
    fieldAt<uintptr_t>(actor, field::character::gripEntry) = table + (gripKind == 1 ? 3 : 1) * 0xb8;
    fieldAt<uintptr_t>(actor, field::character::handParts) = rightHand;
    fieldAt<uintptr_t>(actor, field::character::handParts + 8) = leftHand;
    fieldAt<uintptr_t>(actor, field::character::heldItems) = weapon;
    fieldAt<uintptr_t>(actor, field::character::heldItems + 8) = gripKind == 1 ? weapon : (gripKind == 2 ? secondWeapon : 0);
    for (int i = 0; i < 2; ++i) {
        const uintptr_t part = i ? leftHand : rightHand, mesh = base + 0x690000 + i * 0x1000;
        fieldAt<uintptr_t>(part, field::bodyPart::meshNode) = mesh;
        fieldAt<uintptr_t>(mesh, field::sceneNode::bodyPart) = part;
        const char* name = i ? "18LHand" : "17RHand";
        std::memcpy((void*)(mesh + field::sceneNode::name), name, std::strlen(name) + 1);
    }
    frame(); // neutral sample and real recapture before a new physical press
}
static void click() {
    swingKeys[VK_LBUTTON] = true;
    nativeButtons = 1;
    windowProc((HWND)1, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    frame();
    swingKeys[VK_LBUTTON] = false;
    nativeButtons = 0;
    windowProc((HWND)1, WM_LBUTTONUP, 0, 0);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    fixture::NativeImage image(0x800000);
    if (!image) return 2;
    actor = base + 0x610000; bh = base + 0x620000; head = base + 0x630000;
    rightHand = base + 0x640000; leftHand = base + 0x650000;
    weapon = base + 0x660000; secondWeapon = base + 0x661000;
    thunk(fp::sig::headingFromDirection, (const void*)nativeHeading);
    thunk(fp::sig::aimFilterPush, (const void*)filterPush);
    thunk(fp::sig::aimFilterUpdate, (const void*)filterUpdate);
    if (failures) return 1;
    FlushInstructionCache(GetCurrentProcess(), image.data(), 0x800000);
    for (int mode : {0, 1}) for (int box : {0, 2, 20}) for (int fov : {30, 90, 130})
        for (int grip : {0, 1, 2}) for (int side : {-1, 1}) {
            setup(mode, box, fov, grip);
            frame(float(side * 16));
            const float realHeading = aim.rayYaw;
            click();
            const auto origin = fp::CueSide(cueOrigin.load());
            char label[180];
            std::snprintf(label, sizeof label, "%s box=%d FOV=%d grip=%d %s gesture reaches the native swing and arc",
                          mode ? "TP" : "FP", box, fov, grip, side < 0 ? "left" : "right");
            check(origin == (side < 0 ? fp::CueSide::left : fp::CueSide::right), label);
            check(std::fabs(std::remainder(filterHeading - realHeading, 6.28318530718f)) < 1e-5f &&
                      std::fabs(fieldAt<float>(bh, field::behaviour::aimHeading) - realHeading) < 1e-5f,
                  "native cursor aim receives the real heading after the one-press side request");
        }
    std::printf("Swing selection: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
