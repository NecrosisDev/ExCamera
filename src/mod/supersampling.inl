// Native sampled projections keep their XY jitter and depth coefficient. Only
// authored depth offsets are adapted to the camera that owns this render call.
using RenderViewport = void (*)(uintptr_t);
using TerrainPass = uint64_t (*)(uintptr_t, uintptr_t);
using MultiplyMatrix = void (*)(float*, const float*, const float*);
static RenderViewport originalRenderViewport; // RVA 0x4BAB0: complete viewport presentation
static TerrainPass originalTerrainPass;       // RVA 0x1B8760: distance-scaled terrain sample spread
extern "C" {
MultiplyMatrix excameraOriginalMultiplyMatrix = nullptr; // RVA 0x18C70: native 4x4 multiplication
void excameraMultiplyMatrixHook(float*, const float*, const float*);
}
// Fixtures assign their native multiply spy through this alias and exercise the shipping assembly entry.
static MultiplyMatrix& originalMultiplyMatrix = excameraOriginalMultiplyMatrix;

static constexpr unsigned supersampleSlotCount = 4;
static constexpr uintptr_t supersampleMatrixBytes = 16 * sizeof(float);
struct SupersampleRenderScope {
    bool owned = false;
    uintptr_t viewport = 0, view = 0, camera = 0, controller = 0, root = 0, scene = 0;
    float stockNear = 0.f, farPlane = 0.f, stockOrigin[3]{};
};
struct SupersampleTerrainScope {
    bool active = false;
    float entryP10 = 0.f, distanceRatio = 1.f;
};
static thread_local SupersampleRenderScope supersampleRenderScope;
static thread_local SupersampleTerrainScope supersampleTerrainScope;
static std::atomic<unsigned> supersampleScopes{0}, supersampleCorrections{0},
    supersampleTerrainCorrections{0}, supersampleFaults{0};

static bool admitSupersampleRender(uintptr_t viewport, SupersampleRenderScope& scope) {
    if (!viewport || !TryAcquireSRWLockShared(&cameraLock)) return false;
    bool admitted = false;
    __try {
        uintptr_t view = 0, camera = 0;
        currentCamera(view, camera);
        // A stop request does not restore an already-published camera. This
        // scope finishes that presentation before awaitRestore can remove hooks.
        if (viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport) &&
            InterlockedCompareExchange(&pendingRestore, 0, 0) && !park.active &&
            !InterlockedCompareExchange(&restorationUnknown, 0, 0) && sameOwner(view, camera) &&
            savedRoot && savedScene && !identityChanged()) {
            const float farPlane = *(float*)(camera + field::camera::farPlane);
            bool finite = std::isfinite(stockNear) && stockNear > 0.f && std::isfinite(farPlane) &&
                          (farPlane == 0.f || farPlane > stockNear);
            for (unsigned k = 0; k < 3; ++k) finite = finite && std::isfinite(stockPose[9 + k]);
            if (finite) {
                scope.viewport = viewport;
                scope.view = view;
                scope.camera = camera;
                scope.controller = savedController;
                scope.root = savedRoot;
                scope.scene = savedScene;
                scope.stockNear = stockNear;
                scope.farPlane = farPlane;
                memcpy(scope.stockOrigin, stockPose + 9, sizeof scope.stockOrigin);
                scope.owned = true;
                // Admission and the worker's quiescence check share cameraLock.
                InterlockedIncrement(&renderScopesInFlight);
                admitted = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        supersampleFaults.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockShared(&cameraLock);
    return admitted;
}

static void renderViewport(uintptr_t viewport) {
    const SupersampleRenderScope previousRender = supersampleRenderScope;
    const SupersampleTerrainScope previousTerrain = supersampleTerrainScope;
    SupersampleRenderScope scope;
    const bool admitted = admitSupersampleRender(viewport, scope);
    supersampleRenderScope = scope;
    supersampleTerrainScope = {};
    if (admitted) supersampleScopes.fetch_add(1, std::memory_order_relaxed);
    __try {
        originalRenderViewport(viewport);
    } __finally {
        supersampleTerrainScope = previousTerrain;
        supersampleRenderScope = previousRender;
        if (admitted) InterlockedDecrement(&renderScopesInFlight);
    }
}

// Called only after a render scope or exact matrix family has been identified.
// The original renderer owns these current pointers; no saved pointer is used
// after its viewport, camera or gameplay has been replaced.
static bool supersampleViewMatches() {
    const SupersampleRenderScope& scope = supersampleRenderScope;
    if (!scope.owned || *(uintptr_t*)gameAt(fp::sig::renderView) != scope.view ||
        *(uintptr_t*)gameAt(fp::sig::mainViewport) != scope.viewport)
        return false;
    return *(uintptr_t*)(scope.viewport + field::viewport::view) == scope.view &&
           *(uintptr_t*)(scope.view + field::view::camera) == scope.camera &&
           *(uintptr_t*)(scope.camera + field::camera::controller) == scope.controller &&
           *(uintptr_t*)gameAt(fp::sig::playerRoot) == scope.root &&
           *(uintptr_t*)gameAt(fp::sig::currentScene) == scope.scene;
}

// Covers share the already-admitted native presentation, including the existing
// blend back to overhead. An unrelated viewport/level never inherits a cached skip.
static bool hideCoversFor(uintptr_t level) {
    if (!level || !coversHiddenInFirstPerson.load(std::memory_order_relaxed) ||
        !supersampleRenderScope.owned)
        return false;
    __try {
        return supersampleViewMatches() && levelGrid() == level;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false; // uncertain ownership preserves the native pass
    }
}

static void supersampleCurrentStockOrigin(float origin[3]) {
    const SupersampleRenderScope& scope = supersampleRenderScope;
    memcpy(origin, scope.stockOrigin, sizeof scope.stockOrigin);
    if (!TryAcquireSRWLockShared(&cameraLock)) return;
    __try {
        // Temporal navigation can republish stockPose after the viewport scope
        // starts. Use that matching position for this terrain pass; an already
        // retired owner or a busy lock keeps the admitted scope's fallback.
        if (InterlockedCompareExchange(&pendingRestore, 0, 0) && !park.active &&
            !InterlockedCompareExchange(&restorationUnknown, 0, 0) &&
            savedView == scope.view && savedCamera == scope.camera &&
            savedController == scope.controller && savedRoot == scope.root &&
            savedScene == scope.scene) {
            bool finite = true;
            for (unsigned k = 0; k < 3; ++k) finite = finite && std::isfinite(stockPose[9 + k]);
            if (finite) memcpy(origin, stockPose + 9, sizeof scope.stockOrigin);
        }
    } __finally {
        ReleaseSRWLockShared(&cameraLock);
    }
}

static SupersampleTerrainScope captureSupersampleTerrain() {
    SupersampleTerrainScope terrain;
    if (!supersampleRenderScope.owned) return terrain;
    __try {
        if (supersampleViewMatches() && *(int*)gameAt(fp::sig::sampleCount) > 1) {
            const SupersampleRenderScope& scope = supersampleRenderScope;
            const uintptr_t target = *(uintptr_t*)(scope.camera + field::camera::target);
            if (target) {
                const float* cameraOrigin = (const float*)(scope.camera + field::camera::pose) + 9;
                const float* targetOrigin = (const float*)(target + field::sceneNode::transformOrigin);
                float currentStockOrigin[3];
                supersampleCurrentStockOrigin(currentStockOrigin);
                double closeSquared = 0.0, stockSquared = 0.0;
                for (unsigned k = 0; k < 3; ++k) {
                    const double closeDelta = double(cameraOrigin[k]) - double(targetOrigin[k]);
                    const double stockDelta = double(currentStockOrigin[k]) - double(targetOrigin[k]);
                    closeSquared += closeDelta * closeDelta;
                    stockSquared += stockDelta * stockDelta;
                }
                const float entryP10 = ((const float*)gameAt(fp::sig::sampleProjections))[10];
                if (std::isfinite(closeSquared) && closeSquared > 0.0 && std::isfinite(stockSquared) &&
                    stockSquared > 0.0 && std::isfinite(entryP10)) {
                    // Native 0x1B8760 uses 100 / squared camera-target distance.
                    const double ratio = closeSquared / stockSquared;
                    if (std::isfinite(ratio) && ratio > 0.0 &&
                        ratio <= double((std::numeric_limits<float>::max)()) && float(ratio) > 0.f) {
                        terrain.active = true;
                        terrain.entryP10 = entryP10;
                        terrain.distanceRatio = float(ratio);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        supersampleFaults.fetch_add(1, std::memory_order_relaxed);
    }
    return terrain;
}

static uint64_t terrainPass(uintptr_t manager, uintptr_t renderer) {
    const SupersampleTerrainScope previous = supersampleTerrainScope;
    supersampleTerrainScope = captureSupersampleTerrain();
    uint64_t result = 0;
    __try {
        result = originalTerrainPass(manager, renderer);
    } __finally {
        supersampleTerrainScope = previous;
    }
    return result;
}

static bool supersampleMatrixSlot(float* output, const float* view, const float* projection,
                                  unsigned& slot) {
    const uintptr_t projectionAddress = (uintptr_t)projection;
    uintptr_t displacement = projectionAddress - gameAt(fp::sig::sampleProjections);
    uintptr_t outputBase = gameAt(fp::sig::sampleViewProjections);
    if (displacement >= supersampleSlotCount * supersampleMatrixBytes) {
        displacement = projectionAddress - gameAt(fp::sig::croppedSampleProjections);
        outputBase = gameAt(fp::sig::croppedSampleViewProjections);
    }
    if (displacement >= supersampleSlotCount * supersampleMatrixBytes ||
        displacement % supersampleMatrixBytes != 0 || (uintptr_t)output != outputBase + displacement ||
        (uintptr_t)view != gameAt(fp::sig::sampleViews) + displacement)
        return false;
    slot = unsigned(displacement / supersampleMatrixBytes);
    return true;
}

extern "C" bool excameraPrepareSampleProjection(float* output, const float* view,
                                                 const float* projection, float* localCopy) {
    unsigned slot = 0;
    // The shim enters preparation for every native multiplication. Unrelated
    // argument triples only compare resolved array addresses before forwarding;
    // they do not read matrix contents, access TLS or acquire cameraLock here.
    if (!supersampleMatrixSlot(output, view, projection, slot) || !localCopy ||
        !supersampleRenderScope.owned)
        return false;
    bool changed = false;
    __try {
        const int last = *(int*)gameAt(fp::sig::sampleLast);
        if (*(int*)gameAt(fp::sig::sampleCount) > 1 && last >= 0 &&
            last < int(supersampleSlotCount) && slot <= unsigned(last) && supersampleViewMatches()) {
            const SupersampleRenderScope& scope = supersampleRenderScope;
            const SupersampleTerrainScope& terrain = supersampleTerrainScope;
            changed = fp::correctSampleDepth(projection, scope.stockNear, scope.farPlane,
                                             terrain.active, terrain.entryP10, terrain.distanceRatio,
                                             localCopy);
            if (changed) {
                supersampleCorrections.fetch_add(1, std::memory_order_relaxed);
                if (terrain.active) supersampleTerrainCorrections.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        supersampleFaults.fetch_add(1, std::memory_order_relaxed);
        changed = false;
    }
    return changed;
}

[[maybe_unused]] static void multiplyMatrix(float* output, const float* view, const float* projection) {
    alignas(16) float localCopy[16];
    const bool changed = excameraPrepareSampleProjection(output, view, projection, localCopy);
    originalMultiplyMatrix(output, view, changed ? localCopy : projection);
}
