// What the eye sees: hiding meshes (helmet, body wear, what blocks the third-person camera), the tile covers.
// Part of first_person.cpp (one translation unit): included there, in order.
// ---- First-person view features (render thread only; no C++ unwinding inside __try) ----
// Hiding: what decides whether a registered mesh is drawn is its pass mask mesh+0xE0, which the
// per-sector update 0x58CD0 rebuilds every frame (sector bit 0x10000000 from +0x3A0, | 0x1000001 when
// the bounds pass the view tests 0x79E60 / 0x58AC0). The render passes (0x4E610, 0x4EB30, 0x4EE10,
// 0x4EFE0, 0x4F0B0) and the screen pick 0x8F720 take a mesh only when their pass mask & mesh+0xE0 is
// non-zero; flagsC 0x10000 is not read there, so clearing it hides nothing. A hidden mesh gets, right
// after 0x58CD0 returns for its sector, the value of a mesh outside the view (fp::hiddenPassMask).
// Nothing is stored in the mesh: the next frame recomputes it, so leaving the hide list, F6, F7 or a
// scene change show it again with no undo.
static bool readable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION m;
    if (address < 0x10000 || VirtualQuery((void*)address, &m, sizeof(m)) != sizeof(m)) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return address + size <= (uintptr_t)m.BaseAddress + m.RegionSize;
}
static const char* nodeName(uintptr_t node) {
    return readable(node, 0x2a0) ? (const char*)(node + field::sceneNode::name) : "";
}
// 90520 runs 90C00 AFTER our camera sampler. Its obstruction ray still uses
// the stock controller orbit, not the published close-view camera. Keep the
// native 90710 traversal/parent cleanup and 58CD0 fade recovery, but make its
// two obstruction predicates miss while this controller owns a close view.
using CameraFade = void (*)(uintptr_t);
using FadeBounds = bool (*)(const float*, const float*, const float*);
static CameraFade originalCameraFade; // RVA 0x90C00
static FadeBounds originalFadeBounds; // RVA 0x7A270, shared segment/sphere predicate
static thread_local bool clearCameraObstructions = false;
static bool closeViewFadeOwner(uintptr_t controller) {
    if (!controller || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0) ||
        DWORD(InterlockedCompareExchange(&navThread, 0, 0)) != GetCurrentThreadId())
        return false;
    // The reviewed call follows nav, outside cameraLock. An unexpected nested
    // sampler or concurrent restoration must not recurse/wait on that lock.
    if (!TryAcquireSRWLockShared(&cameraLock)) return false;
    bool owned = false;
    __try {
        uintptr_t view = 0, camera = 0;
        currentCamera(view, camera);
        owned = !park.active && !InterlockedCompareExchange(&restorationUnknown, 0, 0) &&
                controller == savedController && sameOwner(view, camera) && savedRoot && savedScene &&
                !identityChanged();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owned = false;
    }
    ReleaseSRWLockShared(&cameraLock);
    return owned;
}
static void cameraFade(uintptr_t controller) {
    const bool previous = clearCameraObstructions;
    // A nested, unrelated controller gets its own native policy, not the
    // outer camera's scope. Restore even if native traversal faults.
    clearCameraObstructions = closeViewFadeOwner(controller);
    __try {
        originalCameraFade(controller);
    } __finally {
        clearCameraObstructions = previous;
    }
}
static bool fadeBoundsFor(uintptr_t caller, const float* origin, const float* segment, const float* bounds) {
    if (clearCameraObstructions && viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
        !InterlockedCompareExchange(&stopping, 0, 0) &&
        (caller == gameAt(fp::sig::fadeVerticalBoundsReturn) ||
         caller == gameAt(fp::sig::fadeOrbitBoundsReturn)))
        return false;
    return originalFadeBounds(origin, segment, bounds);
}
static bool fadeBounds(const float* origin, const float* segment, const float* bounds) {
    return fadeBoundsFor((uintptr_t)_ReturnAddress(), origin, segment, bounds);
}
// Hide list, sorted (binary search), published by the render thread, read by
// 0x58CD0's thread. Meshes are compared, never dereferenced, unless the sector
// being updated lists them this frame.
static constexpr unsigned hideCapacity = 32768;
static SRWLOCK hideLock = SRWLOCK_INIT;
static uintptr_t hideMeshes[hideCapacity];
static unsigned hideMeshCount = 0;
static uintptr_t pendingHide[hideCapacity];
static unsigned pendingHideCount = 0; // render thread only
static std::atomic<unsigned> hiddenMeshes{0}, maskedDraws{0}, hideOverflow{0};
// Tile covers:
// the black blades and bands are not wall geometry. 0x144610 draws the level's per-tile
// cover meshes (built from the wall sets' ceiling pieces at the wall line) in black with
// the depth test off, fading each tile's alpha by state; 0x1444A0 first writes their
// depth with colour writes masked. In the overhead view they darken what the character
// has not seen; from the eye they are flat black shapes hanging at the wall line, over
// the textured capitals behind them (trace: draw 86 textures the pixel, draw 164 from
// 0x4FAFD -> 0x18240 -> 0x144610 paints it black). A "_black"-range skip at 0x26F230
// changed no pixel (run skins1), so it was removed. In a live first-person frame both
// cover passes are skipped; the overhead view draws them as stock.
using CoverPass = uint64_t (*)(uintptr_t);
using CoverDepth = void (*)(uintptr_t);
static CoverPass originalCoverPass;   // RVA 0x144610 (returns 1: stays in its pass list)
static CoverDepth originalCoverDepth; // RVA 0x1444A0
static std::atomic<bool> skipCovers{false}, coversHiddenInFirstPerson{true};
static std::atomic<unsigned> coverPassesSkipped{0};
static uint64_t coverPass(uintptr_t level) {
    if (skipCovers.load(std::memory_order_relaxed)) {
        coverPassesSkipped.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return originalCoverPass(level);
}
static void coverDepth(uintptr_t level) {
    if (skipCovers.load(std::memory_order_relaxed)) return;
    originalCoverDepth(level);
}
static uintptr_t hideScene = 0; // scene manager the list belongs to; another scene is never masked
static volatile LONG sectorThread = 0;
static std::atomic<int> viewFault{0};
using SectorUpdate = uintptr_t (*)(uintptr_t);
static SectorUpdate originalSectorUpdate; // RVA 0x58CD0
static void hideAdd(uintptr_t mesh) {
    if (pendingHideCount < hideCapacity)
        pendingHide[pendingHideCount++] = mesh;
    else
        ++hideOverflow;
}
// Publishes pendingHide when it differs from the live list.
static void hidePublish() {
    const unsigned n = fp::sortUnique(pendingHide, pendingHideCount);
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    AcquireSRWLockExclusive(&hideLock);
    if (n != hideMeshCount || memcmp(pendingHide, hideMeshes, n * sizeof(uintptr_t))) {
        memcpy(hideMeshes, pendingHide, n * sizeof(uintptr_t));
        hideMeshCount = n;
    }
    hideScene = scene;
    ReleaseSRWLockExclusive(&hideLock);
    hiddenMeshes = n;
    pendingHideCount = 0;
}
static void maskSector(uintptr_t sector) {
    if (*(uintptr_t*)(gameAt(fp::sig::currentScene)) != hideScene) return;
    const int last = *(int*)(sector + field::sector::lastMesh);
    const uintptr_t list = *(uintptr_t*)(sector + field::sector::meshes);
    for (int j = 0; j <= last; ++j) {
        const uintptr_t m = *(uintptr_t*)(list + size_t(j) * 8);
        if (fp::sortedContains(hideMeshes, hideMeshCount, m)) {
            unsigned& e = *(unsigned*)(m + field::sceneNode::passMask);
            e = fp::hiddenPassMask(e);
            ++maskedDraws;
        }
    }
}
static uintptr_t sectorUpdate(uintptr_t sector) {
    const uintptr_t result = originalSectorUpdate(sector);
    InterlockedExchange(&sectorThread, LONG(GetCurrentThreadId()));
    bool fault = false;
    AcquireSRWLockShared(&hideLock);
    // The list is exactly what 0x58CD0 just walked for this sector.
    __try {
        if (hideMeshCount) maskSector(sector);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        viewFault = 1;
        fault = true;
    }
    ReleaseSRWLockShared(&hideLock);
    if (fault) {
        AcquireSRWLockExclusive(&hideLock);
        hideMeshCount = 0;
        hideScene = 0;
        ReleaseSRWLockExclusive(&hideLock);
    }
    return result;
}
// Breadth-first scene walk into a flat list. Returns node count.
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity) {
    if (!readable(scene, 0x2a0)) return 0;
    thread_local fp::PointerSet seen; // the render thread and the game's pick thread both walk
    seen.reset(capacity);
    seen.insert(scene);
    unsigned count = 0, next = 0;
    nodes[count] = scene;
    parents[count++] = ~0u;
    while (next < count) {
        uintptr_t node = nodes[next];
        unsigned index = next++;
        if (!readable(node, 0x2a0)) continue;
        int last = *(int*)(node + field::sceneNode::lastChild);
        uintptr_t list = *(uintptr_t*)(node + field::sceneNode::children);
        if (last < 0 || last >= 6000 || !readable(list, (last + 1) * 8)) continue;
        for (int i = 0; i <= last && count < capacity; ++i) {
            uintptr_t child = *(uintptr_t*)(list + i * 8);
            if (!child) continue;
            if (seen.insert(child)) {
                nodes[count] = child;
                parents[count++] = index;
            }
        }
    }
    return count;
}
static uintptr_t nodes[8192];
static unsigned parents[8192];
// The level being played: the scene object's name (base+0x4F0BD0, +0x140).
// Head wear: equipment is parented under
// body-part nodes (a sword under 17RHand), so anything under the controlled
// character's 10Head node (root+0x11E0 -> +0x18) is head wear. [View] Helmet=show
// (default) draws it from the inside with the game's own lighting; Helmet=hide hides
// it as before meant to. The head itself is never touched (it is part of
// the shared skin and is not drawn from the eye). A worn mesh that covers the view
// centre is hidden in either mode: rays from the eye along the view axis and four
// directions fp::helmetConeDegrees around it, fp::helmetProbeLength long, through
// the mesh's own ray routine (mesh+0x3B0 with context +0x3B8, as the screen pick
// 0x8F720 calls it on meshes with flags8 & 0x89 == 0x89). A mesh that cannot be
// tested is hidden and counted (helmet_unknown): the camera is never blocked.
struct HeadWear {
    uintptr_t node;
    int verdict;
    unsigned age;
}; // verdict fp::HelmetVerdict
static HeadWear headWear[64];
static unsigned headWearCount = 0;
static uintptr_t headScene = 0;
static std::atomic<unsigned> hiddenHeadNodes{0}, headWearNodes{0}, helmetBlocking{0}, helmetUnknown{0},
    helmetTests{0};
static std::atomic<fp::HelmetMode> helmetMode{fp::HelmetMode::show}; // [View] Helmet; F8 settings page
using RayQuery = fp::game::MeshRayQuery;
using MeshRay = int (*)(uintptr_t, RayQuery*);
static int callMeshRay(uintptr_t ray, uintptr_t context, RayQuery* q) {
    return ((MeshRay)ray)(context, q);
}
static int (*meshRay)(uintptr_t, uintptr_t, RayQuery*) = callMeshRay; // replaced by the owned-memory tests
static fp::HelmetVerdict helmetVerdict(uintptr_t node, float length = fp::helmetProbeLength) {
    if (!readable(node, 0x3c0) || (*(unsigned*)(node + 8) & 0x89) != 0x89) return fp::HelmetVerdict::unknown;
    const uintptr_t ray = *(uintptr_t*)(node + field::sceneNode::rayRoutine),
                    context = *(uintptr_t*)(node + field::sceneNode::rayContext);
    // The ray routine is game code (a mesh-type method), never a data pointer.
    if (!context || ray < base + layout.textBegin || ray >= base + layout.textEnd)
        return fp::HelmetVerdict::unknown;
    const uintptr_t owner = *(uintptr_t*)(node + field::sceneNode::sector);
    if (!readable(owner, 0x70)) return fp::HelmetVerdict::unknown;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = lastCamera[9 + i] -
                 *(float*)(owner + field::sector::origin + i * 4); // sector-local, as mesh+0x40
    float directions[5][3];
    fp::helmetProbeDirections(lastCamera, directions);
    const float* m = (const float*)(node + field::sceneNode::transform);
    for (const auto& d : directions) {
        RayQuery q{};
        q.scratchBank = fp::game::viewQueryScratchBank;
        q.distance = length;
        fp::worldToLocalPoint(m, eye, q.origin);
        fp::worldToLocalDirection(m, d, q.direction);
        ++helmetTests;
        if (meshRay(ray, context, &q) >= 0) return fp::HelmetVerdict::blocks;
    }
    return fp::HelmetVerdict::clear;
}
static std::atomic<unsigned> helmetTurnRetests{0};
// Equipment mounted below the head may share a tree with the character's skinned
// geometry. Never classify the model's own skin or body-part mesh as a helmet:
// suppressing their render-pass mask can make the WHOLE player appear transparent.
static bool headWearCandidate(uintptr_t node, uintptr_t controlledRoot) {
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t))) return false;
    const char* name=nodeName(node);
    if (!strncmp(name, "_SKIN", 5) || fp::bodyPartNumber(name)>=0) return false;
    const uintptr_t part=*(uintptr_t*)(node+field::sceneNode::bodyPart);
    if (part && readable(part, field::bodyPart::character+sizeof(uintptr_t)) &&
        *(uintptr_t*)(part+field::bodyPart::character)==controlledRoot)
        return false;
    return true;
}
static void updateHeadWear(bool live) {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    const uintptr_t head = readable(root, 0x1200) ? *(uintptr_t*)(root + field::character::headPart) : 0;
    const uintptr_t headMesh = readable(head, 0x20) ? *(uintptr_t*)(head + field::bodyPart::meshNode) : 0;
    if (!live || !readable(headMesh, 0x2a0) || !*(uintptr_t*)(headMesh + field::sceneNode::sector) ||
        scene != headScene) {
        headWearCount = 0;
        headScene = scene;
        hiddenHeadNodes = 0;
        headWearNodes = 0;
        helmetBlocking = 0;
        helmetUnknown = 0;
        if (!live || !readable(headMesh, 0x2a0) || !*(uintptr_t*)(headMesh + field::sceneNode::sector))
            return;
    }
    const unsigned count = walkScene(headMesh, nodes, parents, 256);
    HeadWear next[64];
    unsigned n = 0;
    for (unsigned j = 1; j < count && n < 64; ++j) {
        if (!headWearCandidate(nodes[j],root)) continue;
        HeadWear w{nodes[j], -2, 0};
        for (unsigned k = 0; k < headWearCount; ++k)
            if (headWear[k].node == w.node) {
                w = headWear[k];
                break;
            }
        next[n++] = w;
    }
    unsigned hidden = 0, blocking = 0, unknown = 0;
    // Tested when it appears, every 30 frames (the head pose relative to the eye moves with animation), and at
    // once when the view has turned fp::helmetRetestCos (8 degrees) since the last test: a fast look no longer
    // shows a strip of helmet until the next round. Hide mode needs no test.
    static float testedForward[3]{};
    const float forward[3] = {-lastCamera[6], -lastCamera[7], -lastCamera[8]};
    const bool turned = fp::helmetRetestDue(testedForward, forward);
    if (turned) {
        memcpy(testedForward, forward, 12);
        ++helmetTurnRetests;
    }
    for (unsigned k = 0; k < n; ++k) {
        HeadWear& w = next[k];
        if (helmetMode == fp::HelmetMode::show && (w.verdict == -2 || turned || ++w.age >= 30)) {
            w.verdict = int(helmetVerdict(w.node));
            w.age = 0;
        }
        const bool hide = fp::hideHeadWear(helmetMode, fp::HelmetVerdict(w.verdict));
        if (hide) {
            hideAdd(w.node);
            ++hidden;
        }
        if (w.verdict == int(fp::HelmetVerdict::blocks)) ++blocking;
        if (w.verdict == int(fp::HelmetVerdict::unknown)) ++unknown;
    }
    memcpy(headWear, next, sizeof(HeadWear) * n);
    headWearCount = n;
    headWearNodes = n;
    hiddenHeadNodes = hidden;
    helmetBlocking = blocking;
    helmetUnknown = unknown;
}
// Body wear in view: gear under the torso, shoulders and upper arms that the
// eye-ray test hits within fp::bodyProbeLength is hidden for that frame (looking down), re-tested every
// 6 frames; gear that cannot be tested is left drawn (unlike the helmet: it is not in front of the eye
// unless the test says so). The skin, the head (above), the hands, legs and feet are never touched.
struct BodyWear {
    uintptr_t node;
    int verdict;
    unsigned age;
};
static BodyWear bodyWear[96];
static unsigned bodyWearCount = 0;
static uintptr_t bodyScene = 0;
static std::atomic<unsigned> bodyWearNodes{0}, hiddenBodyNodes{0};
static std::atomic<int> bodyFault{0};
static void updateBodyWear(bool live) {
    if (live && lastCamera[7] < 0.34f)
        live =
            false; // only while looking down (view forward more than 20 degrees below level): no cost otherwise
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    const uintptr_t rootMesh = readable(root, 0x1200) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    if (!live || !readable(rootMesh, 0x2a0) || scene != bodyScene) {
        bodyWearCount = 0;
        bodyScene = scene;
        bodyWearNodes = 0;
        hiddenBodyNodes = 0;
        if (!live || !readable(rootMesh, 0x2a0)) return;
    }
    const unsigned count = walkScene(rootMesh, nodes, parents, 512);
    BodyWear next[96];
    unsigned n = 0;
    for (unsigned j = 1; j < count && n < 96; ++j) {
        // The character's own skinned torso, hands and head are never clothing.
        if (!headWearCandidate(nodes[j],root)) continue;
        int part = -1;
        for (unsigned a = parents[j], guard = 0; a < count && guard < 64; a = parents[a], ++guard) {
            const int p = fp::bodyPartNumber(nodeName(nodes[a]));
            if (p >= 0) {
                part = p;
                break;
            }
        }
        if (!fp::torsoPart(part)) continue;
        BodyWear w{nodes[j], -2, 0};
        for (unsigned k = 0; k < bodyWearCount; ++k)
            if (bodyWear[k].node == w.node) {
                w = bodyWear[k];
                break;
            }
        next[n++] = w;
    }
    unsigned hidden = 0;
    for (unsigned k = 0; k < n; ++k) {
        BodyWear& w = next[k];
        if (w.verdict == -2 || ++w.age >= 6) {
            w.verdict = int(helmetVerdict(w.node, fp::bodyProbeLength));
            w.age = 0;
        }
        if (w.verdict == int(fp::HelmetVerdict::blocks)) {
            hideAdd(w.node);
            ++hidden;
        }
    }
    memcpy(bodyWear, next, sizeof(BodyWear) * n);
    bodyWearCount = n;
    bodyWearNodes = n;
    hiddenBodyNodes = hidden;
}
// Called from swap under cameraLock. live: a first-person frame is published.
static void tpHideAdd();
static void viewFeatures(bool live) {
    skipCovers =
        live &&
        coversHiddenInFirstPerson.load(); // first and third person both skip the tile covers (darkening)
    const bool ownView =
        live; // Both owned camera modes may hide obstructions; wear hiding is first-person only.
    live = live && viewMode.load() == 0;
    __try {
        if (!viewFault.load()) updateHeadWear(live);
        if (ownView && viewMode.load() != 0 && !viewFault.load())
            tpHideAdd(); // also through the blend to overhead // what stands between the camera and the player
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        viewFault = 1;
        headWearCount = 0;
    }
    // Body wear in its own scope: a fault there stops only body-wear hiding, never the helmet logic.
    __try {
        if (!bodyFault.load()) updateBodyWear(live);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bodyFault = 1;
        bodyWearCount = 0;
    }
    // After a view fault head wear is never hidden again.
    hidePublish();
}
