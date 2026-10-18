// Exercise the real MinHook implementation against owned executable buffers
// and suspended fixture threads. No game process or installed file is opened.
#ifdef NDEBUG
#error "This test requires assertions."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

enum class Fault { none, get, set, setAfterFirst, protect, restore, open, suspend, resume, flush };
static Fault fault = Fault::none;
static DWORD faultThread = 0;
static void* faultPage = nullptr;
static unsigned faultHits = 0, setCalls = 0;

static void inject(Fault mode, DWORD thread = 0, void* page = nullptr) {
    fault = mode;
    faultThread = thread;
    faultPage = page;
    faultHits = setCalls = 0;
}
static BOOL rejected() {
    ++faultHits;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
static BOOL WINAPI fixtureGetContext(HANDLE thread, LPCONTEXT context) {
    if (fault == Fault::get && GetThreadId(thread) == faultThread) return rejected();
    return GetThreadContext(thread, context);
}
static BOOL WINAPI fixtureSetContext(HANDLE thread, const CONTEXT* context) {
    ++setCalls;
    if ((fault == Fault::set && GetThreadId(thread) == faultThread) ||
        (fault == Fault::setAfterFirst && setCalls >= 2)) return rejected();
    return SetThreadContext(thread, context);
}
static BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD old) {
    if (address == faultPage &&
        ((fault == Fault::protect && protection == PAGE_EXECUTE_READWRITE) ||
         (fault == Fault::restore && protection != PAGE_EXECUTE_READWRITE))) return rejected();
    return VirtualProtect(address, size, protection, old);
}
static HANDLE WINAPI fixtureOpenThread(DWORD access, BOOL inherit, DWORD id) {
    if (fault == Fault::open && id == faultThread) {
        rejected();
        return nullptr;
    }
    return OpenThread(access, inherit, id);
}
static DWORD WINAPI fixtureSuspend(HANDLE thread) {
    if (fault == Fault::suspend && GetThreadId(thread) == faultThread) {
        rejected();
        return DWORD(-1);
    }
    return SuspendThread(thread);
}
static DWORD WINAPI fixtureResume(HANDLE thread) {
    if (fault == Fault::resume && GetThreadId(thread) == faultThread) {
        rejected();
        return DWORD(-1);
    }
    return ResumeThread(thread);
}
static BOOL WINAPI fixtureFlush(HANDLE process, LPCVOID address, SIZE_T size) {
    if (fault == Fault::flush && address == faultPage) return rejected();
    return FlushInstructionCache(process, address, size);
}

// These are the production sources, with only OS failure boundaries replaced.
// Keep upstream diagnostics at the same level as the normal vendor build.
#pragma warning(push, 1)
extern "C" {
#include "vendor/minhook/src/hde/hde64.c"
#include "vendor/minhook/src/buffer.c"
#include "vendor/minhook/src/trampoline.c"
#define GetThreadContext fixtureGetContext
#define SetThreadContext fixtureSetContext
#define VirtualProtect fixtureProtect
#define OpenThread fixtureOpenThread
#define SuspendThread fixtureSuspend
#define ResumeThread fixtureResume
#define FlushInstructionCache fixtureFlush
#include "vendor/minhook/src/hook.c"
#undef GetThreadContext
#undef SetThreadContext
#undef VirtualProtect
#undef OpenThread
#undef SuspendThread
#undef ResumeThread
#undef FlushInstructionCache
}
#pragma warning(pop)

static DWORD WINAPI emptyThread(void*) { return 0; }
static __declspec(noinline) int detour(int value) { return value + 100; }

struct ParkedThread {
    HANDLE handle;
    DWORD id;
    CONTEXT startup{};
    ParkedThread() {
        handle = CreateThread(nullptr, 0, emptyThread, nullptr, CREATE_SUSPENDED, &id);
        assert(handle);
        startup.ContextFlags = CONTEXT_CONTROL;
        assert(GetThreadContext(handle, &startup));
    }
    void place(void* at) {
        CONTEXT placed = startup;
        placed.Rip = DWORD64(at);
        assert(SetThreadContext(handle, &placed));
    }
    DWORD64 ip() const {
        CONTEXT current{};
        current.ContextFlags = CONTEXT_CONTROL;
        assert(GetThreadContext(handle, &current));
        return current.Rip;
    }
    ~ParkedThread() {
        // The test RIP is never executed. This also checks that MinHook paired
        // every successful extra suspension with exactly one resume.
        assert(SetThreadContext(handle, &startup));
        assert(ResumeThread(handle) == 1);
        assert(WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0);
        assert(CloseHandle(handle));
    }
};

static const unsigned char originalCode[] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0xb8, 42, 0, 0, 0, 0xc3
};
struct CodeBlock {
    unsigned char* memory;
    SIZE_T size;
    explicit CodeBlock(SIZE_T bytes = 4096) : size(bytes) {
        memory = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        assert(memory);
    }
    unsigned char* function(SIZE_T offset = 0) {
        assert(offset + sizeof originalCode <= size);
        memcpy(memory + offset, originalCode, sizeof originalCode);
        assert(FlushInstructionCache(GetCurrentProcess(), memory + offset, sizeof originalCode));
        return memory + offset;
    }
    void readOnly() {
        DWORD previous;
        assert(VirtualProtect(memory, size, PAGE_EXECUTE_READ, &previous));
    }
    ~CodeBlock() { assert(VirtualFree(memory, 0, MEM_RELEASE)); }
};
static void* create(unsigned char* target) {
    void* original = nullptr;
    assert(MH_CreateHook(target, reinterpret_cast<void*>(detour), &original) == MH_OK);
    return original;
}
static int call(void* function) { return reinterpret_cast<int (*)(int)>(function)(7); }
static void sameCode(unsigned char* target) {
    assert(!memcmp(target, originalCode, sizeof originalCode));
}
static void readOnly(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    assert(VirtualQuery(address, &info, sizeof info) == sizeof info);
    assert(info.Protect == PAGE_EXECUTE_READ);
}
static void writeCode(void* address, const void* bytes, SIZE_T size) {
    DWORD previous, ignored;
    assert(VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous));
    memcpy(address, bytes, size);
    assert(VirtualProtect(address, size, previous, &ignored));
    assert(FlushInstructionCache(GetCurrentProcess(), address, size));
}
static MH_STATUS change(bool enable, bool queued) {
    if (!queued) return enable ? MH_EnableHook(MH_ALL_HOOKS) : MH_DisableHook(MH_ALL_HOOKS);
    assert((enable ? MH_QueueEnableHook(MH_ALL_HOOKS) : MH_QueueDisableHook(MH_ALL_HOOKS)) == MH_OK);
    return MH_ApplyQueued();
}

static void normalTransition() {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    auto trampoline = static_cast<unsigned char*>(create(target));
    ParkedThread thread;
    thread.place(target + 1);
    assert(MH_EnableHook(target) == MH_OK && call(target) == 107);
    assert(thread.ip() == DWORD64(trampoline + 1));
    assert(MH_DisableHook(target) == MH_OK && call(target) == 42);
    assert(thread.ip() == DWORD64(trampoline + 1) && call(trampoline) == 42);
    assert(MH_RemoveHook(target) == MH_OK && thread.ip() == DWORD64(trampoline + 1));
    assert(call(trampoline) == 42);
    sameCode(target);
    readOnly(target);
    puts("PASS: normal enable, disable and remove keep each suspended RIP executable");
}

static void earlyFailure(Fault mode) {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    create(target);
    ParkedThread thread;
    thread.place(target + 1);
    inject(mode, thread.id, target);
    const auto status = MH_EnableHook(target);
    assert(status == ((mode == Fault::get || mode == Fault::set)
        ? MH_ERROR_THREAD_CONTEXT : MH_ERROR_THREAD_SUSPEND));
    assert(faultHits > 0);
    inject(Fault::none);
    sameCode(target);
    assert(thread.ip() == DWORD64(target + 1));
    assert(MH_DisableHook(target) == MH_ERROR_DISABLED);
    assert(MH_RemoveHook(target) == MH_OK);
    readOnly(target);
    printf("PASS: early Win32 failure %d leaves code, RIP and hook state unchanged\n", int(mode));
}

static void batchProtectionFailure(bool enabled, bool queued) {
    CodeBlock first, second;
    auto a = first.function(), b = second.function();
    first.readOnly();
    second.readOnly();
    auto ta = static_cast<unsigned char*>(create(a));
    auto tb = static_cast<unsigned char*>(create(b));
    if (enabled) assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    unsigned char beforeA[sizeof originalCode], beforeB[sizeof originalCode];
    memcpy(beforeA, a, sizeof beforeA);
    memcpy(beforeB, b, sizeof beforeB);
    ParkedThread one, two;
    one.place((enabled ? ta : a) + 1);
    two.place((enabled ? tb : b) + 1);
    const auto ipA = one.ip(), ipB = two.ip();
    inject(Fault::protect, 0, b);
    assert(change(!enabled, queued) == MH_ERROR_MEMORY_PROTECT && faultHits > 0);
    inject(Fault::none);
    assert(!memcmp(a, beforeA, sizeof beforeA) && !memcmp(b, beforeB, sizeof beforeB));
    assert(one.ip() == ipA && two.ip() == ipB);
    readOnly(a);
    readOnly(b);
    assert((queued ? MH_ApplyQueued() : change(!enabled, false)) == MH_OK);
    assert(call(a) == (enabled ? 42 : 107) && call(b) == (enabled ? 42 : 107));
    assert(MH_RemoveHook(a) == MH_OK && MH_RemoveHook(b) == MH_OK);
    printf("PASS: %s %s batch protection failure changes no code or RIP; retry succeeds\n",
           queued ? "queued" : "all-hooks", enabled ? "disable" : "enable");
}

static void rejectedRollbackRetainsTrampoline() {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    auto trampoline = static_cast<unsigned char*>(create(target));
    ParkedThread one, two;
    one.place(target + 1);
    two.place(target + 1);
    inject(Fault::setAfterFirst);
    assert(MH_EnableHook(target) == MH_ERROR_THREAD_CONTEXT && faultHits >= 2);
    assert(MH_HasPendingRecovery() && MH_Recover() == MH_ERROR_THREAD_CONTEXT);
    sameCode(target);
    const auto a = one.ip(), b = two.ip();
    assert((a == DWORD64(target + 1) && b == DWORD64(trampoline + 1)) ||
           (b == DWORD64(target + 1) && a == DWORD64(trampoline + 1)));
    assert(MH_RemoveHook(target) == MH_ERROR_THREAD_CONTEXT);
    assert(call(trampoline) == 42); // failed cleanup did not release this buffer
    inject(Fault::none);
    const unsigned char foreign[] = {0xe9, 0, 0, 0, 0};
    writeCode(target, foreign, sizeof foreign);
    assert(MH_RemoveHook(target) == MH_ERROR_CONFLICT);
    assert(one.ip() == a && two.ip() == b && !memcmp(target, foreign, sizeof foreign));
    writeCode(target, originalCode, sizeof originalCode);
    assert(MH_Recover() == MH_OK && !MH_HasPendingRecovery());
    assert(MH_RemoveHook(target) == MH_OK);
    assert(one.ip() == DWORD64(target + 1) && two.ip() == DWORD64(target + 1));
    puts("PASS: failed context and rollback retain the trampoline; recovery checks entry ownership again");
}

static void retirementWithBlockedContext(bool global) {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    auto trampoline = static_cast<unsigned char*>(create(target));
    assert(MH_EnableHook(target) == MH_OK);
    ParkedThread thread;
    thread.place(trampoline + 1);
    inject(Fault::set, thread.id);
    // Published storage is quarantined, so removal need not speculate about
    // instruction-pointer rewrites or the thread's hidden return addresses.
    assert((global ? MH_Uninitialize() : MH_RemoveHook(target)) == MH_OK);
    assert(faultHits == 0 && thread.ip() == DWORD64(trampoline + 1));
    sameCode(target);
    assert(call(target) == 42 && call(trampoline) == 42);
    inject(Fault::none);
    if (global) assert(MH_Initialize() == MH_OK);
    printf("PASS: %s retains published code without speculative context rewrites\n",
           global ? "uninitialize" : "remove");
}

static void lateProtectionFailure() {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    auto trampoline = create(target);
    inject(Fault::restore, 0, target);
    assert(MH_EnableHook(target) == MH_ERROR_MEMORY_PROTECT && faultHits > 0);
    assert(MH_HasPendingRecovery());
    assert(MH_EnableHook(target) == MH_ERROR_MEMORY_PROTECT); // same-state calls also retry cleanup
    assert(call(target) == 107 && call(trampoline) == 42);
    assert(MH_RemoveHook(target) == MH_ERROR_MEMORY_PROTECT);
    inject(Fault::none);
    assert(MH_RemoveHook(target) == MH_OK);
    sameCode(target);
    readOnly(target);
    puts("PASS: late protection failure records committed bytes and preserves protection for retry");
}

static void failedResume() {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    create(target);
    ParkedThread thread;
    inject(Fault::resume, thread.id);
    assert(MH_EnableHook(target) == MH_ERROR_THREAD_SUSPEND && faultHits > 0);
    assert(MH_HasPendingRecovery() && MH_Recover() == MH_ERROR_THREAD_SUSPEND);
    assert(call(target) == 107);
    inject(Fault::none);
    assert(MH_Recover() == MH_OK && !MH_HasPendingRecovery());
    assert(MH_RemoveHook(target) == MH_OK); // removes the hook after its exact handle was resumed
    sameCode(target);
    puts("PASS: failed resume retains its handle and balances the suspension when retried");
}

static void failedFlush() {
    CodeBlock code;
    auto target = code.function();
    code.readOnly();
    create(target);
    ParkedThread thread;
    inject(Fault::flush, 0, target);
    assert(MH_EnableHook(target) == MH_ERROR_MEMORY_FLUSH && faultHits > 0);
    assert(MH_HasPendingRecovery() && MH_Recover() == MH_ERROR_MEMORY_FLUSH);
    assert(target[0] == 0xe9); // committed bytes are recorded, but no fixture RIP is executed
    assert(MH_RemoveHook(target) == MH_ERROR_MEMORY_FLUSH);
    inject(Fault::none);
    assert(MH_Recover() == MH_OK && !MH_HasPendingRecovery() && call(target) == 107);
    assert(MH_RemoveHook(target) == MH_OK); // recovery kept the committed hook enabled until this removal
    sameCode(target);
    readOnly(target);
    puts("PASS: failed instruction-cache flush retains suspended threads and retries before cleanup");
}

static void generatedInstructionRetainsTrampoline() {
    CodeBlock code;
    auto target = code.memory;
    target[0] = 0xe8; // call the owned callee at +128, then return
    const INT32 relative = 128 - 5;
    memcpy(target + 1, &relative, sizeof relative);
    target[5] = 0xc3;
    const unsigned char callee[] = {0xb8, 42, 0, 0, 0, 0xc3};
    memcpy(target + 128, callee, sizeof callee);
    code.readOnly();
    assert(FlushInstructionCache(GetCurrentProcess(), target, 256));
    auto trampoline = static_cast<unsigned char*>(create(target));
    ParkedThread thread;
    // CALL_ABS emits a real short jump at +6 after its callee returns. It has no
    // original boundary; retiring the allocation keeps it valid without a guess.
    thread.place(trampoline + 6);
    assert(MH_RemoveHook(target) == MH_OK);
    assert(thread.ip() == DWORD64(trampoline + 6) && call(trampoline) == 42);
    puts("PASS: retirement preserves generated instructions without inventing an original boundary");
}

struct PendingCall {
    HANDLE entered, release;
    void* original;
    int result = 0;
};
static PendingCall* activeCall = nullptr;
static __declspec(noinline) int blockedCallee(int) {
    assert(activeCall && SetEvent(activeCall->entered));
    assert(WaitForSingleObject(activeCall->release, 5000) == WAIT_OBJECT_0);
    return 42;
}
static DWORD WINAPI finishOriginal(void* parameter) {
    auto state = static_cast<PendingCall*>(parameter);
    state->result = call(state->original);
    return 0;
}
static void pendingReturnSurvives(bool enabled, bool global) {
    CodeBlock code;
    auto target = code.memory;
    // A regular Win64 prologue reserves shadow space and aligns the stack.
    // The call crosses MinHook's five-byte patch boundary and is relocated.
    const unsigned char body[] = {
        0x48, 0x83, 0xec, 0x28, 0xe8, 119, 0, 0, 0,
        0x48, 0x83, 0xc4, 0x28, 0xc3
    };
    memcpy(target, body, sizeof body);
    target[128] = 0x48;
    target[129] = 0xb8;
    const auto callee = reinterpret_cast<void*>(blockedCallee);
    memcpy(target + 130, &callee, sizeof callee);
    target[138] = 0xff;
    target[139] = 0xe0; // jump to the owned C++ callee
    code.readOnly();
    assert(FlushInstructionCache(GetCurrentProcess(), target, 256));
    auto original = create(target);
    unsigned char trampolineBytes[MEMORY_SLOT_SIZE];
    memcpy(trampolineBytes, original, sizeof trampolineBytes);
    if (enabled) assert(MH_EnableHook(target) == MH_OK);
    PendingCall state{CreateEventW(nullptr, TRUE, FALSE, nullptr),
                      CreateEventW(nullptr, TRUE, FALSE, nullptr), original};
    assert(state.entered && state.release);
    activeCall = &state;
    HANDLE thread = CreateThread(nullptr, 0, finishOriginal, &state, 0, nullptr);
    assert(thread && WaitForSingleObject(state.entered, 5000) == WAIT_OBJECT_0);
    // The real thread is now inside C++/WaitForSingleObject. Its current RIP is
    // outside the trampoline, but its call chain still returns through it.
    assert((global ? MH_Uninitialize() : MH_RemoveHook(target)) == MH_OK);
    MEMORY_BASIC_INFORMATION info{};
    assert(VirtualQuery(original, &info, sizeof info) == sizeof info && info.State == MEM_COMMIT);
    assert(!memcmp(original, trampolineBytes, sizeof trampolineBytes));
    if (global) assert(MH_Initialize() == MH_OK);
    void* replacement = create(target);
    assert(replacement != original && !memcmp(original, trampolineBytes, sizeof trampolineBytes));
    assert(MH_RemoveHook(target) == MH_OK);
    assert(SetEvent(state.release));
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && state.result == 42);
    activeCall = nullptr;
    assert(CloseHandle(thread) && CloseHandle(state.entered) && CloseHandle(state.release));
    assert(!memcmp(target, body, sizeof body));
    printf("PASS: real in-flight return survives %s and new allocation (%s entry)\n",
           global ? "uninitialize" : "remove", enabled ? "enabled" : "never enabled");
}

static void hotPatchFailure() {
    CodeBlock code;
    auto target = code.memory + 64;
    memset(target - 5, 0xcc, 5);
    const unsigned char body[] = {0xc3, 0x90, 1, 2, 3};
    memcpy(target, body, sizeof body);
    code.readOnly();
    assert(FlushInstructionCache(GetCurrentProcess(), code.memory, 128));
    create(target);
    assert(MH_EnableHook(target) == MH_OK && target[-5] == 0xe9 && target[0] == 0xeb);
    ParkedThread thread;
    thread.place(target - 5);
    inject(Fault::protect, 0, code.memory);
    assert(MH_DisableHook(target) == MH_ERROR_MEMORY_PROTECT && faultHits > 0);
    assert(target[-5] == 0xe9 && thread.ip() == DWORD64(target - 5));
    inject(Fault::set, thread.id);
    assert(MH_DisableHook(target) == MH_ERROR_THREAD_CONTEXT && faultHits > 0);
    assert(target[-5] == 0xe9 && thread.ip() == DWORD64(target - 5));
    inject(Fault::none);
    assert(MH_DisableHook(target) == MH_OK && thread.ip() == DWORD64(target));
    assert(MH_RemoveHook(target) == MH_OK && !memcmp(target, body, sizeof body));
    for (int index = -5; index < 0; ++index) assert(target[index] == 0xcc);
    puts("PASS: failed hot-patch disable never resumes into restored prefix padding");
}

static void sharedAndCrossedPages() {
    CodeBlock code(8192);
    auto a = code.function(32), b = code.function(96), cross = code.function(4094);
    code.readOnly();
    create(a);
    create(b);
    create(cross);
    assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    assert(call(a) == 107 && call(b) == 107 && call(cross) == 107);
    readOnly(code.memory);
    readOnly(code.memory + 4096);
    assert(MH_DisableHook(MH_ALL_HOOKS) == MH_OK);
    assert(MH_RemoveHook(a) == MH_OK && MH_RemoveHook(b) == MH_OK && MH_RemoveHook(cross) == MH_OK);
    sameCode(a);
    sameCode(b);
    sameCode(cross);
    readOnly(code.memory);
    readOnly(code.memory + 4096);
    puts("PASS: shared pages and a page-crossing patch retain their original protection");
}

int main() {
    assert(MH_Initialize() == MH_OK);
    normalTransition();
    for (Fault mode : {Fault::get, Fault::set, Fault::open, Fault::suspend}) earlyFailure(mode);
    for (bool enabled : {false, true})
        for (bool queued : {false, true}) batchProtectionFailure(enabled, queued);
    rejectedRollbackRetainsTrampoline();
    retirementWithBlockedContext(false);
    retirementWithBlockedContext(true);
    lateProtectionFailure();
    failedResume();
    failedFlush();
    generatedInstructionRetainsTrampoline();
    for (bool enabled : {false, true})
        for (bool global : {false, true}) pendingReturnSurvives(enabled, global);
    hotPatchFailure();
    sharedAndCrossedPages();
    assert(MH_Uninitialize() == MH_OK);
    puts("MINHOOK TRANSACTION TESTS PASSED; ALL OWNED THREAD STARTUP CONTEXTS RESTORED");
}
