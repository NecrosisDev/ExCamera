// Production teardown and worker-completion adapters with controlled MinHook
// cleanup outcomes. The vendor fixture separately exercises real suspended
// handles; this test proves their recovery still has an owner in the session.
#ifdef NDEBUG
#error "This test requires assertions."
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "first_person.cpp"
#include <cassert>

static bool pendingRecovery = false, committedFirst = true, repeatFailure = false;
static bool clearCallbacks = false;
static unsigned failuresRemaining = 0, recoveryCalls = 0, disableCalls = 0, noticeCalls = 0;
static MH_STATUS initialFailure = MH_ERROR_MEMORY_FLUSH;
static ULONGLONG elapsed = 0;
static std::vector<DWORD> waits;
static std::vector<ULONGLONG> recoveryTimes;
static bool enabledEntry = true;

static BOOL WINAPI pending() { return pendingRecovery ? TRUE : FALSE; }
static MH_STATUS WINAPI recover() {
    assert(pendingRecovery);
    ++recoveryCalls;
    recoveryTimes.push_back(elapsed);
    if (failuresRemaining) {
        --failuresRemaining;
        return initialFailure;
    }
    pendingRecovery = false;
    if (clearCallbacks) {
        // A resumed callback can finally return its physical/GL borrows.
        InterlockedExchange(&poseBuildsInFlight, 0);
        InterlockedExchange(&cueCallsInFlight, 0);
    }
    return MH_OK;
}
static void WINAPI waitFor(DWORD milliseconds) {
    assert(milliseconds == 50 || milliseconds == 100);
    waits.push_back(milliseconds);
    elapsed += milliseconds;
}
static MH_STATUS WINAPI disableHooks(LPVOID target) {
    assert(target == MH_ALL_HOOKS);
    assert(!pendingRecovery); // New transactions cannot start ahead of cleanup.
    ++disableCalls;
    if (disableCalls == 1 || repeatFailure) {
        enabledEntry = !committedFirst;
        pendingRecovery = true;
        return initialFailure;
    }
    enabledEntry = false;
    return MH_OK;
}
static MH_STATUS WINAPI rejectWithoutDebt(LPVOID target) {
    assert(target == MH_ALL_HOOKS);
    ++disableCalls;
    return MH_ERROR_MEMORY_PROTECT;
}
static bool noNotice() {
    assert(!pendingRecovery); // Do not acquire reporting/UI locks while suspended.
    ++noticeCalls;
    return false;
}
static bool closingNoticeCreatesDebt() {
    ++noticeCalls;
    if (noticeCalls == 1) {
        pendingRecovery = true;
        failuresRemaining = 1;
    }
    return false;
}
static void reset() {
    pendingRecovery = false;
    committedFirst = true;
    repeatFailure = clearCallbacks = false;
    failuresRemaining = recoveryCalls = disableCalls = noticeCalls = 0;
    initialFailure = MH_ERROR_MEMORY_FLUSH;
    elapsed = 0;
    waits.clear();
    recoveryTimes.clear();
    enabledEntry = true;
    nativeDisableHook = disableHooks;
    nativeHookRecoveryPending = pending;
    recoverNativeHookState = recover;
    hookRecoveryWait = waitFor;
    InterlockedExchange(&protectionFailure, 0);
    InterlockedExchange(&poseBuildsInFlight, 0);
    InterlockedExchange(&cueCallsInFlight, 0);
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (MH_STATUS failure : {MH_ERROR_THREAD_CONTEXT, MH_ERROR_THREAD_SUSPEND,
                              MH_ERROR_MEMORY_FLUSH, MH_ERROR_MEMORY_PROTECT}) {
        for (bool committed : {false, true}) {
            reset();
            initialFailure = failure;
            committedFirst = committed;
            failuresRemaining = 2;
            assert(disableNativeHooks() == MH_OK);
            assert(!pendingRecovery && !enabledEntry && disableCalls == 2 && recoveryCalls == 3);
            assert((waits == std::vector<DWORD>{100, 100}));
            assert((recoveryTimes == std::vector<ULONGLONG>{0, 100, 200}));
        }
    }
    puts("PASS: production disable owns context/resume/flush/protection cleanup before its single transaction retry");

    reset();
    pendingRecovery = true; // Debt left by a rejected activation attempt.
    failuresRemaining = 2;
    assert(disableNativeHooks() == MH_OK);
    assert(!pendingRecovery && disableCalls == 2 && recoveryCalls == 4);
    assert((waits == std::vector<DWORD>{100, 100}));
    puts("PASS: activation rollback services inherited recovery debt before its first disable transaction");

    reset();
    repeatFailure = true;
    failuresRemaining = 1;
    assert(disableNativeHooks() == MH_ERROR_MEMORY_FLUSH);
    assert(disableCalls == 2 && recoveryCalls == 3 && !pendingRecovery && !enabledEntry);
    puts("PASS: second failed transaction is reported only after its suspended-thread cleanup is serviced");

    reset();
    nativeDisableHook = rejectWithoutDebt;
    assert(disableNativeHooks() == MH_ERROR_MEMORY_PROTECT);
    assert(disableCalls == 1 && !recoveryCalls && waits.empty());
    puts("PASS: a rejection without recovery debt does not create a retry loop");

    reset();
    failuresRemaining = 2;
    clearCallbacks = true;
    InterlockedExchange(&poseBuildsInFlight, 1);
    InterlockedExchange(&cueCallsInFlight, 1);
    assert(rollbackDispatch(false, false, false));
    assert(!pendingRecovery && !poseBuildsInFlight && !cueCallsInFlight && recoveryCalls == 3);
    puts("PASS: actual rollback completes native recovery before draining callbacks that could be suspended");

    reset();
    pendingRecovery = true;
    failuresRemaining = 3;
    InterlockedExchange(&stopping, 1);
    InterlockedExchange(&active, 0);
    setActivationPhase(fp::ActivationPhase::failed);
    finishSessionWorker(noNotice);
    assert(!pendingRecovery && recoveryCalls == 4 && noticeCalls == 1 && !disableCalls);
    assert((waits == std::vector<DWORD>{100, 100, 100}));
    assert(activationPhase == fp::ActivationPhase::failed && stopping && !active);
    puts("PASS: absent/dismissed notice cannot abandon cleanup or re-arm the failed session");

    reset();
    finishSessionWorker(closingNoticeCreatesDebt);
    assert(!pendingRecovery && noticeCalls == 2 && recoveryCalls == 2 && !disableCalls);
    assert((waits == std::vector<DWORD>{50, 100}));
    puts("PASS: worker rechecks recovery debt created during the final notice callback");

    reset();
    finishSessionWorker(noNotice);
    assert(!recoveryCalls && !disableCalls && noticeCalls == 1 && waits.empty());
    puts("HOOK RECOVERY ADAPTER TESTS PASSED; OWNED SPIES ONLY");
}
