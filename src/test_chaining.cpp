// Chaining (OtherHooks=allow, or the player said yes): MinHook on top of another program's detour. Both run,
// this mod's first; unhooking gives the other detour back byte for byte. The two forms other hooks take.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "vendor/minhook/include/MinHook.h"
static volatile int gameCalls = 0, otherCalls = 0, oursCalls = 0;
extern "C" __declspec(noinline) int gameFn(int x) {
    gameCalls++;
    volatile int a = x;
    a = a * 3 + 1;
    a = a ^ 7;
    return a;
}
__declspec(noinline) int otherDetour(int x) {
    otherCalls++;
    return x + 1000;
} // the other mod (does not call the game)
static int (*trampoline)(int);
__declspec(noinline) int ourDetour(int x) {
    oursCalls++;
    return trampoline(x) + 1;
}
static void patch(void* at, const unsigned char* code, size_t n) {
    DWORD o;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &o);
    memcpy(at, code, n);
    VirtualProtect(at, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}
static void run(const char* form, bool absolute) {
    unsigned char saved[16];
    memcpy(saved, (void*)gameFn, 16);
    unsigned char d[14];
    size_t n;
    if (absolute) {
        d[0] = 0xFF;
        d[1] = 0x25;
        memset(d + 2, 0, 4);
        void* t = (void*)otherDetour;
        memcpy(d + 6, &t, 8);
        n = 14;
    } else {
        d[0] = 0xE9;
        int32_t rel = int32_t((char*)otherDetour - ((char*)gameFn + 5));
        memcpy(d + 1, &rel, 4);
        n = 5;
    }
    patch((void*)gameFn, d, n);
    gameCalls = otherCalls = oursCalls = 0;
    int (*volatile call)(int) = gameFn;
    assert(call(1) == 1001 && otherCalls == 1); // the other mod alone
    assert(MH_CreateHook((void*)gameFn, (void*)ourDetour, (void**)&trampoline) == MH_OK &&
           MH_EnableHook((void*)gameFn) == MH_OK);
    assert(call(1) == 1002 && oursCalls == 1 && otherCalls == 2 &&
           gameCalls == 0); // ours, then through to the other mod
    assert(MH_DisableHook((void*)gameFn) == MH_OK && MH_RemoveHook((void*)gameFn) == MH_OK);
    assert(!memcmp((void*)gameFn, d, n)); // the other mod's detour is back, byte for byte
    assert(call(1) == 1001 && oursCalls == 1 && otherCalls == 3);
    patch((void*)gameFn, saved, 16);
    assert(call(2) == (((2 * 3 + 1) ^ 7)) && gameCalls == 1);
    printf("PASS: chaining on a %s detour: both run, ours first; unhook restores theirs exactly\n", form);
}
int main() {
    assert(MH_Initialize() == MH_OK);
    run("jmp rel32 (E9)", false);
    run("jmp [rip] absolute (FF 25)", true);
    MH_Uninitialize();
    puts("CHAIN TESTS PASSED");
}
