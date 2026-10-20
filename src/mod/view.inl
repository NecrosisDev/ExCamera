// What the eye sees: hiding meshes (helmet, body wear, what blocks the third-person camera), the tile covers.
// Part of first_person.cpp (one translation unit): included there, in order.
// ---- First-person view features (render thread only; no C++ unwinding inside __try) ----
// Hiding: what decides whether a registered mesh is drawn is its pass mask mesh+0xE0, which the
// per-sector update 0x58CD0 rebuilds every frame (sector bit 0x10000000 from +0x3A0, | 0x1000001 when
// the bounds pass the view tests 0x79E60 / 0x58AC0). The render passes (0x4E610, 0x4EB30, 0x4EE10,
// 0x4EFE0, 0x4F0B0) and the screen pick 0x8F720 take a mesh only when their pass mask & mesh+0xE0 is
// non-zero; flagsC 0x10000 is not read there, so clearing it hides nothing. A hidden mesh gets, right
// after 0x58CD0 returns for its sector, the value of a mesh outside the view (fp::hiddenPassMask).
// No persistent mask is stored: the next native update recomputes it, so leaving the hide list, F6, F7 or a
// scene change show it again with no undo.
static bool readableRegionContains(const MEMORY_BASIC_INFORMATION& region, uintptr_t address, size_t size) {
    const uintptr_t begin = (uintptr_t)region.BaseAddress;
    return region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           address >= 0x10000 && address >= begin && address - begin <= region.RegionSize &&
           size <= region.RegionSize - (address - begin);
}
static bool readableProfileBody(uintptr_t address, size_t size, MEMORY_BASIC_INFORMATION* region) {
    MEMORY_BASIC_INFORMATION m;
    if (address < 0x10000 || VirtualQuery((void*)address, &m, sizeof(m)) != sizeof(m) ||
        !readableRegionContains(m, address, size)) return false;
    if (region) *region = m;
    return true;
}
static bool readable(uintptr_t address, size_t size) {
    return fp::profile::invoke<fp::profile::Hook::memoryQuery>(readableProfileBody, address, size,
                                                            (MEMORY_BASIC_INFORMATION*)nullptr);
}
static bool readable(uintptr_t address, size_t size, MEMORY_BASIC_INFORMATION& region) {
    return fp::profile::invoke<fp::profile::Hook::memoryQuery>(readableProfileBody, address, size, &region);
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
static std::atomic<bool> coversHiddenInFirstPerson{true};
static bool hideCoversFor(uintptr_t level); // current native render owner, not last swap
static std::atomic<unsigned> coverPassesSkipped{0};
static uint64_t coverPass(uintptr_t level) {
    if (hideCoversFor(level)) {
        coverPassesSkipped.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return originalCoverPass(level);
}
static void coverDepth(uintptr_t level) {
    if (hideCoversFor(level)) return;
    originalCoverDepth(level);
}
static uintptr_t hideScene = 0; // scene manager the list belongs to; another scene is never masked
// Whole-head visibility is independent of the helmet option. These exact-node
// identities travel with the hide list; they never describe a subtree or _SKIN.
struct HiddenPlayerHead { uintptr_t root = 0, node = 0; };
static HiddenPlayerHead pendingPlayerHead, publishedPlayerHead;
// Exact headwear entries share the hide-list lock and are revalidated when used.
struct HiddenHeadWear {
    uintptr_t root = 0, head = 0, scene = 0, nodes[64]{};
    unsigned count = 0;
};
static HiddenHeadWear pendingHeadWear, publishedHeadWear;
struct HiddenBodyWear {
    uintptr_t root = 0, mesh = 0, scene = 0, nodes[96]{};
    unsigned count = 0;
};
static HiddenBodyWear pendingBodyWear, publishedBodyWear;
static bool bodyWearMaskCurrent(uintptr_t node, uintptr_t sector);

// TP hides retain exact mesh-to-prop provenance until the sector applies them.
// Actor ownership can change after the render thread published the list.
struct HiddenTpProps {
    struct Entry { uintptr_t node = 0; unsigned targets = 0; };
    uintptr_t root = 0, scene = 0, tops[16]{};
    unsigned count = 0, meshCount = 0;
    Entry meshes[16 * 256]{};
    void reset() {
        root = scene = 0;
        count = meshCount = 0;
    }
};
static HiddenTpProps pendingTpProps, publishedTpProps;
static bool tpPropMaskCurrent(uintptr_t node, uintptr_t sector, signed char (&verdicts)[16]);

static bool headWearMaskCurrent(uintptr_t node, uintptr_t sector);
static uintptr_t verifiedPlayerHead(uintptr_t root) {
    if (!readable(root, field::character::headPart + sizeof(uintptr_t))) return 0;
    const uintptr_t part = *(uintptr_t*)(root + field::character::headPart);
    if (!readable(part, field::bodyPart::character + sizeof(uintptr_t)) ||
        *(uintptr_t*)(part + field::bodyPart::character) != root) return 0;
    const uintptr_t node = *(uintptr_t*)(part + field::bodyPart::meshNode);
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t)) ||
        *(uintptr_t*)(node + field::sceneNode::bodyPart) != part ||
        fp::bodyPartNumber(nodeName(node)) != 10) return 0;
    return node;
}
// Rechecked at application, not merely when the preceding frame published it.
static bool playerHeadMaskCurrent(uintptr_t sector) {
    return publishedPlayerHead.node && viewMode.load() == 0 &&
           InterlockedCompareExchange(&active, 0, 0) &&
           InterlockedCompareExchange(&cameraReady, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) &&
           *(uintptr_t*)gameAt(fp::sig::playerRoot) == publishedPlayerHead.root &&
           verifiedPlayerHead(publishedPlayerHead.root) == publishedPlayerHead.node &&
           *(uintptr_t*)(publishedPlayerHead.node + field::sceneNode::sector) == sector;
}
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
    std::sort(pendingTpProps.meshes, pendingTpProps.meshes + pendingTpProps.meshCount,
              [](const HiddenTpProps::Entry& a, const HiddenTpProps::Entry& b) { return a.node < b.node; });
    unsigned unique = 0;
    for (unsigned i = 0; i < pendingTpProps.meshCount; ++i) {
        const auto entry = pendingTpProps.meshes[i];
        if (unique && pendingTpProps.meshes[unique - 1].node == entry.node)
            pendingTpProps.meshes[unique - 1].targets |= entry.targets;
        else pendingTpProps.meshes[unique++] = entry;
    }
    pendingTpProps.meshCount = unique;
    publishedTpProps.root = pendingTpProps.root;
    publishedTpProps.scene = pendingTpProps.scene;
    publishedTpProps.count = pendingTpProps.count;
    publishedTpProps.meshCount = pendingTpProps.meshCount;
    memcpy(publishedTpProps.tops, pendingTpProps.tops, pendingTpProps.count * sizeof(uintptr_t));
    memcpy(publishedTpProps.meshes, pendingTpProps.meshes, pendingTpProps.meshCount * sizeof(HiddenTpProps::Entry));
    pendingTpProps.reset();
    publishedPlayerHead = pendingPlayerHead;
    pendingPlayerHead = {};
    publishedHeadWear = pendingHeadWear;
    pendingHeadWear = {};
    publishedBodyWear = pendingBodyWear;
    pendingBodyWear = {};
    ReleaseSRWLockExclusive(&hideLock);
    hiddenMeshes = n;
    pendingHideCount = 0;
}
static void maskSector(uintptr_t sector) {
    if (*(uintptr_t*)(gameAt(fp::sig::currentScene)) != hideScene) return;
    signed char tpVerdicts[16]{}; // Each prop scans the actor roster at most once per sector.
    const int last = *(int*)(sector + field::sector::lastMesh);
    const uintptr_t list = *(uintptr_t*)(sector + field::sector::meshes);
    for (int j = 0; j <= last; ++j) {
        const uintptr_t m = *(uintptr_t*)(list + size_t(j) * 8);
        if (m == publishedPlayerHead.node && !playerHeadMaskCurrent(sector)) continue;
        if (fp::sortedContains(hideMeshes, hideMeshCount, m)) {
            if (!headWearMaskCurrent(m, sector) || !bodyWearMaskCurrent(m, sector) ||
                !tpPropMaskCurrent(m, sector, tpVerdicts)) continue;
            unsigned& e = *(unsigned*)(m + field::sceneNode::passMask);
            e = fp::hiddenPassMask(e);
            ++maskedDraws;
        }
    }
}
static uintptr_t sectorUpdateProfileBody(uintptr_t sector) {
    const uintptr_t result = fp::profile::invoke<fp::profile::Hook::sectorNative>(originalSectorUpdate, sector);
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
static uintptr_t sectorUpdate(uintptr_t sector) {
    return fp::profile::invoke<fp::profile::Hook::sector>(sectorUpdateProfileBody, sector);
}
// Breadth-first scene walk into a flat list. Returns node count.
static unsigned walkSceneVisitProfileBody(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity,
                               void (*visit)(uintptr_t, bool)) {
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
        const bool valid = readable(node, 0x2a0);
        // A visitor may compact already-consumed queue entries, never unread
        // ones. No native callback runs between validation and the flags edit.
        if (visit) visit(node, valid);
        if (!valid) continue;
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
static unsigned walkSceneVisit(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity,
                               void (*visit)(uintptr_t, bool)) {
    return fp::profile::invoke<fp::profile::Hook::sceneWalk>(walkSceneVisitProfileBody, scene, nodes, parents, capacity, visit);
}
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity) {
    return walkSceneVisit(scene, nodes, parents, capacity, nullptr);
}
static uintptr_t nodes[8192];
static unsigned parents[8192];
// The level being played: the scene object's name (base+0x4F0BD0, +0x140).
// Head wear: equipment is parented under
// body-part nodes (a sword under 17RHand), so anything under the controlled
// character's 10Head node (root+0x11E0 -> +0x18) is head wear. [View] Helmet=show
// (default) retains native rendering, animation and face culling; Helmet=hide hides
// verified head attachments. The separate verified 10Head drawable is hidden
// independently above; the shared _SKIN node is never hidden. A worn mesh that covers the view
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
// Latest scan, not lifetime counters. Independent status atomics: no snapshot guarantee.
// 0 inactive, 1 camera owner mismatch, 2 unverified head, 3 no sector, 4 scanned, 5 fault.
static std::atomic<unsigned> headWearScanState{0}, headWearWalked{0}, headWearRejected{0},
    headWearScanLimit{0}; // flags 1: walk bound reached; 2: accepted-candidate bound reached
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
static bool bodyWearCandidate(uintptr_t node, uintptr_t controlledRoot) {
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t))) return false;
    const char* name=nodeName(node);
    if (!strncmp(name, "_SKIN", 5) || fp::bodyPartNumber(name)>=0) return false;
    const uintptr_t part=*(uintptr_t*)(node+field::sceneNode::bodyPart);
    if (part && readable(part, field::bodyPart::character+sizeof(uintptr_t)) &&
        *(uintptr_t*)(part+field::bodyPart::character)==controlledRoot)
        return false;
    return true;
}
// Reuse the existing clothing classification and native ancestry, not a new
// body/physics ownership heuristic. Same-address valid-clothing ABA is outside
// this current actor/scene/branch check, as it is for TP prop provenance.
static bool bodyWearMaskCurrent(uintptr_t node, uintptr_t sector) {
    const HiddenBodyWear& hidden = publishedBodyWear;
    if (hidden.count > 96) return false;
    bool listed = false;
    for (unsigned i = 0; i < hidden.count; ++i)
        if (hidden.nodes[i] == node) { listed = true; break; }
    if (!listed) return true;
    if (viewMode.load() != 0 || !InterlockedCompareExchange(&active, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        *(uintptr_t*)gameAt(fp::sig::playerRoot) != hidden.root ||
        *(uintptr_t*)gameAt(fp::sig::currentScene) != hidden.scene ||
        !readable(hidden.root, field::character::sceneNode + sizeof(uintptr_t)) ||
        *(uintptr_t*)(hidden.root + field::character::sceneNode) != hidden.mesh ||
        !readable(hidden.mesh, field::sceneNode::sector + sizeof(uintptr_t)) ||
        *(uintptr_t*)(hidden.mesh + field::sceneNode::sector) != sector ||
        !bodyWearCandidate(node, hidden.root)) return false;
    uintptr_t seen[64];
    unsigned count = 0;
    int part = -1;
    for (uintptr_t at = node; at != hidden.mesh;) {
        if (!at || count == 64 || !readable(at, field::sceneNode::sector + sizeof(uintptr_t)) ||
            *(uintptr_t*)(at + field::sceneNode::sector) != sector) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == at) return false;
        seen[count++] = at;
        if (at != node && part < 0) {
            part = fp::bodyPartNumber(nodeName(at));
            if (part >= 0 && !fp::torsoPart(part)) return false;
        }
        at = *(uintptr_t*)(at + field::sceneNode::parent);
    }
    if (part < 0) part = fp::bodyPartNumber(nodeName(hidden.mesh)); // Publication includes nodes[0].
    return fp::torsoPart(part);
}
// Native 0x07FB80 parents equipment meshes below a body-part mesh; 0x1551B0
// propagates character ownership to attached physics parts. Ownership therefore
// cannot distinguish a helmet from bare skin. Only accept the verified head's
// descendant branch, excluding skin, anatomical nodes and foreign actors.
static bool headWearCandidate(uintptr_t node, uintptr_t controlledRoot) {
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t))) return false;
    const char* name = nodeName(node);
    if (!strncmp(name, "_SKIN", 5) || fp::bodyPartNumber(name) >= 0) return false;
    const uintptr_t part = *(uintptr_t*)(node + field::sceneNode::bodyPart);
    if (!part) return true; // Non-physical helmet meshes use the same head subtree.
    if (!readable(part, field::bodyPart::flags + sizeof(unsigned))) return false;
    const unsigned flags = *(unsigned*)(part + field::bodyPart::flags);
    // 0x1A2CE0 gates +0x580 on the motile-part family. Head cloth uses 0x60
    // (0x1726C0 / 0x158B30), not this layout. Never read its unrelated +0x580.
    if ((flags & 0xff000000) == 0x1a000000) return false; // A nested character, not equipment.
    if ((flags & 0x12000000) != 0x12000000) return true;
    if (!readable(part, field::bodyPart::character + sizeof(uintptr_t))) return false;
    const uintptr_t owner = *(uintptr_t*)(part + field::bodyPart::character);
    return !owner || owner == part || owner == controlledRoot;
}
static bool headWearBranch(unsigned index, unsigned count, uintptr_t controlledRoot) {
    for (unsigned guard = 0; index && index < count && guard < 256; ++guard) {
        if (!headWearCandidate(nodes[index], controlledRoot)) return false;
        const unsigned parent = parents[index];
        if (parent >= index ||
            *(uintptr_t*)(nodes[index] + field::sceneNode::parent) != nodes[parent]) return false;
        index = parent;
    }
    return index == 0;
}
// A same-scene unequip, reparent or controlled-actor handoff must not let a
// previous frame hide someone else's equipment. No shared traversal scratch is used.
static bool headWearMaskCurrent(uintptr_t node, uintptr_t sector) {
    bool listed = false;
    for (unsigned i = 0; i < publishedHeadWear.count; ++i)
        if (publishedHeadWear.nodes[i] == node) { listed = true; break; }
    if (!listed) return true; // Other visibility policies retain their own checks.
    const uintptr_t root = publishedHeadWear.root, head = publishedHeadWear.head;
    if (viewMode.load() != 0 || !InterlockedCompareExchange(&active, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) ||
        *(uintptr_t*)gameAt(fp::sig::playerRoot) != root ||
        *(uintptr_t*)gameAt(fp::sig::currentScene) != publishedHeadWear.scene ||
        verifiedPlayerHead(root) != head ||
        !head || *(uintptr_t*)(head + field::sceneNode::sector) != sector) return false;
    for (unsigned guard = 0; node != head && guard < 256; ++guard) {
        if (!headWearCandidate(node, root) ||
            *(uintptr_t*)(node + field::sceneNode::sector) != sector) return false;
        const uintptr_t parent = *(uintptr_t*)(node + field::sceneNode::parent);
        if (!readable(parent, 0x2a0)) return false;
        const int last = *(int*)(parent + field::sceneNode::lastChild);
        const uintptr_t children = *(uintptr_t*)(parent + field::sceneNode::children);
        if (last < 0 || last >= 6000 || !readable(children, (last + 1) * 8)) return false;
        bool attached = false;
        for (int i = 0; i <= last; ++i)
            if (*(uintptr_t*)(children + i * 8) == node) { attached = true; break; }
        if (!attached) return false;
        node = parent;
    }
    return node == head;
}
static void updateHeadWear(bool live) {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    headWearWalked.store(0, std::memory_order_relaxed);
    headWearRejected.store(0, std::memory_order_relaxed);
    headWearScanLimit.store(0, std::memory_order_relaxed);
    headWearScanState.store(!live ? 0 : (root != savedRoot || scene != savedScene) ? 1 : 2,
                            std::memory_order_relaxed);
    live = live && root == savedRoot && scene == savedScene;
    pendingHeadWear = {};
    if (!live) {
        headWearCount = 0;
        headScene = scene;
        hiddenHeadNodes = 0;
        headWearNodes = 0;
        helmetBlocking = 0;
        helmetUnknown = 0;
        return;
    }
    const uintptr_t headMesh = verifiedPlayerHead(root);
    const bool headReadable = readable(headMesh, 0x2a0);
    if (headReadable)
        headWearScanState.store(*(uintptr_t*)(headMesh + field::sceneNode::sector) ? 4 : 3,
                                std::memory_order_relaxed);
    if (!headReadable || !*(uintptr_t*)(headMesh + field::sceneNode::sector) ||
        scene != headScene) {
        headWearCount = 0;
        headScene = scene;
        hiddenHeadNodes = 0;
        headWearNodes = 0;
        helmetBlocking = 0;
        helmetUnknown = 0;
        if (!readable(headMesh, 0x2a0) || !*(uintptr_t*)(headMesh + field::sceneNode::sector)) return;
    }
    pendingHeadWear.root = root;
    pendingHeadWear.head = headMesh;
    pendingHeadWear.scene = scene;
    const unsigned count = walkScene(headMesh, nodes, parents, 256);
    headWearWalked.store(count, std::memory_order_relaxed);
    HeadWear next[64];
    unsigned n = 0, rejected = 0;
    for (unsigned j = 1; j < count && n < 64; ++j) {
        if (!headWearBranch(j, count, root)) { ++rejected; continue; }
        HeadWear w{nodes[j], -2, 0};
        for (unsigned k = 0; k < headWearCount; ++k)
            if (headWear[k].node == w.node) {
                w = headWear[k];
                break;
            }
        next[n++] = w;
    }
    headWearRejected.store(rejected, std::memory_order_relaxed);
    headWearScanLimit.store((count == 256 ? 1u : 0u) | (n == 64 ? 2u : 0u), std::memory_order_relaxed);
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
        if (helmetMode == fp::HelmetMode::hide) w.verdict = -2; // Show must probe the current pose.
        if (helmetMode == fp::HelmetMode::show && (w.verdict == -2 || turned || ++w.age >= 30)) {
            w.verdict = int(helmetVerdict(w.node));
            w.age = 0;
        }
        const bool hide = fp::hideHeadWear(helmetMode, fp::HelmetVerdict(w.verdict));
        if (hide) {
            hideAdd(w.node);
            pendingHeadWear.nodes[pendingHeadWear.count++] = w.node;
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
// unless the test says so). This clothing filter never touches skin, head, hands, legs or feet.
// The verified head drawable has its own first-person visibility policy above.
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
    pendingBodyWear = {};
    if (live && lastCamera[7] < 0.34f)
        live =
            false; // only while looking down (view forward more than 20 degrees below level): no cost otherwise
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    live = live && root == savedRoot && scene == savedScene;
    if (!live) {
        bodyWearCount = 0;
        bodyScene = scene;
        bodyWearNodes = 0;
        hiddenBodyNodes = 0;
        return;
    }
    const uintptr_t rootMesh = readable(root, 0x1200) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    if (!readable(rootMesh, 0x2a0) || scene != bodyScene) {
        bodyWearCount = 0;
        bodyScene = scene;
        bodyWearNodes = 0;
        hiddenBodyNodes = 0;
        if (!readable(rootMesh, 0x2a0)) return;
    }
    pendingBodyWear.root = root;
    pendingBodyWear.mesh = rootMesh;
    pendingBodyWear.scene = scene;
    const unsigned count = walkScene(rootMesh, nodes, parents, 512);
    BodyWear next[96];
    unsigned n = 0;
    for (unsigned j = 1; j < count && n < 96; ++j) {
        // The character's own skinned torso, hands and head are never clothing.
        if (!bodyWearCandidate(nodes[j],root)) continue;
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
            pendingBodyWear.nodes[pendingBodyWear.count++] = w.node;
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
static void viewFeaturesProfileBody(bool live) {
    const bool ownView =
        live; // Both owned camera modes may hide obstructions; wear hiding is first-person only.
    live = live && viewMode.load() == 0;
    pendingPlayerHead = {};
    pendingHeadWear = {};
    pendingBodyWear = {};
    pendingTpProps.reset();
    __try {
        if (live && !viewFault.load()) {
            const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
            const uintptr_t scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
            // Do not publish a replacement character against the previous camera.
            if (root == savedRoot && scene == savedScene) {
                const uintptr_t node = verifiedPlayerHead(root);
                if (node) {
                    pendingPlayerHead = {root, node};
                    hideAdd(node);
                }
            }
        }

        if (!viewFault.load()) updateHeadWear(live);
        if (ownView && viewMode.load() != 0 && !viewFault.load())
            tpHideAdd(); // also through the blend to overhead // what stands between the camera and the player
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        viewFault = 1;
        headWearScanState.store(5, std::memory_order_relaxed);
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
static void viewFeatures(bool live) {
    return fp::profile::invoke<fp::profile::Hook::viewFeatures>(viewFeaturesProfileBody, live);
}
