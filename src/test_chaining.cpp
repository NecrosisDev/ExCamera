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
#include <initializer_list>
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
    assert(MH_DisableHook((void*)gameFn) == MH_OK);
    assert(!memcmp((void*)gameFn, d, n)); // the other mod's detour is back, byte for byte
    assert(call(1) == 1001 && oursCalls == 1 && otherCalls == 3);
    assert(MH_EnableHook((void*)gameFn) == MH_OK);
    assert(call(1) == 1002 && oursCalls == 2 && otherCalls == 4); // unchanged chain safely re-arms
    assert(MH_DisableHook((void*)gameFn) == MH_OK && MH_RemoveHook((void*)gameFn) == MH_OK);
    patch((void*)gameFn, saved, 16);
    assert(call(2) == (((2 * 3 + 1) ^ 7)) && gameCalls == 1);
    printf("PASS: chaining on a %s detour: both run, ours first; unhook restores theirs exactly\n", form);
}

static void otherJump(void* target, unsigned char (&out)[5]) {
    out[0] = 0xe9;
    const int32_t rel = int32_t((char*)otherDetour - ((char*)target + 5));
    memcpy(out + 1, &rel, 4);
}
static MH_STATUS change(bool enable, int api) {
    if (api == 0) return enable ? MH_EnableHook((void*)gameFn) : MH_DisableHook((void*)gameFn);
    if (api == 1) return enable ? MH_EnableHook(MH_ALL_HOOKS) : MH_DisableHook(MH_ALL_HOOKS);
    assert((enable ? MH_QueueEnableHook(MH_ALL_HOOKS) : MH_QueueDisableHook(MH_ALL_HOOKS)) == MH_OK);
    return MH_ApplyQueued();
}
static void changedEntry(int api, bool wasEnabled) {
    unsigned char saved[32], own[5], foreign[5];
    memcpy(saved, (void*)gameFn, sizeof saved);
    otherJump((void*)gameFn, foreign);
    assert(MH_CreateHook((void*)gameFn, (void*)ourDetour, (void**)&trampoline) == MH_OK);
    assert(MH_EnableHook((void*)gameFn) == MH_OK);
    memcpy(own, (void*)gameFn, sizeof own);
    if (!wasEnabled) assert(MH_DisableHook((void*)gameFn) == MH_OK);
    patch((void*)gameFn, foreign, sizeof foreign);
    assert(change(!wasEnabled, api) == MH_ERROR_CONFLICT);
    assert(!memcmp((void*)gameFn, foreign, sizeof foreign));
    int (*volatile call)(int) = gameFn;
    assert(call(1) == 1001); // the replacement remains reachable and is never bypassed
    if (wasEnabled) {
        // A caller may still hold our trampoline: neither removal nor global
        // teardown may free it when the entry is now owned by another detour.
        assert(MH_RemoveHook((void*)gameFn) == MH_ERROR_CONFLICT);
        assert(MH_Uninitialize() == MH_ERROR_CONFLICT);
        assert(trampoline(2) == (((2 * 3 + 1) ^ 7)));
        patch((void*)gameFn, own, sizeof own);
        assert(change(false, api) == MH_OK);
    }
    assert(MH_RemoveHook((void*)gameFn) == MH_OK);
    patch((void*)gameFn, saved, sizeof saved);
    printf("PASS: %s entry change refused and preserved (API %d)\n",
           wasEnabled ? "live takeover" : "stopped/re-arm", api);
}
static DWORD WINAPI emptyThread(void*) { return 0; }
static void conflictKeepsThreadIp(int api, bool wasEnabled) {
    unsigned char saved[32], own[5], foreign[5];
    memcpy(saved, (void*)gameFn, sizeof saved);
    otherJump((void*)gameFn, foreign);
    assert(MH_CreateHook((void*)gameFn, (void*)ourDetour, (void**)&trampoline) == MH_OK);
    if (wasEnabled) {
        assert(MH_EnableHook((void*)gameFn) == MH_OK);
        memcpy(own, (void*)gameFn, sizeof own);
    }
    patch((void*)gameFn, foreign, sizeof foreign);
    // The thread never runs with this test IP. On a rejected batch, MinHook must
    // not remap its entry/trampoline address using obsolete instruction offsets.
    HANDLE thread = CreateThread(nullptr, 0, emptyThread, nullptr, CREATE_SUSPENDED, nullptr);
    assert(thread);
    CONTEXT original{};
    original.ContextFlags = CONTEXT_CONTROL;
    assert(GetThreadContext(thread, &original));
    CONTEXT placed = original;
    placed.Rip = (DWORD64)(wasEnabled ? (void*)trampoline : (void*)gameFn);
    assert(SetThreadContext(thread, &placed));
    assert(change(!wasEnabled, api) == MH_ERROR_CONFLICT);
    CONTEXT after{};
    after.ContextFlags = CONTEXT_CONTROL;
    assert(GetThreadContext(thread, &after) && after.Rip == placed.Rip);
    assert(SetThreadContext(thread, &original));
    assert(ResumeThread(thread) == 1);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    if (wasEnabled) {
        patch((void*)gameFn, own, sizeof own);
        assert(MH_DisableHook((void*)gameFn) == MH_OK);
    }
    assert(MH_RemoveHook((void*)gameFn) == MH_OK);
    patch((void*)gameFn, saved, sizeof saved);
    printf("PASS: conflict leaves suspended thread IP unchanged (%s, API %d)\n",
           wasEnabled ? "disable" : "enable", api);
}
__declspec(noinline) int secondGame(int x) {
    volatile int value = x;
    value = value * 5 + 3;
    return value ^ 11;
}
static void batchConflict(bool wasEnabled, bool queued) {
    unsigned char savedA[32], savedB[32], ownA[5], ownB[5], foreign[5];
    memcpy(savedA, (void*)gameFn, sizeof savedA);
    memcpy(savedB, (void*)secondGame, sizeof savedB);
    otherJump((void*)secondGame, foreign);
    assert(MH_CreateHook((void*)gameFn, (void*)ourDetour, (void**)&trampoline) == MH_OK);
    assert(MH_CreateHook((void*)secondGame, (void*)otherDetour, nullptr) == MH_OK);
    if (wasEnabled) assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    memcpy(ownA, (void*)gameFn, sizeof ownA);
    memcpy(ownB, (void*)secondGame, sizeof ownB);
    patch((void*)secondGame, foreign, sizeof foreign);
    assert(change(!wasEnabled, queued ? 2 : 1) == MH_ERROR_CONFLICT);
    assert(!memcmp((void*)gameFn, ownA, sizeof ownA)); // earlier entry in the batch was untouched
    assert(!memcmp((void*)secondGame, foreign, sizeof foreign));
    if (wasEnabled) assert(MH_DisableHook((void*)gameFn) == MH_OK); // unaffected hooks can be removed
    assert(MH_RemoveHook((void*)gameFn) == MH_OK);
    patch((void*)secondGame, ownB, sizeof ownB);
    if (wasEnabled) assert(MH_DisableHook((void*)secondGame) == MH_OK);
    assert(MH_RemoveHook((void*)secondGame) == MH_OK);
    patch((void*)gameFn, savedA, sizeof savedA);
    patch((void*)secondGame, savedB, sizeof savedB);
    printf("PASS: entire %s batch validated before changing its first entry (%s)\n",
           queued ? "queued" : "all-hooks", wasEnabled ? "disable" : "enable");
}
static void relocatedImmediate() {
    auto code = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(code);
    // mov rax, imm64; ret: the relocated instruction extends beyond the five
    // patch bytes. A retained trampoline must notice changes to that tail.
    const unsigned char body[] = {0x48, 0xb8, 0x21, 0, 0, 0, 0, 0, 0, 0, 0xc3};
    memcpy(code, body, sizeof body);
    assert(MH_CreateHook(code, (void*)ourDetour, (void**)&trampoline) == MH_OK);
    assert(MH_EnableHook(code) == MH_OK && MH_DisableHook(code) == MH_OK);
    code[7] = 1;
    FlushInstructionCache(GetCurrentProcess(), code, sizeof body);
    assert(!memcmp(code, body, 5) && MH_EnableHook(code) == MH_ERROR_CONFLICT && code[7] == 1);
    assert(MH_RemoveHook(code) == MH_OK);
    VirtualFree(code, 0, MEM_RELEASE);
    puts("PASS: re-arm checks relocated immediate bytes beyond the patch itself");
}
static void hotPatchTakeover() {
    auto memory = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(memory);
    unsigned char* code = memory + 64;
    memset(code - 5, 0x90, 5);
    const unsigned char body[] = {0xc3, 0x90, 1, 2, 3}; // enough for a short jump, not for a normal long one
    memcpy(code, body, sizeof body);
    assert(MH_CreateHook(code, (void*)ourDetour, (void**)&trampoline) == MH_OK);
    assert(MH_EnableHook(code) == MH_OK && code[0] == 0xeb && code[-5] == 0xe9);
    unsigned char own[7];
    memcpy(own, code - 5, sizeof own);
    code[-4] ^= 1; // another owner changed the long jump above the entry
    const unsigned char foreign = code[-4];
    FlushInstructionCache(GetCurrentProcess(), code - 5, sizeof own);
    assert(MH_DisableHook(code) == MH_ERROR_CONFLICT && code[-4] == foreign);
    patch(code - 5, own, sizeof own);
    assert(MH_DisableHook(code) == MH_OK && MH_RemoveHook(code) == MH_OK);
    assert(!memcmp(code, body, sizeof body));
    VirtualFree(memory, 0, MEM_RELEASE);
    puts("PASS: hot-patch prefix ownership is checked and preserved on takeover");
}
int main() {
    assert(MH_Initialize() == MH_OK);
    run("jmp rel32 (E9)", false);
    run("jmp [rip] absolute (FF 25)", true);
    for (int api = 0; api < 3; ++api)
        for (bool enabled : {false, true}) {
            changedEntry(api, enabled);
            conflictKeepsThreadIp(api, enabled);
        }
    for (bool enabled : {false, true})
        for (bool queued : {false, true}) batchConflict(enabled, queued);
    relocatedImmediate();
    hotPatchTakeover();
    assert(MH_Uninitialize() == MH_OK);
    puts("CHAIN TESTS PASSED");
}
