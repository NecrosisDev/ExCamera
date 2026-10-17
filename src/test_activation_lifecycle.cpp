// Calls the production export and status writer with an owned worker stub.
// No game process, executable, installation, native hook or window is used.
#ifdef NDEBUG
#error "The tests require assert()."
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "first_person.cpp"
#include <cassert>

static HANDLE workerFinished = nullptr;
static volatile LONG workerStarts = 0;
static DWORD WINAPI ownedWorker(void*) {
    InterlockedIncrement(&workerStarts);
    SetEvent(workerFinished);
    return 0;
}
static fp::ActivationStatus snapshot() {
    FILE* file = tmpfile();
    assert(file);
    writeRunningStatus(file, Aim{});
    const long length = ftell(file);
    assert(length > 0);
    std::string data(size_t(length), '\0');
    rewind(file);
    assert(fread(&data[0], 1, data.size(), file) == data.size());
    fclose(file);
    fp::ActivationStatus status;
    assert(fp::readActivationStatus(data.data(), data.size(), status));
    return status;
}
static fp::ActivationResult verdict(const fp::ActivationStatus& status, uint64_t request) {
    return fp::activationResult(status, GetCurrentProcessId(), activationProcessStarted(), request,
                                FP_REVISION, FP_BUILD);
}
int main() {
    using Phase = fp::ActivationPhase;
    using Result = fp::ActivationResult;
    workerFinished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(workerFinished);
    sessionEntry = ownedWorker;
    assert(!enableWithStatus(nullptr) && !sessionStarted);
    assert(enableWithStatus((void*)uintptr_t(101)));
    assert(WaitForSingleObject(workerFinished, 5000) == WAIT_OBJECT_0);
    assert(workerStarts == 1 && sessionStarted);
    assert(verdict(snapshot(), 101) == Result::waiting);
    setActivationPhase(Phase::running);
    const fp::ActivationStatus first = snapshot();
    assert(verdict(first, 101) == Result::ready);
    assert(enableWithStatus((void*)uintptr_t(102)) && !rearmRequested && workerStarts == 1);
    assert(verdict(first, 102) == Result::waiting);
    assert(verdict(snapshot(), 102) == Result::ready);

    // F7 already requested stop, but the worker has not reached its cleanup loop.
    InterlockedExchange(&stopping, 1);
    assert(enableWithStatus((void*)uintptr_t(103)) && rearmRequested);
    assert(verdict(snapshot(), 103) == Result::waiting);
    setActivationPhase(Phase::stopped);
    assert(enableWithStatus((void*)uintptr_t(104)) && rearmRequested && workerStarts == 1);
    waitForRearmKey(); // Consumes the explicit request without reading gameplay input.
    assert(!rearmRequested);
    setActivationPhase(Phase::starting);
    assert(verdict(snapshot(), 104) == Result::waiting);
    rearmRunning();
    setActivationPhase(Phase::running);
    assert(verdict(snapshot(), 104) == Result::ready);

    assert(disable() && stopping && emtkOff);
    assert(enableWithStatus((void*)uintptr_t(105)) && rearmRequested && !emtkOff);
    waitForRearmKey();
    rearmRunning();
    setActivationPhase(Phase::failed);
    assert(verdict(snapshot(), 105) == Result::failed);
    assert(!enableWithStatus((void*)uintptr_t(106)) && activationRequest == 105 && !rearmRequested);
    assert(workerStarts == 1);
    CloseHandle(workerFinished);
    puts("ACTIVATION LIFECYCLE: one worker, fresh running re-entry, pending/clean F7 re-arm, "
         "EMTK re-enable and fault refusal passed. Owned fixture only.");
}
