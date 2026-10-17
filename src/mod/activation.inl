// The worker publishes lifecycle results; exports only identify the request.
#include "../activation_status.h"
static SRWLOCK activationLock = SRWLOCK_INIT;
static fp::ActivationPhase activationPhase = fp::ActivationPhase::idle;
static uint64_t activationRequest = 0;
static void setActivationPhase(fp::ActivationPhase phase) {
    AcquireSRWLockExclusive(&activationLock);
    activationPhase = phase;
    ReleaseSRWLockExclusive(&activationLock);
}
static uint64_t activationProcessStarted() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
static void writeActivationStatus(fp::JsonOut& j) {
    AcquireSRWLockShared(&activationLock);
    const uint64_t request = activationRequest;
    const fp::ActivationPhase phase = fp::effectiveActivationPhase(
        activationPhase, InterlockedCompareExchange(&stopping, 0, 0) != 0);
    ReleaseSRWLockShared(&activationLock);
    j.unum("pid", GetCurrentProcessId())
        .num("revision", FP_REVISION)
        .str("build", FP_BUILD)
        .unum("process_started", activationProcessStarted())
        .unum("activation_request", request)
        .str("activation_state", fp::activationPhaseText(phase));
}
