// Native target-provider scope. The camera remains the published physical view;
// only native screen coordinates and sector-to-world projection operands are borrowed.
using PlaneHit = void (*)(float*, uintptr_t, int, int, const float*);
static PlaneHit originalPlaneHit;
using AimWorldToLocal = void (*)(float*, const float*, const float*);
static AimWorldToLocal originalAimWorldToLocal;
// Native picks take the viewport, not its rendering view or camera. Camera
// owners call this under cameraLock before borrowing any native pick operation.
static uintptr_t pickViewportForCamera(uintptr_t camera) {
    const uintptr_t viewport = *(uintptr_t*)gameAt(fp::sig::mainViewport);
    if (!camera || !readable(viewport, field::viewport::pickCamera + sizeof(uintptr_t)) ||
        *(uintptr_t*)(viewport + field::viewport::pickCamera) != camera)
        return 0;
    const uintptr_t view = *(uintptr_t*)(viewport + field::viewport::view);
    return readable(view, field::view::camera + sizeof(uintptr_t)) &&
                   *(uintptr_t*)(view + field::view::camera) == camera
               ? viewport
               : 0;
}
static bool sceneNodeWorldOrigin(uintptr_t node, float* point);
// Read-only manipulation resistance, sampled before input takes aimLock.
// No temporal cache: release, a new grab or a view handoff cannot retain drag.
static float heldAimGain(uintptr_t root, float width, float height) {
    if (!root || width <= 40.f || height <= 40.f || !closeViewContext() ||
        !TryAcquireSRWLockShared(&cameraLock)) return 1.f;
    float gain = 1.f;
    __try {
        const uintptr_t viewport = pickViewportForCamera(savedCamera);
        const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
        if (viewport && savedRoot == root && savedScene == *(uintptr_t*)gameAt(fp::sig::currentScene) &&
            *(uintptr_t*)(viewport + field::viewport::view) == savedView && !park.active &&
            readable(drag, field::drag::viewport + sizeof(uintptr_t)) &&
            *(uintptr_t*)(drag + field::drag::viewport) == viewport &&
            (*(unsigned*)(drag + field::drag::flags) & (0x2000 | 0x4000 | 0x10000 | 0x20000))) {
            const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
            float world[3];
            if (object && sceneNodeWorldOrigin(object, world)) {
                float delta[3];
                for (int i = 0; i < 3; ++i) delta[i] = world[i] - lastCamera[9 + i];
                float x = 0, y = 0;
                if (fp::projectCursor(delta, lastCamera, width, height, renderedFov, x, y)) {
                    const float nx = (x - width * .5f) / (width * .5f);
                    const float ny = (y - height * .5f) / (height * .5f);
                    const float radius = std::clamp(std::sqrt(nx * nx + ny * ny), 0.f, 1.f);
                    const float resistance = radius * radius * (3.f - 2.f * radius);
                    gain = 1.f - .8f * resistance;
                } else {
                    // A finite object behind the achieved camera remains resistant.
                    const float back = delta[0] * lastCamera[6] + delta[1] * lastCamera[7] +
                                       delta[2] * lastCamera[8];
                    if (std::isfinite(back) && back >= -.01f) gain = .2f;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gain = 1.f;
    }
    ReleaseSRWLockShared(&cameraLock);
    return gain;
}
// Native target selection and the drawn reticle share one screen projection.
// Smoothing, visibility and interaction clearance belong only to presentation.
static bool aimCursorPosition(const Aim& a, int mode, const float* pose, float width, float height, float fov,
                              float& x, float& y) {
    if (mode == 1 && !trackingPresented) {
        const bool free = tpAimFree.load();
        x = width * .5f + (free ? tpAimX.load() : 0.f);
        y = height * .5f + (free ? tpAimY.load() : 0.f);
        return std::isfinite(x) && std::isfinite(y);
    }
    if (mode != 0 && mode != 1) return false;
    float direction[3];
    for (int i = 0; i < 3; ++i)
        direction[i] = a.point[i] - a.origin[i] +
                       (mode == 1 ? eyeGlobal[i].load() - pose[9 + i] : 0.f);
    return fp::projectCursor(direction, pose, width, height, fov, x, y) && std::isfinite(x) &&
           std::isfinite(y);
}
struct NativeAimScope {
    uintptr_t behavior = 0, root = 0, head = 0, scene = 0, sector = 0, viewport = 0, camera = 0;
    uintptr_t opponent = 0, opponentSector = 0;
    fp::InputStamp stamp{};
    unsigned revision = 0;
    float* output = nullptr;
    float sectorOrigin[3]{}, opponentOrigin[3]{}, rayOrigin[3]{}, rayDirection[3]{};
    float result[3]{}, fallbackDistance = 1000.f;
    int x = 0, y = 0;
    bool planeSeen = false, selected = false;
};
static thread_local NativeAimScope* nativeAimScope = nullptr;
static thread_local unsigned nativeAimCameraDepth = 0;
static bool finiteAimVector(const float* p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
static uintptr_t aimSector(uintptr_t root) {
    const auto mesh = readable(root, field::character::sceneNode + 8)
                          ? *(uintptr_t*)(root + field::character::sceneNode)
                          : 0;
    return readable(mesh, field::sceneNode::sector + 8) ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
}
static bool nativeAimEligible(uintptr_t root, uintptr_t behavior) {
    // Aim continues during accepted attacks and native movement/recovery. Guard
    // initiation's pressBlocked/action-state restrictions do not belong here.
    if (!gameplayOwnsPointer() || !controls(behavior) ||
        *(uintptr_t*)(root + field::character::behaviour) != behavior ||
        !*(unsigned char*)(root + field::character::combatStance) ||
        *(unsigned char*)(behavior + field::behaviour::interactionLatch) ||
        *(int*)gameAt(fp::sig::pointerOverride) > 0 || park.active ||
        InterlockedCompareExchange(&restorationUnknown, 0, 0))
        return false;
    const auto drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    const auto gui = *(uintptr_t*)gameAt(fp::sig::guiManager);
    return !(drag && *(unsigned*)(drag + field::drag::flags)) &&
           !(gui && *(unsigned*)(gui + field::gui::dragFormat));
}
static bool nativeAimCurrent(const NativeAimScope& s) {
    Aim a;
    return getAim(s.behavior, a) && nativeAimEligible(s.root, s.behavior) && a.revision == s.revision &&
           a.root == s.root && a.head == s.head && s.stamp.mode == viewMode.load() &&
           s.stamp.generation == inputOwner.generation() &&
           s.stamp.policyRevision == inputOwner.policyRevision() &&
           s.scene == *(uintptr_t*)gameAt(fp::sig::currentScene) &&
           s.viewport == *(uintptr_t*)(s.behavior + field::behaviour::view) &&
           s.viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport) &&
           s.camera == savedCamera &&
           *(uintptr_t*)(s.viewport + field::viewport::view) == savedView &&
           pickViewportForCamera(s.camera) == s.viewport && s.root == savedRoot && s.scene == savedScene &&
           aimSector(s.root) == s.sector &&
           !memcmp(s.sectorOrigin, (void*)(s.sector + field::sector::origin), 12) &&
           *(uintptr_t*)(s.root + field::character::aimOpponent) == s.opponent &&
           (!s.opponent ||
            (aimSector(s.opponent) == s.opponentSector &&
             !memcmp(s.opponentOrigin, (void*)(s.opponentSector + field::sector::origin), 12)));
}
static void nativePointUnscoped(uintptr_t behavior, float* out) {
    auto previous = nativeAimScope;
    nativeAimScope = nullptr;
    __try {
        originalPoint(behavior, out);
    } __finally {
        nativeAimScope = previous;
    }
}
static void aimWorldToLocalFor(uintptr_t caller, float* out, const float* in, const float* pose) {
    auto* s = nativeAimScope;
    const bool targetProjection = caller == gameAt(fp::sig::aimRootProjectReturn) ||
                                  caller == gameAt(fp::sig::aimHeadProjectReturn) ||
                                  caller == gameAt(fp::sig::aimPrimaryProjectReturn) ||
                                  caller == gameAt(fp::sig::aimSecondaryProjectReturn);
    float world[3];
    if (s && targetProjection && s->opponent && nativeAimCurrent(*s) &&
        pose == (float*)(s->camera + field::camera::pose)) {
        for (int i = 0; i < 3; ++i) world[i] = in[i] + s->opponentOrigin[i];
        if (finiteAimVector(world)) in = world;
    }
    originalAimWorldToLocal(out, in, pose);
}
static void aimWorldToLocal(float* out, const float* in, const float* pose) {
    aimWorldToLocalFor((uintptr_t)_ReturnAddress(), out, in, pose);
}
static bool nativeAimPlane(uintptr_t caller, float* out, uintptr_t viewport, int x, int y,
                           const float* plane) {
    auto* s = nativeAimScope;
    if (!s || caller != gameAt(fp::sig::aimPlaneReturn) || viewport != s->viewport) return false;
    // Fail closed to the finite current ray if a native callback changed ownership.
    // This exact provider call must never fall back to the horizontal divide.
    float distance = s->fallbackDistance;
    bool selected = false;
    if (nativeAimCurrent(*s) && x == s->x && y == s->y) {
        const uintptr_t frame = (uintptr_t)plane + field::aimFrame::planeBack;
        if (readable(frame - field::aimFrame::unassistedBack, field::aimFrame::unassistedBack) &&
            *(uintptr_t*)(frame - field::aimFrame::behaviorBack) == s->behavior &&
            *(float**)(frame - field::aimFrame::outputBack) == s->output &&
            !*(unsigned char*)(frame - field::aimFrame::unassistedBack)) {
            const float radius = *(float*)(frame - field::aimFrame::radiusBack);
            const float d2 = *(float*)(frame - field::aimFrame::distanceBack);
            const float* anchor = (const float*)(frame - field::aimFrame::selectedBack);
            double depth = 0;
            for (int i = 0; i < 3; ++i) depth += (double(anchor[i]) - s->rayOrigin[i]) * s->rayDirection[i];
            if (finiteAimVector(anchor) && std::isfinite(radius) && radius > 0 && std::isfinite(d2) &&
                d2 >= 0 && std::isfinite(depth) && depth > 0) {
                // Same inner/outer radii and linear blend as AE2E0, applied to
                // depth along the real view ray instead of an overhead height.
                const float weight = std::clamp(2.f - std::sqrt(d2) / radius, 0.f, 1.f);
                const double blended = double(s->fallbackDistance) * (1.0 - weight) + depth * weight;
                if (blended > 0 && blended < 1e7) {
                    distance = float(blended);
                    selected = true;
                }
            }
        }
    }
    float adapted[4] = {s->rayDirection[0], s->rayDirection[1], s->rayDirection[2], 0};
    for (int i = 0; i < 3; ++i) {
        s->result[i] = s->rayOrigin[i] + s->rayDirection[i] * distance;
        adapted[3] += adapted[i] * s->result[i];
    }
    // Keep native unprojection, intersection arithmetic and distance publication.
    originalPlaneHit(out, viewport, s->x, s->y, adapted);
    if (!finiteAimVector(out))
        memcpy(out, s->result, 12);
    else
        memcpy(s->result, out, 12);
    s->selected = selected;
    s->planeSeen = true;
    return true;
}
static void nativeAimPick(uintptr_t caller, uintptr_t viewport, char hit, uintptr_t out) {
    auto* s = nativeAimScope;
    if (!s || caller != gameAt(fp::sig::aimPickReturn) || viewport != s->viewport || !s->planeSeen ||
        s->selected || !hit || !nativeAimCurrent(*s))
        return;
    const uintptr_t mesh = *(uintptr_t*)out;
    if (!mesh || ((uintptr_t (*)(uintptr_t))gameAt(fp::sig::aimMeshRoot))(mesh) ==
                     *(uintptr_t*)(s->root + field::character::sceneNode))
        return;
    const float* p = (const float*)(out + 8);
    const float t = *(float*)(out + field::pick::distance);
    double forward = 0;
    for (int i = 0; i < 3; ++i) forward += (double(p[i]) - s->rayOrigin[i]) * s->rayDirection[i];
    if (!finiteAimVector(p) || !std::isfinite(t) || t < 0 || !std::isfinite(forward) || forward < 0) return;
    memcpy(s->result, p, 12);
}
static bool nativeAimPointLocked(uintptr_t behavior, float* out, const Aim& a) {
    // A foreign point-slot predecessor may have a different call/frame contract.
    // Keep the existing close-view fallback instead of adapting its internals.
    if (originalPoint != (Point)gameAt(fp::sig::aimPoint)) return false;
    NativeAimScope s;
    s.behavior = behavior;
    s.root = a.root;
    s.head = a.head;
    s.revision = a.revision;
    s.scene = a.attentionStamp.scene;
    s.stamp = a.attentionStamp;
    s.output = out;
    if (!a.revision || s.stamp.root != a.root || s.stamp.head != a.head || !s.scene ||
        !nativeAimEligible(s.root, behavior) || !savedCamera)
        return false;
    s.viewport = *(uintptr_t*)(behavior + field::behaviour::view);
    if (!readable(s.viewport, field::viewport::pickCamera + 8)) return false;
    s.camera = *(uintptr_t*)(s.viewport + field::viewport::pickCamera);
    s.sector = aimSector(s.root);
    if (!readable(s.sector, field::sector::origin + 12)) return false;
    memcpy(s.sectorOrigin, (void*)(s.sector + field::sector::origin), 12);
    s.opponent = *(uintptr_t*)(s.root + field::character::aimOpponent);
    if (s.opponent) {
        s.opponentSector = aimSector(s.opponent);
        if (!readable(s.opponentSector, field::sector::origin + 12)) return false;
        memcpy(s.opponentOrigin, (void*)(s.opponentSector + field::sector::origin), 12);
    }
    if (!finiteAimVector(s.sectorOrigin) || !finiteAimVector(s.opponentOrigin) || !nativeAimCurrent(s))
        return false;
    const int width = *(int*)(s.viewport + field::viewport::width);
    const int height = *(int*)(s.viewport + field::viewport::height);
    if (width < 40 || height < 40) return false;
    float px = 0, py = 0;
    if (!aimCursorPosition(a, s.stamp.mode, (float*)(s.camera + field::camera::pose), float(width),
                           float(height), renderedFov, px, py))
        return false;
    if (px < 0 || py < 0 || px >= width || py >= height)
        return false;
    s.x = std::clamp(int(std::lround(px)), 0, width - 1);
    s.y = std::clamp(int(std::lround(py)), 0, height - 1);
    float ray[6]{};
    ((void (*)(uintptr_t, float, float, float*))gameAt(fp::sig::aimScreenRay))(s.viewport, float(s.x),
                                                                               float(s.y), ray);
    const float* pose = (const float*)(s.camera + field::camera::pose);
    double length2 = 0;
    for (int i = 0; i < 3; ++i) {
        s.rayOrigin[i] = pose[9 + i] + ray[0] * pose[i] + ray[1] * pose[3 + i] + ray[2] * pose[6 + i];
        s.rayDirection[i] = ray[3] * pose[i] + ray[4] * pose[3 + i] + ray[5] * pose[6 + i];
        length2 += double(s.rayDirection[i]) * s.rayDirection[i];
    }
    if (!finiteAimVector(s.rayOrigin) || !std::isfinite(length2) || length2 < 1e-12) return false;
    const float length = float(std::sqrt(length2));
    for (float& v : s.rayDirection) v /= length;
    double eyeDepth = 0;
    for (int i = 0; i < 3; ++i)
        eyeDepth += (double(a.origin[i]) + s.sectorOrigin[i] - s.rayOrigin[i]) * s.rayDirection[i];
    if (!std::isfinite(eyeDepth) || std::fabs(eyeDepth) > 1e6) return false;
    s.fallbackDistance = float(std::fmax(0.0, eyeDepth) + 1000.0);
    if (!nativeAimCurrent(s)) return false;
    auto previous = nativeAimScope;
    int* cursorX = (int*)gameAt(fp::sig::aimCursorX);
    int* cursorY = (int*)gameAt(fp::sig::aimCursorY);
    auto* pickFlag = (unsigned char*)gameAt(fp::sig::aimPickActive);
    const int oldX = *cursorX, oldY = *cursorY;
    const unsigned char oldPickFlag = *pickFlag;
    nativeAimScope = &s;
    *cursorX = s.x;
    *cursorY = s.y;
    bool accepted = false;
    __try {
        originalPoint(behavior, out);
        if (s.planeSeen && nativeAimCurrent(s)) {
            float local[3];
            for (int i = 0; i < 3; ++i) local[i] = s.result[i] - s.sectorOrigin[i];
            if (finiteAimVector(local)) {
                memcpy(out, local, 12);
                memcpy((void*)(behavior + field::behaviour::aimPoint), local, 12);
                accepted = true;
            }
        }
    } __finally {
        if (*cursorX == s.x) *cursorX = oldX;
        if (*cursorY == s.y) *cursorY = oldY;
        if (*pickFlag <= 1) *pickFlag = oldPickFlag;
        nativeAimScope = previous;
    }
    return accepted;
}
static bool nativeAimPoint(uintptr_t behavior, float* out, const Aim& a) {
    // A nested point call shares its outer scope's camera lock. An unrelated
    // camera owner never waits on us or observes a temporary camera pose.
    const bool acquire = nativeAimCameraDepth == 0;
    if (acquire && !TryAcquireSRWLockShared(&cameraLock)) return false;
    ++nativeAimCameraDepth;
    bool result = false;
    __try {
        result = nativeAimPointLocked(behavior, out, a);
    } __finally {
        --nativeAimCameraDepth;
        if (acquire) ReleaseSRWLockShared(&cameraLock);
    }
    return result;
}

// Picking and holding: the game's pick rays past your own body, held-object distance and drop spot, grab reach, doors and lids, the crosshair position.
// Part of first_person.cpp (one translation unit): included there, in order.
// The held object (a scene node; skipped by its own placement pick), 0 when nothing is held.
static uintptr_t heldNode() {
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    const uintptr_t object = readable(drag, 0x20) ? *(uintptr_t*)(drag + field::drag::object) : 0;
    return readable(object, 0x2a0) ? object : 0;
}
static char heldPick(uintptr_t object, uintptr_t viewport, float x, float y, uintptr_t out,
                     bool gameplay = false) {
    const auto sampledGeneration = holdGeneration.load();
    const int mode = viewMode.load();
    const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    // Native unprojection leaves out untouched off the viewport. Clear it so
    // a blocked reticle cannot reuse a previous valid direction or hit.
    memset((void*)out, 0, 0x34);
    const char hit = pickPastSelf(ownedPickRay, viewport, x, y, out, object);
    if (holdGeneration.load() != sampledGeneration || viewMode.load() != mode ||
        root != *(uintptr_t*)gameAt(fp::sig::playerRoot) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0) ||
        (gameplay && gameplayHeldObject(viewport) != object))
        return 0;
    const float* dir = (const float*)(out + field::pick::direction);
    const float* origin = (const float*)(out + field::pick::origin);
    const float length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!std::isfinite(length) || length < 0.5f || !std::isfinite(origin[0] + origin[1] + origin[2]))
        return 0;
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    if (!readable(drag, 0x20) || *(uintptr_t*)(drag + field::drag::object) != object)
        return 0; // released/replaced between the placement and this pick
    const ULONGLONG now = GetTickCount64();
    if (holdObject.load() != object) { // a new hold: at the distance it was grabbed at, from the eye
        const float grabbed = grabStartDistance.load();
        const float start = std::fmin(std::fmax(grabbed > 0.f ? grabbed : fp::holdNear * 2, fp::holdNear),
                                      float(grabRange.load()));
        holdObject = object;
        holdTarget = start;
        holdApplied = start;
        holdTick = now;
    }
    const float dt = holdTick ? float(now - holdTick) * 0.001f : 0.f;
    holdTick = now;
    holdApplied = fp::holdApproach(holdApplied.load(), holdTarget.load(), dt);
    {
        static fp::PushRate push;
        static uintptr_t pushed = 0; // how fast the wheel is pushing it out, for a fling
        static ULONGLONG pushedGeneration = 0;
        const ULONGLONG generation = holdGeneration.load();
        if (pushed != object || pushedGeneration != generation) {
            push = fp::PushRate{};
            pushed = object;
            pushedGeneration = generation;
        }
        push.step(holdApplied.load(), now);
        pushSpeed = push.pushing(now);
        pushAt = now;
        for (int i = 0; i < 3; ++i) pushDir[i] = dir[i];
    }
    if (!heldPointAlongView(hit, out)) return 0;
    ++holdPlacements;
    return 1;
}
static bool prepareHeldPlacement(uintptr_t object, uintptr_t viewport, float height, unsigned char* ray) {
    const int mode = viewMode.load();
    const auto generation = holdGeneration.load();
    const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    LONG x = 0, y = 0;
    if (!reticleForUi(&x, &y)) {
        const uintptr_t gui = *(uintptr_t*)gameAt(fp::sig::guiManager);
        if (!readable(gui, field::gui::pointerX + 2 * sizeof(LONG))) return false;
        x = *(LONG*)(gui + field::gui::pointerX);
        y = ((LONG*)(gui + field::gui::pointerX))[1];
    }
    if (!heldPick(object, viewport, float(x), float(y), (uintptr_t)ray)) return false;
    // Native 1183C0 must take its hit branch, never its ray/height fallback.
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    return viewMode.load() == mode && holdGeneration.load() == generation &&
           InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
           InterlockedCompareExchange(&cameraReady, 0, 0) &&
           root == *(uintptr_t*)gameAt(fp::sig::playerRoot) &&
           viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport) && readable(drag, 0x20) &&
           *(uintptr_t*)(drag + field::drag::object) == object && std::isfinite(height) &&
           ((float*)(ray + 8))[1] >= height;
}
static char pickRayFor(uintptr_t caller, uintptr_t viewport, float x, float y, uintptr_t out) {
    if (out && placingThread == GetCurrentThreadId() && placingReady && viewport == placingViewport &&
        caller == gameAt(fp::sig::placementPickReturn)) {
        memcpy((void*)out, placingRay, sizeof placingRay);
        return 1;
    }
    const char hit = pickPastSelf(originalPickRay, viewport, x, y, out);
    nativeAimPick(caller, viewport, hit, out);
    return hit;
}
static uintptr_t heldQuery(uintptr_t object, uintptr_t viewport, uintptr_t out, float height) {
    pendingPlacement = {};
    if (!out || !object || closeWorldHeldObject(viewport) != object)
        return originalHeldQuery(object, viewport, out, height);
    // UI/focus handoff pauses this world hold; it must not reactivate the
    // overhead waist-plane/slot query while the close-view camera remains live.
    if (gameplayHeldObject(viewport) != object) return 0;
    LONG x = 0, y = 0;
    if (!reticleForUi(&x, &y)) {
        // The configured free-cursor hold uses the game's current pointer.
        // A stale gameplay reticle is otherwise a rejected query.
        const uintptr_t gui = *(uintptr_t*)gameAt(fp::sig::guiManager);
        if (!InterlockedCompareExchange(&rmbFree, 0, 0) ||
            !readable(gui, field::gui::pointerX + 2 * sizeof(LONG)))
            return 0;
        x = *(LONG*)(gui + field::gui::pointerX);
        y = ((LONG*)(gui + field::gui::pointerX))[1];
    }
    alignas(8) unsigned char ray[0x40]{};
    if (!heldPick(object, viewport, float(x), float(y), (uintptr_t)ray, true) ||
        gameplayHeldObject(viewport) != object)
        return 0;
    // Ordinary close-view holding has no downward admission, waist-plane
    // projection or overhead slot snap. Keep the real ray for native 118000's
    // collision sweep. 11F610 still accepts/rejects and records the pose;
    // native release consumes that accepted history.
    memcpy((void*)out, ray + 8, 12);
    memcpy((void*)(out + 12), ray + field::pick::direction, 12);
    pendingPlacement.object = object;
    pendingPlacement.viewport = viewport;
    pendingPlacement.query = out;
    pendingPlacement.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    pendingPlacement.generation = holdGeneration.load();
    pendingPlacement.mode = viewMode.load();
    pendingPlacement.distance = *(float*)(ray + field::pick::distance);
    memcpy(pendingPlacement.values, (void*)out, sizeof pendingPlacement.values);
    ++dropSpots;
    return 1;
}
static char pickRay(uintptr_t viewport, float x, float y, uintptr_t out) {
    return pickRayFor((uintptr_t)_ReturnAddress(), viewport, x, y, out);
}
// Grab reach. 0x11A690 starts every grab (items 0x2000, doors 0x4000/0x10000,
// free constraints 0x20000, locked-door rattle 0x80000, ...) from the pick
// 0x8FB30(viewport, x, y, out, filter) returning to 0x11A79E; the
// hover check 0x11E6F0 (cursor "can use" state) uses the same pick, returning to 0x11E76C. out
// has 0x8F720's layout (+0x2C distance along the unit ray from the eye). In first person a hit
// beyond the grab range (F8) counts as nothing hit at those two sites: the click does nothing (no grab,
// no warp) and the cursor does not show the object as usable. The top-down view is untouched.
using GrabPick = char (*)(uintptr_t, float, float, uintptr_t, void*);
static GrabPick originalGrabPick; // RVA 0x8FB30
// Grab reach (17c) by return address: 0x11A79E grab start, 0x11E76C hover check. Every grab
// start also forgets the last hold, so a re-grab of the same item starts at its native distance.
static char grabPickResult(uintptr_t caller, char hit, uintptr_t out) {
    if (caller == gameAt(fp::sig::grabStartPickReturn)) {
        resetHeldGrab();
        doorDistance = hit && out ? *(float*)(out + field::pick::distance) : 0.f;
        doorCameraOffset = 0;
        lastDoorObject = 0;
        doorGrabPointValid = false;
        if (hit && out) {
            const float* d = (const float*)(out + field::pick::direction);
            const float* o = (const float*)(out + field::pick::origin);
            const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
            grabStartDistance = fp::eyeDistance(o, d, *(float*)(out + field::pick::distance), eye);
            doorGrabPointValid = true;
            for (int i = 0; i < 3; ++i) {
                doorGrabPoint[i] = o[i] + d[i] * doorDistance.load();
                if (!std::isfinite(doorGrabPoint[i])) doorGrabPointValid = false;
            }
        }
    }
    if (!out || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0))
        return hit;
    float reachDistance = *(float*)(out + field::pick::distance);
    if (viewMode.load() == 1 &&
        hit) { // the pick ray starts at the third-person camera; reach is measured from the eye
        const float* d = (const float*)(out + field::pick::direction);
        const float* o = (const float*)(out + field::pick::origin);
        float head[3];
        readReachHead(head);
        float s = 0;
        for (int i = 0; i < 3; ++i) {
            const float p = o[i] + d[i] * reachDistance - head[i];
            s += p * p;
        }
        if (std::isfinite(s)) reachDistance = std::sqrt(s);
        if (caller == gameAt(fp::sig::grabStartPickReturn))
            doorCameraOffset = *(float*)(out + field::pick::distance) - reachDistance;
    }
    const bool tooFar = hit && fp::grabTooFar(reachDistance, float(grabRange.load()));
    if (caller == gameAt(fp::sig::hoverPickReturn)) {
        hoverOutOfRange = tooFar;
        hoverRangeAt = GetTickCount64();
    }
    if (hit &&
        (caller == gameAt(fp::sig::grabStartPickReturn) || caller == gameAt(fp::sig::hoverPickReturn) ||
         caller == gameAt(fp::sig::doubleClickPickReturn)) &&
        tooFar) {
        if (caller == gameAt(fp::sig::grabStartPickReturn)) ++grabsRefused;
        return 0;
    }
    return hit;
}
static char grabPickFor(uintptr_t caller, uintptr_t viewport, float x, float y, uintptr_t out, void* filter) {
    // 8FB30 is shared by viewport widgets. Reticle, range and grab history
    // belong only to the main gameplay viewport.
    if (!viewport || viewport != *(uintptr_t*)gameAt(fp::sig::mainViewport))
        return originalGrabPick(viewport, x, y, out, filter);
    if (caller == gameAt(fp::sig::grabStartPickReturn) || caller == gameAt(fp::sig::hoverPickReturn) ||
        caller == gameAt(fp::sig::doubleClickPickReturn)) {
        LONG rx = 0, ry = 0;
        if (reticleForUi(&rx, &ry)) {
            x = float(rx);
            y = float(ry);
        }
    }
    char hit = 0;
    if (!selfPickWanted() || selfPickSet)
        hit = originalGrabPick(viewport, x, y, out, filter);
    else
        __try {
            selfPickMask();
            hit = originalGrabPick(viewport, x, y, out, filter);
        } __finally {
            selfPickRestore();
        } // as pickPastSelf: never your own body
    return grabPickResult(caller, hit, out);
}
static char grabPick(uintptr_t viewport, float x, float y, uintptr_t out, void* filter) {
    return grabPickFor((uintptr_t)_ReturnAddress(), viewport, x, y, out, filter);
}
// The game's cursor warp 0x22780 (client point -> SetCursorPos). Picking up an object
// (0x11A690 -> 0x116A50, return 0x116AAC) warps the cursor onto the object; first person reads
// the cursor's offset from centre as look, so the warp became a view snap (live:
// pitch -0.67 -> -1.15 on grabbing a stone block). While first person owns the pointer that one
// warp is skipped; the reticle stays the pointer.
using CursorWarp = void (*)(int, int);
static CursorWarp originalCursorWarp; // RVA 0x22780
static void cursorWarp(int x, int y) {
    if ((uintptr_t)_ReturnAddress() == gameAt(fp::sig::pickupWarpReturn) && gameplayOwnsPointer()) {
        ++warpsSkipped;
        return;
    }
    originalCursorWarp(x, y);
}
// ---- Doors, lids, levers and gates held in place ----
// A door (drag 0x4000) or a hinged thing (0x10000) follows a point held where it was grabbed: on the
// pointer's ray at the grab's distance (the wheel moves it nearer or farther), put on the plane the game
// uses for that drag. The game's handlers do the rest as before: a door's spring pulls toward that point
// (normalised to 100 from its pivot), a lid's angle stays clamped, both pause beyond the game's reach
// (350 / 300 from the body) and end on release. They normally run only when the pointer moves, so with
// mouse-look they also run once a frame. The intersection 0x8F530 is replaced only at the four calls the
// two handlers make; the game's own has no guard for a ray along the plane.
using DragHandler = unsigned long long (*)(uintptr_t, uintptr_t, int);
static DragHandler originalConstraintHandler; // RVA 0x11BA30, native 0x20000 physical constraint
static DragHandler originalDoorHandler,
    originalHingeHandler; // RVA 0x11B400, 0x11BC50 (message 0: pointer moved, 0x80: let go)
static unsigned long long constrainedHandler(DragHandler native, uintptr_t drag, uintptr_t ui, int message);
static unsigned long long doorHandler(uintptr_t drag, uintptr_t ui, int message) {
    return constrainedHandler(originalDoorHandler, drag, ui, message);
}
static unsigned long long hingeHandler(uintptr_t drag, uintptr_t ui, int message) {
    return constrainedHandler(originalHingeHandler, drag, ui, message);
}
static unsigned long long constraintHandler(uintptr_t drag, uintptr_t ui, int message) {
    return constrainedHandler(originalConstraintHandler, drag, ui, message);
}
static uintptr_t heldDoorDrag() { // the drag manager while it holds a door or a hinged thing
    const uintptr_t drag = *(uintptr_t*)(gameAt(fp::sig::dragManager));
    if (!readable(drag, 0x110) || !(*(unsigned*)(drag + 8) & (0x4000 | 0x10000 | 0x20000)) ||
        !*(uintptr_t*)(drag + field::drag::object))
        return 0;
    return drag;
}
static bool heldDoorLive() {
    return doorDistance.load() > 0 && InterlockedCompareExchange(&active, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) && viewMode.load() < 2;
}
struct HeldPlanePoint {
    bool valid = false;
    float plane[4]{}, point[3]{};
};
static HeldPlanePoint heldPlanes[4]; // door, hinge primary/alternate, free constraint; independent histories
static std::atomic<bool> doorReleaseWanted{false};
static int heldPlaneSlot(uintptr_t caller) {
    if (caller == gameAt(fp::sig::doorPlaneReturn)) return 0;
    if (caller == gameAt(fp::sig::hingePlaneReturnA) || caller == gameAt(fp::sig::hingePlaneReturnB))
        return 1;
    if (caller == gameAt(fp::sig::hingePlaneReturnC)) return 2;
    if (caller == gameAt(fp::sig::constraintPlaneReturn)) return 3;
    return -1;
}
static bool projectHeldSeed(const float* plane, float* out) {
    if (!doorGrabPointValid) return false;
    const float nn = plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2];
    if (!(nn > 0.5f) || !std::isfinite(nn) || !std::isfinite(plane[3])) return false;
    const float t =
        (doorGrabPoint[0] * plane[0] + doorGrabPoint[1] * plane[1] + doorGrabPoint[2] * plane[2] - plane[3]) /
        nn;
    for (int i = 0; i < 3; ++i) {
        out[i] = doorGrabPoint[i] - plane[i] * t;
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}
static bool heldViewportMatches(uintptr_t drag, uintptr_t viewport) {
    return viewport && viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport) &&
           readable(drag, field::drag::viewport + sizeof(uintptr_t)) &&
           *(uintptr_t*)(drag + field::drag::viewport) == viewport;
}
static bool heldPlanePoint(int slot, uintptr_t drag, uintptr_t viewport, int x, int y, const float* plane,
                           float* out) {
    if (!heldViewportMatches(drag, viewport)) return false;
    const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
    const int mode = viewMode.load();
    const auto generation = holdGeneration.load();
    if (lastDoorObject != object) {
        for (auto& saved : heldPlanes) saved = {};
        lastDoorObject = object;
    }
    LONG rx = 0, ry = 0;
    const bool reticle = reticleForUi(&rx, &ry);
    if (reticle) {
        x = int(rx);
        y = int(ry);
    }
    // Native GUI coordinates are deliberate only during the configured free-cursor hold.
    // A stale reticle otherwise freezes the last accepted constraint target.
    const bool pointer = reticle || InterlockedCompareExchange(&rmbFree, 0, 0);
    alignas(8) unsigned char ray[0x40]{};
    if (pointer) ownedPickRay(viewport, float(x), float(y), (uintptr_t)ray);
    if (viewMode.load() != mode || holdGeneration.load() != generation || !heldDoorLive() ||
        heldDoorDrag() != drag || !heldViewportMatches(drag, viewport) ||
        *(uintptr_t*)(drag + field::drag::object) != object)
        return false;
    float p[3]{};
    bool ok = pointer && fp::heldPointOnPlane((const float*)(ray + field::pick::origin),
                                              (const float*)(ray + field::pick::direction),
                                              doorDistance.load(), plane, p);
    if (pointer && slot == 3) {
        // 0x20000's plane is captured from the overhead camera at grab time,
        // not a physical hinge. Supply a bounded actual-view target instead.
        const float* o = (const float*)(ray + field::pick::origin);
        const float* d = (const float*)(ray + field::pick::direction);
        const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
        const float distance =
            std::clamp(doorDistance.load() - doorCameraOffset.load(), fp::holdNear, float(grabRange.load()));
        const float norm = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        const float t = fp::holdAlongRay(o, d, eye, distance);
        ok = std::isfinite(norm) && norm > 0.5f && std::isfinite(t) && t >= 0.f;
        if (ok)
            for (int i = 0; i < 3; ++i) p[i] = o[i] + d[i] * t;
    }
    const auto safe = [drag, slot](const float* q) {
        if (!std::isfinite(q[0] + q[1] + q[2])) return false;
        if (slot != 0) return true;
        const float* pivot = (const float*)(drag + field::drag::pivot);
        const float dx = q[0] - pivot[0], dz = q[2] - pivot[2];
        return std::isfinite(dx * dx + dz * dz) && dx * dx + dz * dz >= 100.f;
    };
    auto& saved = heldPlanes[slot];
    if (!ok || !safe(p)) {
        if (saved.valid && !memcmp(saved.plane, plane, sizeof saved.plane) && safe(saved.point))
            memcpy(p, saved.point, sizeof p);
        else {
            if (slot == 3 && doorGrabPointValid)
                memcpy(p, doorGrabPoint, sizeof p);
            else if (!projectHeldSeed(plane, p))
                return false;
            if (!safe(p)) return false;
        }
    }
    memcpy(out, p, sizeof p);
    memcpy(saved.point, p, sizeof p);
    memcpy(saved.plane, plane, sizeof saved.plane);
    saved.valid = true;
    ++doorHolds;
    // Only publish a distance when a usable current ray supplied the target.
    if (ok) {
        const float* o = (const float*)(ray + field::pick::origin);
        float n = 0;
        for (int i = 0; i < 3; ++i) n += (p[i] - o[i]) * (p[i] - o[i]);
        if (std::isfinite(n)) *(float*)gameAt(fp::sig::dragHitDistance) = std::sqrt(n);
    }
    return true;
}
struct HeldPlanePresentation {
    uintptr_t drag = 0;
    bool valid[4]{};
    float points[4][3]{};
};
static thread_local HeldPlanePresentation heldPlanePresentation;
static void planeHitFor(uintptr_t caller, float* out, uintptr_t viewport, int x, int y, const float* plane) {
    if (nativeAimPlane(caller, out, viewport, x, y, plane)) return;
    const int slot = heldPlaneSlot(caller);
    const uintptr_t drag = slot >= 0 && heldDoorLive() ? heldDoorDrag() : 0;
    if (!drag || !heldViewportMatches(drag, viewport)) {
        originalPlaneHit(out, viewport, x, y, plane);
        return;
    }
    if (heldPlanePresentation.drag == drag && heldPlanePresentation.valid[slot]) {
        memcpy(out, heldPlanePresentation.points[slot], 12);
        return;
    }
    if (heldPlanePoint(slot, drag, viewport, x, y, plane, out)) return;
    // Handler updates are preflighted below. The remaining direct caller is
    // hinge initialization, immediately after a finite grab: seed its physical
    // plane from that grab rather than invoking an unbounded screen intersection.
    if (doorGrabPointValid)
        memcpy(out, doorGrabPoint, 12);
    else
        memset(out, 0, 12);
}
static unsigned long long constrainedHandler(DragHandler native, uintptr_t drag, uintptr_t ui, int message) {
    if (message == 0x80 || !heldDoorLive() || heldDoorDrag() != drag) return native(drag, ui, message);
    const uintptr_t viewport = *(uintptr_t*)(drag + field::drag::viewport);
    if (!heldViewportMatches(drag, viewport)) return native(drag, ui, message);
    if (!closeViewContext() || InterlockedCompareExchange(&cursorFree, 0, 0) ||
        InterlockedCompareExchange(&windowFree, 0, 0))
        return 3;
    LONG freshX = 0, freshY = 0;
    if (!InterlockedCompareExchange(&rmbFree, 0, 0) &&
        (!reticleForUi(&freshX, &freshY) || freshX < 0 || freshY < 0))
        return 3;
    const unsigned flags = *(unsigned*)(drag + field::drag::flags);
    // Keep native reach-pause cleanup, without evaluating any new target.
    if (!(flags & 0x20000) && *(unsigned char*)(drag + field::drag::invalidDrop))
        return native(drag, ui, message);
    HeldPlanePoint previousPlanes[4];
    memcpy(previousPlanes, heldPlanes, sizeof previousPlanes);
    const uintptr_t previousObject = lastDoorObject;
    const auto rollback = [&]() {
        memcpy(heldPlanes, previousPlanes, sizeof heldPlanes);
        lastDoorObject = previousObject;
    };
    const int x = *(int*)(ui + field::gui::pointerX), y = ((int*)(ui + field::gui::pointerX))[1];
    HeldPlanePresentation current;
    current.drag = drag;
    if (*(unsigned*)(drag + field::drag::flags) & 0x4000) {
        const float plane[4] = {0, 1, 0, *(float*)(drag + field::drag::planeHeight)};
        current.valid[0] = heldPlanePoint(0, drag, viewport, x, y, plane, current.points[0]);
        if (!current.valid[0]) {
            rollback();
            return 3;
        }
    } else if (*(unsigned*)(drag + field::drag::flags) & 0x20000) {
        current.valid[3] = heldPlanePoint(3, drag, viewport, x, y,
                                          (const float*)(drag + field::drag::hingePlane), current.points[3]);
        if (!current.valid[3]) {
            rollback();
            return 3;
        }
    } else {
        current.valid[1] = heldPlanePoint(1, drag, viewport, x, y,
                                          (const float*)(drag + field::drag::hingePlane), current.points[1]);
        current.valid[2] = heldPlanePoint(
            2, drag, viewport, x, y, (const float*)(drag + field::drag::alternatePlane), current.points[2]);
        if (!current.valid[1] || !current.valid[2]) {
            rollback();
            return 3;
        }
    }
    const auto previous = heldPlanePresentation;
    heldPlanePresentation = current;
    unsigned long long result = 0;
    LONG rx = x, ry = y;
    reticleForUi(&rx, &ry);
    auto* pointer = (LONG*)(ui + field::gui::pointerX);
    bool completed = false;
    __try {
        pointer[0] = rx;
        pointer[1] = ry;
        result = native(drag, ui, message);
        completed = true;
    } __finally {
        if (!completed) rollback();
        restoreGuiPointer(ui, field::gui::pointerX, rx, ry, x, y);
        heldPlanePresentation = previous;
    }
    return result;
}
static void planeHit(float* out, uintptr_t viewport, int x, int y, const float* plane) {
    planeHitFor((uintptr_t)_ReturnAddress(), out, viewport, x, y, plane);
}
static bool onGameThread() {
    const LONG game = InterlockedCompareExchange(&navThread, 0, 0);
    return !game || DWORD(game) == GetCurrentThreadId();
}
static void sendDoorMessage(uintptr_t drag, int message) {
    const uintptr_t ui = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    if (!ui) return;
    __try {
        const unsigned flags = *(unsigned*)(drag + 8);
        const auto handler =
            (flags & 0x4000) ? doorHandler : ((flags & 0x20000) ? constraintHandler : hingeHandler);
        handler(drag, ui, message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        doorDistance = 0;
    }
}
// Once a frame on the game's thread: run the held door's handler as a pointer move would, so turning the
// view carries it; a stop or a release asked for elsewhere lets go. The swap runs on the same thread as the
// game's own handler calls and after them in the frame; the
// physics thread reads the spring either way, as it does against the game's own calls.
static bool gameViewportFocused() {
    return *(uintptr_t*)(gameAt(fp::sig::focusedViewport)) == *(uintptr_t*)(gameAt(fp::sig::mainViewport));
} // the gate 0x121AF0 checks before its drag calls
static void heldDoorStep() {
    if (doorDistance.load() <= 0) return;
    if (!onGameThread()) return;
    const uintptr_t drag = heldDoorDrag();
    if (!drag) {
        doorDistance = 0;
        lastDoorObject = 0;
        doorReleaseWanted = false;
        return;
    }
    if (doorReleaseWanted.exchange(false) || InterlockedCompareExchange(&stopping, 0, 0)) {
        doorDistance = 0;
        lastDoorObject = 0;
        sendDoorMessage(drag, 0x80);
        return;
    }
    if (!heldDoorLive() || !gameViewportFocused() ||
        !heldViewportMatches(drag, *(uintptr_t*)(drag + field::drag::viewport))) return;
    sendDoorMessage(drag, 0);
}
// Let go of a held door, as the game does when the button comes up: now on the game's thread, else at its
// next frame.
static void releaseHeldDoor() {
    if (doorDistance.load() <= 0) return;
    if (!onGameThread()) {
        doorReleaseWanted = true;
        return;
    }
    const uintptr_t drag = heldDoorDrag();
    doorDistance = 0;
    lastDoorObject = 0;
    if (drag) sendDoorMessage(drag, 0x80);
}
// The combat toggle (0x11FED0) drops whatever is dragged through 0x119C30, which is written for items: for a
// door or lid it would copy a stale item transform into the door and leave its spring attached. A held door
// or lid is let go as the button release does instead; items keep the game's path.
using DragDrop = void (*)(uintptr_t);
static DragDrop originalDragDrop; // RVA 0x119C30
// Native item release queues its physics work (0x117000 -> 0x1A890 -> 0x116EF0).
// Capture the push at submission, keyed by that exact native allocation, and apply
// it after the queued release has completed its body updates and freed its payload.
// A new hold cannot lend its velocity to an older release. The small bounded table
// owns only metadata; a full or canceled native queue cannot retain game objects.
using QueueWork = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using ApplyHeldRelease = uintptr_t (*)(uintptr_t);
static QueueWork originalQueueWork;
static ApplyHeldRelease originalApplyHeldRelease;
struct QueuedFling {
    uintptr_t payload = 0, body = 0, object = 0, root = 0, scene = 0;
    ULONGLONG sequence = 0, hold = 0;
    unsigned generation = 0;
    float velocity[3]{}, step = 0.f;
};
static SRWLOCK flingQueueLock = SRWLOCK_INIT;
static std::array<QueuedFling, 8> queuedFlings;
static ULONGLONG flingSequence = 0;
static QueuedFling takeQueuedFling(uintptr_t payload) {
    QueuedFling result;
    AcquireSRWLockExclusive(&flingQueueLock);
    for (auto& queued : queuedFlings) {
        if (queued.payload == payload) {
            result = queued;
            queued = {};
            break;
        }
    }
    ReleaseSRWLockExclusive(&flingQueueLock);
    return result;
}
static void rememberQueuedFling(QueuedFling release) {
    AcquireSRWLockExclusive(&flingQueueLock);
    auto* slot = &queuedFlings[0];
    for (auto& queued : queuedFlings) {
        if (!queued.payload || queued.payload == release.payload) {
            slot = &queued;
            break;
        }
        if (queued.sequence < slot->sequence) slot = &queued;
    }
    release.sequence = ++flingSequence;
    *slot = release; // bounded overflow loses a fling, never applies it to another release
    ReleaseSRWLockExclusive(&flingQueueLock);
}
static bool flingViewOwned() {
    return base && viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0);
}
static QueuedFling captureQueuedFling(uintptr_t payload) {
    QueuedFling release;
    if (!flingViewOwned()) return release;
    __try {
        const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
        if (!readable(drag, field::drag::invalidDrop + 1) ||
            *(unsigned*)(drag + field::drag::flags) != 0x2000 ||
            *(unsigned char*)(drag + field::drag::invalidDrop) ||
            !readable(payload, field::heldRelease::body + sizeof(uintptr_t)))
            return release;
        const uintptr_t object = *(uintptr_t*)(drag + field::drag::object),
                        body = *(uintptr_t*)(payload + field::heldRelease::body);
        if (!object || object != holdObject.load() ||
            !readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t)) ||
            *(uintptr_t*)(object + field::sceneNode::bodyPart) != body)
            return release;
        const ULONGLONG now = GetTickCount64();
        const float speed = now - pushAt.load() <= fp::flingFreshMs ? pushSpeed.load() : 0.f;
        const float step = physicsStep.load();
        if (!(speed > 1.f) || !std::isfinite(speed) || !(step > 0.f && step < 0.1f)) return release;
        release.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        release.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
        if (!release.root || !release.scene) return {};
        for (int i = 0; i < 3; ++i) {
            release.velocity[i] = pushDir[i].load() * speed;
            if (!std::isfinite(release.velocity[i])) return {};
        }
        release.payload = payload;
        release.object = object;
        release.body = body;
        release.hold = holdGeneration.load();
        release.generation = inputOwner.generation();
        release.step = step;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        release = {};
    }
    return release;
}
static uintptr_t queueWorkFor(uintptr_t caller, uintptr_t queue, uintptr_t callback, uintptr_t payload) {
    if (caller == gameAt(fp::sig::heldReleaseQueueReturn) &&
        callback == gameAt(fp::sig::applyHeldRelease)) {
        takeQueuedFling(payload); // a later native allocation cannot inherit stale metadata
        const QueuedFling release = captureQueuedFling(payload);
        if (release.payload) rememberQueuedFling(release);
    }
    return originalQueueWork(queue, callback, payload);
}
static uintptr_t queueWork(uintptr_t queue, uintptr_t callback, uintptr_t payload) {
    return queueWorkFor((uintptr_t)_ReturnAddress(), queue, callback, payload);
}
static uintptr_t applyHeldRelease(uintptr_t payload) {
    QueuedFling release = takeQueuedFling(payload);
    if (release.payload &&
        (!readable(payload, field::heldRelease::body + sizeof(uintptr_t)) ||
         *(uintptr_t*)(payload + field::heldRelease::body) != release.body))
        release = {};
    const uintptr_t result = originalApplyHeldRelease(payload); // frees payload; never read it again
    if (!release.body || !flingViewOwned() || inputOwner.generation() != release.generation ||
        *(uintptr_t*)gameAt(fp::sig::playerRoot) != release.root ||
        *(uintptr_t*)gameAt(fp::sig::currentScene) != release.scene)
        return result;
    if (holdGeneration.load() != release.hold) {
        // Grab start clears holdObject before the first placement publishes it.
        // The native drag manager already owns the new grab during that gap.
        const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
        if (holdObject.load() == release.object ||
            (readable(drag, field::drag::object + sizeof(uintptr_t)) &&
             *(unsigned*)(drag + field::drag::flags) == 0x2000 &&
             *(uintptr_t*)(drag + field::drag::object) == release.object))
            return result;
    }
    const uintptr_t body = release.body, object = release.object;
    if (!readable(body, field::bodyPart::history2 + 48) ||
        !readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t)) ||
        *(uintptr_t*)(body + field::bodyPart::meshNode) != object ||
        *(uintptr_t*)(object + field::sceneNode::bodyPart) != body)
        return result;
    if (fp::flingHistory(release.velocity, release.step, (const float(*)[3])(body + field::bodyPart::points),
                         (float(*)[3])(body + field::bodyPart::history),
                         (float(*)[3])(body + field::bodyPart::history2)))
        ++flings;
    return result;
}
static void dragDrop(uintptr_t drag) {
    const uintptr_t ui = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    const bool ownView = viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
                         !InterlockedCompareExchange(&stopping, 0, 0);
    if (ownView && ui && readable(drag, 0x20) && (*(unsigned*)(drag + 8) & (0x4000 | 0x10000 | 0x20000)) &&
        *(uintptr_t*)(drag + field::drag::object)) {
        doorDistance = 0;
        lastDoorObject = 0;
        sendDoorMessage(drag, 0x80);
        return;
    }
    originalDragDrop(drag);
}
static void f8WindowTick(bool show, bool title);
static void f8MouseStep();
// The title screen: no controlled character (root 0, as the park logic reads gameplay) for 2 s, so
// a load's brief gap does not count. Worker thread.
static ULONGLONG noPlayerSince = 0;
static bool atTitle() {
    const ULONGLONG now = GetTickCount64();
    if (*(uintptr_t*)(gameAt(fp::sig::playerRoot))) {
        noPlayerSince = 0;
        return false;
    }
    if (!noPlayerSince) noPlayerSince = now;
    const bool title = now - noPlayerSince >= 2000;
    return title;
}
static void cursorPositionFor(uintptr_t caller, LONG* x, LONG* y) {
    if (caller == gameAt(fp::sig::uiPollCursorReturn)) {
        if (guiPointerPresentation.active) {
            *x = guiPointerPresentation.x;
            *y = guiPointerPresentation.y;
            guiPointerPresentation.read = true;
            return;
        }
        if (!reticleForUi(x, y)) originalCursorPosition(x, y);
        return;
    }
    if (caller != gameAt(fp::sig::crosshairCursorReturn)) {
        originalCursorPosition(x, y);
        return;
    }
    if (!gameplayOwnsPointer() || !closeViewContext()) {
        originalCursorPosition(x, y);
        return;
    }
    CameraGuard guard;
    HWND window = gameWindow();
    RECT rect{};
    Aim a;
    AcquireSRWLockShared(&aimLock);
    a = aim;
    ReleaseSRWLockShared(&aimLock);
    const uintptr_t viewport = pickViewportForCamera(savedCamera);
    const bool currentCamera = viewport && *(uintptr_t*)(viewport + field::viewport::view) == savedView;
    if (window && currentCamera && a.valid && a.root == filterRoot && a.head == filterHead &&
        a.root == *(uintptr_t*)(gameAt(fp::sig::playerRoot)) && GetClientRect(window, &rect) &&
        rect.right > 40 && rect.bottom > 40) {
        auto mesh = *(uintptr_t*)(a.root + field::character::sceneNode),
             scene = mesh ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
        if (scene) {
            // Project through the achieved physical camera. Only exploration
            // presentation has the existing 35 ms filter and 1.5 px dead-zone.
            float px = 0, py = 0;
            const bool projected = aimCursorPosition(a, viewMode.load(), lastCamera, float(rect.right),
                                                     float(rect.bottom), renderedFov, px, py);
            if (projected) {
                const bool combat = *(unsigned char*)(a.root + field::character::combatStance) != 0;
                static float sx = -1, sy = -1;
                static ULONGLONG st = 0;
                const ULONGLONG nowMs = GetTickCount64();
                const float dt = st ? float(nowMs - st) / 1000.f : 1.f;
                st = nowMs;
                const float dx = px - sx, dy = py - sy;
                if (sx < 0 || dt > 0.25f || dx * dx + dy * dy > 80.f * 80.f || viewMode.load() != 0 ||
                    combat) {
                    sx = px;
                    sy = py;
                } else {
                    const float k = 1.f - std::exp(-dt / 0.035f);
                    sx += dx * k;
                    sy += dy * k;
                }
                static float shownX = -1, shownY = -1;
                const float dead = 1.5f * float(rect.bottom) / 1080.f; // tiny wobbles do not move it at all
                if (shownX < 0 || std::fabs(sx - shownX) > dead || std::fabs(sy - shownY) > dead ||
                    viewMode.load() != 0 || combat) {
                    shownX = sx;
                    shownY = sy;
                }
                px = shownX;
                py = shownY;
            }
            if (projected) {
                const bool inside = px >= 0 && py >= 0 && px <= rect.right - 1 && py <= rect.bottom - 1;
                if (px < 0) px = 0;
                if (px > rect.right - 1) px = float(rect.right - 1);
                if (py < 0) py = 0;
                if (py > rect.bottom - 1) py = float(rect.bottom - 1);
                *x = LONG(std::lround(px));
                *y = LONG(std::lround(py));
                InterlockedIncrement(&nativeCursorUpdates);
                // An edge-clamped direction hint is not a point the player can pick.
                const int clear =
                    inside ? pickClearance(float(*x), float(*y), float(rect.right), float(rect.bottom)) : -1;
                if (inside) {
                    ++pickChecks;
                    lastClearance = clear;
                }
                AcquireSRWLockExclusive(&reticleLock);
                if (inside) {
                    reticleX = *x;
                    reticleY = *y;
                    reticleClear = clear;
                    reticleTick = GetTickCount64();
                    reticleFrame = InterlockedCompareExchange(&swapCalls, 0, 0);
                } else
                    reticleFrame = -1;
                ReleaseSRWLockExclusive(&reticleLock);
                // A hidden crosshair is drawn off the window. This read feeds only the crosshair draw; picks and the
                // attack cue use the reticle published above, so doors, pick-ups and talking still work. Only the
                // mod's own first and third person ever hide it; the overhead view and its blend are the game's.
                const bool combat = *(unsigned char*)(a.root + field::character::combatStance) != 0;
                static bool markerShown = false; // the aim leads the head: show where it will land
                {
                    float d[3];
                    float n = 0;
                    for (int i = 0; i < 3; ++i) {
                        d[i] = a.point[i] - a.origin[i];
                        n += d[i] * d[i];
                    }
                    n = std::sqrt(n);
                    const float c =
                        n > 0 ? -(d[0] * lastCamera[6] + d[1] * lastCamera[7] + d[2] * lastCamera[8]) / n
                              : 1.f; // the camera's forward is its back axis reversed
                    markerShown = aimMarker.load() && viewMode.load() == 0 &&
                                  fp::aimMarkerStep(markerShown, std::acos(std::clamp(c, -1.f, 1.f)));
                }
                if (aimIndicator.load() == 0 || (aimIndicator.load() == 1 && !combat) ||
                    (trackingPresented && !inside) || (viewMode.load() < 2 &&
                    !fp::crosshairDrawn(combat, swinging.load(), hideSwingCrosshair.load(),
                                        idleCrosshairAlways.load(), markerShown,
                                        hoverOutOfRange.load() &&
                                            GetTickCount64() - hoverRangeAt.load() <= reticleFreshMs))) {
                    *x = rect.right + 256;
                    *y = rect.bottom + 256;
                    if (swinging.load()) ++swingHides;
                }
                return;
            }
        }
    }
    AcquireSRWLockExclusive(&reticleLock);
    reticleFrame = -1;
    ReleaseSRWLockExclusive(&reticleLock);
    if (trackingPresented && currentCamera && rect.right > 40 && rect.bottom > 40 &&
        a.valid && a.root == filterRoot && a.head == filterHead &&
        closeViewContext()) {
        *x = rect.right + 256;
        *y = rect.bottom + 256;
        return;
    }
    originalCursorPosition(x, y);
    if (hideSwingCrosshair.load() && swinging.load() && currentCamera && a.valid &&
        a.root == filterRoot && a.head == filterHead &&
        a.root == *(uintptr_t*)gameAt(fp::sig::playerRoot) && closeViewContext()) {
        HWND w = gameWindow();
        RECT r;
        if (w && GetClientRect(w, &r)) {
            *x = r.right + 256;
            *y = r.bottom + 256;
            ++swingHides;
        }
    } // also when the projection failed
}
static void cursorPosition(LONG* x, LONG* y) {
    cursorPositionFor((uintptr_t)_ReturnAddress(), x, y);
}
// These helpers are called only while cameraLock is owned. Never write a
// remembered native pointer unless the current view still owns that camera.
static void currentCamera(uintptr_t& view, uintptr_t& camera) {
    auto viewport = *(uintptr_t*)(gameAt(fp::sig::mainViewport));
    view = viewport ? *(uintptr_t*)(viewport + field::viewport::view) : 0;
    camera = view ? *(uintptr_t*)(view + field::view::camera) : 0;
}
static void resetViewHistory() {
    cameraEntry = {};
    trackingView.clear();
    trackingPresented = false;
    cameraFrame.valid = false;
    InterlockedExchange(&cameraReady, 0);
    nearPlane.reset();
    filterRoot = filterHead = 0;
    releaseInput();
    AcquireSRWLockExclusive(&aimLock);
    aim.valid = false;
    aim.entryPending = false;
    ReleaseSRWLockExclusive(&aimLock);
}
static bool sameOwner(uintptr_t view, uintptr_t camera) {
    return savedCamera && view == savedView && camera == savedCamera &&
           *(uintptr_t*)(camera + field::camera::controller) == savedController;
}
static bool uiOpen() {
    return *(int*)(gameAt(fp::sig::uiOpenCount)) > 0;
}
static bool identityChanged() {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    return !root || root != savedRoot || *(uintptr_t*)(gameAt(fp::sig::currentScene)) != savedScene;
}
static void writeStock(uintptr_t view, uintptr_t camera, const float* pose, float fov, float stockNearValue) {
    memcpy((void*)(camera + field::camera::pose), pose, 48);
    memcpy((void*)(camera + field::camera::pose2), pose, 48);
    *(float*)(camera + field::camera::fov) = fov;
    *(float*)(camera + field::camera::nearPlane) = stockNearValue;
    *(unsigned*)(camera + field::camera::dirty) = 1;
    rebuildInverse(camera + field::camera::inverse, camera + field::camera::pose);
    rebuildProjection(view);
}
// Releases the saved record without a fault. The caller has already restored
// the camera or established that restoration is not possible to prove here.
static void parkCamera(fp::OwnerChange kind) {
    park.active = true;
    park.unverified = kind == fp::OwnerChange::parkRelease;
    park.rearmPending = true;
    park.root = savedRoot;
    park.scene = savedScene;
    park.stable = 0;
    park.left = identityChanged();
    savedCamera = savedView = savedController = savedRoot = savedScene = 0;
    InterlockedExchange(&pendingRestore, 0);
    resetViewHistory();
    parkKind = int(kind);
    ++parks;
    InterlockedExchange(&parkedNow, 1);
}
// The current camera of the current view is live: if it is the retired one and
// still carries our values, put its stock values back. Either way the record
// is settled (a rewritten object at that address holds nothing of ours).
static void reclaimRetired(uintptr_t view, uintptr_t camera) {
    // After a stop declared restoration unknown, nothing is written by address.
    if (InterlockedCompareExchange(&restorationUnknown, 0, 0)) return;
    if (!camera || camera == savedCamera) return;
    for (Retired& r : retired) {
        if (!r.camera || camera != r.camera) continue;
        if (*(float*)(camera + field::camera::fov) == r.writtenFov &&
            *(float*)(camera + field::camera::nearPlane) == r.writtenNear) {
            writeStock(view, camera, r.pose, r.fov, r.nearPlane);
            ++reclaims;
        }
        r.camera = 0;
        park.unverified = false;
    }
}
// A park during live play (same gameplay, no menu). savedRoot/savedScene still
// name the gameplay being parked. Repeats inside one gameplay back off.
static void parkLive(fp::OwnerChange kind) {
    if (park.root != savedRoot || park.scene != savedScene) {
        park.recent = fp::LiveParks{};
        park.backoffUntil = 0;
    }
    parkCamera(kind);
    const ULONGLONG now = GetTickCount64();
    if (fp::recordLivePark(park.recent, now)) {
        park.backoffUntil = now + fp::liveParkWindowMs;
        ++parkBackoffs;
    }
}
static bool checkSavedOwner(uintptr_t view, uintptr_t camera) {
    if (InterlockedCompareExchange(&restorationUnknown, 0, 0)) return false;
    if (!savedCamera) return true;
    if (sameOwner(view, camera)) return true;
    const bool current = camera && camera == savedCamera;
    const bool ours = current && *(float*)(camera + field::camera::fov) == renderedFov &&
                      *(float*)(camera + field::camera::nearPlane) == renderedNear;
    const fp::OwnerChange kind = fp::classifyOwnerChange(current, ours);
    if (kind == fp::OwnerChange::parkRestore) writeStock(view, camera, stockPose, stockFov, stockNear);
    const bool live = !identityChanged() && !uiOpen();
    if (kind == fp::OwnerChange::parkRelease) {
        reclaimRetired(view, camera); // Settle an older record first.
        // Never dereferenced unless it is again the current camera of the current view.
        // A free slot, else one whose gameplay is gone, else the oldest: dropping an
        // unresolved record of this gameplay makes restoration unknown (stop says so).
        unsigned slot = retiredSlots;
        for (unsigned k = 0; k < retiredSlots && slot == retiredSlots; ++k)
            if (!retired[k].camera) slot = k;
        for (unsigned k = 0; k < retiredSlots && slot == retiredSlots; ++k)
            if (retired[k].left) slot = k;
        if (slot == retiredSlots) {
            slot = 0;
            InterlockedExchange(&restorationUnknown, 1);
        }
        for (unsigned k = slot; k + 1 < retiredSlots; ++k)
            retired[k] = retired[k + 1]; // keep age order: newest last
        Retired& r = retired[retiredSlots - 1];
        r.camera = savedCamera;
        r.root = savedRoot;
        r.scene = savedScene;
        r.left = false;
        memcpy(r.pose, stockPose, 48);
        r.fov = stockFov;
        r.nearPlane = stockNear;
        r.writtenFov = renderedFov;
        r.writtenNear = renderedNear;
    }
    if (live)
        parkLive(kind);
    else
        parkCamera(kind);
    return true;
}
// Render thread, every frame, cameraLock owned. Decides when a parked first
// person may re-attach. A released camera of the same gameplay stays retired
// (reclaimed if it is ever current again) and no longer stops the mod.
static void updatePark(uintptr_t view, uintptr_t camera) {
    reclaimRetired(view, camera);
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot)),
                    scene = *(uintptr_t*)(gameAt(fp::sig::currentScene));
    for (Retired& r : retired)
        if (r.camera && (root != r.root || scene != r.scene)) r.left = true;
    if (!park.active) return;
    if (root != park.root || scene != park.scene) park.left = true;
    if (fp::parkStep(park.left, root != 0, uiOpen(), GetTickCount64() < park.backoffUntil, park.stable) ==
        fp::ParkStep::rearm)
        park.active = false; // nav attaches like a fresh F6.
}
static bool restoreOwnedCamera(uintptr_t view, uintptr_t camera) {
    if (!checkSavedOwner(view, camera)) return false;
    if (!savedCamera) return true;
    writeStock(view, camera, stockPose, stockFov, stockNear);
    savedCamera = savedView = savedController = savedRoot = savedScene = 0;
    InterlockedExchange(&pendingRestore, 0);
    return true;
}
