// Picking and holding: the game's pick rays past your own body, held-object distance and drop spot, grab reach, doors and lids, the crosshair position.
// Part of first_person.cpp (one translation unit): included there, in order.
// The held object (a scene node; skipped by its own placement pick), 0 when nothing is held.
static uintptr_t heldNode() {
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    const uintptr_t object = readable(drag, 0x20) ? *(uintptr_t*)(drag + field::drag::object) : 0;
    return readable(object, 0x2a0) ? object : 0;
}
static char pickRay(uintptr_t camera, float x, float y, uintptr_t out) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    bool dropping = false;
    if (caller == gameAt(fp::sig::dropSpotPickReturn) && out) {
        __try {
            dropping = heldDropWanted();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dropping = false;
        }
    }
    if (dropping) {
        memset((void*)out, 0, 0x34);
        return dropSpot(pickPastSelf(originalPickRay, camera, x, y, out, heldNode()), out);
    }
    const bool placing = placingThread == GetCurrentThreadId() && out;
    // 0x8F720 leaves out untouched when the pointer is off the viewport (the reticle reports -1,-1
    // when the eye is in a wall): cleared, the native drag sees a level ray and leaves the object.
    if (placing) memset((void*)out, 0, 0x34);
    const char hit = pickPastSelf(originalPickRay, camera, x, y, out, placing ? heldNode() : 0);
    if (!placing) return hit;
    const float* dir = (const float*)(out + field::pick::direction);
    const float* origin = (const float*)(out + field::pick::origin);
    const float reached = *(float*)(out + field::pick::distance);
    const float length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!std::isfinite(length) || length < 0.5f || !std::isfinite(origin[0] + origin[1] + origin[2]))
        return hit;
    const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
    if (!readable(drag, 0x20)) return hit; // released between the placement and this pick
    const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
    const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
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
    float t = fp::holdAlongRay(origin, dir, eye, holdApplied.load());
    if (t < 0.f) return hit; // pointing away from the character: the game's
    t = holdReachClamp(origin, dir, t);
    if (hit && t >= reached) return hit; // something nearer (never the held object: it is skipped)
    if (const uintptr_t level = levelGrid()) t = levelClear(level, origin, dir, t);
    if (t < 1.f) return hit; // the eye itself is in geometry: native
    for (int i = 0; i < 3; i++) ((float*)(out + 8))[i] = origin[i] + dir[i] * t;
    *(float*)(out + field::pick::distance) = t;
    ++holdPlacements;
    return 1;
}
// Grab reach. 0x11A690 starts every grab (items 0x2000, doors 0x4000/0x10000,
// hand 0x80000, ...) from the pick 0x8FB30(camera, x, y, out, filter) returning to 0x11A79E; the
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
        grabStartDistance = 0.f;
        if (hit && out) {
            const float* d = (const float*)(out + field::pick::direction);
            const float* o = (const float*)(out + field::pick::origin);
            const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
            grabStartDistance = fp::eyeDistance(o, d, *(float*)(out + field::pick::distance), eye);
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
        (caller == gameAt(fp::sig::grabStartPickReturn) || caller == gameAt(fp::sig::hoverPickReturn)) &&
        tooFar) {
        if (caller == gameAt(fp::sig::grabStartPickReturn)) ++grabsRefused;
        return 0;
    }
    return hit;
}
static char grabPick(uintptr_t camera, float x, float y, uintptr_t out, void* filter) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    char hit = 0;
    if (!selfPickWanted() || selfPickSet)
        hit = originalGrabPick(camera, x, y, out, filter);
    else
        __try {
            selfPickMask();
            hit = originalGrabPick(camera, x, y, out, filter);
        } __finally {
            selfPickRestore();
        } // as pickPastSelf: never your own body
    return grabPickResult(caller, hit, out);
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
static DragHandler originalDoorHandler,
    originalHingeHandler; // RVA 0x11B400, 0x11BC50 (message 0: pointer moved, 0x80: let go)
static unsigned long long doorHandler(uintptr_t drag, uintptr_t ui, int message) {
    return originalDoorHandler(drag, ui, message);
}
static unsigned long long hingeHandler(uintptr_t drag, uintptr_t ui, int message) {
    return originalHingeHandler(drag, ui, message);
}
using PlaneHit = void (*)(float*, uintptr_t, int, int, const float*);
static PlaneHit originalPlaneHit; // RVA 0x8F530 (out, camera, x, y, plane {n, d})
static uintptr_t heldDoorDrag() { // the drag manager while it holds a door or a hinged thing
    const uintptr_t drag = *(uintptr_t*)(gameAt(fp::sig::dragManager));
    if (!readable(drag, 0x110) || !(*(unsigned*)(drag + 8) & (0x4000 | 0x10000)) ||
        !*(uintptr_t*)(drag + field::drag::object))
        return 0;
    return drag;
}
static bool heldDoorLive() {
    return doorDistance.load() > 0 && InterlockedCompareExchange(&active, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) && viewMode.load() < 2;
}
static float lastDoorPoint[3]{}; // the last good held point, for lastDoorObject
static std::atomic<bool> doorReleaseWanted{false};
static void planeHitFor(uintptr_t caller, float* out, uintptr_t camera, int x, int y, const float* plane) {
    const bool door = caller == gameAt(fp::sig::doorPlaneReturn),
               hinge = caller == gameAt(fp::sig::hingePlaneReturnB) ||
                       caller == gameAt(fp::sig::hingePlaneReturnC) ||
                       caller == gameAt(fp::sig::hingePlaneReturnA);
    const uintptr_t drag = (door || hinge) && heldDoorLive() ? heldDoorDrag() : 0;
    if (!drag) {
        originalPlaneHit(out, camera, x, y, plane);
        return;
    }
    const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
    alignas(8) unsigned char ray[0x40]{};
    originalPickRay(camera, float(x), float(y),
                    (uintptr_t)ray); // +0x14 direction, +0x20 origin (untouched off the view)
    float p[3];
    bool ok =
        fp::heldPointOnPlane((const float*)(ray + field::pick::origin),
                             (const float*)(ray + field::pick::direction), doorDistance.load(), plane, p);
    if (ok && door) {
        const float* pivot = (const float*)(drag + field::drag::pivot);
        const float dx = p[0] - pivot[0], dz = p[2] - pivot[2];
        ok = dx * dx + dz * dz >= 100.f;
    } // at the pivot the direction is undefined
    if (!ok) {
        if (lastDoorObject == object) {
            memcpy(out, lastDoorPoint, 12);
            return;
        } // keep the last good point (the game's own intersection has no guard)
        originalPlaneHit(out, camera, x, y, plane);
        return; // the first frame of a grab: the game's, at the clicked point
    }
    memcpy(out, p, 12);
    memcpy(lastDoorPoint, p, 12);
    lastDoorObject = object;
    ++doorHolds;
    {
        const float* o = (const float*)(ray + field::pick::origin);
        float s = 0;
        for (int i = 0; i < 3; i++) s += (p[i] - o[i]) * (p[i] - o[i]);
        *(float*)(gameAt(fp::sig::dragHitDistance)) = std::sqrt(s);
    } // the hit distance the game keeps
}
static void planeHit(float* out, uintptr_t camera, int x, int y, const float* plane) {
    planeHitFor((uintptr_t)_ReturnAddress(), out, camera, x, y, plane);
}
static bool onGameThread() {
    const LONG game = InterlockedCompareExchange(&navThread, 0, 0);
    return !game || DWORD(game) == GetCurrentThreadId();
}
static void sendDoorMessage(uintptr_t drag, int message) {
    const uintptr_t ui = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    if (!ui) return;
    __try {
        ((*(unsigned*)(drag + 8) & 0x4000) ? originalDoorHandler : originalHingeHandler)(drag, ui, message);
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
    if (!heldDoorLive() || !gameViewportFocused()) return;
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
// Let go while the wheel pushes: after the game's own release (which stops the body), the body's point history is
// rewritten for the push's speed along the pointer's ray (fp::flingHistory). Only an item this mod was holding.
static void fling(uintptr_t body) {
    const ULONGLONG now = GetTickCount64();
    const float speed = now - pushAt.load() <= fp::flingFreshMs ? pushSpeed.load() : 0.f;
    const float v[3] = {pushDir[0].load() * speed, pushDir[1].load() * speed, pushDir[2].load() * speed};
    if (!readable(body, field::bodyPart::history2 + 48)) return;
    if (fp::flingHistory(v, physicsStep.load(), (const float(*)[3])(body + field::bodyPart::points),
                         (float(*)[3])(body + field::bodyPart::history),
                         (float(*)[3])(body + field::bodyPart::history2)))
        ++flings;
}
static void dragDrop(uintptr_t drag) {
    const uintptr_t ui = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    const bool ownView = viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
                         !InterlockedCompareExchange(&stopping, 0, 0);
    if (ownView && ui && readable(drag, 0x20) &&
        (*(unsigned*)(drag + 8) & (0x4000 | 0x10000)) && *(uintptr_t*)(drag + field::drag::object)) {
        doorDistance = 0;
        lastDoorObject = 0;
        sendDoorMessage(drag, 0x80);
        return;
    }
    uintptr_t body = 0; // captured first: the release clears the drag
    if (ownView && readable(drag, 0x20) && *(unsigned*)(drag + 8) == 0x2000) {
        const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
        if (object && object == holdObject.load() && readable(object, field::sceneNode::bodyPart + 8))
            body = *(uintptr_t*)(object + field::sceneNode::bodyPart);
    }
    originalDragDrop(drag);
    if (body) fling(body);
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
static void cursorPosition(LONG* x, LONG* y) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    if (caller == gameAt(fp::sig::uiPollCursorReturn)) {
        if (!reticleForUi(x, y)) originalCursorPosition(x, y);
        return;
    }
    if (caller != gameAt(fp::sig::crosshairCursorReturn)) {
        originalCursorPosition(x, y);
        return;
    }
    if (!gameplayOwnsPointer()) {
        originalCursorPosition(x, y);
        return;
    }
    CameraGuard guard;
    HWND window = gameWindow();
    RECT rect;
    Aim a;
    AcquireSRWLockShared(&aimLock);
    a = aim;
    ReleaseSRWLockShared(&aimLock);
    if (window && savedCamera && a.valid && a.root == filterRoot && a.head == filterHead &&
        a.root == *(uintptr_t*)(gameAt(fp::sig::playerRoot)) && GetClientRect(window, &rect) &&
        rect.right > 40 && rect.bottom > 40) {
        auto mesh = *(uintptr_t*)(a.root + field::character::sceneNode),
             scene = mesh ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
        if (scene) {
            // Project the aim direction, not a far point anchored at the swaying body (that parallax was most of
            // the jitter); third person aims along the centre ray. The drawn spot is low-passed (35 ms) behind a
            // 1.5 px dead-zone. The camera itself is never smoothed.
            float delta[3];
            for (int i = 0; i < 3; i++) delta[i] = a.point[i] - a.origin[i];
            float px, py;
            bool projected = fp::projectCursor(delta, lastCamera, float(rect.right), float(rect.bottom),
                                               renderedFov, px, py);
            if (viewMode.load() !=
                0) { // third person: the centre, or the free combat crosshair's place in its box
                const bool free = viewMode.load() == 1 && tpAimFree.load();
                px = float(rect.right) * 0.5f + (free ? tpAimX.load() : 0.f);
                py = float(rect.bottom) * 0.5f + (free ? tpAimY.load() : 0.f);
                projected = true;
            }
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
                if (viewMode.load() < 2 &&
                    !fp::crosshairDrawn(combat, swinging.load(), hideSwingCrosshair.load(),
                                        idleCrosshairAlways.load(), markerShown,
                                        hoverOutOfRange.load() &&
                                            GetTickCount64() - hoverRangeAt.load() <= reticleFreshMs)) {
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
    originalCursorPosition(x, y);
    if (hideSwingCrosshair.load() && swinging.load()) {
        HWND w = gameWindow();
        RECT r;
        if (w && GetClientRect(w, &r)) {
            *x = r.right + 256;
            *y = r.bottom + 256;
            ++swingHides;
        }
    } // also when the projection failed
}
// These helpers are called only while cameraLock is owned. Never write a
// remembered native pointer unless the current view still owns that camera.
static void currentCamera(uintptr_t& view, uintptr_t& camera) {
    auto viewport = *(uintptr_t*)(gameAt(fp::sig::mainViewport));
    view = viewport ? *(uintptr_t*)(viewport + field::viewport::view) : 0;
    camera = view ? *(uintptr_t*)(view + field::view::camera) : 0;
}
static void resetViewHistory() {
    InterlockedExchange(&cameraReady, 0);
    nearPlane.reset();
    filterRoot = filterHead = 0;
    releaseInput();
    AcquireSRWLockExclusive(&aimLock);
    aim.valid = false;
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
