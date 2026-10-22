// What the eye sees: hiding meshes (helmet, body wear, what blocks the third-person camera), the tile covers.
// Part of first_person.cpp (one translation unit): included there, in order.

static inline bool readableRegionContains(const MEMORY_BASIC_INFORMATION& region, uintptr_t address, size_t size) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
    return region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           address >= 0x10000 && address >= begin && address - begin <= region.RegionSize &&
           size <= region.RegionSize - (address - begin);
}

// The last 32 readable regions this thread saw in the current frame (swap serial): the game's heaps are a
// handful of large committed regions, so most checks land in one already queried. A region is never kept
// past the frame it was queried in.
struct RegionCache { LONG frame = -1; unsigned count = 0, next = 0; MEMORY_BASIC_INFORMATION regions[32]{}; };
static thread_local RegionCache regionCache;

static bool readableProfileBody(uintptr_t address, size_t size, MEMORY_BASIC_INFORMATION* region) {
    if (address < 0x10000) return false;
    RegionCache& cache = regionCache;
    const LONG frame = InterlockedCompareExchange(&swapCalls, 0, 0);
    if (cache.frame != frame) { cache.frame = frame; cache.count = cache.next = 0; }
    for (unsigned k = 0; k < cache.count; ++k)
        if (readableRegionContains(cache.regions[k], address, size)) {
            if (region) *region = cache.regions[k];
            return true;
        }
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof(m)) != sizeof(m) ||
        !readableRegionContains(m, address, size)) return false;
    cache.regions[cache.next] = m;
    cache.next = (cache.next + 1) % 32;
    cache.count = (std::min)(cache.count + 1, 32u);
    if (region) *region = m;
    return true;
}

static bool readable(uintptr_t address, size_t size) {
    return fp::profile::invoke<fp::profile::Hook::memoryQuery>(readableProfileBody, address, size,
                                                            static_cast<MEMORY_BASIC_INFORMATION*>(nullptr));
}

static bool readable(uintptr_t address, size_t size, MEMORY_BASIC_INFORMATION& region) {
    return fp::profile::invoke<fp::profile::Hook::memoryQuery>(readableProfileBody, address, size, &region);
}

static inline const char* nodeName(uintptr_t node) {
    return readable(node, 0x2a0) ? reinterpret_cast<const char*>(node + field::sceneNode::name) : "";
}

// region: a fresh query of this node made within the same call.
static inline const char* nodeName(uintptr_t node, const MEMORY_BASIC_INFORMATION& region) {
    return readableRegionContains(region, node, 0x2a0) ? reinterpret_cast<const char*>(node + field::sceneNode::name)
                                                       : nodeName(node);
}

using CameraFade = void (*)(uintptr_t);
using FadeBounds = bool (*)(const float*, const float*, const float*);
static CameraFade originalCameraFade;
static FadeBounds originalFadeBounds;
static thread_local bool clearCameraObstructions = false;

static bool closeViewFadeOwner(uintptr_t controller) {
    if (!controller || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0) ||
        DWORD(InterlockedCompareExchange(&navThread, 0, 0)) != GetCurrentThreadId())
        return false;
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
    return fadeBoundsFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), origin, segment, bounds);
}

static constexpr unsigned hideCapacity = 32768;
static SRWLOCK hideLock = SRWLOCK_INIT;
static uintptr_t hideMeshes[hideCapacity];
static unsigned hideMeshCount = 0;
static uintptr_t pendingHide[hideCapacity];
static unsigned pendingHideCount = 0;
static std::atomic<unsigned> hiddenMeshes{0}, maskedDraws{0}, hideOverflow{0};

using CoverPass = uint64_t (*)(uintptr_t);
using CoverDepth = void (*)(uintptr_t);
static CoverPass originalCoverPass;
static CoverDepth originalCoverDepth;
static std::atomic<bool> coversHiddenInFirstPerson{true};
static bool hideCoversFor(uintptr_t level);
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

static uintptr_t hideScene = 0;

struct HiddenPlayerHead { uintptr_t root = 0, node = 0; };
static HiddenPlayerHead pendingPlayerHead, publishedPlayerHead;

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
    const uintptr_t part = *reinterpret_cast<const uintptr_t*>(root + field::character::headPart);
    if (!readable(part, field::bodyPart::character + sizeof(uintptr_t)) ||
        *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character) != root) return 0;
    const uintptr_t node = *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::meshNode);
    MEMORY_BASIC_INFORMATION region;
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t), region) ||
        *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::bodyPart) != part ||
        fp::bodyPartNumber(nodeName(node, region)) != 10) return 0;
    return node;
}

// The player's own body: a part's mesh or a piece hung on a part (armour), or the
// aggregate skin under the actor. Held items hang on the hand node without a part.
static bool playerBodyNode(uintptr_t root, uintptr_t node) {
    if (!readable(node, field::sceneNode::name + 16) || !readable(root, field::character::sceneNode + 8)) return false;
    const uintptr_t part = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::bodyPart);
    if (part && readable(part, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root) return true;
    const uintptr_t actor = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
    return actor && *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) == actor && !strncmp(nodeName(node), "_SKIN", 5);
}

static bool playerHeadMaskCurrent(uintptr_t sector) {
    return publishedPlayerHead.node && viewMode.load() == 0 &&
           InterlockedCompareExchange(&active, 0, 0) &&
           InterlockedCompareExchange(&cameraReady, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) &&
           *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) == publishedPlayerHead.root &&
           verifiedPlayerHead(publishedPlayerHead.root) == publishedPlayerHead.node &&
           *reinterpret_cast<const uintptr_t*>(publishedPlayerHead.node + field::sceneNode::sector) == sector;
}

static volatile LONG sectorThread = 0;
static std::atomic<int> viewFault{0};
using SectorUpdate = uintptr_t (*)(uintptr_t);
static SectorUpdate originalSectorUpdate;

static void hideAdd(uintptr_t mesh) {
    if (pendingHideCount < hideCapacity)
        pendingHide[pendingHideCount++] = mesh;
    else
        ++hideOverflow;
}

static void hidePublish() {
    const unsigned n = fp::sortUnique(pendingHide, pendingHideCount);
    const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
    AcquireSRWLockExclusive(&hideLock);
    if (n != hideMeshCount || std::memcmp(pendingHide, hideMeshes, n * sizeof(uintptr_t))) {
        std::memcpy(hideMeshes, pendingHide, n * sizeof(uintptr_t));
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
    std::memcpy(publishedTpProps.tops, pendingTpProps.tops, pendingTpProps.count * sizeof(uintptr_t));
    std::memcpy(publishedTpProps.meshes, pendingTpProps.meshes, pendingTpProps.meshCount * sizeof(HiddenTpProps::Entry));
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
    if (*reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) != hideScene) return;
    signed char tpVerdicts[16]{};
    const int last = *reinterpret_cast<const int*>(sector + field::sector::lastMesh);
    const uintptr_t list = *reinterpret_cast<const uintptr_t*>(sector + field::sector::meshes);
    for (int j = 0; j <= last; ++j) {
        const uintptr_t m = *reinterpret_cast<const uintptr_t*>(list + size_t(j) * 8);
        if (m == publishedPlayerHead.node && !playerHeadMaskCurrent(sector)) continue;
        if (fp::sortedContains(hideMeshes, hideMeshCount, m)) {
            if (!headWearMaskCurrent(m, sector) || !bodyWearMaskCurrent(m, sector) ||
                !tpPropMaskCurrent(m, sector, tpVerdicts)) continue;
            unsigned& e = *reinterpret_cast<unsigned*>(m + field::sceneNode::passMask);
            e = fp::hiddenPassMask(e);
            ++maskedDraws;
        }
    }
}

// Draw distance. After the native sector update has rebuilt each mesh's pass mask (fades, light weights and
// callbacks already run), a drawn mesh whose bounding sphere lies beyond the cutoff from the camera is dropped
// from every pass. Bigger things stay longer (the cutoff scales with the radius, 0.5x to 2x around 50 cm);
// things over 4 m across, the player's own body and anything within 12 m are never touched; other characters
// keep twice the distance. A mesh once dropped comes back only 10% nearer, so it does not flicker at the edge.
static bool effectsLowTier(); // view_effects.inl: the frame runs slow
static constexpr unsigned cullSlots = 8192;
static uintptr_t culledMeshes[cullSlots]; // open addressing; 1 marks a freed slot
static uintptr_t culledScene = 0;
static std::atomic<unsigned> distanceCulled{0};
static bool culledBefore(uintptr_t m) {
    for (unsigned i = unsigned(m >> 4) % cullSlots, n = 0; n < 16; ++n, i = (i + 1) % cullSlots) {
        if (culledMeshes[i] == m) return true;
        if (!culledMeshes[i]) return false;
    }
    return false;
}
static void rememberCulled(uintptr_t m, bool culled) {
    for (unsigned i = unsigned(m >> 4) % cullSlots, n = 0; n < 16; ++n, i = (i + 1) % cullSlots) {
        if (culledMeshes[i] == m) { if (!culled) culledMeshes[i] = 1; return; }
        if (!culledMeshes[i] || culledMeshes[i] == 1) { if (culled) culledMeshes[i] = m; return; }
    }
}
static void distanceCull(uintptr_t sector) {
    const int metres = drawDistanceM.load(std::memory_order_relaxed);
    if (metres <= 0) return;
    const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
    if (scene != culledScene) { std::memset(culledMeshes, 0, sizeof culledMeshes); culledScene = scene; }
    uintptr_t view = 0, camera = 0;
    currentCamera(view, camera);
    if (!camera || !readable(camera + field::camera::pose, 48) || !readable(sector, field::sector::meshes + 8)) return;
    const float* eye = reinterpret_cast<const float*>(camera + field::camera::pose) + 9;
    const float* origin = reinterpret_cast<const float*>(sector + field::sector::origin);
    const float local[3] = {eye[0] - origin[0], eye[1] - origin[1], eye[2] - origin[2]};
    const float reach = float(metres) * 100.f * (effectsLowTier() ? 0.7f : 1.f); // auto quality: nearer while slow
    const uintptr_t player = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const int last = *reinterpret_cast<const int*>(sector + field::sector::lastMesh);
    const uintptr_t list = *reinterpret_cast<const uintptr_t*>(sector + field::sector::meshes);
    if (last < 0 || last > 65535 || !readable(list, (size_t(last) + 1) * 8)) return;
    MEMORY_BASIC_INFORMATION region{};
    for (int j = 0; j <= last; ++j) {
        const uintptr_t m = *reinterpret_cast<const uintptr_t*>(list + size_t(j) * 8);
        if (!m || (!readableRegionContains(region, m, field::sceneNode::bodyPart + 8) &&
                   !readable(m, field::sceneNode::bodyPart + 8, region))) continue;
        unsigned& e = *reinterpret_cast<unsigned*>(m + field::sceneNode::passMask);
        if (!(e & 1)) continue;
        const float* c = reinterpret_cast<const float*>(m + field::sceneNode::boundsCentre);
        const float radius = *reinterpret_cast<const float*>(m + field::sceneNode::boundsRadius);
        if (!std::isfinite(radius) || radius > 200.f || radius < 0.f) continue;
        const float dx = c[0] - local[0], dy = c[1] - local[1], dz = c[2] - local[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - radius;
        if (!std::isfinite(distance) || distance < 1200.f) { rememberCulled(m, false); continue; }
        float cutoff = reach * std::clamp(radius / 50.f, 0.5f, 2.f);
        const uintptr_t part = *reinterpret_cast<const uintptr_t*>(m + field::sceneNode::bodyPart);
        if (part) {
            if (!readable(part, field::bodyPart::character + 8)) continue;
            const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character);
            if (owner == player) continue;
            if (owner) cutoff = (std::max)(cutoff, reach) * 2.f;
        }
        if (culledBefore(m)) cutoff *= 0.9f;
        const bool cull = distance > cutoff;
        rememberCulled(m, cull);
        if (cull) {
            e &= ~0x1000001u; // only the bits the frustum tests set: as if it were outside the view
            distanceCulled.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Lights per object. The queue (26F230), its flush recount (26F420) and the binder walk a mesh's light list
// from its head while entries are active, up to six. Clearing the active flag of the entry after the Nth
// stops all three there, keeping the N strongest (the list is sorted). The game rereads that flag when it
// recomputes a mesh's lights, so every trim is put back at the end of the same frame (the swap, after the
// render), never in the next: between frames a round or level change can free the meshes, and the write would
// land in freed memory. A trim the swap did not put back is dropped unwritten.
struct LightTrim { uintptr_t entry; unsigned active; };
static LightTrim lightTrims[16384];
static unsigned lightTrimCount = 0;
static LONG lightTrimFrame = -1;
static std::atomic<unsigned> lightsTrimmed{0};
static void restoreLightTrims() { // render thread, at the swap
    for (unsigned k = 0; k < lightTrimCount; ++k)
        if (readable(lightTrims[k].entry, field::lightEntry::active + 4))
            *reinterpret_cast<unsigned*>(lightTrims[k].entry + field::lightEntry::active) = lightTrims[k].active;
    lightTrimCount = 0;
}
static void trimLights(uintptr_t sector) {
    int keep = objectLightCap.load(std::memory_order_relaxed);
    if (keep < 6 && effectsLowTier()) keep = (std::max)(keep - 1, 2); // auto quality: one fewer while slow
    if (keep >= 6 || keep < 1 || !readable(sector, field::sector::meshes + 8)) return;
    constexpr size_t entryBytes = 0x18;
    const int lightCount = *reinterpret_cast<const int*>(sector + field::sector::lightCount);
    if (lightCount <= keep || lightCount > 4096) return;
    const int last = *reinterpret_cast<const int*>(sector + field::sector::lastMesh);
    const uintptr_t list = *reinterpret_cast<const uintptr_t*>(sector + field::sector::meshes);
    if (last < 0 || last > 65535 || !readable(list, (size_t(last) + 1) * 8)) return;
    for (int j = 0; j <= last && lightTrimCount < std::size(lightTrims); ++j) {
        const uintptr_t m = *reinterpret_cast<const uintptr_t*>(list + size_t(j) * 8);
        if (!m || !readable(m, field::sceneNode::lights + 8)) continue;
        if (!(*reinterpret_cast<const unsigned*>(m + field::sceneNode::passMask) & 1)) continue;
        int at = *reinterpret_cast<const int*>(m + field::sceneNode::lightHead);
        const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(m + field::sceneNode::lights);
        if (at < 0 || !readable(entries, size_t(lightCount) * entryBytes)) continue;
        for (int k = 0; k < keep && at >= 0 && at < lightCount; ++k) {
            const uintptr_t e = entries + size_t(at) * entryBytes;
            if (!*reinterpret_cast<const unsigned*>(e + field::lightEntry::active)) { at = -1; break; }
            at = *reinterpret_cast<const int*>(e + field::lightEntry::next);
        }
        if (at < 0 || at >= lightCount) continue;
        unsigned& flag = *reinterpret_cast<unsigned*>(entries + size_t(at) * entryBytes + field::lightEntry::active);
        if (!flag) continue;
        lightTrims[lightTrimCount++] = {entries + size_t(at) * entryBytes, flag};
        flag = 0;
        lightsTrimmed.fetch_add(1, std::memory_order_relaxed);
    }
}

// Torch shadows, measured only. 0x4F4A0 renders, per viewport, each light's shadow map whose dirty faces (+10)
// meet the faces needed this frame (+14). Counted and timed for the status file. A turn-taking budget that put
// maps off to later frames was tried and removed: the game's shadow pool hands maps between lights, so a put-off
// map showed another light's shadows (a 10 Hz flicker), and the whole scheduler costs about 0.15 ms of CPU.
using ShadowSchedule = void (*)(uintptr_t);
static ShadowSchedule originalShadowSchedule;
static std::atomic<unsigned> shadowRenders{0};
static std::atomic<float> shadowScheduleMs{0.f}; // eased CPU time of the game's shadow renders, per call
static void countShadows(uintptr_t viewport) {
    if (!readable(viewport, field::viewport::scene + 8)) return;
    const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(viewport + field::viewport::scene);
    if (!scene || !readable(scene, field::scene::lights + 8)) return;
    const int count = *reinterpret_cast<const int*>(scene + field::scene::lightCount);
    const uintptr_t lights = *reinterpret_cast<const uintptr_t*>(scene + field::scene::lights);
    if (count <= 0 || count > 4096 || !readable(lights, size_t(count) * 8)) return;
    unsigned due = 0;
    for (int i = 0; i < count; ++i) {
        const uintptr_t light = reinterpret_cast<const uintptr_t*>(lights)[i];
        if (!light || !readable(light, field::light::shadow + 8)) continue;
        const uintptr_t map = *reinterpret_cast<const uintptr_t*>(light + field::light::shadow);
        if (!map || *reinterpret_cast<const int*>(light + field::light::kind) == 2 ||
            !readable(map, field::shadowMap::needed + 4))
            continue;
        if (*reinterpret_cast<const unsigned*>(map + field::shadowMap::dirty) &
            *reinterpret_cast<const unsigned*>(map + field::shadowMap::needed))
            ++due;
    }
    shadowRenders.fetch_add(due, std::memory_order_relaxed);
}
static void shadowSchedule(uintptr_t viewport) {
    __try { countShadows(viewport); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    LARGE_INTEGER t0{}, t1{}, f{};
    QueryPerformanceCounter(&t0);
    __try {
        originalShadowSchedule(viewport);
    } __finally {
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        if (f.QuadPart) {
            const float ms = float(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
            const float eased = shadowScheduleMs.load(std::memory_order_relaxed);
            shadowScheduleMs.store(eased + (ms - eased) * 0.1f, std::memory_order_relaxed);
        }
    }
}

static uintptr_t sectorUpdateProfileBody(uintptr_t sector) {
    const LONG frame = InterlockedCompareExchange(&swapCalls, 0, 0);
    if (frame != lightTrimFrame) { // a new frame; the swap put the last one's trims back
        lightTrimCount = 0;
        lightTrimFrame = frame;
    }
    const uintptr_t result = fp::profile::invoke<fp::profile::Hook::sectorNative>(originalSectorUpdate, sector);
    InterlockedExchange(&sectorThread, LONG(GetCurrentThreadId()));
    bool fault = false;
    AcquireSRWLockShared(&hideLock);
    __try {
        if (hideMeshCount) maskSector(sector);
        distanceCull(sector);
        trimLights(sector);
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

static unsigned walkSceneVisitProfileBody(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity,
                               void (*visit)(uintptr_t, bool)) {
    MEMORY_BASIC_INFORMATION region;
    if (!readable(scene, 0x2a0, region)) return 0;
    thread_local fp::PointerSet seen;
    seen.reset(capacity);
    seen.insert(scene);
    unsigned count = 0, next = 0;
    nodes[count] = scene;
    parents[count++] = ~0u;
    while (next < count) {
        uintptr_t node = nodes[next];
        unsigned index = next++;
        // Visitors make no native call, so the last node query's region stays current.
        const bool valid = readableRegionContains(region, node, 0x2a0) || readable(node, 0x2a0, region);
        if (visit) visit(node, valid);
        if (!valid) continue;
        const int last = *reinterpret_cast<const int*>(node + field::sceneNode::lastChild);
        const uintptr_t list = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::children);
        if (last < 0 || last >= 6000 || !readable(list, (last + 1) * 8)) continue;
        for (int i = 0; i <= last && count < capacity; ++i) {
            const uintptr_t child = *reinterpret_cast<const uintptr_t*>(list + i * 8);
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

struct HeadWear {
    uintptr_t node;
    int verdict;
    unsigned age;
};
static HeadWear headWear[64];
static unsigned headWearCount = 0;
static uintptr_t headScene = 0;
static std::atomic<unsigned> hiddenHeadNodes{0}, headWearNodes{0}, helmetBlocking{0}, helmetUnknown{0},
    helmetTests{0};
static std::atomic<fp::HelmetMode> helmetMode{fp::HelmetMode::show};
// Latest scan, not lifetime counters. Independent status atomics: no snapshot guarantee.
// 0 inactive, 1 camera owner mismatch, 2 unverified head, 3 no sector, 4 scanned, 5 fault.
static std::atomic<unsigned> headWearScanState{0}, headWearWalked{0}, headWearRejected{0},
    headWearScanLimit{0}; // flags 1: walk bound reached; 2: accepted-candidate bound reached

using RayQuery = fp::game::MeshRayQuery;
using MeshRay = int (*)(uintptr_t, RayQuery*);
static int callMeshRay(uintptr_t ray, uintptr_t context, RayQuery* q) {
    return reinterpret_cast<MeshRay>(ray)(context, q);
}
static int (*meshRay)(uintptr_t, uintptr_t, RayQuery*) = callMeshRay; // replaced by the owned-memory tests

static fp::HelmetVerdict helmetVerdict(uintptr_t node, float length = fp::helmetProbeLength) {
    if (!readable(node, 0x3c0) || (*reinterpret_cast<const unsigned*>(node + 8) & 0x89) != 0x89) return fp::HelmetVerdict::unknown;
    const uintptr_t ray = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::rayRoutine),
                    context = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::rayContext);
    if (!context || ray < base + layout.textBegin || ray >= base + layout.textEnd)
        return fp::HelmetVerdict::unknown;
    const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::sector);
    if (!readable(owner, 0x70)) return fp::HelmetVerdict::unknown;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = lastCamera[9 + i] - *reinterpret_cast<const float*>(owner + field::sector::origin + i * 4);
    float directions[5][3];
    fp::helmetProbeDirections(lastCamera, directions);
    const auto m = reinterpret_cast<const float*>(node + field::sceneNode::transform);
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

static bool bodyWearCandidate(uintptr_t node, uintptr_t controlledRoot) {
    MEMORY_BASIC_INFORMATION region;
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t), region)) return false;
    const char* name = nodeName(node, region);
    if (!std::strncmp(name, "_SKIN", 5) || fp::bodyPartNumber(name) >= 0) return false;
    const uintptr_t part = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::bodyPart);
    if (part && readable(part, field::bodyPart::character + sizeof(uintptr_t)) &&
        *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character) == controlledRoot)
        return false;
    return true;
}

static bool bodyWearMaskCurrent(uintptr_t node, uintptr_t sector) {
    const HiddenBodyWear& hidden = publishedBodyWear;
    if (hidden.count > 96) return false;
    bool listed = false;
    for (unsigned i = 0; i < hidden.count; ++i)
        if (hidden.nodes[i] == node) { listed = true; break; }
    if (!listed) return true;
    if (viewMode.load() != 0 || !InterlockedCompareExchange(&active, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) != hidden.root ||
        *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) != hidden.scene ||
        !readable(hidden.root, field::character::sceneNode + sizeof(uintptr_t)) ||
        *reinterpret_cast<const uintptr_t*>(hidden.root + field::character::sceneNode) != hidden.mesh ||
        !readable(hidden.mesh, field::sceneNode::sector + sizeof(uintptr_t)) ||
        *reinterpret_cast<const uintptr_t*>(hidden.mesh + field::sceneNode::sector) != sector ||
        !bodyWearCandidate(node, hidden.root)) return false;
    uintptr_t seen[64];
    unsigned count = 0;
    int part = -1;
    for (uintptr_t at = node; at != hidden.mesh;) {
        if (!at || count == 64 || !readable(at, field::sceneNode::sector + sizeof(uintptr_t)) ||
            *reinterpret_cast<const uintptr_t*>(at + field::sceneNode::sector) != sector) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == at) return false;
        seen[count++] = at;
        if (at != node && part < 0) {
            part = fp::bodyPartNumber(nodeName(at));
            if (part >= 0 && !fp::torsoPart(part)) return false;
        }
        at = *reinterpret_cast<const uintptr_t*>(at + field::sceneNode::parent);
    }
    if (part < 0) part = fp::bodyPartNumber(nodeName(hidden.mesh));
    return fp::torsoPart(part);
}

static bool headWearCandidate(uintptr_t node, uintptr_t controlledRoot) {
    MEMORY_BASIC_INFORMATION region;
    if (!readable(node, field::sceneNode::bodyPart + sizeof(uintptr_t), region)) return false;
    const char* name = nodeName(node, region);
    if (!std::strncmp(name, "_SKIN", 5) || fp::bodyPartNumber(name) >= 0) return false;
    const uintptr_t part = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::bodyPart);
    if (!part) return true;
    if (!readable(part, field::bodyPart::flags + sizeof(unsigned))) return false;
    const unsigned flags = *reinterpret_cast<const unsigned*>(part + field::bodyPart::flags);
    if ((flags & 0xff000000) == 0x1a000000) return false;
    if ((flags & 0x12000000) != 0x12000000) return true;
    if (!readable(part, field::bodyPart::character + sizeof(uintptr_t))) return false;
    const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character);
    return !owner || owner == part || owner == controlledRoot;
}

// branch[] holds the verdicts of every lower index; parents always precede their children.
static bool headWearBranch(unsigned index, const bool* branch, uintptr_t controlledRoot) {
    if (!headWearCandidate(nodes[index], controlledRoot)) return false;
    const unsigned parent = parents[index];
    return parent < index &&
           *reinterpret_cast<const uintptr_t*>(nodes[index] + field::sceneNode::parent) == nodes[parent] &&
           (parent == 0 || branch[parent]);
}

static bool headWearMaskCurrent(uintptr_t node, uintptr_t sector) {
    bool listed = false;
    for (unsigned i = 0; i < publishedHeadWear.count; ++i)
        if (publishedHeadWear.nodes[i] == node) { listed = true; break; }
    if (!listed) return true;
    const uintptr_t root = publishedHeadWear.root, head = publishedHeadWear.head;
    if (viewMode.load() != 0 || !InterlockedCompareExchange(&active, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) ||
        *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) != root ||
        *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) != publishedHeadWear.scene ||
        verifiedPlayerHead(root) != head ||
        !head || *reinterpret_cast<const uintptr_t*>(head + field::sceneNode::sector) != sector) return false;
    for (unsigned guard = 0; node != head && guard < 256; ++guard) {
        if (!headWearCandidate(node, root) ||
            *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::sector) != sector) return false;
        const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::parent);
        if (!readable(parent, 0x2a0)) return false;
        const int last = *reinterpret_cast<const int*>(parent + field::sceneNode::lastChild);
        const uintptr_t children = *reinterpret_cast<const uintptr_t*>(parent + field::sceneNode::children);
        if (last < 0 || last >= 6000 || !readable(children, (last + 1) * 8)) return false;
        bool attached = false;
        for (int i = 0; i <= last; ++i)
            if (*reinterpret_cast<const uintptr_t*>(children + i * 8) == node) { attached = true; break; }
        if (!attached) return false;
        node = parent;
    }
    return node == head;
}

static void updateHeadWear(bool live) {
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
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
        headWearScanState.store(*reinterpret_cast<const uintptr_t*>(headMesh + field::sceneNode::sector) ? 4 : 3,
                                std::memory_order_relaxed);
    if (!headReadable || !*reinterpret_cast<const uintptr_t*>(headMesh + field::sceneNode::sector) ||
        scene != headScene) {
        headWearCount = 0;
        headScene = scene;
        hiddenHeadNodes = 0;
        headWearNodes = 0;
        helmetBlocking = 0;
        helmetUnknown = 0;
        if (!readable(headMesh, 0x2a0) || !*reinterpret_cast<const uintptr_t*>(headMesh + field::sceneNode::sector)) return;
    }
    pendingHeadWear.root = root;
    pendingHeadWear.head = headMesh;
    pendingHeadWear.scene = scene;
    const unsigned count = walkScene(headMesh, nodes, parents, 256);
    headWearWalked.store(count, std::memory_order_relaxed);
    HeadWear next[64];
    unsigned n = 0, rejected = 0;
    bool branch[256];
    for (unsigned j = 1; j < count && n < 64; ++j) {
        branch[j] = headWearBranch(j, branch, root);
        if (!branch[j]) { ++rejected; continue; }
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
    static float testedForward[3]{};
    const float forward[3] = {-lastCamera[6], -lastCamera[7], -lastCamera[8]};
    const bool turned = fp::helmetRetestDue(testedForward, forward);
    if (turned) {
        std::memcpy(testedForward, forward, 12);
        ++helmetTurnRetests;
    }
    for (unsigned k = 0; k < n; ++k) {
        HeadWear& w = next[k];
        if (helmetMode.load() == fp::HelmetMode::hide) w.verdict = -2;
        if (helmetMode.load() == fp::HelmetMode::show && (w.verdict == -2 || turned || ++w.age >= 30)) {
            w.verdict = int(helmetVerdict(w.node));
            w.age = 0;
        }
        const bool hide = fp::hideHeadWear(helmetMode.load(), fp::HelmetVerdict(w.verdict));
        if (hide) {
            hideAdd(w.node);
            pendingHeadWear.nodes[pendingHeadWear.count++] = w.node;
            ++hidden;
        }
        if (w.verdict == int(fp::HelmetVerdict::blocks)) ++blocking;
        if (w.verdict == int(fp::HelmetVerdict::unknown)) ++unknown;
    }
    std::memcpy(headWear, next, sizeof(HeadWear) * n);
    headWearCount = n;
    headWearNodes = n;
    hiddenHeadNodes = hidden;
    helmetBlocking = blocking;
    helmetUnknown = unknown;
}

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
        live = false;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
    live = live && root == savedRoot && scene == savedScene;
    if (!live) {
        bodyWearCount = 0;
        bodyScene = scene;
        bodyWearNodes = 0;
        hiddenBodyNodes = 0;
        return;
    }
    const uintptr_t rootMesh = readable(root, 0x1200) ? *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode) : 0;
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
        if (!bodyWearCandidate(nodes[j], root)) continue;
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
    std::memcpy(bodyWear, next, sizeof(BodyWear) * n);
    bodyWearCount = n;
    bodyWearNodes = n;
    hiddenBodyNodes = hidden;
}

static void tpHideAdd();
static void viewFeaturesProfileBody(bool live) {
    const bool ownView = live;
    live = live && viewMode.load() == 0;
    pendingPlayerHead = {};
    pendingHeadWear = {};
    pendingBodyWear = {};
    pendingTpProps.reset();
    __try {
        if (live && !viewFault.load()) {
            const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
            const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
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
            tpHideAdd();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        viewFault = 1;
        headWearScanState.store(5, std::memory_order_relaxed);
        headWearCount = 0;
    }
    __try {
        if (!bodyFault.load()) updateBodyWear(live);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bodyFault = 1;
        bodyWearCount = 0;
    }
    hidePublish();
}

static void viewFeatures(bool live) {
    return fp::profile::invoke<fp::profile::Hook::viewFeatures>(viewFeaturesProfileBody, live);
}
