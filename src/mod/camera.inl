// The camera: the replaced sampler (nav) that puts the view at the head's eye, or behind the shoulder.
// Part of first_person.cpp (one translation unit): included there, in order.

inline bool cameraPosePickProfileBody(uintptr_t camera, const float* pose, float x, float y, uintptr_t out,
                           uintptr_t ignoredHeld = 0) {
    const uintptr_t viewport = pickViewportForCamera(camera);
    if (!viewport || !originalPickRay || !pose) return false;
    const uintptr_t view = *(uintptr_t*)(viewport + field::viewport::view);
    if (!readable(view, field::view::pickFarDepth + sizeof(float))) return false;
    const float pickFar = *(float*)(view + field::view::pickFarDepth);
    if (!std::isfinite(pickFar) || pickFar <= 0.f) return false;
    bool got = false;
    bool mutated = false;
    float keptNear = 0.f;
    float kept[24]{};
    __try {
        keptNear = *(float*)(camera + field::camera::nearPlane);
        std::memcpy(kept, (void*)(camera + field::camera::pose), 48);
        std::memcpy(kept + 12, (void*)(camera + field::camera::pose2), 48);
        __try {
            mutated = true;
            std::memcpy((void*)(camera + field::camera::pose), pose, 48);
            std::memcpy((void*)(camera + field::camera::pose2), pose, 48);
            rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
            *(float*)(camera + field::camera::nearPlane) = 0.f;
            got = pickPastSelf(ownedPickRay, viewport, x, y, out, ignoredHeld) != 0;
        } __finally {
            if (mutated) {
                __try {
                    *(float*)(camera + field::camera::nearPlane) = keptNear;
                    std::memcpy((void*)(camera + field::camera::pose), kept, 48);
                    std::memcpy((void*)(camera + field::camera::pose2), kept + 12, 48);
                    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    } __except (countFaultAndHandle(&cameraFaults)) {
        got = false;
    }
    return got;
}

inline bool cameraPosePick(uintptr_t camera, const float* pose, float x, float y, uintptr_t out,
                           uintptr_t ignoredHeld = 0) {
    return fp::profile::invoke<fp::profile::Hook::cameraPick>(cameraPosePickProfileBody, camera, pose, x, y, out, ignoredHeld);
}

static std::atomic<unsigned> propNearHits{0};

inline float centrePropDepth(uintptr_t camera, const float* pose, uintptr_t root) {
    RECT client{};
    if (!originalPickRay || !hookedWindow || !cachedWindowClient(hookedWindow, client) || client.right < 40 ||
        client.bottom < 40)
        return -1.f;
    alignas(8) unsigned char hit[0x40]{};
    const uintptr_t held = heldNode();
    const bool got =
        cameraPosePick(camera, pose, static_cast<float>(client.right) * 0.5f, static_cast<float>(client.bottom) * 0.5f, (uintptr_t)hit,
                       held);
    const float t = *(float*)(hit + field::pick::distance);
    if (!got || !std::isfinite(t) || t <= 0.f || t > 16.f) return -1.f;
    const uintptr_t node = *(uintptr_t*)hit,
                    playerMesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    unsigned depth = 0;
    MEMORY_BASIC_INFORMATION region{};
    for (uintptr_t at = node;
         at && depth < 64 && (readableRegionContains(region, at, 0x130) || readable(at, 0x130, region));
         at = *(uintptr_t*)(at + field::sceneNode::parent), ++depth) {
        if (at == playerMesh || at == held) return -1.f;
        if (at == *(uintptr_t*)(gameAt(fp::sig::world))) break;
    }
    ++propNearHits;
    return t;
}

static constexpr size_t partRenderSampleSlot = 0x140;
using PartRenderSampler = float (*)(uintptr_t, float);

inline float callPartRenderSampler(uintptr_t sampler, uintptr_t part, float offset) {
    return ((PartRenderSampler)sampler)(part, offset);
}

static float (*partRenderSampler)(uintptr_t, uintptr_t, float) = callPartRenderSampler;

inline bool sampleHeadRenderPose(uintptr_t head, uintptr_t mesh, float offset, float* pose) noexcept {
    __try {
        if (!std::isfinite(offset) || !readable(head, sizeof(uintptr_t)) ||
            !readable(mesh + field::sceneNode::renderTransform, 16 * sizeof(float)))
            return false;
        const uintptr_t table = *(uintptr_t*)head;
        if (!readable(table + partRenderSampleSlot, sizeof(uintptr_t))) return false;
        const uintptr_t sampler = gameAt(fp::sig::headRenderSample);
        if (!sampler || *(uintptr_t*)(table + partRenderSampleSlot) != sampler) return false;
        partRenderSampler(sampler, head, offset);
        if (*(uintptr_t*)(head + field::bodyPart::meshNode) != mesh ||
            *(uintptr_t*)(mesh + field::sceneNode::bodyPart) != head)
            return false;
        const float* sampled = (const float*)(mesh + field::sceneNode::renderTransform);
        for (int row = 0; row < 4; ++row)
            for (int axis = 0; axis < 3; ++axis) pose[row * 3 + axis] = sampled[row * 4 + axis];
        return true;
    } __except (countFaultAndHandle(&poseFaults)) {
        return false;
    }
}

inline bool cameraEntryYaw(uintptr_t root, uintptr_t sector, float& yaw) noexcept {
    __try {
        float selfWorld[3];
        addSectorOrigin((const float*)(root + field::character::position), sector, selfWorld);
        const uintptr_t opponent = *(uintptr_t*)(root + field::character::aimOpponent);
        if (opponent && opponent != root && readable(opponent, field::character::lifeFlags + 4) &&
            !(*(unsigned*)(opponent + field::character::lifeFlags) & 0x100)) {
            const uintptr_t otherSector = characterSector(opponent);
            if (readable(otherSector, field::sector::origin + 12)) {
                float otherWorld[3];
                addSectorOrigin((const float*)(opponent + field::character::position), otherSector, otherWorld);
                const float x = otherWorld[0] - selfWorld[0];
                const float z = otherWorld[2] - selfWorld[2];
                if (std::isfinite(x) && std::isfinite(z) && x * x + z * z > 1.f) {
                    yaw = std::atan2(x, z);
                    return true;
                }
            }
        }
        const auto* direction = (const float*)(root + field::character::facing);
        if (!std::isfinite(direction[0]) || !std::isfinite(direction[2]) ||
            direction[0] * direction[0] + direction[2] * direction[2] < 1.e-6f) return false;
        yaw = std::atan2(direction[0], direction[2]);
        return true;
    } __except (countFaultAndHandle(&cameraFaults)) {
        return false;
    }
}

inline bool updateCameraEntry(uintptr_t root, uintptr_t head, uintptr_t scene, uintptr_t sector) {
    CameraEntryState next = cameraEntry;
    float body[3], distance2 = 0.f;
    addSectorOrigin((const float*)(root + field::character::position), sector, body);
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(body[axis])) return false;
        const float delta = body[axis] - next.body[axis];
        distance2 += delta * delta;
    }
    const bool menu = formShown(*(uintptr_t*)gameAt(fp::sig::endMatchMenu));
    const bool arenaStart = next.menuSeen && !menu;
    const bool entry = !next.valid || next.root != root || next.head != head || next.scene != scene ||
                     distance2 > 500.f * 500.f || arenaStart;
    const bool hadHistory = next.valid;
    const bool combat = *(unsigned char*)(root + field::character::combatStance) != 0;
    next.root = root;
    next.head = head;
    next.scene = scene;
    std::memcpy(next.body, body, sizeof body);
    next.valid = true;
    next.menuSeen = menu;
    if (entry) {
        next.requested = combat || arenaStart;
        next.pending = false;
    }
    cameraEntry = next;
    if (!next.requested || menu || !combat) return false;
    AcquireSRWLockShared(&aimLock);
    const bool established = !hadHistory && aim.valid && aim.root == root && aim.head == head;
    ReleaseSRWLockShared(&aimLock);
    if (established) { cameraEntry.requested = false; return false; }
    float yaw = 0.f;
    if (!cameraEntryYaw(root, sector, yaw)) return false;
    resetViewHistory();
    AcquireSRWLockExclusive(&aimLock);
    aim.valid = false;
    aim.root = root;
    aim.head = head;
    aim.entryPending = true;
    aim.entryYaw = yaw;
    ReleaseSRWLockExclusive(&aimLock);
    thirdPerson.reset();
    tpCombatK = 0.f;
    fp::holdEnd(headHold);
    publishHold();
    next.requested = false;
    next.pending = true;
    next.yaw = yaw;
    cameraEntry = next;
    return true;
}

inline void sampleHeadDiagnostics(uintptr_t head, const float h[12]) {
    if (!headDiagnosticRequested.load(std::memory_order_relaxed) ||
        !headDiagnosticRequested.exchange(false, std::memory_order_relaxed)) return;
    if (readable(head + field::bodyPart::points, 48)) {
        const float(*pt)[3] = (const float(*)[3])(head + field::bodyPart::points);
        float mid[3]{};
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i) mid[i] += 0.25f * pt[k][i];
        float reach = 0.f;
        for (int k = 0; k < 4; ++k) {
            float d = 0.f;
            for (int i = 0; i < 3; ++i) d += (pt[k][i] - mid[i]) * (pt[k][i] - mid[i]);
            reach = std::fmax(reach, std::sqrt(d));
        }
        float a2 = 0.f, a3 = 0.f;
        for (int i = 0; i < 3; ++i) {
            a2 += (mid[i] - h[9 + i]) * h[3 + i];
            a3 += (mid[i] - h[9 + i]) * h[6 + i];
        }
        if (std::isfinite(a2 + a3 + reach)) {
            headMid2 = a2;
            headMid3 = a3;
            headReach = reach;
        }
    }
}

struct NavPassKey {
    int mode = 0;
    int distance = 0;
    int shoulder = 0;
    int height = 0;
    int blockers = 0;
    int lag = 0;
    int nearTenths = 0;
    int eyeBack = 0;
    bool tight = false;
    bool combat = false;
    bool down = false;
    bool lookFollow = false;
};

inline NavPassKey captureNavPassKey(int mode) noexcept {
    NavPassKey key{};
    key.mode = mode;
    key.distance = tpDistance.load(std::memory_order_relaxed);
    key.shoulder = tpShoulder.load(std::memory_order_relaxed);
    key.height = tpHeight.load(std::memory_order_relaxed);
    key.blockers = tpBlockers.load(std::memory_order_relaxed);
    key.lag = tpLag.load(std::memory_order_relaxed);
    key.nearTenths = nearClip.load(std::memory_order_relaxed);
    key.eyeBack = eyeSetback.load(std::memory_order_relaxed);
    key.tight = tpTight.load(std::memory_order_relaxed);
    key.combat = tpCombat.load(std::memory_order_relaxed);
    key.down = tpDown.load(std::memory_order_relaxed);
    key.lookFollow = tpLookFollow.load(std::memory_order_relaxed);
    return key;
}

inline bool sameNavPassKey(const NavPassKey& a, const NavPassKey& b) noexcept {
    return a.mode == b.mode && a.distance == b.distance && a.shoulder == b.shoulder &&
           a.height == b.height && a.blockers == b.blockers && a.lag == b.lag &&
           a.nearTenths == b.nearTenths && a.eyeBack == b.eyeBack &&
           a.tight == b.tight && a.combat == b.combat && a.down == b.down && a.lookFollow == b.lookFollow;
}

struct NavSolvedPass {
    bool valid = false;
    bool sideEffectsApplied = false;
    LONG serial = -1;
    uintptr_t scene = 0;
    uintptr_t sector = 0;
    NavPassKey key{};
    CameraFilterState state{};
    float pose[12]{};
    float target[3]{};
    float headYaw = 0.f;
    float headPitch = 0.f;
    float nearValue = 1.f;
    TpAimRequest aim{};
};

static NavSolvedPass navPassCache;
static ULONGLONG cameraBlendOf = 0;
static float cameraBlendFromFov = 75.f;

static void resetCameraPassCache() {
    navPassCache = {};
    cameraBlendOf = 0;
    cameraBlendFromFov = 75.f;
}

// cameraLock is held by navProfileBody, which releases it even when a fault unwinds past.
inline uintptr_t navLocked(void* self, float offset) {
    InterlockedExchange(&navThread, LONG(GetCurrentThreadId()));
    uintptr_t view = 0, camera = 0;
    currentCamera(view, camera);
    reclaimRetired(view, camera);
    const uintptr_t priorRoot = savedRoot, priorScene = savedScene;
    bool hadOwned = false;
    if (savedCamera && checkSavedOwner(view, camera) && savedController == (uintptr_t)self) {
        hadOwned = true;
        restoreOwnedCamera(view, camera);
    }
    uintptr_t result = fp::profile::invoke<fp::profile::Hook::navNative>(originalNav, self, offset);
    currentCamera(view, camera);
    if (!checkSavedOwner(view, camera)) return result;
    if (!camera) {
        resetViewHistory();
        return result;
    }
    if (*(void**)(camera + field::camera::controller) != self) {
        if (hadOwned) resetViewHistory();
        return result;
    }
    auto nativeReturn = [&]() {
        if (hadOwned) {
            *(unsigned*)(camera + field::camera::dirty) = 1;
            rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
            rebuildProjection(view);
        }
        return result;
    };
    auto root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) || !root) {
        resetViewHistory();
        return nativeReturn();
    }
    if (park.active) {
        resetViewHistory();
        return nativeReturn();
    }
    bool wasLive = hadOwned && filterRoot == root && filterHead;
    LARGE_INTEGER now;
    auto commit = [&](const float* pose, float fov = -1.f) {
        std::memcpy(stockPose, (void*)(camera + field::camera::pose), 48);
        stockFov = *(float*)(camera + field::camera::fov);
        {
            const float n = *(float*)(camera + field::camera::nearPlane);
            if (!(camera == savedCamera && n == renderedNear && n != 1.f)) stockNear = n;
        }
        savedCamera = camera;
        savedView = view;
        savedController = (uintptr_t)self;
        savedRoot = root;
        savedScene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
        if (park.rearmPending) {
            park.rearmPending = false;
            ++rearms;
            InterlockedExchange(&parkedNow, 0);
        }
        InterlockedExchange(&pendingRestore, 1);
        std::memcpy((void*)(camera + field::camera::pose), pose, 48);
        std::memcpy((void*)(camera + field::camera::pose2), pose, 48);
        rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
        renderedFov = fov > 0.f && std::isfinite(fov) ? fov : static_cast<float>(selectedFov.load());
        *(float*)(camera + field::camera::fov) = renderedFov;
        *(float*)(camera + field::camera::nearPlane) = renderedNear;
        *(unsigned*)(camera + field::camera::dirty) = 1;
        rebuildProjection(view);
    };
    auto reject = [&](int why = 7) {
        ++headRejects[why < 0 || why > 7 ? 7 : why];
        if (wasLive) {
            if (uiOpen() || *(uintptr_t*)(gameAt(fp::sig::currentScene)) != priorScene) {
                resetViewHistory();
                fp::holdEnd(headHold);
                publishHold();
                savedRoot = priorRoot;
                savedScene = priorScene;
                parkCamera(fp::OwnerChange::parkRestore);
                return nativeReturn();
            }
            if (fp::holdStep(headHold) == fp::HoldStep::hold &&
                !InterlockedCompareExchange(&stopping, 0, 0) && InterlockedCompareExchange(&active, 0, 0)) {
                publishHold();
                QueryPerformanceCounter(&now);
                filterTime = now;
                cameraFrame.valid = false;
                navPassCache = {};
                float held[12];
                std::memcpy(held, lastCamera, 48);
                commit(held, renderedFov);
                return result;
            }
            publishHold();
            resetViewHistory();
            savedRoot = priorRoot;
            savedScene = priorScene;
            parkLive(fp::OwnerChange::parkRestore);
            return nativeReturn();
        }
        resetViewHistory();
        return nativeReturn();
    };
    auto head = *(uintptr_t*)(root + field::character::headPart);
    if (!head) return reject(0);
    if (*(uintptr_t*)(head + field::bodyPart::character) != root) return reject(1);
    auto mesh = *(uintptr_t*)(head + field::bodyPart::meshNode);
    if (!mesh || *(uintptr_t*)(mesh + field::sceneNode::bodyPart) != head) return reject(2);
    const auto scene = *(uintptr_t*)(mesh + field::sceneNode::sector);
    if (!scene) return reject(5);
    const uintptr_t worldScene = *(uintptr_t*)gameAt(fp::sig::currentScene);
    if (viewMode.load() < 2 && updateCameraEntry(root, head, worldScene, scene)) wasLive = false;
    const int mode = viewMode.load();
    const LONG serial = InterlockedCompareExchange(&swapCalls, 0, 0);
    const bool sameLifetime = filterRoot == root && filterHead == head &&
                              cameraFrame.view == view && cameraFrame.camera == camera &&
                              cameraFrame.controller == (uintptr_t)self && cameraFrame.root == root &&
                              cameraFrame.head == head && cameraFrame.scene == worldScene && cameraFrame.mode == mode;
    if (!cameraFrame.valid || !sameLifetime || cameraFrame.serial != serial) {
        navPassCache = {};
        const CameraFilterState before = sameLifetime
                                             ? CameraFilterState{thirdPerson, nearPlane, tpCombatK}
                                             : CameraFilterState{};
        QueryPerformanceCounter(&now);
        cameraFrame = {};
        cameraFrame.valid = true;
        cameraFrame.serial = serial;
        cameraFrame.view = view;
        cameraFrame.camera = camera;
        cameraFrame.controller = (uintptr_t)self;
        cameraFrame.root = root;
        cameraFrame.head = head;
        cameraFrame.scene = worldScene;
        cameraFrame.mode = mode;
        cameraFrame.effectiveFov = fp::resolveCameraFov(mode, static_cast<float>(selectedFov.load()), static_cast<float>(tpFov.load()));
        cameraFrame.time = now;
        cameraFrame.tick = GetTickCount64();
        cameraFrame.seconds = sameLifetime && frequency.QuadPart
                                  ? static_cast<float>(double(now.QuadPart - filterTime.QuadPart) / double(frequency.QuadPart))
                                  : 1.f;
        cameraFrame.before = before;
        AcquireSRWLockShared(&aimLock);
        if (aim.valid && aim.root == root && aim.head == head &&
            std::isfinite(aim.yaw) && std::isfinite(aim.pitch)) {
            cameraFrame.eyeLookValid = true;
            cameraFrame.eyeYaw = aim.yaw;
            cameraFrame.eyePitch = aim.pitch;
            cameraEntry.pending = false;
        } else if (cameraEntry.pending && cameraEntry.root == root &&
                   cameraEntry.head == head && cameraEntry.scene == worldScene) {
            cameraFrame.eyeLookValid = true;
            cameraFrame.eyeYaw = cameraEntry.yaw;
            cameraFrame.eyePitch = 0.f;
        }
        ReleaseSRWLockShared(&aimLock);
        if (!sameLifetime) trackingView.clear();
        auto tracked = trackingSnapshot();
        if (tracked.received <= trackingAcceptAfter.load()) tracked.valid = false;
        const bool acceptTracking = trackingEnabled.load() && mode < 2 &&
                                    InterlockedCompareExchange(&inputEnabled, 0, 0) && !cursorFreeNow();
        trackingView.update(tracked, cameraFrame.tick, cameraFrame.seconds, acceptTracking,
                            trackingReset.load());
        std::memcpy(cameraFrame.trackingOffset, trackingView.shown, sizeof cameraFrame.trackingOffset);
        cameraFrame.tracked = false;
        float identity[12]{};
        fp::basis(0, 0, identity);
        for (int i = 0; i < 9; ++i)
            cameraFrame.tracked = cameraFrame.tracked ||
                                  std::fabs(identity[i] - cameraFrame.trackingOffset[i]) > 1.e-5f;

        const float frameDt = (std::min)((std::max)(cameraFrame.seconds, 0.001f), 0.1f);
        if (mode == 0) {
            const bool sprinting = ((physicalMovement() & 1) != 0) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && !cursorFreeNow() && !uiOpen();
            const float targetFovOffset = sprinting ? 4.0f : 0.0f;
            sprintFovOffset += (targetFovOffset - sprintFovOffset) * (std::min)(frameDt * 6.0f, 1.0f);
            cameraFrame.effectiveFov += sprintFovOffset;

            const float decay = std::exp(-frameDt / 0.08f);
            float curPitch = cameraImpactPitch.load(std::memory_order_relaxed) * decay;
            float curYaw = cameraImpactYaw.load(std::memory_order_relaxed) * decay;
            float curRoll = cameraImpactRoll.load(std::memory_order_relaxed) * decay;
            if (std::abs(curPitch) < 1e-4f) curPitch = 0.f;
            if (std::abs(curYaw) < 1e-4f) curYaw = 0.f;
            if (std::abs(curRoll) < 1e-4f) curRoll = 0.f;
            cameraImpactPitch.store(curPitch, std::memory_order_relaxed);
            cameraImpactYaw.store(curYaw, std::memory_order_relaxed);
            cameraImpactRoll.store(curRoll, std::memory_order_relaxed);

            const bool stationary = (physicalMovement() == 0) && !cursorFreeNow() && !uiOpen();
            if (stationary) {
                idleBreatheWeight = (std::min)(1.0f, idleBreatheWeight + frameDt * 0.8f);
                idleBreathePhase += frameDt * 1.57f;
            } else {
                idleBreatheWeight = (std::max)(0.0f, idleBreatheWeight - frameDt * 4.0f);
            }
        } else {
            sprintFovOffset = 0.f;
            idleBreatheWeight = 0.f;
        }
    }
    const NavPassKey passKey = captureNavPassKey(mode);
    const bool evaluateCentre =
        mode != 2 && (!cameraFrame.centreReady || !navPassCache.valid || !sameNavPassKey(navPassCache.key, passKey));
    float h[12], centreHead[12];
    if (evaluateCentre && offset != 0.f && !sampleHeadRenderPose(head, mesh, 0.f, centreHead)) return reject(3);
    if (!sampleHeadRenderPose(head, mesh, offset, h)) return reject(3);
    if (evaluateCentre && offset == 0.f) std::memcpy(centreHead, h, sizeof h);

    auto headProblem = [](const float sampled[12]) noexcept {
        for (int i = 0; i < 12; ++i)
            if (!std::isfinite(sampled[i])) return 3;
        for (int axis = 0; axis < 3; ++axis) {
            float n = 0.f;
            for (int j = 0; j < 3; ++j) n += sampled[axis * 3 + j] * sampled[axis * 3 + j];
            if (n < 0.25f || n > 4.f) return 4;
        }
        return 0;
    };

    if (const int problem = headProblem(h)) return reject(problem);
    if (evaluateCentre)
        if (const int problem = headProblem(centreHead)) return reject(problem);

    float requestedTarget[3], centreTarget[3]{}, body[3], pose[12];
    fp::eyePoint(h, requestedTarget, static_cast<float>(passKey.eyeBack));
    addSectorOrigin(requestedTarget, scene, requestedTarget);
    if (evaluateCentre) {
        fp::eyePoint(centreHead, centreTarget, static_cast<float>(passKey.eyeBack));
        addSectorOrigin(centreTarget, scene, centreTarget);
    }
    addSectorOrigin((const float*)(root + field::character::position), scene, body);
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(requestedTarget[i]) || !std::isfinite(body[i]) ||
            (evaluateCentre && !std::isfinite(centreTarget[i]))) return reject(6);
    }
    sampleHeadDiagnostics(head, h);
    if (!fp::headPose(h, requestedTarget, pose)) return reject(4);

    if (viewMode.load() == 2) {
        const float t = static_cast<float>(GetTickCount64() - tpBlendStart.load()) / 300.f;
        float game[12];
        std::memcpy(game, (void*)(camera + field::camera::pose), 48);
        bool ok = true;
        for (float v : game)
            if (!std::isfinite(v)) ok = false;
        if (t >= 1.f || !ok || InterlockedCompareExchange(&stopping, 0, 0)) {
            viewMode = 0;
            InterlockedExchange(&active, 0);
            AcquireSRWLockExclusive(&aimLock);
            aim.valid = false;
            ReleaseSRWLockExclusive(&aimLock);
            thirdPerson.reset();
            resetViewHistory();
            return nativeReturn();
        }
        fp::blendPose(lastCamera, game, t, pose);
        const ULONGLONG blendStart = tpBlendStart.load();
        if (cameraBlendOf != blendStart) {
            cameraBlendOf = blendStart;
            cameraBlendFromFov = renderedFov;
        }
        const float gameFov = *(float*)(camera + field::camera::fov),
                    s = t * t * (3.f - 2.f * t);
        commit(pose, std::isfinite(gameFov) && gameFov > 1.f && gameFov < 170.f
                         ? cameraBlendFromFov + (gameFov - cameraBlendFromFov) * s
                         : -1.f);
        return result;
    }

    auto applyFirstPersonEffects = [&](float evaluatedPose[12]) {
        if (cameraFrame.tracked)
            fp::composeView(evaluatedPose, cameraFrame.trackingOffset, evaluatedPose);
        const float breathePitch = 0.0015f * std::cos(idleBreathePhase) * idleBreatheWeight;
        const float breatheVertical = 0.35f * std::sin(idleBreathePhase) * idleBreatheWeight;
        const float totalPitch = cameraImpactPitch.load(std::memory_order_relaxed) + breathePitch;
        const float totalYaw = cameraImpactYaw.load(std::memory_order_relaxed);
        if (std::abs(totalPitch) > 1e-5f || std::abs(totalYaw) > 1e-5f) {
            float nudge[12]{};
            fp::basis(totalYaw, totalPitch, nudge);
            fp::composeView(evaluatedPose, nudge, evaluatedPose);
        }
        // The hit's roll, scaled with the rest at its source: right and up turn about the view axis.
        if (const float roll = cameraImpactRoll.load(std::memory_order_relaxed); std::abs(roll) > 1e-5f) {
            const float c = std::cos(roll), s = std::sin(roll);
            const float tilt[12] = {-c, s, 0.f, s, c, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f};
            fp::composeView(evaluatedPose, tilt, evaluatedPose);
        }
        evaluatedPose[10] += breatheVertical;
    };

    auto evaluate = [&](const float sampledHead[12], const float target[3], float dt,
                        CameraFilterState& state, float evaluatedPose[12], float& evaluatedNear,
                        TpAimRequest& request, float& outHeadYaw, float& outHeadPitch) {
        if (!fp::headPose(sampledHead, target, evaluatedPose)) return 4;
        if (cameraFrame.eyeLookValid)
            fp::stabilizeEyeRotation(evaluatedPose, cameraFrame.eyeYaw, cameraFrame.eyePitch);
        outHeadYaw = std::atan2(-evaluatedPose[6], -evaluatedPose[8]);
        outHeadPitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -evaluatedPose[7])));
        if (mode == 1) {
            float yaw = outHeadYaw, pitch = outHeadPitch;
            request = captureTpAimRequest(root, head, scene, yaw, pitch);
            if (cameraFrame.eyeLookValid) { yaw = cameraFrame.eyeYaw; pitch = cameraFrame.eyePitch; }
            float trackedFacing[12]{};
            if (cameraFrame.tracked) {
                fp::basis(yaw, pitch, trackedFacing);
                fp::composeView(trackedFacing, cameraFrame.trackingOffset, trackedFacing);
            }
            const uintptr_t level = levelGrid();
            RECT client{};
            const bool sized =
                hookedWindow && cachedWindowClient(hookedWindow, client) && client.right > 40 && client.bottom > 40;
            const int blockers = passKey.blockers;
            auto clear = [&](const float* origin, const float* dir, float length, bool levelOnly) {
                float allowed = level && levelRay && levelRay2 ? levelClear(level, origin, dir, length)
                                                               : length;
                if (levelOnly) return allowed;
                if (sized && originalPickRay && allowed > 60.f) {
                    float trial[12];
                    std::memcpy(trial, evaluatedPose, 36);
                    for (int i = 0; i < 3; ++i) trial[9 + i] = origin[i] + dir[i] * allowed;
                    float d[3];
                    for (int i = 0; i < 3; ++i) d[i] = origin[i] - trial[9 + i];
                    float px, py;
                    const float shownFov = *(float*)(camera + field::camera::fov);
                    if (std::isfinite(shownFov) && shownFov > 1.f && shownFov < 170.f &&
                        fp::projectCursor(d, trial, static_cast<float>(client.right), static_cast<float>(client.bottom), shownFov, px,
                                          py)) {
                        alignas(8) unsigned char hit[0x40]{};
                        const bool got = cameraPosePick(camera, trial, px, py, (uintptr_t)hit);
                        const float t = *(float*)(hit + field::pick::distance);
                        float selfDist = 1e9f;
                        if (got && std::isfinite(t)) {
                            float s = 0.f;
                            for (int i = 0; i < 3; ++i) {
                                const float p = trial[9 + i] - dir[i] * t - target[i];
                                s += p * p;
                            }
                            selfDist = std::sqrt(s);
                        }
                        const uintptr_t hitNode = got ? *(uintptr_t*)hit : 0;
                        if (got && std::isfinite(t) && t > 0.f && t < allowed - 45.f && selfDist > 70.f) {
                            if (blockers == fp::blockersPush ||
                                tpHideHit(hitNode, root, blockers == fp::blockersHide) == 0) {
                                allowed = std::fmax(12.f, allowed - t - 10.f);
                                ++tpPropPulls;
                            }
                        }
                    }
                }
                return allowed;
            };
            {
                const bool fighting = passKey.combat && readable(root, 0x1300) &&
                                      *(unsigned char*)(root + field::character::combatStance) != 0;
                const float a = std::isfinite(dt) && dt >= 0.f && dt <= 0.5f ? 1.f - std::exp(-dt / 0.35f) : 1.f;
                state.combat += ((fighting ? 1.f : 0.f) - state.combat) * a;
            }
            state.third.lag = static_cast<float>(passKey.lag);
            {
                const bool downNow =
                    passKey.down &&
                    fp::knockdownStep(state.third.knock, *(unsigned*)(root + field::character::stateFlags),
                                      *(unsigned*)(root + field::character::lifeFlags), cameraFrame.tick);
                if (!passKey.down) state.third.knock = fp::KnockdownFollow{};
                if (downNow)
                    for (int i = 0; i < 3; ++i)
                        state.third.downPoint[i] = 0.5f * (target[i] + body[i]) + (i == 1 ? 40.f : 0.f);
                state.third.down = downNow;
            }
            // Follow the look angle (fp::lookFollowFraming): out of combat and on your feet only, eased in and out
            // so the boom glides, and a wall that pins the camera still decides how far back it gets.
            {
                const float wanted = passKey.lookFollow && state.combat < 0.1f && !state.third.down
                                         ? fp::lookFollowTarget(pitch) : 0.f;
                const bool fresh = !state.third.valid || !std::isfinite(dt) || dt < 0.f || dt > 0.5f;
                state.third.look = fresh ? wanted
                                         : state.third.look + (wanted - state.third.look) *
                                                                  (1.f - std::exp(-dt / fp::lookFollowEaseSeconds));
            }
            float lookScale = 1.f, lookHeight = 0.f;
            fp::lookFollowFraming(state.third.look, lookScale, lookHeight);
            state.third.update(target, yaw, pitch, static_cast<float>(passKey.distance) * (1.f - 0.2f * state.combat) * lookScale,
                               static_cast<float>(passKey.shoulder) * (1.f - 0.7f * state.combat),
                               static_cast<float>(passKey.height) + lookHeight, dt,
                               clear, evaluatedPose, passKey.tight,
                               cameraFrame.tracked ? trackedFacing : nullptr);
        } else {
            state.third.reset();
            state.combat = 0.f;
            applyFirstPersonEffects(evaluatedPose);
        }
        {
            const float minimumNear = static_cast<float>(passKey.nearTenths) * 0.1f;
            float clearance = viewClearance(evaluatedPose, cameraFrame.effectiveFov, 2.f * cameraNearMaximum);
            if (mode == 0 && !(clearance >= 0.f && clearance <= 2.f * minimumNear)) {
                const float prop = centrePropDepth(camera, evaluatedPose, root);
                if (prop > 0.f && (clearance < 0.f || prop < clearance)) clearance = prop;
            }
            evaluatedNear =
                state.clipFilter.update(clearance < 0.f ? 2.f : clearance, dt, minimumNear, cameraNearMaximum);
        }
        for (int i = 0; i < 12; ++i)
            if (!std::isfinite(evaluatedPose[i])) return 7;
        return std::isfinite(evaluatedNear) && evaluatedNear > 0.f ? 0 : 7;
    };

    auto storePassCache = [&](const CameraFilterState& solvedState, const float solvedPose[12],
                              const float solvedTarget[3], float solvedHeadYaw, float solvedHeadPitch,
                              float solvedNear, const TpAimRequest& solvedAim) {
        navPassCache.valid = true;
        navPassCache.sideEffectsApplied = false;
        navPassCache.serial = serial;
        navPassCache.scene = worldScene;
        navPassCache.sector = scene;
        navPassCache.key = passKey;
        navPassCache.state = solvedState;
        std::memcpy(navPassCache.pose, solvedPose, sizeof navPassCache.pose);
        std::memcpy(navPassCache.target, solvedTarget, sizeof navPassCache.target);
        navPassCache.headYaw = solvedHeadYaw;
        navPassCache.headPitch = solvedHeadPitch;
        navPassCache.nearValue = solvedNear;
        navPassCache.aim = solvedAim;
    };

    CameraFilterState centre = cameraFrame.before, shown = cameraFrame.before;
    float centrePose[12]{}, centreNear = renderedNear, shownNear = renderedNear;
    float centreHeadYaw = 0.f, centreHeadPitch = 0.f;
    TpAimRequest centreAim, cameraAim;
    bool reusedSideEffects = false;
    if (evaluateCentre) {
        if (const int problem = evaluate(centreHead, centreTarget, cameraFrame.seconds, centre,
                                         centrePose, centreNear, centreAim, centreHeadYaw, centreHeadPitch))
            return reject(problem);
        storePassCache(centre, centrePose, centreTarget, centreHeadYaw, centreHeadPitch, centreNear, centreAim);
    }
    if (offset == 0.f && evaluateCentre) {
        shown = centre;
        std::memcpy(pose, centrePose, sizeof pose);
        shownNear = centreNear;
        cameraAim = centreAim;
    } else {
        const float elapsed = std::isfinite(cameraFrame.seconds) && cameraFrame.seconds >= 0.f &&
                                      cameraFrame.seconds <= 0.5f
                                  ? cameraFrame.seconds
                                  : 1.f;
        float targetDist2 = 1e9f;
        bool canReuse = navPassCache.valid && navPassCache.serial == serial &&
                        navPassCache.scene == worldScene && navPassCache.sector == scene &&
                        sameNavPassKey(navPassCache.key, passKey);
        if (canReuse) {
            targetDist2 = 0.f;
            for (int i = 0; i < 3; ++i) {
                const float delta = requestedTarget[i] - navPassCache.target[i];
                targetDist2 += delta * delta;
            }
            if (!std::isfinite(targetDist2) || targetDist2 > 64.f) canReuse = false;
        }
        if (canReuse && mode == 1 && !cameraFrame.eyeLookValid) {
            const float candYaw = std::atan2(-pose[6], -pose[8]);
            const float candPitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -pose[7])));
            if (std::fabs(fp::wrapPi(candYaw - navPassCache.headYaw)) > 1e-3f ||
                std::fabs(candPitch - navPassCache.headPitch) > 1e-3f)
                canReuse = false;
        }
        if (canReuse) {
            shown = navPassCache.state;
            shownNear = navPassCache.nearValue;
            cameraAim = navPassCache.aim;
            reusedSideEffects = navPassCache.sideEffectsApplied;
            if (mode == 0) {
                if (cameraFrame.eyeLookValid)
                    fp::stabilizeEyeRotation(pose, cameraFrame.eyeYaw, cameraFrame.eyePitch);
                applyFirstPersonEffects(pose);
            } else {
                std::memcpy(pose, navPassCache.pose, sizeof pose);
                if (targetDist2 > 1e-6f && !shown.third.down &&
                    shown.third.lastPivotPulled <= 0.f && shown.third.lastLostHeight <= 0.f) {
                    const float lagClamped = std::clamp(shown.third.lag, 0.f, 100.f);
                    const float tau = 0.08f + 0.002f * lagClamped;
                    const float ax = elapsed >= 0.5f ? 1.f : (1.f - std::exp(-elapsed / tau));
                    const float ay = elapsed >= 0.5f ? 1.f : (1.f - std::exp(-elapsed / (std::max)(tau, 0.15f)));
                    for (int i = 0; i < 3; ++i) {
                        const float step = (requestedTarget[i] - navPassCache.target[i]) * (i == 1 ? ay : ax);
                        shown.third.pivot[i] += step;
                        pose[9 + i] += step;
                    }
                }
            }
        } else {
            float shownHeadYaw = 0.f, shownHeadPitch = 0.f;
            if (const int problem = evaluate(h, requestedTarget, elapsed, shown, pose, shownNear, cameraAim,
                                             shownHeadYaw, shownHeadPitch))
                return reject(problem);
            storePassCache(shown, pose, requestedTarget, shownHeadYaw, shownHeadPitch, shownNear, cameraAim);
            reusedSideEffects = false;
        }
    }
    for (float f : pose)
        if (!std::isfinite(f)) return reject(7);
    if (InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&active, 0, 0)) {
        resetViewHistory();
        return nativeReturn();
    }
    if (evaluateCentre) {
        thirdPerson = centre.third;
        nearPlane = centre.clipFilter;
        tpCombatK = centre.combat;
        cameraFrame.centreReady = true;
        filterTime = cameraFrame.time;
    }
    renderedNear = shownNear;
    if (mode == 1) {
        tpDrop = requestedTarget[1] - body[1];
        tpCeilingRoom = shown.third.lastCeilingRoom;
        tpLostHeight = shown.third.lastLostHeight;
        tpPivotPulled = shown.third.lastPivotPulled;
        tpAllowed = shown.third.lastAllowed;
        tpDownNow = shown.third.down;
    }
    commit(pose, cameraFrame.effectiveFov);
    if (!reusedSideEffects) {
        if (mode == 1 && passKey.blockers != fp::blockersPush)
            tpHideKeep(pose + 9, shown.third.pivot, savedScene);
        else if (tpHiddenCount)
            tpHideClear();
        if (mode == 1)
            thirdPersonAim(camera, pose, requestedTarget, scene, cameraAim);
        else {
            AcquireSRWLockExclusive(&aimLock);
            tpAimSet = false;
            ReleaseSRWLockExclusive(&aimLock);
        }
        if (navPassCache.valid) navPassCache.sideEffectsApplied = true;
    }
    if (headHold.run) {
        fp::holdEnd(headHold);
        publishHold();
    }
    filterRoot = root;
    filterHead = head;
    std::memcpy(lastHead, h, 48);
    std::memcpy(lastCamera, pose, 48);
    trackingPresented = cameraFrame.tracked;
    ++samples;
    float untracked[12];
    if (!cameraFrame.tracked)
        std::memcpy(untracked, pose, sizeof untracked);
    else if (mode == 1) {
        float yaw = 0.f, pitch = 0.f;
        captureTpAimRequest(root, head, scene, yaw, pitch);
        fp::basis(yaw, pitch, untracked);
    } else {
        if (!fp::headPose(h, requestedTarget, untracked)) return reject(4);
        if (cameraFrame.eyeLookValid) fp::stabilizeEyeRotation(untracked, cameraFrame.eyeYaw, cameraFrame.eyePitch);
    }
    viewYaw = std::atan2(-untracked[6], -untracked[8]);
    viewPitch = std::asin(std::clamp(-untracked[7], -1.f, 1.f));
    viewRoot = root;
    {
        float headWorld[3];
        addSectorOrigin(h + 9, scene, headWorld);
        for (int i = 0; i < 3; i++) lastHeadGlobal[i] = headWorld[i];
    }
    publishReachHead(lastHeadGlobal);
    for (int i = 0; i < 3; i++) eyeGlobal[i] = requestedTarget[i];
    cameraOwnershipState.store(1, std::memory_order_relaxed);
    InterlockedExchange(&cameraReady, 1);
    return result;
}

inline uintptr_t navProfileBody(void* self, float offset) {
    healthHit(fp::HealthHook::navSampler);
    fp::profile::invoke<fp::profile::Hook::cameraLock>(AcquireSRWLockExclusive, &cameraLock);
    uintptr_t result = 0;
    __try {
        __try {
            result = navLocked(self, offset);
        } __finally {
            ReleaseSRWLockExclusive(&cameraLock);
        }
    } __except (countFaultAndContinue(&cameraFaults)) {
    }
    return result;
}

inline uintptr_t nav(void* self, float offset) {
    return fp::profile::invoke<fp::profile::Hook::nav>(navProfileBody, self, offset);
}
