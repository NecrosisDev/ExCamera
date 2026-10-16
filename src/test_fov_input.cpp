// Calls the real windowProc in an owned test process; never attaches to Exanima.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "first_person.cpp"
static int forwarded = 0, checks = 0, failures = 0;
static LRESULT CALLBACK fallback(HWND, UINT, WPARAM, LPARAM) {
    ++forwarded;
    return 1234;
}
static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) ++failures;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
}
static LPARAM bits(unsigned flags) {
    return static_cast<LPARAM>(static_cast<unsigned long long>(flags));
}
static void reset(int fov = 75) {
    selectedFov.store(fov);
    forwarded = 0;
    originalWindowProc = fallback;
    InterlockedExchange(&inputEnabled, 0);
    InterlockedExchange(&heldKeys, 0);
    InterlockedExchange(&tappedKeys, 0);
    InterlockedExchange(&tappedButtons, 0);
}
int main() {
    reset();
    auto r = windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 80 && forwarded == 0 && r == 0, "F10 system key widens 75 to 80");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 80 && forwarded == 0, "ordinary F10 also widens once");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 70 && forwarded == 0, "F9 narrows 75 to 70");
    reset(100);
    windowProc(nullptr, WM_KEYDOWN, VK_F11, 1);
    check(selectedFov.load() == 75 && forwarded == 0, "F11 resets to 75");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, bits(0x40000001u));
    windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xc0000001u));
    check(selectedFov.load() == 80 && forwarded == 0,
          "system F10 repeat/up consumed without another change or menu activation");
    reset();
    windowProc(nullptr, WM_KEYDOWN, VK_F10, 1);
    windowProc(nullptr, WM_KEYDOWN, VK_F10, bits(0x40000001u));
    windowProc(nullptr, WM_KEYUP, VK_F10, bits(0xc0000001u));
    check(selectedFov.load() == 80 && forwarded == 0, "ordinary F10 repeat/up consumed");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, bits(0x20000001u));
    windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xe0000001u));
    check(selectedFov.load() == 75 && forwarded == 2, "Alt+F10 remains native");
    reset();
    r = windowProc(nullptr, WM_SYSKEYDOWN, VK_F4, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 1 && r == 1234, "Alt+F4 remains native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_TAB, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 1, "Alt+Tab remains native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F9, bits(0x20000001u));
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F11, bits(0x20000001u));
    check(selectedFov.load() == 75 && forwarded == 2, "Alt+F9 and Alt+F11 remain native");
    reset();
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F6, 1);
    check(selectedFov.load() == 75 && forwarded == 1,
          "unrelated system key is not promoted to normal hotkey");
    reset(130);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 130, "upper FOV bound remains 130");
    reset(30);
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 30, "lower FOV bound remains 30");
    reset(128);
    windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
    check(selectedFov.load() == 130, "near-upper FOV value clamps to 130");
    reset(32);
    windowProc(nullptr, WM_KEYDOWN, VK_F9, 1);
    check(selectedFov.load() == 30, "near-lower FOV value clamps to 30");
    reset(30);
    for (int i = 0; i < 20; ++i) {
        windowProc(nullptr, WM_SYSKEYDOWN, VK_F10, 1);
        windowProc(nullptr, WM_SYSKEYUP, VK_F10, bits(0xc0000001u));
    }
    check(selectedFov.load() == 130 && forwarded == 0, "20 discrete F10 presses span the full 30-130 range");
    reset();
    InterlockedExchange(&inputEnabled, 1);
    windowProc(nullptr, WM_KEYDOWN, 'W', 1);
    check(heldKeys == 1 && tappedKeys == 1 && forwarded == 1, "movement press is unchanged");
    windowProc(nullptr, WM_KEYUP, 'W', bits(0xc0000001u));
    check(heldKeys == 0 && forwarded == 2, "movement release is unchanged");
    windowProc(nullptr, WM_KEYDOWN, 'W', 1);
    windowProc(nullptr, WM_KILLFOCUS, 0, 0);
    check(inputEnabled == 0 && heldKeys == 0 && tappedKeys == 0, "focus loss still clears movement capture");
    reset();
    windowProc(nullptr, WM_LBUTTONDOWN, VK_F10, 0);
    check(selectedFov.load() == 75 && forwarded == 1, "non-key message cannot change FOV");
    std::printf("SUMMARY: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
