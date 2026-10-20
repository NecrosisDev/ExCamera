// The frame hook (wglSwapBuffers), the dispatch slots' install and rollback, third-person hiding and aim.
// Part of first_person.cpp (one translation unit): included there, in order.
// View-scoped head draw suppression. Exanima's shared _SKIN node is a sibling
// of 10Head. Never mask _SKIN: that would remove the rest of the character.
// Instead do NOT enqueue only the player's separate 10Head drawable for the
// first-person MAIN colour pass (4E610 -> 26F230, call return 4E720).
// Other submission callsites, including 4EE10 -> 26F230 (return 4EF9E), continue unchanged.
// Native collision/body-part data and scene passMask are never modified.
using MeshQueue = void (*)(uintptr_t, int, unsigned short);
static MeshQueue originalMeshQueue = nullptr;
static std::atomic<unsigned> firstPersonHeadDrawsSkipped{0};
static bool shouldSkipPlayerHeadDraw(bool mainColorCall, bool ownFirstPerson,
                                     bool identityVerified, uintptr_t submission,
                                     uintptr_t headNode) {
    return mainColorCall && ownFirstPerson && identityVerified &&
           submission && submission==headNode;
}
__declspec(noinline) static void meshQueue(uintptr_t node, int material, unsigned short flags) {
    // This shared queue has multiple submission callsites.
    // Check the *specific* native callsite before touching player
    // state; all other paths are forwarded byte-for-byte.
    const bool mainColorCall=(uintptr_t)_ReturnAddress()==gameAt(fp::sig::mainMeshQueueReturn);
    if (mainColorCall && viewMode.load()==0 && supersampleRenderScope.owned) {
        bool skip=false;
        __try {
            if (supersampleViewMatches()) {
                const uintptr_t root=supersampleRenderScope.root;
                if (readable(root,field::character::headPart+sizeof(uintptr_t))) {
                    const uintptr_t head=*(uintptr_t*)(root+field::character::headPart);
                    if (readable(head,field::bodyPart::meshNode+sizeof(uintptr_t))) {
                        const uintptr_t headNode=*(uintptr_t*)(head+field::bodyPart::meshNode);
                        // A dedicated physical head drawable, NOT a generic
                        // skinned body mesh. A pointer mismatch fails open.
                        const bool headValid =
                            readable(headNode,field::sceneNode::bodyPart+sizeof(uintptr_t)) &&
                            *(uintptr_t*)(headNode+field::sceneNode::bodyPart)==head &&
                            fp::bodyPartNumber(nodeName(headNode))==10;
                        skip=shouldSkipPlayerHeadDraw(true,true,headValid,node,headNode);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            skip=false; // Uncertain ownership: preserve native rendering.
        }
        if (skip) {
            firstPersonHeadDrawsSkipped.fetch_add(1,std::memory_order_relaxed);
            return;
        }
    }
    originalMeshQueue(node,material,flags);
}
static BOOL WINAPI swap(HDC dc) {
    fp::CueSide cueSideNow = fp::CueSide::none;
    LONG cueX = 0, cueY = 0;
    float cueFillNow = 0, cueAlphaNow = 0;
    {
        CameraGuard guard;
        InterlockedExchange(&swapThread, LONG(GetCurrentThreadId()));
        if (attackCue && InterlockedCompareExchange(&active, 0, 0) &&
            !InterlockedCompareExchange(&stopping, 0, 0) && InterlockedCompareExchange(&inputEnabled, 0, 0) &&
            !cursorFreeNow()) {
            AcquireSRWLockShared(&reticleLock);
            const bool drawn = reticleFrame == InterlockedCompareExchange(&swapCalls, 0, 0);
            cueX = reticleX;
            cueY = reticleY;
            ReleaseSRWLockShared(&reticleLock);
            if (drawn) {
                const int phase = cueArcPhase.load();
                cueAlphaNow =
                    fp::windupShown(phase, cueArcFade.load(), cueArcPublished.load(), GetTickCount64());
                if (cueAlphaNow > 0) {
                    cueSideNow = fp::cueSide(fp::CueSide(cueArcSide.load()), lastCamera[1]);
                    cueFillNow = cueArcFill.load();
                }
            }
        }
        viewFeatures(savedCamera && InterlockedCompareExchange(&cameraReady, 0, 0) &&
                     InterlockedCompareExchange(&active, 0, 0) &&
                     !InterlockedCompareExchange(&stopping, 0, 0));
        if (!gameWindow() || *(int*)(gameAt(fp::sig::uiOpenCount)) > 0) releaseInput();
        windowFreeStep();
        crouchFrame();
        InterlockedIncrement(&swapCalls);
        if (savedCamera) {
            uintptr_t view = 0, camera = 0;
            currentCamera(view, camera);
            const uintptr_t viewport = *(uintptr_t*)(gameAt(fp::sig::mainViewport));
            if (viewport)
                pickCamera = *(uintptr_t*)(viewport + field::viewport::pickCamera) == savedCamera ? 1 : 0;
            if (checkSavedOwner(view, camera) &&
                (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0))) {
                restoreOwnedCamera(view, camera);
                resetViewHistory();
            }
        }
        {
            uintptr_t view = 0, camera = 0;
            currentCamera(view, camera);
            updatePark(view, camera);
        }
    }
    heldDoorStep();
    f8MouseStep(); // the panel gets the mouse at the title too
    presentCue(cueSideNow, cueX, cueY, cueFillNow, cueAlphaNow);
    const BOOL presented = originalSwap(dc);
    return presented;
}
// The native GUI owns its cursor and capture; there is no second settings HWND.
static void f8MouseStep() {}
static bool replace(uintptr_t address, void* expected, void* value) {
    DWORD old, tmp;
    if (!VirtualProtect((void*)address, 8, PAGE_READWRITE, &old)) return false;
    bool ok = InterlockedCompareExchangePointer((void* volatile*)address, value, expected) == expected;
    if (!VirtualProtect((void*)address, 8, old, &tmp)) InterlockedExchange(&protectionFailure, 1);
    return ok;
}
// MinHook rejects a changed entry before changing bytes or suspended-thread IPs.
// If one target was taken over, remove the still-owned hooks individually and
// retain all trampolines; the caller reports an incomplete removal honestly.
static MH_STATUS(WINAPI* nativeDisableHook)(LPVOID) = MH_DisableHook;
static BOOL(WINAPI* nativeHookRecoveryPending)() = MH_HasPendingRecovery;
static MH_STATUS(WINAPI* recoverNativeHookState)() = MH_Recover;
static void(WINAPI* hookRecoveryWait)(DWORD) = Sleep;
// Worker only, with no gameplay lock held. A suspended callback may own rawLock
// or another lock needed by fault reporting: finish this debt before reporting
// or waiting for callbacks. A timeout must never abandon our suspension count.
static void awaitNativeHookRecovery() {
    while (nativeHookRecoveryPending()) {
        recoverNativeHookState();
        if (nativeHookRecoveryPending()) hookRecoveryWait(100);
    }
}
static MH_STATUS disableNativeHooks() {
    awaitNativeHookRecovery();
    MH_STATUS status = nativeDisableHook(MH_ALL_HOOKS);
    if (nativeHookRecoveryPending()) {
        awaitNativeHookRecovery();
        // The first transaction may have failed before or after committing.
        // One fresh disable establishes its final state after cleanup succeeds.
        status = nativeDisableHook(MH_ALL_HOOKS);
        awaitNativeHookRecovery();
    }
    if (status != MH_ERROR_CONFLICT) return status;
    bool seen[fp::sig::targetCount]{};
    const char* changed = "unknown";
    for (const auto& s : fp::sig::signatures) {
        if (!s.hook || seen[s.target] || !layout.rva[s.target]) continue;
        seen[s.target] = true;
        if (nativeDisableHook((void*)gameAt(s.target)) == MH_ERROR_CONFLICT)
            changed = fp::sig::targetNames[s.target];
        awaitNativeHookRecovery();
    }
    for (const char* name : {"wglSwapBuffers", "wglMakeCurrent", "wglDeleteContext"}) {
        const auto entry = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
        if (entry && nativeDisableHook((void*)entry) == MH_ERROR_CONFLICT) changed = name;
        awaitNativeHookRecovery();
    }
    snprintf(faultDetail, sizeof faultDetail,
             "native hook entry changed (%s); replacement code and retained trampolines kept; restart required",
             changed);
    return status;
}
static bool rollbackDispatch(bool installedNav, bool installedPoint, bool installedBehavior) {
    bool ok = true;
    if (installedNav) ok = replace(gameAt(fp::sig::navSlot), (void*)nav, (void*)originalNav) && ok;
    if (installedPoint) ok = replace(gameAt(fp::sig::pointSlot), (void*)point, (void*)originalPoint) && ok;
    if (installedBehavior)
        ok = replace(gameAt(fp::sig::behaviorSlot), (void*)behavior, (void*)originalBehavior) && ok;
    const bool unhooked = disableNativeHooks() == MH_OK;
    // Hook removal does not drain already-entered pose callbacks. They restore
    // their own scoped borrows; the worker never writes physics part fields.
    const bool posesReturned = awaitPoseBuilders();
    const bool cuesReturned = awaitCueRetirement();
    return unhooked && posesReturned && cuesReturned && ok && !InterlockedCompareExchange(&protectionFailure, 0, 0);
}
static void normalize(float* v) {
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(n) || n < 1.e-6f) {
        v[0] = v[1] = 0;
        v[2] = 1;
        return;
    }
    for (int i = 0; i < 3; i++) v[i] /= n;
}
// Only verified placed props may be hidden. Characters and their gear always
// pull the camera in; the player's own tree is ignored. Never infer actor identity
// from a mesh type flag: use the native world character roster and scene ancestry.
struct TpHidden {
    uintptr_t top;
    ULONGLONG until;
};
static TpHidden tpHiddenTops[16];
static unsigned tpHiddenCount = 0; // nav thread
static SRWLOCK tpHideLock = SRWLOCK_INIT;
static uintptr_t tpHideTops[16];
static unsigned tpHideTopCount = 0; // published to the render thread
static uintptr_t tpHideScene = 0;
static bool tpAncestry(uintptr_t node, uintptr_t world, uintptr_t* ancestors, unsigned& count) {
    count = 0;
    for (uintptr_t at = node; at && at != world; at = *(uintptr_t*)(at + field::sceneNode::parent)) {
        if (count == 64 || !readable(at, field::sceneNode::parent + sizeof(uintptr_t))) return false;
        for (unsigned i = 0; i < count; ++i)
            if (ancestors[i] == at) return false;
        ancestors[count++] = at;
    }
    return count != 0;
}
// 1: verified hideable prop; 2: own tree / loose item ignored; 0: pull in.
// Also called before retaining or publishing a hide, since ownership may change.
static int tpHideCandidate(uintptr_t hitNode, uintptr_t root, bool hideObjects, uintptr_t& target) {
    target = 0;
    const uintptr_t world = *(uintptr_t*)(gameAt(fp::sig::world));
    if (!readable(root, field::character::sceneNode + sizeof(uintptr_t))) return 0;
    const uintptr_t playerMesh = *(uintptr_t*)(root + field::character::sceneNode);
    uintptr_t ancestors[64];
    unsigned count = 0;
    if (!playerMesh || !tpAncestry(hitNode, world, ancestors, count)) return 0;
    for (unsigned i = 0; i < count; ++i)
        if (ancestors[i] == playerMesh) return 2;
    if (!readable(world, field::worldObject::characters + sizeof(uintptr_t))) return 0;
    const uintptr_t roster = *(uintptr_t*)(world + field::worldObject::characters);
    if (!readable(roster, field::list::items + sizeof(uintptr_t))) return 0;
    const int characters = *(int*)(roster + field::list::count);
    const uintptr_t entries = *(uintptr_t*)(roster + field::list::items);
    if (characters < 0 || characters > 4096 ||
        (characters && !readable(entries, size_t(characters) * sizeof(uintptr_t)))) return 0;
    // Choose the placed-object ancestor, but do not admit it until every roster
    // entry is checked. A character could itself be below that placed subtree.
    bool loose = false;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned flags = *(unsigned*)(ancestors[i] + 8);
        if ((flags & 0x7000) == 0x4000) loose = true;
        if ((flags & 0x2000) && (flags & 0xfff) != 0x11) target = ancestors[i];
    }
    bool playerListed = false;
    for (int i = 0; i < characters; ++i) {
        const uintptr_t character = *(uintptr_t*)(entries + size_t(i) * sizeof(uintptr_t));
        if (!readable(character, field::character::sceneNode + sizeof(uintptr_t))) return 0;
        const uintptr_t mesh = *(uintptr_t*)(character + field::character::sceneNode);
        if (!readable(mesh, field::sceneNode::parent + sizeof(uintptr_t))) return 0;
        if (character == root && mesh == playerMesh) playerListed = true;
        for (unsigned j = 0; j < count; ++j)
            if (ancestors[j] == mesh) return 0;
        // Smart never hides props, so ancestry containment cannot admit a hide
        // there. Avoid native-memory walks that cannot affect its decision.
        if (target && hideObjects) {
            uintptr_t characterAncestors[64];
            unsigned characterCount = 0;
            if (!tpAncestry(mesh, world, characterAncestors, characterCount)) return 0;
            for (unsigned j = 0; j < characterCount; ++j)
                if (characterAncestors[j] == target) return 0;
        }
    }
    // An empty/transitional roster cannot establish that a subtree has no actor.
    if (!playerListed) return 0;
    // Also guard the controlled tree independently of the hit's ancestry.
    if (target && hideObjects) {
        uintptr_t playerAncestors[64];
        unsigned playerCount = 0;
        if (!tpAncestry(playerMesh, world, playerAncestors, playerCount)) return 0;
        for (unsigned i = 0; i < playerCount; ++i)
            if (playerAncestors[i] == target) return 0;
    }
    if (loose) return 2;
    return target && hideObjects ? 1 : 0;
}
static int tpHideHit(uintptr_t hitNode, uintptr_t root, bool hideObjects) {
    uintptr_t target = 0;
    const int verdict = tpHideCandidate(hitNode, root, hideObjects, target);
    if (verdict != 1) return verdict;
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    if (scene != tpHideScene) {
        tpHiddenCount = 0;
        tpHideScene = scene;
    }
    const ULONGLONG now = GetTickCount64();
    for (unsigned i = 0; i < tpHiddenCount; ++i)
        if (tpHiddenTops[i].top == target) {
            tpHiddenTops[i].until = now + 700;
            return 1;
        }
    if (tpHiddenCount >= 16) return 0;
    tpHiddenTops[tpHiddenCount++] = {target, now + 700};
    ++tpHidden;
    return 1;
}
// Nav thread, after the pose: keep each hidden thing while its place lies within 120 cm of the
// camera-to-pivot line (an object's origin can sit off the line it blocks), drop it 0.7 s after; a new
// scene drops them all. Published for the render thread with the scene they belong to.
static uintptr_t tpHidePublishedScene = 0;
static bool sceneNodeWorldOrigin(uintptr_t node, float* point) {
    if (!readable(node, field::sceneNode::sector + sizeof(uintptr_t))) return false;
    const uintptr_t sector = *(uintptr_t*)(node + field::sceneNode::sector);
    if (!readable(sector, field::sector::origin + 3 * sizeof(float))) return false;
    for (unsigned k = 0; k < 3; ++k) {
        point[k] = *(float*)(node + field::sceneNode::transformOrigin + k * sizeof(float)) +
                   *(float*)(sector + field::sector::origin + k * sizeof(float));
        if (!std::isfinite(point[k])) return false;
    }
    return true;
}
static void tpHideKeep(const float* camera, const float* pivot, uintptr_t scene) {
    const ULONGLONG now = GetTickCount64();
    unsigned n = 0;
    if (tpBlockers.load() != fp::blockersHide || scene != tpHideScene ||
        *(uintptr_t*)(gameAt(fp::sig::currentScene)) != scene)
        tpHiddenCount = 0;
    for (unsigned i = 0; i < tpHiddenCount; ++i) {
        TpHidden h = tpHiddenTops[i];
        uintptr_t target = 0;
        if (tpHideCandidate(h.top, savedRoot, tpBlockers.load() == fp::blockersHide, target) != 1 ||
            target != h.top) continue;
        float p[3];
        if (!sceneNodeWorldOrigin(h.top, p)) continue;
        {
            float d[3], s = 0, l2 = 0;
            for (int k = 0; k < 3; ++k) {
                d[k] = pivot[k] - camera[k];
                l2 += d[k] * d[k];
                s += (p[k] - camera[k]) * d[k];
            }
            const float u = l2 > 1e-6f ? std::fmin(1.f, std::fmax(0.f, s / l2)) : 0.f;
            float e = 0;
            for (int k = 0; k < 3; ++k) {
                const float q = camera[k] + d[k] * u - p[k];
                e += q * q;
            }
            if (std::isfinite(e) && e < 120.f * 120.f) h.until = now + 700;
        }
        if (now < h.until) tpHiddenTops[n++] = h;
    }
    tpHiddenCount = n;
    AcquireSRWLockExclusive(&tpHideLock);
    tpHideTopCount = n;
    tpHidePublishedScene = tpHideScene;
    for (unsigned i = 0; i < n; ++i) tpHideTops[i] = tpHiddenTops[i].top;
    ReleaseSRWLockExclusive(&tpHideLock);
}
static void tpHideClear() {
    tpHiddenCount = 0;
    AcquireSRWLockExclusive(&tpHideLock);
    tpHideTopCount = 0;
    ReleaseSRWLockExclusive(&tpHideLock);
}
// Render thread (viewFeatures): expand admitted prop subtrees into this frame's
// hide list, only in their owning scene, with a 256-node traversal limit.
static uintptr_t tpWalk[256];
static unsigned tpWalkParents[256];
static void tpHideAdd() {
    if (tpBlockers.load() != fp::blockersHide) return;
    uintptr_t tops[16];
    unsigned n = 0;
    uintptr_t owner = 0;
    AcquireSRWLockShared(&tpHideLock);
    n = tpHideTopCount;
    owner = tpHidePublishedScene;
    memcpy(tops, tpHideTops, n * sizeof(uintptr_t));
    ReleaseSRWLockShared(&tpHideLock);
    if (!n || owner != *(uintptr_t*)(gameAt(fp::sig::currentScene))) return;
    for (unsigned i = 0; i < n; ++i) {
        uintptr_t target = 0;
        if (tpHideCandidate(tops[i], savedRoot, tpBlockers.load() == fp::blockersHide, target) != 1 ||
            target != tops[i]) continue;
        const unsigned c = walkScene(tops[i], tpWalk, tpWalkParents, 256);
        for (unsigned j = 0; j < c; ++j) hideAdd(tpWalk[j]);
    }
}
// Third-person physical head gaze still needs world depth. Combat targeting
// belongs to AE2E0; this sample is not another opponent/weapon target selector.
// Reuse the native pick's near origin and direction for both aim-box modes.
static void thirdPersonAim(uintptr_t camera, const float* pose, const float* eye, uintptr_t scene,
                           const TpAimRequest& request) {
    // A tracked free-look view must never feed its cursor hit back into weapon/head aim.
    // Keep the existing finite input ray while tracking or its release fade is active.
    if (trackingEnabled.load() || trackingPresented) {
        AcquireSRWLockExclusive(&aimLock);
        tpAimSet = false;
        ReleaseSRWLockExclusive(&aimLock);
        return;
    }
    // Keep matching depth available during a valid refresh; only a failed
    // query retires it so input can fall back without reusing a stale owner.
    auto discard = [&]() {
        AcquireSRWLockExclusive(&aimLock);
        if (sameTpAimRequest(tpAimSample, request)) tpAimSet = false;
        ReleaseSRWLockExclusive(&aimLock);
    };
    const uintptr_t viewport = pickViewportForCamera(camera);
    RECT client{};
    if (!viewport || !originalPickRay || !request.valid || request.sector != scene || !hookedWindow ||
        !GetClientRect(hookedWindow, &client) || client.right != request.width ||
        client.bottom != request.height || *(int*)(viewport + field::viewport::width) != request.width ||
        *(int*)(viewport + field::viewport::height) != request.height ||
        *(float*)(camera + field::camera::fov) != request.fov)
        return discard();
    AcquireSRWLockShared(&aimLock);
    const bool current = tpAimRequestCurrent(request);
    ReleaseSRWLockShared(&aimLock);
    if (!current) return discard();
    const float x = float(request.width) * .5f + request.x, y = float(request.height) * .5f + request.y;
    alignas(8) unsigned char hit[0x40]{};
    bool got = false;
    __try {
        got = ownedPickRay(viewport, x, y, (uintptr_t)hit) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return discard();
    }
    const float* fwd = (const float*)(hit + field::pick::direction);
    const float* from = (const float*)(hit + field::pick::origin);
    const float length2 = fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2];
    if (!std::isfinite(length2) || std::fabs(length2 - 1.f) >= .01f || !finiteAimVector(from))
        return discard();
    // A native miss still supplies the real ray. Its overhead ground-plane
    // distance can be infinite or behind us, so keep the finite forward fallback.
    float t = 1000.f;
    const float h = *(float*)(hit + field::pick::distance);
    if (got && std::isfinite(h) && h > 0.f && h < t) {
        float s = 0;
        for (int i = 0; i < 3; ++i) {
            const float p = from[i] + fwd[i] * h - eye[i];
            s += p * p;
        }
        if (s > 70.f * 70.f) t = h;
    }
    if (const uintptr_t level = levelGrid(); level && levelRay && levelRay2)
        t = std::fmin(t, levelClear(level, from, fwd, t) + 8.f);
    const float cameraForward[3] = {-pose[6], -pose[7], -pose[8]};
    float along = 0.f, rayForward = 0.f;
    for (int i = 0; i < 3; ++i) {
        along += (eye[i] - from[i]) * cameraForward[i];
        rayForward += fwd[i] * cameraForward[i];
    }
    if (rayForward > 0.01f) t = std::fmax(t, (along + 20.f) / rayForward);
    float local[3];
    for (int i = 0; i < 3; ++i)
        local[i] = from[i] + fwd[i] * t - *(float*)(scene + field::sector::origin + i * 4);
    bool ok = true;
    for (float v : local)
        if (!std::isfinite(v)) ok = false;
    RECT after{};
    ok = ok && pickViewportForCamera(camera) == viewport &&
         *(int*)(viewport + field::viewport::width) == request.width &&
         *(int*)(viewport + field::viewport::height) == request.height &&
         GetClientRect(hookedWindow, &after) && after.right == request.width &&
         after.bottom == request.height && *(float*)(camera + field::camera::fov) == request.fov;
    AcquireSRWLockExclusive(&aimLock);
    if (ok && tpAimRequestCurrent(request)) {
        memcpy(tpAimLocal, local, 12);
        tpAimSample = request;
        tpAimSet = true;
    } else if (sameTpAimRequest(tpAimSample, request))
        tpAimSet = false;
    ReleaseSRWLockExclusive(&aimLock);
}
