// The camera: the replaced sampler (nav) that puts the view at the head's eye, or behind the shoulder.
// Part of first_person.cpp (one translation unit): included there, in order.
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
        // stock fields / first-person inverse and projection matrices behind.
        restoreOwnedCamera(view, camera);
    }
    uintptr_t result = original(self, offset);
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
    const bool wasLive = hadOwned && filterRoot == root && filterHead;
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
                // no slide toward the game's camera. The
                // last good view is held as it was; a park then cuts to the game's camera and the
                // re-arm cuts straight back to the head.
                float held[12];
                memcpy(held, lastCamera, 48); // the last good view (lastCamera is not changed while held)
                commit(held);
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
    float h[12];
    memcpy(h, (void*)(mesh + field::sceneNode::transform), 48);
    for (float f : h)
        if (!std::isfinite(f)) return reject(3);
    for (int axis = 0; axis < 3; axis++) {
        float n = 0;
        for (int j = 0; j < 3; j++) n += h[axis * 3 + j] * h[axis * 3 + j];
        if (n < 0.25f || n > 4.f) return reject(4);
    }
    auto scene = *(uintptr_t*)(mesh + field::sceneNode::sector);
    if (!scene) return reject(5);
    float target[3], body[3], pose[12];
    for (int i = 0; i < 3; i++) {
        const float sceneOffset = *(float*)(scene + field::sector::origin + i * 4);
        target[i] = h[9 + i] + h[3 + i] * fp::eyeAlongAxis2 + h[6 + i] * fp::eyeAlongAxis3 + sceneOffset;
        body[i] = *(float*)(root + field::character::position + i * 4) + sceneOffset;
        if (!std::isfinite(target[i]) || !std::isfinite(body[i])) return reject(6);
    }
    QueryPerformanceCounter(&now);
    const float dt = frequency.QuadPart
                         ? float(double(now.QuadPart - filterTime.QuadPart) / double(frequency.QuadPart))
                         : 1.f;
    // the camera is exactly the head, every
    // frame: the head's axes and its eye mount (fp::headPose). It gets no input and no smoothing of its
    // own; the only steadying is Head stabilisation, on the head (steadyHeadAround).
    if (!fp::headPose(h, target, pose))
        return reject(4); // up and forward (nearly) parallel: no view can be made
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
    if (viewMode.load() == 1) {
        if (filterRoot != root || filterHead != head) thirdPerson.reset();
        float yaw = std::atan2(-pose[6], -pose[8]),
              pitch =
                  std::asin(std::fmax(-1.f, std::fmin(1.f, -pose[7]))); // no aim yet: where the head faces
        {
            AcquireSRWLockShared(&aimLock);
            if (aim.valid && aim.root == root && aim.head == head) {
                yaw = aim.yaw;
                pitch = aim.pitch;
            }
            ReleaseSRWLockShared(&aimLock);
        }
        const uintptr_t level = levelGrid();
        RECT client{};
        const bool sized =
            hookedWindow && GetClientRect(hookedWindow, &client) && client.right > 40 && client.bottom > 40;
        auto clear = [&](const float* origin, const float* dir, float length) {
            float allowed = level && levelRay && levelRay2 ? levelClear(level, origin, dir, length)
                                                           : length; // walls and floors (the level grid)
            // straight up (the pivot's room under the ceiling, fp::ThirdPerson): the level alone. Side rays and the
            // pick would start off the eye and see the player's own body or gear. A camera ray is never vertical:
            // pitch is at most 66 degrees.
            if (dir[1] > 0.999f) return allowed;
            if (tpHide.load() && level && levelRay &&
                levelRay2) { // a fat probe: four more wall rays 15 cm around the line
                for (int k = 0; k < 4; ++k) {
                    float o[3];
                    const float sx = (k & 1) ? 15.f : -15.f, sy = (k & 2) ? 15.f : -15.f;
                    for (int i = 0; i < 3; ++i) o[i] = origin[i] + pose[i] * sx + pose[3 + i] * sy;
                    const float a = levelClear(level, o, dir, length);
                    if (std::isfinite(a) && a >= 30.f && a < allowed) allowed = a;
                }
            } // a side ray that starts in a wall says nothing
            // props, doors and people are not in the level grid: ask the game's own pick ray from the would-be
            // camera toward the pivot; something hit well before the pivot pulls the camera in front of it
            if (sized && originalPickRay && allowed > 60.f) {
                float trial[12];
                memcpy(trial, pose, 36);
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
                    float kept[24];
                    memcpy(kept, (void*)(camera + field::camera::pose), 48);
                    memcpy(kept + 12, (void*)(camera + field::camera::pose2), 48);
                    memcpy((void*)(camera + field::camera::pose), trial, 48);
                    memcpy((void*)(camera + field::camera::pose2), trial, 48);
                    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
                    alignas(8) unsigned char hit[0x40]{};
                    bool got = false;
                    __try {
                        got = originalPickRay(camera, px, py, (uintptr_t)hit) != 0;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        got = false;
                    }
                    memcpy((void*)(camera + field::camera::pose), kept, 48);
                    memcpy((void*)(camera + field::camera::pose2), kept + 12, 48);
                    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
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
                        if (!tpHide.load() ||
                            tpHideHit(hitNode, root) ==
                                0) { // hidden (or a loose item, ignored) instead of shoving the camera
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
            tpCombatK += ((fighting ? 1.f : 0.f) - tpCombatK) * a;
        }
        thirdPerson.lag = float(tpLag.load());
        {
            static bool wasDown = false;
            static ULONGLONG lowSince = 0; // down after the eye has been
            bool downNow = false; // under 70 cm above the root's place for 0.25 s, up again above 100 cm
            if (tpDown.load()) {
                const float drop = target[1] - body[1];
                const ULONGLONG ms = GetTickCount64();
                if (!std::isfinite(drop))
                    lowSince = 0;
                else if (drop < 70.f) {
                    if (!lowSince) lowSince = ms;
                } else if (drop > 100.f)
                    lowSince = 0;
                downNow = lowSince && (wasDown || ms - lowSince >= 250);
                if (downNow)
                    for (int i = 0; i < 3; ++i)
                        thirdPerson.downPoint[i] = 0.5f * (target[i] + body[i]) + (i == 1 ? 40.f : 0.f);
            }
            wasDown = downNow;
            thirdPerson.down = downNow;
        }
        thirdPerson.update(target, yaw, pitch, float(tpDistance.load()) * (1.f - 0.2f * tpCombatK),
                           float(tpShoulder.load()) * (1.f - 0.7f * tpCombatK), float(tpHeight.load()), dt,
                           clear, pose, tpTight.load());
    } else {
        thirdPerson.reset();
        tpCombatK = 0.f;
        if (tpHiddenCount) tpHideClear();
    } // back in first person: no third-person state left
    {
        const float clearance =
            viewClearance(pose); // unknown (no level grid): today's 1 cm, never the lower floor
        renderedNear =
            nearPlane.update(clearance < 0.f ? 2.f : clearance, dt, float(nearClip.load()) * 0.1f, 3.f);
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
    commit(pose, viewMode.load() == 1 && tpFov.load() > 0 ? float(tpFov.load())
                                                          : -1.f); // third person's own field of view
    if (viewMode.load() == 1 && tpHide.load())
        tpHideKeep(pose + 9, thirdPerson.pivot, scene);
    else if (tpHiddenCount)
        tpHideClear();
    if (viewMode.load() == 1)
        thirdPersonAim(camera, pose, target, scene); // where the crosshair is
    else if (tpAimSet) {
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
    filterTime = now;
    memcpy(lastHead, h, 48);
    memcpy(lastCamera, pose, 48);
    ++samples;
    viewYaw = std::atan2(-pose[6], -pose[8]);
    viewPitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -pose[7])));
    viewRoot = root;
    for (int i = 0; i < 3; i++)
        lastHeadGlobal[i] = h[9 + i] + *(float*)(scene + field::sector::origin + i * 4);
    InterlockedExchange(&cameraReady, 1);
    return result;
}
// Installs the hooks, dispatch slots, window procedure and optional raw mouse.
// On failure everything installed is rolled back; reason/hooks say what to report.
