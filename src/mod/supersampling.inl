// Native sampled projections keep their XY jitter and depth coefficient. Only
// authored depth offsets are adapted to the camera that owns this render call.
using RenderViewport = void (*)(uintptr_t);
using TerrainPass = uint64_t (*)(uintptr_t, uintptr_t);
using MultiplyMatrix = void (*)(float*, const float*, const float*);

static RenderViewport originalRenderViewport;
static TerrainPass originalTerrainPass;
extern "C" {
MultiplyMatrix excameraOriginalMultiplyMatrix = nullptr;
void excameraMultiplyMatrixHook(float*, const float*, const float*);
}
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

inline bool admitSupersampleRender(uintptr_t viewport, SupersampleRenderScope& scope) {
    if (!viewport || !TryAcquireSRWLockShared(&cameraLock)) return false;
    bool admitted = false;
    __try {
        uintptr_t view = 0, camera = 0;
        currentCamera(view, camera);
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
                std::memcpy(scope.stockOrigin, stockPose + 9, sizeof scope.stockOrigin);
                scope.owned = true;
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

inline void renderViewportProfileBody(uintptr_t viewport) {
    const SupersampleRenderScope previousRender = supersampleRenderScope;
    const SupersampleTerrainScope previousTerrain = supersampleTerrainScope;
    SupersampleRenderScope scope;
    const bool admitted = admitSupersampleRender(viewport, scope);
    supersampleRenderScope = scope;
    supersampleTerrainScope = {};
    if (admitted) supersampleScopes.fetch_add(1, std::memory_order_relaxed);
    __try {
        fp::profile::invoke<fp::profile::Hook::viewportNative>(originalRenderViewport, viewport);
    } __finally {
        supersampleTerrainScope = previousTerrain;
        supersampleRenderScope = previousRender;
        if (admitted) InterlockedDecrement(&renderScopesInFlight);
    }
}

inline void renderViewport(uintptr_t viewport) {
    return fp::profile::invoke<fp::profile::Hook::viewport>(renderViewportProfileBody, viewport);
}

inline bool supersampleViewMatches() {
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

static bool hideCoversFor(uintptr_t level) {
    if (!level || !coversHiddenInFirstPerson.load(std::memory_order_relaxed) ||
        !supersampleRenderScope.owned)
        return false;
    __try {
        return supersampleViewMatches() && levelGrid() == level;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void supersampleCurrentStockOrigin(float origin[3]) {
    const SupersampleRenderScope& scope = supersampleRenderScope;
    std::memcpy(origin, scope.stockOrigin, sizeof scope.stockOrigin);
    if (!TryAcquireSRWLockShared(&cameraLock)) return;
    __try {
        if (InterlockedCompareExchange(&pendingRestore, 0, 0) && !park.active &&
            !InterlockedCompareExchange(&restorationUnknown, 0, 0) &&
            savedView == scope.view && savedCamera == scope.camera &&
            savedController == scope.controller && savedRoot == scope.root &&
            savedScene == scope.scene) {
            bool finite = true;
            for (unsigned k = 0; k < 3; ++k) finite = finite && std::isfinite(stockPose[9 + k]);
            if (finite) std::memcpy(origin, stockPose + 9, sizeof scope.stockOrigin);
        }
    } __finally {
        ReleaseSRWLockShared(&cameraLock);
    }
}

inline SupersampleTerrainScope captureSupersampleTerrain() {
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

                const double closeDeltaX = double(cameraOrigin[0]) - double(targetOrigin[0]);
                const double closeDeltaY = double(cameraOrigin[1]) - double(targetOrigin[1]);
                const double closeDeltaZ = double(cameraOrigin[2]) - double(targetOrigin[2]);
                const double stockDeltaX = double(currentStockOrigin[0]) - double(targetOrigin[0]);
                const double stockDeltaY = double(currentStockOrigin[1]) - double(targetOrigin[1]);
                const double stockDeltaZ = double(currentStockOrigin[2]) - double(targetOrigin[2]);

                const double closeSquared = closeDeltaX * closeDeltaX + closeDeltaY * closeDeltaY + closeDeltaZ * closeDeltaZ;
                const double stockSquared = stockDeltaX * stockDeltaX + stockDeltaY * stockDeltaY + stockDeltaZ * stockDeltaZ;

                const float entryP10 = ((const float*)gameAt(fp::sig::sampleProjections))[10];
                if (std::isfinite(closeSquared) && closeSquared > 0.0 && std::isfinite(stockSquared) &&
                    stockSquared > 0.0 && std::isfinite(entryP10)) {
                    const double ratio = closeSquared / stockSquared;
                    if (std::isfinite(ratio) && ratio > 0.0 &&
                        ratio <= double((std::numeric_limits<float>::max)()) && float(ratio) > 0.f) {
                        terrain.active = true;
                        terrain.entryP10 = entryP10;
                        terrain.distanceRatio = static_cast<float>(ratio);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        supersampleFaults.fetch_add(1, std::memory_order_relaxed);
    }
    return terrain;
}

inline uint64_t terrainPassProfileBody(uintptr_t manager, uintptr_t renderer) {
    const SupersampleTerrainScope previous = supersampleTerrainScope;
    supersampleTerrainScope = captureSupersampleTerrain();
    uint64_t result = 0;
    __try {
        result = fp::profile::invoke<fp::profile::Hook::terrainNative>(originalTerrainPass, manager, renderer);
    } __finally {
        supersampleTerrainScope = previous;
    }
    return result;
}

inline uint64_t terrainPass(uintptr_t manager, uintptr_t renderer) {
    return fp::profile::invoke<fp::profile::Hook::terrain>(terrainPassProfileBody, manager, renderer);
}

inline bool supersampleMatrixSlot(float* output, const float* view, const float* projection,
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
    slot = static_cast<unsigned>(displacement / supersampleMatrixBytes);
    return true;
}

inline bool excameraPrepareSampleProjectionProfileBody(float* output, const float* view,
                                                 const float* projection, float* localCopy) {
    unsigned slot = 0;
    if (!supersampleMatrixSlot(output, view, projection, slot) || !localCopy ||
        !supersampleRenderScope.owned)
        return false;
    bool changed = false;
    __try {
        const int last = *(int*)gameAt(fp::sig::sampleLast);
        if (*(int*)gameAt(fp::sig::sampleCount) > 1 && last >= 0 &&
            last < static_cast<int>(supersampleSlotCount) && slot <= static_cast<unsigned>(last) && supersampleViewMatches()) {
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

extern "C" bool excameraPrepareSampleProjection(float* output, const float* view,
                                                 const float* projection, float* localCopy) {
    return fp::profile::invoke<fp::profile::Hook::projection>(excameraPrepareSampleProjectionProfileBody, output, view, projection, localCopy);
}

[[maybe_unused]] inline void multiplyMatrix(float* output, const float* view, const float* projection) {
    alignas(16) float localCopy[16];
    const bool changed = excameraPrepareSampleProjection(output, view, projection, localCopy);
    originalMultiplyMatrix(output, view, changed ? localCopy : projection);
}
