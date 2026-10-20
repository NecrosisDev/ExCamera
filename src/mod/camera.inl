// The camera: the replaced sampler (nav) that puts the view at the head's eye, or behind the shoulder.
// Part of first_person.cpp (one translation unit): included there, in order.
// Camera collision probes borrow a trial pose through one owner. Both callers
// retain their physical camera policy; native unprojection receives its viewport.
// 8F460 starts screen rays at camera+1CC; 4C520 scales their origin by that
// depth. Collision must also see the interval between the eye and the old near
// plane. Borrow zero only during this query, without rebuilding projection:
// its far endpoint still supplies the same nonzero direction, now from the eye.
// Ordinary interaction picks never use this adapter.
static bool cameraPosePick(uintptr_t camera, const float* pose, float x, float y, uintptr_t out,
                           uintptr_t ignoredHeld = 0) {
    const uintptr_t viewport = pickViewportForCamera(camera);
    if (!viewport || !originalPickRay) return false;
    const uintptr_t view = *(uintptr_t*)(viewport + field::viewport::view);
    if (!readable(view, field::view::pickFarDepth + sizeof(float))) return false;
    const float pickFar = *(float*)(view + field::view::pickFarDepth);
    // 8F720 normalizes the ray before testing meshes. Reject a degenerate
    // cached endpoint before it can send a zero/NaN direction to native rays.
    if (!std::isfinite(pickFar) || pickFar <= 0.f) return false;
    const float keptNear = *(float*)(camera + field::camera::nearPlane);
    float kept[24];
    memcpy(kept, (void*)(camera + field::camera::pose), 48);
    memcpy(kept + 12, (void*)(camera + field::camera::pose2), 48);
    memcpy((void*)(camera + field::camera::pose), pose, 48);
    memcpy((void*)(camera + field::camera::pose2), pose, 48);
    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
    bool got = false;
    *(float*)(camera + field::camera::nearPlane) = 0.f;
    __try {
        // Nav owns this thread and the camera lock. Exclude the controlled
        // tree before native nearest-hit selection, not after it has masked a
        // farther blocker. Only FP clearance supplies an additional held item;
        // TP retains its existing prop/held-object collision policy.
        got = pickPastSelf(ownedPickRay, viewport, x, y, out, ignoredHeld) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        got = false;
    }
    // Nested collision queries restore zero to this scope; this scope restores
    // the real near plane even when the native query faults.
    *(float*)(camera + field::camera::nearPlane) = keptNear;
    memcpy((void*)(camera + field::camera::pose), kept, 48);
    memcpy((void*)(camera + field::camera::pose2), kept + 12, 48);
    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
    return got;
}
// The near plane's other half: the level grid has walls and floors only, so a prop, door or person right in
// front of the eye cut into view. The game's own pick at the screen centre, from this frame's pose, gives the
// view depth of what is there (within 16 cm), never the player's own body, gear or held item. -1: nothing.
static std::atomic<unsigned> propNearHits{0};
static float centrePropDepth(uintptr_t camera, const float* pose, uintptr_t root) {
    RECT client{};
    if (!originalPickRay || !hookedWindow || !GetClientRect(hookedWindow, &client) || client.right < 40 ||
        client.bottom < 40)
        return -1.f;
    alignas(8) unsigned char hit[0x40]{};
    const uintptr_t held = heldNode();
    const bool got =
        cameraPosePick(camera, pose, float(client.right) * .5f, float(client.bottom) * .5f, (uintptr_t)hit,
                       held);
    const float t = *(float*)(hit + field::pick::distance);
    if (!got || !std::isfinite(t) || t <= 0.f || t > 16.f) return -1.f;
    const uintptr_t node = *(uintptr_t*)hit,
                    playerMesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    unsigned depth = 0;
    for (uintptr_t at = node; at && depth < 64 && readable(at, 0x130);
         at = *(uintptr_t*)(at + field::sceneNode::parent), ++depth) {
        if (at == playerMesh || at == held) return -1.f;
        if (at == *(uintptr_t*)(gameAt(fp::sig::world))) break;
    }
    ++propNearHits;
    return t;
}
// The physics head is sampled at the same time offset as this camera. Native
// 0x8B6E0 interpolates its tetrahedron and 0x8B640 writes mesh+A0; mesh+40 can
// still describe another update when nav is called before the render traversal.
// Reading that older pose makes the attached view and the drawn body disagree
// during movement. Sampling changes only the native render matrix, not physics.
static constexpr size_t partRenderSampleSlot = 0x140;
using PartRenderSampler = float (*)(uintptr_t, float);
static float callPartRenderSampler(uintptr_t sampler, uintptr_t part, float offset) {
    return ((PartRenderSampler)sampler)(part, offset);
}
static float (*partRenderSampler)(uintptr_t, uintptr_t, float) = callPartRenderSampler;
static bool sampleHeadRenderPose(uintptr_t head, uintptr_t mesh, float offset, float* pose) {
    __try {
        if (!std::isfinite(offset) || !readable(head, sizeof(uintptr_t)) ||
            !readable(mesh + field::sceneNode::renderTransform, 16 * sizeof(float)))
            return false;
        const uintptr_t table = *(uintptr_t*)head;
        if (!readable(table + partRenderSampleSlot, sizeof(uintptr_t))) return false;
        // The reviewed head type inherits this native sampler. An unexpected
        // virtual method is not called; a lost source follows the hold/park path.
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Uses only already-reviewed character/sector fields and the native selected
// opponent. No target selection, native pose write or additional hook is added.
static bool cameraEntryYaw(uintptr_t root, uintptr_t sector, float& yaw) {
    __try {
        const auto* position=(const float*)(root+field::character::position);
        const auto* origin=(const float*)(sector+field::sector::origin);
        const uintptr_t opponent=*(uintptr_t*)(root+field::character::aimOpponent);
        if (opponent && opponent!=root && readable(opponent,field::character::lifeFlags+4) &&
            !(*(unsigned*)(opponent+field::character::lifeFlags)&0x100)) {
            const uintptr_t otherSector=aimSector(opponent);
            if (readable(otherSector,field::sector::origin+12)) {
                const auto* other=(const float*)(opponent+field::character::position);
                const auto* otherOrigin=(const float*)(otherSector+field::sector::origin);
                const float x=(other[0]+otherOrigin[0])-(position[0]+origin[0]);
                const float z=(other[2]+otherOrigin[2])-(position[2]+origin[2]);
                if (std::isfinite(x) && std::isfinite(z) && x*x+z*z>1.f) {
                    yaw=std::atan2(x,z);
                    return true;
                }
            }
        }
        const auto* direction=(const float*)(root+field::character::facing);
        if (!std::isfinite(direction[0]) || !std::isfinite(direction[2]) ||
            direction[0]*direction[0]+direction[2]*direction[2]<1.e-6f) return false;
        yaw=std::atan2(direction[0],direction[2]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool updateCameraEntry(uintptr_t root, uintptr_t head, uintptr_t scene, uintptr_t sector) {
    CameraEntryState next=cameraEntry;
    const auto* position=(const float*)(root+field::character::position);
    const auto* origin=(const float*)(sector+field::sector::origin);
    float body[3], distance2=0.f;
    for (int axis=0;axis<3;++axis) {
        body[axis]=position[axis]+origin[axis];
        if (!std::isfinite(body[axis])) return false;
        const float delta=body[axis]-next.body[axis];
        distance2+=delta*delta;
    }
    const bool menu=formShown(*(uintptr_t*)gameAt(fp::sig::endMatchMenu));
    // Five metres between camera samples is a relocation, not normal footwork.
    // Comparing world positions avoids false handoffs at sector boundaries.
    const bool arenaStart=next.menuSeen && !menu;
    const bool entry=!next.valid || next.root!=root || next.head!=head || next.scene!=scene ||
                     distance2>500.f*500.f || arenaStart;
    const bool hadHistory=next.valid;
    const bool combat=*(unsigned char*)(root+field::character::combatStance)!=0;
    next.root=root; next.head=head; next.scene=scene;
    memcpy(next.body,body,sizeof body);
    next.valid=true; next.menuSeen=menu;
    if (entry) {
        next.requested=combat || arenaStart;
        next.pending=false;
    }
    cameraEntry=next;
    if (!next.requested || menu || !combat) return false;
    // An input sample already established on initial attachment is authoritative.
    // Retained history on a later spawn must not override the new native facing.
    AcquireSRWLockShared(&aimLock);
    const bool established=!hadHistory && aim.valid && aim.root==root && aim.head==head;
    ReleaseSRWLockShared(&aimLock);
    if (established) { cameraEntry.requested=false; return false; }
    float yaw=0.f;
    if (!cameraEntryYaw(root,sector,yaw)) return false; // native spawn not ready yet
    resetViewHistory();
    AcquireSRWLockExclusive(&aimLock);
    aim.valid=false;
    aim.root=root; aim.head=head;
    aim.entryPending=true; aim.entryYaw=yaw;
    ReleaseSRWLockExclusive(&aimLock);
    thirdPerson.reset();
    tpCombatK=0.f;
    fp::holdEnd(headHold);
    publishHold();
    next.requested=false; next.pending=true; next.yaw=yaw;
    cameraEntry=next;
    return true;
}
static uintptr_t nav(void* self, float offset) {
    healthHit(fp::HealthHook::navSampler);
    CameraGuard guard;
    InterlockedExchange(&navThread, LONG(GetCurrentThreadId()));
    uintptr_t view = 0, camera = 0;
    currentCamera(view, camera);
    reclaimRetired(view, camera); // Before the sampler and before any stock capture.
    const uintptr_t priorRoot = savedRoot, priorScene = savedScene;
    bool hadOwned = false;
    if (savedCamera && checkSavedOwner(view, camera) && savedController == (uintptr_t)self) {
        hadOwned = true;
        // Restore ALL stock fields and derived matrices before the native sampler.
        // A stationary sampler or any later rejected source must not leave mixed
        // stock fields / first-person expanded pose and projection matrices behind.
        restoreOwnedCamera(view, camera);
    }
    uintptr_t result = originalNav(self, offset);
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
        // The original sampler may have changed the stock pose/FOV since our
        // pre-restore. On any no-FP return, complete its current derived state too.
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
    // Parked: wait for updatePark to allow re-attachment.
    if (park.active) {
        resetViewHistory();
        return nativeReturn();
    }
    bool wasLive = hadOwned && filterRoot == root && filterHead;
    LARGE_INTEGER now;
    // Publishes pose (a filtered head pose, or the last one while holding) exactly
    // as every first-person frame: stock captured from the native sampler first.
    auto commit = [&](const float* pose, float fov = -1.f) {
        memcpy(stockPose, (void*)(camera + field::camera::pose), 48);
        stockFov = *(float*)(camera + field::camera::fov);
        {
            const float n = *(float*)(camera + field::camera::nearPlane);
            if (!(camera == savedCamera && n == renderedNear && n != 1.f)) stockNear = n;
        } // our own dynamic near is never taken as stock
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
        memcpy((void*)(camera + field::camera::pose), pose, 48);
        memcpy((void*)(camera + field::camera::pose2), pose, 48);
        rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
        renderedFov = fov > 0.f && std::isfinite(fov) ? fov : float(selectedFov.load());
        *(float*)(camera + field::camera::fov) = renderedFov;
        *(float*)(camera + field::camera::nearPlane) = renderedNear;
        *(unsigned*)(camera + field::camera::dirty) = 1;
        rebuildProjection(view);
    };
    auto reject = [&](int why = 7) {
        ++headRejects[why < 0 || why > 7 ? 7 : why];
        if (wasLive) {
            // Stock is already restored (hadOwned). A source lost while a game UI is
            // open or the scene changed is game flow (death, unload): park, no fault.
            if (uiOpen() || *(uintptr_t*)(gameAt(fp::sig::currentScene)) != priorScene) {
                resetViewHistory();
                fp::holdEnd(headHold);
                publishHold();
                savedRoot = priorRoot;
                savedScene = priorScene;
                parkCamera(fp::OwnerChange::parkRestore);
                return nativeReturn();
            }
            // Live play: one bad head frame (hard fall, knockdown spike)
            // holds the last good view with input kept; a bounded run of them parks
            // with stock restored and re-arms once the head reads sane.
            if (fp::holdStep(headHold) == fp::HoldStep::hold &&
                !InterlockedCompareExchange(&stopping, 0, 0) && InterlockedCompareExchange(&active, 0, 0)) {
                publishHold();
                QueryPerformanceCounter(&now);
                filterTime = now; // Resume without a long filter step.
                cameraFrame.valid = false;
                // no slide toward the game's camera. The
                // last good view is held as it was; a park then cuts to the game's camera and the
                // re-arm cuts straight back to the head.
                float held[12];
                memcpy(held, lastCamera, 48); // the last good view (lastCamera is not changed while held)
                commit(held, renderedFov); // Keep the held view's own FOV, including a TP override.
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
    if (viewMode.load()<2 && updateCameraEntry(root,head,worldScene,scene)) wasLive=false;
    const int mode = viewMode.load();
    const LONG serial = InterlockedCompareExchange(&swapCalls, 0, 0);
    const bool sameLifetime = filterRoot == root && filterHead == head &&
                              cameraFrame.view == view && cameraFrame.camera == camera &&
                              cameraFrame.controller == (uintptr_t)self && cameraFrame.root == root &&
                              cameraFrame.head == head && cameraFrame.scene == worldScene && cameraFrame.mode == mode;
    if (!cameraFrame.valid || !sameLifetime || cameraFrame.serial != serial) {
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
        cameraFrame.effectiveFov = fp::resolveCameraFov(mode, float(selectedFov.load()), float(tpFov.load()));
        cameraFrame.time = now;
        cameraFrame.tick = GetTickCount64();
        cameraFrame.seconds = sameLifetime && frequency.QuadPart
                                  ? float(double(now.QuadPart - filterTime.QuadPart) / double(frequency.QuadPart))
                                  : 1.f;
        cameraFrame.before = before;
        AcquireSRWLockShared(&aimLock);
        if (aim.valid && aim.root == root && aim.head == head &&
            std::isfinite(aim.yaw) && std::isfinite(aim.pitch)) {
            cameraFrame.eyeLookValid = true;
            cameraFrame.eyeYaw = aim.yaw;
            cameraFrame.eyePitch = aim.pitch;
            cameraEntry.pending=false;
        } else if (cameraEntry.pending && cameraEntry.root==root &&
                   cameraEntry.head==head && cameraEntry.scene==worldScene) {
            // Hold the entry orientation until the input owner has consumed this
            // displayed view. A physics head pose can still be settling at spawn.
            cameraFrame.eyeLookValid=true;
            cameraFrame.eyeYaw=cameraEntry.yaw;
            cameraFrame.eyePitch=0.f;
        }
        ReleaseSRWLockShared(&aimLock);
        if (!sameLifetime) trackingView.clear();
        auto tracked = trackingSnapshot();
        if (tracked.received <= trackingAcceptAfter.load()) tracked.valid = false;
        const bool acceptTracking = trackingEnabled.load() && mode < 2 &&
                                    InterlockedCompareExchange(&inputEnabled, 0, 0) && !cursorFreeNow();
        trackingView.update(tracked, cameraFrame.tick, cameraFrame.seconds, acceptTracking,
                            trackingReset.load());
        memcpy(cameraFrame.trackingOffset, trackingView.shown, sizeof cameraFrame.trackingOffset);
        // Keep final-view aim reprojection active until the offset has faded completely.
        cameraFrame.tracked = false;
        float identity[12]{};
        fp::basis(0, 0, identity);
        for (int i = 0; i < 9; ++i)
            cameraFrame.tracked = cameraFrame.tracked ||
                                  std::fabs(identity[i] - cameraFrame.trackingOffset[i]) > 1.e-5f;
    }
    // The normal draw has a zero-offset camera update before its temporal
    // samples. A nonzero first request also works: establish the centre, then
    // leave the native render matrix at the exact offset the caller requested.
    const bool evaluateCentre = mode != 2 && (!cameraFrame.centreReady || offset == 0.f);
    float h[12], centreHead[12];
    if (evaluateCentre && offset != 0.f && !sampleHeadRenderPose(head, mesh, 0.f, centreHead)) return reject(3);
    if (!sampleHeadRenderPose(head, mesh, offset, h)) return reject(3);
    if (evaluateCentre && offset == 0.f) memcpy(centreHead, h, sizeof h);
    auto headProblem = [](const float* sampled) {
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
    float requestedTarget[3], centreTarget[3], body[3], pose[12];
    fp::eyePoint(h, requestedTarget, float(eyeSetback.load()));
    if (evaluateCentre) fp::eyePoint(centreHead, centreTarget, float(eyeSetback.load()));
    for (int i = 0; i < 3; i++) {
        const float sceneOffset = *(float*)(scene + field::sector::origin + i * 4);
        requestedTarget[i] += sceneOffset;
        if (evaluateCentre) centreTarget[i] += sceneOffset;
        body[i] = *(float*)(root + field::character::position + i * 4) + sceneOffset;
        if (!std::isfinite(requestedTarget[i]) || !std::isfinite(body[i]) ||
            (evaluateCentre && !std::isfinite(centreTarget[i]))) return reject(6);
    }
    // Where the physics head's points sit in the head's own frame (the status file reports them): their middle along the
    // up and forward axes, and how far the farthest is from it. Diagnostic: the eye setback's room.
    if (readable(head + field::bodyPart::points, 48)) {
        const float(*pt)[3] = (const float(*)[3])(head + field::bodyPart::points);
        float mid[3]{};
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i) mid[i] += 0.25f * pt[k][i];
        float reach = 0;
        for (int k = 0; k < 4; ++k) {
            float d = 0;
            for (int i = 0; i < 3; ++i) d += (pt[k][i] - mid[i]) * (pt[k][i] - mid[i]);
            reach = std::fmax(reach, std::sqrt(d));
        }
        float a2 = 0, a3 = 0;
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
    // First person keeps the exact sampled physical head pose. The camera's
    // optional filters are evaluated separately below, against one frame epoch.
    if (!fp::headPose(h, requestedTarget, pose)) return reject(4);
    // third person: its own pose; first person above is unchanged.
    if (viewMode.load() == 2) { // from the last third-person view to the game's own camera, then the game's
        const float t = float(GetTickCount64() - tpBlendStart.load()) / 300.f;
        float game[12];
        memcpy(game, (void*)(camera + field::camera::pose), 48);
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
        static ULONGLONG blendOf = 0;
        static float fromFov = 75.f;
        if (blendOf != tpBlendStart.load()) {
            blendOf = tpBlendStart.load();
            fromFov = renderedFov;
        } // the FOV the blend started from
        const float gameFov = *(float*)(camera + field::camera::fov),
                    s = t * t * (3.f - 2.f * t); // the same smoothstep as the pose
        commit(pose, std::isfinite(gameFov) && gameFov > 1.f && gameFov < 170.f
                         ? fromFov + (gameFov - fromFov) * s
                         : -1.f);
        return result;
    }
    auto evaluate = [&](const float* sampledHead, const float* target, float dt,
                        CameraFilterState& state, float* evaluatedPose, float& evaluatedNear,
                        TpAimRequest& request) {
        if (!fp::headPose(sampledHead, target, evaluatedPose)) return 4;
        if (cameraFrame.eyeLookValid)
            fp::stabilizeEyeRotation(evaluatedPose, cameraFrame.eyeYaw, cameraFrame.eyePitch);
        if (mode == 1) {
            float yaw = std::atan2(-evaluatedPose[6], -evaluatedPose[8]),
                  pitch =
                      std::asin(std::fmax(-1.f, std::fmin(1.f, -evaluatedPose[7]))); // no aim yet: where the head faces
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
                hookedWindow && GetClientRect(hookedWindow, &client) && client.right > 40 && client.bottom > 40;
            // fp::ThirdPerson's casts. levelOnly: walls and floors alone (the room above the eye, the eye-to-pivot check,
            // the corners of the camera's box). Otherwise also props, doors and people, by the game's own pick.
            const int blockers = tpBlockers.load();
            auto clear = [&](const float* origin, const float* dir, float length, bool levelOnly) {
                float allowed = level && levelRay && levelRay2 ? levelClear(level, origin, dir, length)
                                                               : length; // walls and floors (the level grid)
                if (levelOnly) return allowed;
                // props, doors and people are not in the level grid: ask the game's own pick ray from the would-be
                // camera toward the pivot; something hit well before the pivot pulls the camera in front of it
                if (sized && originalPickRay && allowed > 60.f) {
                    float trial[12];
                    memcpy(trial, evaluatedPose, 36);
                    for (int i = 0; i < 3; ++i) trial[9 + i] = origin[i] + dir[i] * allowed;
                    float d[3];
                    for (int i = 0; i < 3; ++i) d[i] = origin[i] - trial[9 + i];
                    float px, py;
                    const float shownFov =
                        *(float*)(camera +
                                  field::camera::
                                      fov); // the projection the camera holds now (0x8F720 unprojects through it)
                    if (std::isfinite(shownFov) && shownFov > 1.f && shownFov < 170.f &&
                        fp::projectCursor(d, trial, float(client.right), float(client.bottom), shownFov, px,
                                          py)) {
                        alignas(8) unsigned char hit[0x40]{};
                        const bool got = cameraPosePick(camera, trial, px, py, (uintptr_t)hit);
                        const float t = *(float*)(hit + field::pick::distance);
                        float self = 1e9f;
                        if (got && std::isfinite(t)) {
                            float s = 0;
                            for (int i = 0; i < 3; ++i) {
                                const float p = trial[9 + i] - dir[i] * t - target[i];
                                s += p * p;
                            }
                            self = std::sqrt(s);
                        } // the hit, from the eye
                        const uintptr_t hitNode = got ? *(uintptr_t*)hit : 0;
                        if (got && std::isfinite(t) && t > 0.f && t < allowed - 45.f && self > 70.f) {
                            if (blockers == fp::blockersPush ||
                                tpHideHit(hitNode, root, blockers == fp::blockersHide) ==
                                    0) { // else hidden (or a loose item, ignored)
                                allowed = std::fmax(12.f, allowed - t - 10.f);
                                ++tpPropPulls;
                            }
                        } // never the character itself
                    }
                }
                return allowed;
            };
            {
                const bool fighting = tpCombat.load() && readable(root, 0x1300) &&
                                      *(unsigned char*)(root + field::character::combatStance) !=
                                          0; // combat mode (the stance byte the input step reads)
                const float a = std::isfinite(dt) && dt >= 0.f && dt <= 0.5f ? 1.f - std::exp(-dt / 0.35f) : 1.f;
                state.combat += ((fighting ? 1.f : 0.f) - state.combat) * a;
            }
            state.third.lag = float(tpLag.load());
            {
                const bool downNow =
                    tpDown.load() &&
                    fp::knockdownStep(state.third.knock, *(unsigned*)(root + field::character::stateFlags),
                                      *(unsigned*)(root + field::character::lifeFlags), cameraFrame.tick);
                if (!tpDown.load()) state.third.knock = fp::KnockdownFollow{};
                if (downNow)
                    for (int i = 0; i < 3; ++i)
                        state.third.downPoint[i] = 0.5f * (target[i] + body[i]) + (i == 1 ? 40.f : 0.f);
                state.third.down = downNow;
            }
            state.third.update(target, yaw, pitch, float(tpDistance.load()) * (1.f - 0.2f * state.combat),
                               float(tpShoulder.load()) * (1.f - 0.7f * state.combat), float(tpHeight.load()), dt,
                               clear, evaluatedPose, tpTight.load(),
                               cameraFrame.tracked ? trackedFacing : nullptr);
        } else {
            state.third.reset();
            state.combat = 0.f;
        } // back in first person: no third-person state left
        // TP already used the complete rotation frame for boom and collision: no
        // Euler decomposition/heading flip at a vertical pole and no post-collision orbit.
        if (cameraFrame.tracked && mode == 0)
            fp::composeView(evaluatedPose, cameraFrame.trackingOffset, evaluatedPose);
        {
            float clearance = viewClearance(evaluatedPose, cameraFrame.effectiveFov); // unknown (no level grid): today's 1 cm, never the lower floor
            if (mode == 0 && clearance != 0.f) {
                const float prop =
                    centrePropDepth(camera, evaluatedPose, root); // props, doors and people are not in the level grid
                if (prop > 0.f && (clearance < 0.f || prop < clearance)) clearance = prop;
            }
            evaluatedNear =
                state.clipFilter.update(clearance < 0.f ? 2.f : clearance, dt, float(nearClip.load()) * 0.1f, 3.f);
        }
        for (int i = 0; i < 12; ++i)
            if (!std::isfinite(evaluatedPose[i])) return 7;
        return std::isfinite(evaluatedNear) && evaluatedNear > 0.f ? 0 : 7;
    };
    CameraFilterState centre = cameraFrame.before, shown = cameraFrame.before;
    float centrePose[12], centreNear = renderedNear, shownNear = renderedNear;
    TpAimRequest centreAim, cameraAim;
    if (evaluateCentre) {
        if (const int problem = evaluate(centreHead, centreTarget, cameraFrame.seconds, centre,
                                         centrePose, centreNear, centreAim))
            return reject(problem);
    }
    if (offset == 0.f) {
        shown = centre;
        memcpy(pose, centrePose, sizeof pose);
        shownNear = centreNear;
        cameraAim = centreAim;
    } else {
        // Native offsets are milliseconds around the presentation centre, not
        // frame fractions. Evaluate from the same prior centre even when the
        // renderer asks for later samples first. Before available history, hold
        // its time at zero; a hitch uses the existing fresh-camera reset path.
        const float elapsed = std::isfinite(cameraFrame.seconds) && cameraFrame.seconds >= 0.f &&
                                      cameraFrame.seconds <= 0.5f
                                  ? std::fmax(0.f, cameraFrame.seconds + offset * 0.001f)
                                  : 1.f;
        if (const int problem = evaluate(h, requestedTarget, elapsed, shown, pose, shownNear, cameraAim))
            return reject(problem);
    }
    for (float f : pose)
        if (!std::isfinite(f)) return reject(7);
    // The worker may request stop while the native call/filter is in progress.
    // Recheck here; even if stop lands immediately AFTER this check, the worker
    // cannot accept quiescence until this lock is released with pendingRestore set.
    if (InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&active, 0, 0)) {
        resetViewHistory();
        return nativeReturn();
    }
    // Sampling writes the requested pose, but only the centre result owns
    // persistent spring, velocity and near-plane history.
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
    commit(pose, cameraFrame.effectiveFov); // Exactly the FOV used by this epoch's clearance rays.
    if (viewMode.load() == 1 && tpBlockers.load() != fp::blockersPush)
        tpHideKeep(pose + 9, shown.third.pivot, savedScene); // Hide ownership is the world scene, not the head's sector.
    else if (tpHiddenCount)
        tpHideClear();
    if (viewMode.load() == 1)
        thirdPersonAim(camera, pose, requestedTarget, scene, cameraAim);
    else {
        AcquireSRWLockExclusive(&aimLock);
        tpAimSet = false;
        ReleaseSRWLockExclusive(&aimLock);
    }
    if (headHold.run) {
        fp::holdEnd(headHold);
        publishHold();
    }
    filterRoot = root;
    filterHead = head;
    memcpy(lastHead, h, 48);
    memcpy(lastCamera, pose, 48);
    trackingPresented = cameraFrame.tracked;
    ++samples;
    // These feed mouse-aim initialization and held-item fallback, not presentation.
    float untracked[12];
    if (!cameraFrame.tracked)
        memcpy(untracked, pose, sizeof untracked);
    else if (mode == 1) {
        float yaw = 0, pitch = 0;
        captureTpAimRequest(root, head, scene, yaw, pitch);
        fp::basis(yaw, pitch, untracked);
    } else {
        if (!fp::headPose(h, requestedTarget, untracked)) return reject(4);
        if (cameraFrame.eyeLookValid) fp::stabilizeEyeRotation(untracked, cameraFrame.eyeYaw, cameraFrame.eyePitch);
    }
    viewYaw = std::atan2(-untracked[6], -untracked[8]);
    viewPitch = std::asin(std::clamp(-untracked[7], -1.f, 1.f));
    viewRoot = root;
    for (int i = 0; i < 3; i++)
        lastHeadGlobal[i] = h[9 + i] + *(float*)(scene + field::sector::origin + i * 4);
    publishReachHead(lastHeadGlobal);
    for (int i = 0; i < 3; i++) eyeGlobal[i] = requestedTarget[i]; // the eye (with the setback), for the hold distance
    InterlockedExchange(&cameraReady, 1);
    return result;
}
// Installs the hooks, dispatch slots, window procedure and optional raw mouse.
// On failure everything installed is rolled back; reason/hooks say what to report.
