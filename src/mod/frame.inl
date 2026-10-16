// The frame hook (wglSwapBuffers), the dispatch slots' install and rollback, third-person hiding and aim.
// Part of first_person.cpp (one translation unit): included there, in order.
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
// while the
// panel is open, the game's window must not hold the mouse. The title menu keeps it captured
// (SetCapture) or clipped after a click, so every move and click went to the game, not the panel. On
// the game's own thread (capture belongs to it), between frames; each change is logged once.
// gameCursorIndex: the game's chosen cursor (-1: hidden); gameCursorTable: its HCURSOR table (filled by 0x24810).
static void f8MouseStep() {
    // Over the game's window the cursor is the game's own choice, set every frame so a pointer the panel
    // showed never stays behind: its cursor at the title, in menus and in the overhead view (the game sets
    // it only when the mouse moves), and none while it draws its own crosshair.
    {
        POINT at{};
        HWND under = GetCursorPos(&at) ? WindowFromPoint(at) : nullptr;
        DWORD pid = 0;
        if (under) GetWindowThreadProcessId(under, &pid);
        DWORD owner = 0;
        const bool overGame = under && pid == GetCurrentProcessId() &&
                              GetWindowThreadProcessId(under, &owner) == GetCurrentThreadId();
        if (overGame) {
            const int slot = fp::gameCursorSlot(*(int*)gameAt(fp::sig::gameCursorIndex));
            SetCursor(slot >= 0 ? ((HCURSOR*)gameAt(fp::sig::gameCursorTable))[slot] : nullptr);
        }
    }
    if (!InterlockedCompareExchange(&cursorFree, 0, 0) || !gameWindow()) return;
    const HWND cap = GetCapture();
    if (cap && cap == hookedWindow) {
        ReleaseCapture();
    }
    RECT clip{}, screen{0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (GetClipCursor(&clip) &&
        (clip.right - clip.left < screen.right || clip.bottom - clip.top < screen.bottom)) {
        ClipCursor(nullptr);
        InterlockedExchange(&cursorClipped, 0);
    }
}
static bool replace(uintptr_t address, void* expected, void* value) {
    DWORD old, tmp;
    if (!VirtualProtect((void*)address, 8, PAGE_READWRITE, &old)) return false;
    bool ok = InterlockedCompareExchangePointer((void* volatile*)address, value, expected) == expected;
    if (!VirtualProtect((void*)address, 8, old, &tmp)) InterlockedExchange(&protectionFailure, 1);
    return ok;
}
static bool rollbackDispatch(bool installedNav, bool installedPoint, bool installedBehavior) {
    bool ok = true;
    if (installedNav) ok = replace(gameAt(fp::sig::navSlot), (void*)nav, (void*)original) && ok;
    if (installedPoint) ok = replace(gameAt(fp::sig::pointSlot), (void*)point, (void*)originalPoint) && ok;
    if (installedBehavior)
        ok = replace(gameAt(fp::sig::behaviorSlot), (void*)behavior, (void*)originalBehavior) && ok;
    const bool unhooked = MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
    if (unhooked) __try {
            spineBendRelease();
        } __except (EXCEPTION_EXECUTE_HANDLER) {} // nothing writes the bend any more: the game's pose again
    return unhooked && ok && !InterlockedCompareExchange(&protectionFailure, 0, 0);
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
// what the camera's line of sight hits (a placed object or a character, never the player)
// is hidden for as long as it stands near the line from the camera to the pivot; its whole scene subtree
// goes into the per-frame hide list (the pass mask the helmet uses), so the pick no longer sees it either.
struct TpHidden {
    uintptr_t top;
    ULONGLONG until;
};
static TpHidden tpHiddenTops[16];
static unsigned tpHiddenCount = 0; // nav thread
static SRWLOCK tpHideLock = SRWLOCK_INIT;
static uintptr_t tpHideTops[16];
static unsigned tpHideTopCount = 0; // published to the render thread
// only a CHARACTER (its mesh tree, found in the level's character
// list) or a PLACED OBJECT (the highest ancestor flagged 0x2000 that is not a group) is hidden; groups,
// tiles and wall pieces are walls (pull in); loose items (0x4000) are ignored (neither); nothing on the
// player's own tree. The list belongs to one scene and is dropped when the scene changes.
static uintptr_t tpHideScene = 0;
static int tpHideHit(uintptr_t hitNode,
                     uintptr_t root) { // 1 hidden, 2 ignored (small loose item), 0 a wall: pull in
    if (!readable(hitNode, 0x2a0)) return 0;
    const uintptr_t W = *(uintptr_t*)(gameAt(fp::sig::world));
    const uintptr_t playerMesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    uintptr_t anc[64];
    unsigned n = 0;
    for (uintptr_t at = hitNode; n < 64 && at && at != W && readable(at, 0x130);
         at = *(uintptr_t*)(at + field::sceneNode::parent)) {
        if (at == playerMesh) return 2;
        anc[n++] = at;
    } // the player's own (weapon, gear)
    uintptr_t target = 0;
    {
        const uintptr_t list = readable(W, 0x1c30) ? *(uintptr_t*)(W + field::worldObject::characters)
                                                   : 0; // characters (the dead too)
        if (readable(list, 0x118)) {
            const int count = *(int*)(list + field::list::count);
            const uintptr_t array = *(uintptr_t*)(list + field::list::items);
            if (count > 0 && count <= 4096 && readable(array, size_t(count) * 8))
                for (int i = 0; i < count && !target; ++i) {
                    const uintptr_t c = *(uintptr_t*)(array + size_t(i) * 8);
                    if (!readable(c, 0x20) || c == root) continue;
                    const uintptr_t m = *(uintptr_t*)(c + field::character::sceneNode);
                    for (unsigned k = 0; k < n; ++k)
                        if (anc[k] == m) {
                            target = m;
                            break;
                        }
                }
        }
    }
    if (!target) {
        for (unsigned k = 0; k < n; ++k) {
            const unsigned f = *(unsigned*)(anc[k] + 8);
            if ((f & 0x7000) == 0x4000) return 2; // a loose item: too small to matter
            if ((f & 0x2000) && (f & 0xfff) != 0x11) target = anc[k];
        }
    } // the highest placed object
    if (!target) return 0;
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
static void tpHideKeep(const float* camera, const float* pivot, uintptr_t scene) {
    const ULONGLONG now = GetTickCount64();
    unsigned n = 0;
    if (*(uintptr_t*)(gameAt(fp::sig::currentScene)) != tpHideScene) tpHiddenCount = 0;
    for (unsigned i = 0; i < tpHiddenCount; ++i) {
        TpHidden h = tpHiddenTops[i];
        if (readable(h.top, 0x2d0)) {
            float p[3];
            for (int k = 0; k < 3; ++k)
                p[k] = *(float*)(h.top + field::sceneNode::poseOrigin + k * 4) +
                       *(float*)(scene + field::sector::origin + k * 4);
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
        if (now < h.until && readable(h.top, 0x2a0)) tpHiddenTops[n++] = h;
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
// Render thread (viewFeatures): the hidden things' meshes into this frame's hide list (only in the scene
// they were found in; a character or an object is at most 256 nodes).
static uintptr_t tpWalk[256];
static unsigned tpWalkParents[256];
static void tpHideAdd() {
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
        const unsigned c = walkScene(tops[i], tpWalk, tpWalkParents, 256);
        for (unsigned j = 0; j < c; ++j) hideAdd(tpWalk[j]);
    }
}
// the point under the third-person crosshair (the screen centre): the game's own pick ray from the
// camera just committed, else the level grid, else 1000 units out; the character's own body (within 70
// of the eye) does not count. Published in the aim's (scene-local) frame for the input step.
static void thirdPersonAim(uintptr_t camera, const float* pose, const float* eye, uintptr_t scene) {
    const float fwd[3] = {-pose[6], -pose[7], -pose[8]};
    float t = 1000.f;
    RECT client{};
    if (originalPickRay && hookedWindow && GetClientRect(hookedWindow, &client) && client.right > 40 &&
        client.bottom > 40) {
        alignas(8) unsigned char hit[0x40]{};
        bool got = false;
        __try {
            got = originalPickRay(camera, float(client.right) * 0.5f, float(client.bottom) * 0.5f,
                                  (uintptr_t)hit) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            got = false;
        }
        const float h = *(float*)(hit + field::pick::distance);
        if (got && std::isfinite(h) && h > 0.f && h < t) {
            float s = 0;
            for (int i = 0; i < 3; ++i) {
                const float p = pose[9 + i] + fwd[i] * h - eye[i];
                s += p * p;
            }
            if (std::sqrt(s) > 70.f) t = h;
        }
    }
    if (const uintptr_t level = levelGrid(); level && levelRay && levelRay2)
        t = std::fmin(t, levelClear(level, pose + 9, fwd, t) + 8.f);
    float local[3];
    for (int i = 0; i < 3; ++i)
        local[i] = pose[9 + i] + fwd[i] * t - *(float*)(scene + field::sector::origin + i * 4);
    bool ok = true;
    for (float v : local)
        if (!std::isfinite(v)) ok = false;
    AcquireSRWLockExclusive(&aimLock);
    if (ok) {
        memcpy(tpAimLocal, local, 12);
        tpAimSet = true;
    }
    ReleaseSRWLockExclusive(&aimLock);
}
