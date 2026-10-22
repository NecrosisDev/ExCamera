// Optional reviewed-build free-item throw. Native release settles for8 physics
// steps; retain only metadata until the SAME live ordinary body integrates again.
// No scene timer, constraint, current-point or settling-flag edits. One uniform
// older-history delta enters the native integrator after its exact8..0 sequence.
using QueueWork = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using ApplyHeldRelease = uintptr_t (*)(uintptr_t);
using HeldBodyIntegrate = void (*)(uintptr_t);
using HeldBodyDestroy = void (*)(uintptr_t, intptr_t);
static QueueWork originalQueueWork;
static ApplyHeldRelease originalApplyHeldRelease;
static HeldBodyIntegrate originalHeldBodyIntegrate;
static HeldBodyDestroy originalHeldBodyDestroy;
static std::atomic<bool> heldThrowReady{false};
static bool heldThrowHooksCreated = false;
static constexpr ULONGLONG heldThrowDeadlineMs = 250;
enum class FlingPhase : unsigned char { empty, queued, nativeRelease, settling };
struct QueuedFling {
    FlingPhase phase = FlingPhase::empty;
    uintptr_t payload = 0, body = 0, object = 0, root = 0, scene = 0, sector = 0,
              context = 0, thread = 0;
    ULONGLONG sequence = 0, hold = 0, cancellation = 0, queuedAt = 0;
    unsigned generation = 0;
    DWORD threadId = 0;
    int mode = 0, frequency = 0, expectedSettling = 8;
    float velocity[3]{}, step = 0.f;
};
// All metadata AND the final12-float write share heldThrowLock with cancellation.
// No native callback is ever made while that lock is held.
static std::array<QueuedFling, 8> queuedFlings;
// Admission hint only. Publish every table mutation under heldThrowLock; a true
// reader still takes that lock and revalidates the complete current ticket.
static std::atomic<bool> queuedFlingsPresent{false};
static void publishQueuedFlingsLocked() {
    for (const auto& entry : queuedFlings)
        if (entry.phase != FlingPhase::empty) {
            queuedFlingsPresent.store(true, std::memory_order_release);
            return;
        }
    queuedFlingsPresent.store(false, std::memory_order_release);
}
static ULONGLONG flingSequence = 0, flingLifetimeEpoch = 0;
static thread_local unsigned heldBodyIntegrationDepth = 0;
static void cancelQueuedHeldThrowsLocked() {
    for (auto& entry : queuedFlings) entry = {};
    queuedFlingsPresent.store(false, std::memory_order_release);
}
static void cancelBodyFlingsLocked(uintptr_t body) {
    bool changed = false;
    for (auto& entry : queuedFlings)
        if (entry.body == body) {
            changed = changed || entry.phase != FlingPhase::empty;
            entry = {};
        }
    if (changed) publishQueuedFlingsLocked();
}
static QueuedFling* findFlingLocked(ULONGLONG sequence) {
    if (sequence)
        for (auto& entry : queuedFlings)
            if (entry.sequence == sequence && entry.phase != FlingPhase::empty) return &entry;
    return nullptr;
}
static bool flingViewOwned() {
    return base && heldThrowReady.load() && viewMode.load() < 2 &&
           InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0);
}
// Called only under native queue/held-body ownership, or with a live integrator
// argument. No background pass dereferences stored body addresses.
static bool heldThrowBodyIdentity(const QueuedFling& release) {
    const uintptr_t body = release.body, object = release.object;
    if (!body || !object || !readable(body, field::bodyPart::releaseSettling + sizeof(int)) ||
        !readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t)) ||
        *(uintptr_t*)(body + field::bodyPart::metadata) != gameAt(fp::sig::heldBodyMetadata) ||
        (*(unsigned*)(body + field::bodyPart::flags) & 0xf0000000u) != 0x10000000u ||
        (*(unsigned*)(body + field::bodyPart::motionFlags) & 0x40u) ||
        *(uintptr_t*)(body + field::bodyPart::meshNode) != object ||
        *(uintptr_t*)(object + field::sceneNode::bodyPart) != body ||
        (*(unsigned*)(object + field::sceneNode::queryFlags) & 0x100000u) ||
        *(uintptr_t*)(body + field::bodyPart::rootPart) != body ||
        *(uintptr_t*)(body + field::bodyPart::parentPart) != body ||
        *(int*)(body + field::bodyPart::childCount) != 0 ||
        *(int*)(body + field::bodyPart::constraintCount) != 0 ||
        *(uintptr_t*)(object + field::sceneNode::sector) != release.sector ||
        !readable(release.sector, field::sector::scene + sizeof(uintptr_t)) ||
        *(uintptr_t*)(release.sector + field::sector::scene) != release.scene ||
        !readable(release.scene, field::scene::physicsContext + sizeof(uintptr_t)) ||
        *(uintptr_t*)(release.scene + field::scene::physicsContext) != release.context ||
        !readable(release.context, field::physicsContext::running + 1) ||
        *(uintptr_t*)(release.context + field::physicsContext::scene) != release.scene ||
        *(int*)(release.context + field::physicsContext::frequencyHz) != release.frequency)
        return false;
    const unsigned lifecycle = *(unsigned*)(release.scene + field::scene::lifecycleFlags);
    return (lifecycle & 0x2000000u) && !(lifecycle & 0x4000000u);
}
// The work queue is drained BEFORE the scheduler publishes current scene/context
// or running159. Publication requires its real worker, not those later stage bits.
static bool heldThrowWorker(const QueuedFling& release, bool integrating) {
    if (!release.thread || !release.threadId ||
        *(uintptr_t*)gameAt(fp::sig::nativePhysicsThread) != release.thread ||
        !readable(release.thread, field::nativeThread::id + sizeof(DWORD)) ||
        *(DWORD*)(release.thread + field::nativeThread::id) != release.threadId ||
        GetCurrentThreadId() != release.threadId)
        return false;
    return !integrating ||
           (*(uintptr_t*)gameAt(fp::sig::nativePhysicsContext) == release.context &&
            *(uintptr_t*)gameAt(fp::sig::nativePhysicsScene) == release.scene &&
            *(unsigned char*)(release.context + field::physicsContext::running));
}
static bool heldThrowOwnerLocked(const QueuedFling& release) {
    return flingViewOwned() && release.mode == viewMode.load() &&
           release.cancellation == heldThrowCancellation && release.hold == holdGeneration.load() &&
           release.generation == inputOwner.generation() &&
           *(uintptr_t*)gameAt(fp::sig::playerRoot) == release.root &&
           *(uintptr_t*)gameAt(fp::sig::currentScene) == release.scene &&
           GetTickCount64() - release.queuedAt <= heldThrowDeadlineMs;
}
static QueuedFling captureQueuedFling(uintptr_t payload, uintptr_t body, const HeldThrowIntent& intent) {
    QueuedFling release;
    if (!flingViewOwned()) return release;
    __try {
        const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
        if (!readable(drag, field::drag::invalidDrop + 1) ||
            *(unsigned*)(drag + field::drag::flags) != 0x2000 ||
            *(unsigned char*)(drag + field::drag::invalidDrop)) return release;
        const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
        if (!object || object != intent.object || !body || intent.hold != holdGeneration.load() ||
            intent.generation != inputOwner.generation() ||
            !readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t))) return release;
        const ULONGLONG now = GetTickCount64();
        if (now - intent.at > fp::flingFreshMs || !(intent.speed > 1.f) || !std::isfinite(intent.speed))
            return release;
        release.body = body; release.object = object;
        release.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        release.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
        if (!release.root || !release.scene || release.root != intent.root || release.scene != intent.scene)
            return {};
        release.sector = *(uintptr_t*)(object + field::sceneNode::sector);
        if (!readable(release.sector, field::sector::scene + sizeof(uintptr_t)) ||
            !readable(release.scene, field::scene::physicsContext + sizeof(uintptr_t))) return {};
        release.context = *(uintptr_t*)(release.scene + field::scene::physicsContext);
        if (!readable(release.context, field::physicsContext::running + 1)) return {};
        release.frequency = *(int*)(release.context + field::physicsContext::frequencyHz);
        if (release.frequency <= 10 || release.frequency > 1000) return {};
        release.step = 1.f / static_cast<float>(release.frequency);
        release.thread = *(uintptr_t*)gameAt(fp::sig::nativePhysicsThread);
        if (!readable(release.thread, field::nativeThread::id + sizeof(DWORD))) return {};
        release.threadId = *(DWORD*)(release.thread + field::nativeThread::id);
        if (!release.threadId || !heldThrowBodyIdentity(release)) return {};
        for (int i = 0; i < 3; ++i) {
            release.velocity[i] = intent.direction[i] * intent.speed;
            if (!std::isfinite(release.velocity[i])) return {};
        }
        release.payload = payload; release.phase = FlingPhase::queued;
        release.hold = intent.hold; release.cancellation = intent.cancellation;
        release.generation = intent.generation; release.mode = viewMode.load();
        release.queuedAt = now;
    } __except (EXCEPTION_EXECUTE_HANDLER) { release = {}; }
    return release;
}
static uintptr_t queueWorkFor(uintptr_t caller, uintptr_t queue, uintptr_t callback, uintptr_t payload) {
    const bool withPose = caller == gameAt(fp::sig::heldReleaseQueueReturn) &&
                          callback == gameAt(fp::sig::applyHeldRelease);
    const bool withoutPose = heldThrowReady.load() &&
        caller == gameAt(fp::sig::heldReleaseWithoutPoseQueueReturn) &&
        callback == gameAt(fp::sig::applyHeldReleaseWithoutPose);
    if (withPose || withoutPose) {
        ULONGLONG lifetime;
        AcquireSRWLockExclusive(&heldThrowLock);
        lifetime = flingLifetimeEpoch;
        ReleaseSRWLockExclusive(&heldThrowLock);
        uintptr_t body = withoutPose ? payload : 0;
        __try {
            if (withPose && readable(payload, field::heldRelease::body + sizeof(uintptr_t)))
                body = *(uintptr_t*)(payload + field::heldRelease::body);
        } __except (EXCEPTION_EXECUTE_HANDLER) { body = 0; }
        const HeldThrowIntent intent = withPose ? takeHeldThrow() : HeldThrowIntent{};
        const QueuedFling release = withPose ? captureQueuedFling(payload, body, intent) : QueuedFling{};
        AcquireSRWLockExclusive(&heldThrowLock);
        // Every native release invalidates an earlier body ticket, even no-wheel
        // drops. Freed payload addresses never key a settling-body ticket.
        if (body) cancelBodyFlingsLocked(body);
        const ULONGLONG now = GetTickCount64();
        for (auto& entry : queuedFlings)
            if ((entry.phase != FlingPhase::empty && now - entry.queuedAt > heldThrowDeadlineMs) ||
                ((entry.phase == FlingPhase::queued || entry.phase == FlingPhase::nativeRelease) &&
                 entry.payload == payload)) entry = {};
        __try {
            if (release.phase == FlingPhase::queued && lifetime == flingLifetimeEpoch &&
                heldThrowOwnerLocked(release)) {
                for (auto& entry : queuedFlings)
                    if (entry.phase == FlingPhase::empty) {
                        entry = release; entry.sequence = ++flingSequence;
                        break; // A full table loses only this optional throw, never a native job.
                    }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        publishQueuedFlingsLocked();
        ReleaseSRWLockExclusive(&heldThrowLock);
    }
    return originalQueueWork(queue, callback, payload);
}
static uintptr_t queueWork(uintptr_t queue, uintptr_t callback, uintptr_t payload) {
    return queueWorkFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), queue, callback, payload);
}
static uintptr_t applyHeldRelease(uintptr_t payload) {
    ULONGLONG serial = 0;
    AcquireSRWLockExclusive(&heldThrowLock);
    for (auto& entry : queuedFlings) {
        if (entry.phase != FlingPhase::queued || entry.payload != payload) continue;
        __try {
            if (heldThrowOwnerLocked(entry) && heldThrowBodyIdentity(entry) &&
                heldThrowWorker(entry, false) &&
                readable(payload, field::heldRelease::body + sizeof(uintptr_t)) &&
                *(uintptr_t*)(payload + field::heldRelease::body) == entry.body) {
                serial = entry.sequence; entry.phase = FlingPhase::nativeRelease;
            } else entry = {};
        } __except (EXCEPTION_EXECUTE_HANDLER) { entry = {}; }
        break;
    }
    publishQueuedFlingsLocked();
    ReleaseSRWLockExclusive(&heldThrowLock);
    uintptr_t result = 0;
    bool completed = false;
    __try {
        result = originalApplyHeldRelease(payload); // frees payload; never read it again
        completed = true;
    } __finally {
        AcquireSRWLockExclusive(&heldThrowLock);
        QueuedFling* entry = findFlingLocked(serial);
        if (entry) {
            __try {
                if (completed && entry->phase == FlingPhase::nativeRelease &&
                    heldThrowOwnerLocked(*entry) && heldThrowBodyIdentity(*entry) &&
                    heldThrowWorker(*entry, false) &&
                    *(int*)(entry->body + field::bodyPart::releaseSettling) == 8 &&
                    (*(unsigned*)(entry->body + field::bodyPart::motionFlags) & 0x1000u)) {
                    entry->payload = 0; entry->expectedSettling = 8;
                    entry->phase = FlingPhase::settling;
                } else *entry = {};
            } __except (EXCEPTION_EXECUTE_HANDLER) { *entry = {}; }
        }
        publishQueuedFlingsLocked();
        ReleaseSRWLockExclusive(&heldThrowLock);
    }
    return result;
}
static void heldBodyIntegrateProfileBody(uintptr_t body) {
    ++heldBodyIntegrationDepth;
    // An empty observation linearizes before a concurrent enqueue. Keep depth
    // tracking, native forwarding and unconditional fault cancellation outside
    // the gate: a native call may enqueue before it faults or recursively calls us.
    if (queuedFlingsPresent.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&heldThrowLock);
        for (auto& entry : queuedFlings) {
            if (entry.phase == FlingPhase::empty || entry.body != body) continue;
            __try {
                const bool valid = heldBodyIntegrationDepth == 1 && entry.phase == FlingPhase::settling &&
                    heldThrowOwnerLocked(entry) && heldThrowBodyIdentity(entry) && heldThrowWorker(entry, true) &&
                    (*(unsigned*)(body + field::bodyPart::motionFlags) & 0x1000u) &&
                    *(int*)(body + field::bodyPart::releaseSettling) == entry.expectedSettling;
                if (!valid) entry = {};
                else if (entry.expectedSettling > 0) --entry.expectedSettling;
                else {
                    // Same lock as clearHeldThrow/new-grab/destructor invalidation.
                    // Staged helper changes only12 older-history floats, all or none.
                    const bool applied = heldThrowOwnerLocked(entry) &&
                        fp::addFlingHistoryDelta(entry.velocity, entry.step,
                        reinterpret_cast<float(*)[3]>(body + field::bodyPart::history2));
                    entry = {};
                    if (applied) ++flings;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { entry = {}; }
            publishQueuedFlingsLocked();
            break;
        }
        ReleaseSRWLockExclusive(&heldThrowLock);
    }
    bool completed = false;
    __try { fp::profile::invoke<fp::profile::Hook::heldNative>(originalHeldBodyIntegrate, body); completed = true; }
    __finally {
        if (!completed) {
            AcquireSRWLockExclusive(&heldThrowLock);
            cancelBodyFlingsLocked(body);
            ReleaseSRWLockExclusive(&heldThrowLock);
        }
        --heldBodyIntegrationDepth;
    }
}
static void heldBodyIntegrate(uintptr_t body) {
    return fp::profile::invoke<fp::profile::Hook::heldIntegrate>(heldBodyIntegrateProfileBody, body);
}
static void heldBodyDestroy(uintptr_t body, intptr_t deleting) {
    AcquireSRWLockExclusive(&heldThrowLock);
    ++flingLifetimeEpoch; // fences a capture not yet published in the table
    cancelBodyFlingsLocked(body);
    ReleaseSRWLockExclusive(&heldThrowLock);
    originalHeldBodyDestroy(body, deleting);
}
