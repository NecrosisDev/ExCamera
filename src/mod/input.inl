// Input: the game window procedure (F6-F11, right mouse, raw mouse), mouse-look aim, the input and behaviour hooks, head steadying, movement, the attack cue's state.
// Part of first_person.cpp (one translation unit): included there, in order.
static bool closeViewContext();
static void recordHeldThrow(int wheel, float distance);
static bool manualCombatNow();
static bool bindingContextNow();
static bool nativeSettingsBindingInput(UINT message, WPARAM wp, LPARAM lp);
// Generic modifiers cover either side; explicitly left/right bindings remain
// distinct. Native pollInput slot 6 sets keyWord bit 0x80 for alternate attack.
static bool alternateBindingKey(unsigned key) {
    const unsigned alternate = nativeBinding(6);
    if (!key || key >= 256 || !alternate || alternate >= 256) return false;
    if (key == alternate) return true;
    constexpr unsigned modifiers[][3] = {{VK_MENU, VK_LMENU, VK_RMENU},
                                         {VK_CONTROL, VK_LCONTROL, VK_RCONTROL},
                                         {VK_SHIFT, VK_LSHIFT, VK_RSHIFT}};
    for (const auto& family : modifiers)
        if ((key == family[0] && (alternate == family[1] || alternate == family[2])) ||
            (alternate == family[0] && (key == family[1] || key == family[2])))
            return true;
    return false;
}
// Holding an item (drag 0x2000) while the mod drives the view: the rotate key then turns it.
static bool holdingItem() {
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        viewMode.load() >= 2)
        return false;
    int kind = 0;
    __try {
        kind = dragKind();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kind = 0;
    }
    return kind == 1;
}
// The physical key is decoded independently of today's binding. A rebind
// cannot transfer an older press's release to a different native action.
static int bindingMessageKey(UINT message, WPARAM wp) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        return VK_LBUTTON;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP:
        return VK_RBUTTON;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONUP:
        return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return wp > 0 && wp < 256 ? int(wp) : 0;
    }
    return 0;
}
// Down (1), up (0), or a message for a different physical key (-1).
static int rotateKeyMessage(UINT message, WPARAM wp, int key) {
    if (!key || bindingMessageKey(message, wp) != key) return -1;
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                   message == WM_XBUTTONUP || message == WM_KEYUP || message == WM_SYSKEYUP
               ? 0
               : 1;
}
// A different window procedure was active while this hook session was absent:
// it owned those releases. Ordinary input/profile handoffs keep this ledger.
static void resetBindingWindowOwnership() {
    inputOwner.newWindowSession();
}
// Observe only messages that survived held-item/lean ownership. Suppression
// survives an up so even a fully released overlap cannot replay as a tap.
static void combatMouseMessage(UINT message, WPARAM wp) {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        // wParam describes this queued event. Async state can already belong
        // to the next press and would retroactively consume a released guard's attack.
        const bool overlap = manualCombatNow() && (wp & MK_RBUTTON);
        inputOwner.attackDown(overlap);
    } else if ((message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) && manualCombatNow() &&
               (wp & MK_LBUTTON)) {
        inputOwner.suppressAttack();
    }
}
static void consumeNativeBinding(int key) {
    *(unsigned*)gameAt(fp::sig::keyWord) &= ~actionMask(WPARAM(key));
    if (key == VK_LBUTTON) *(unsigned*)gameAt(fp::sig::buttonWord) &= ~1u;
    if (key == VK_RBUTTON) *(unsigned*)gameAt(fp::sig::buttonWord) &= ~2u;
}
// float atomics have no fetch_add before C++20
static void addTo(std::atomic<float>& a, float v) {
    float o = a.load();
    while (!a.compare_exchange_weak(o, o + v)) {}
}
static SRWLOCK worldPointerLock = SRWLOCK_INIT;
static fp::PointerGesture worldPointer;
static fp::InputStamp worldPointerStamp() {
    fp::InputStamp stamp;
    stamp.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    stamp.head = stamp.root ? *(uintptr_t*)(stamp.root + field::character::headPart) : 0;
    stamp.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
    stamp.mode = viewMode.load();
    stamp.generation = inputOwner.generation();
    stamp.policyRevision = inputOwner.policyRevision();
    return stamp;
}
// Called with the accepted look sample, before a free-aim box consumes it.
static void worldPointerMotion(float dx, float dy) {
    const fp::InputStamp stamp = worldPointerStamp();
    AcquireSRWLockExclusive(&worldPointerLock);
    worldPointer.sync(stamp);
    worldPointer.move(dx, dy);
    ReleaseSRWLockExclusive(&worldPointerLock);
}
struct GuiPointerPresentation {
    bool active = false, read = false;
    LONG x = 0, y = 0;
    uintptr_t gui = 0, viewport = 0, camera = 0, controller = 0, view = 0;
    fp::InputStamp stamp;
    LONG priorX = 0, priorY = 0;
    bool compare = false, rejected = false, nativeOnly = false;
};
static thread_local GuiPointerPresentation guiPointerPresentation;
static WindowInstallation windowInstallationSnapshot() {
    AcquireSRWLockShared(&windowInstallationLock);
    const WindowInstallation result = windowInstallation;
    ReleaseSRWLockShared(&windowInstallationLock);
    return result;
}
static void retireWindowInstallation(const WindowInstallation& owner) {
    AcquireSRWLockExclusive(&windowInstallationLock);
    if (windowInstallation.window == owner.window &&
        windowInstallation.generation == owner.generation)
        windowInstallation.retired = true;
    ReleaseSRWLockExclusive(&windowInstallationLock);
}
static LRESULT CALLBACK windowProc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    const WindowInstallation owner = windowInstallationSnapshot();
    // A foreign/stale direct invocation has no predecessor we can reconstruct.
    // Do not borrow another window's procedure or mutate its input ownership.
    if (owner.window != w || !owner.previous || owner.retired)
        return DefWindowProcW(w, message, wp, lp);
    if (message == WM_NCDESTROY) {
        const LRESULT result = CallWindowProcW(owner.previous, w, message, wp, lp);
        retireWindowInstallation(owner); // exact generation, after native destruction
        return result;
    }
    if (InterlockedCompareExchange(&stopping, 0, 0))
        return CallWindowProcW(owner.previous, w, message, wp, lp);
    // While choosing an ExCamera binding, the native Settings UI owns this
    // single press. Never let F6-F11, mouse buttons or an alternate attack
    // leak through to gameplay or native Settings shortcuts during capture.
    if (nativeSettingsBindingInput(message,wp,lp)) {
        return (message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ||
                message==WM_XBUTTONDBLCLK) ? TRUE : 0;
    }
    // Plain F10 is a system key on Windows; leave Alt shortcuts with Exanima.
    // Consume owned presses, repeats and releases so F10 cannot activate a menu.
    const bool fovKey = wp == VK_F9 || wp == VK_F10 || wp == VK_F11;
    const bool fovMessage = message == WM_KEYDOWN || message == WM_KEYUP ||
                            (wp == VK_F10 && (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP));
    if (InterlockedCompareExchange(&active, 0, 0) && viewMode.load() < 2 && fovKey && fovMessage &&
        !(lp & (1LL << 29))) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lp & (1LL << 30)))
            adjustFov(wp == VK_F10 ? 5 : -5, wp == VK_F11);
        return 0;
    }
    const unsigned released = keyMask(wp);
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && released)
        inputOwner.movementUp(released);
    const int bindingKey = bindingMessageKey(message, wp);
    if (bindingKey) {
        const int edge = rotateKeyMessage(message, wp, bindingKey);
        const bool xbutton =
            message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK || message == WM_XBUTTONUP;
        if (edge == 0 && inputOwner.windowRelease(bindingKey)) return xbutton ? TRUE : 0;
        if (edge == 1) {
            const bool repeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (lp & (1LL << 30));
            if (repeat && inputOwner.windowOwns(bindingKey)) return 0;
            // A new physical down also retires a release lost outside this window.
            if (!repeat) inputOwner.windowBegin(bindingKey);
            const bool canTake = !repeat && bindingContextNow();
            if (canTake && bindingKey == rotateHeldKey.load() && holdingItem()) {
                inputOwner.windowClaim(bindingKey);
                return xbutton ? TRUE : 0;
            }
            if (canTake && trackingEnabled.load() && bindingKey == trackingRecenterKey.load()) {
                inputOwner.windowClaim(bindingKey);
                ++trackingReset;
                return xbutton ? TRUE : 0;
            }
        }
    }
    combatMouseMessage(message, wp);
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP) {
        bool eaten = false;
        rmbEvent(message == WM_RBUTTONUP ? fp::RmbEvent::up : fp::RmbEvent::down,
                 message != WM_RBUTTONUP && rmbEligible(), &eaten);
        if (eaten) return 0;
    }
    // Middle click in gameplay immediately shifts the requested view toward
    // today's cursor ray. Keep the corresponding up owned, even if the mode changes.
    // Native UI and any held-item rotation binding retain first refusal.
    if ((message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) &&
        bindingContextNow() && !holdingItem()) {
        inputOwner.windowClaim(VK_MBUTTON);
        recenterClicks.fetch_add(1);
        return 0;
    }
    // Existing close-view suppression still applies if the pointer is free.
    if (closeViewContext() && (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK)) return 0;
    if (gameplayOwnsPointer()) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) inputOwner.buttonDown(1);
        if (message == WM_RBUTTONDOWN) inputOwner.buttonDown(2);
    }
    if (message == WM_INPUT && InterlockedCompareExchange(&rawRegistered, 0, 0)) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            AcquireSRWLockExclusive(&rawLock);
            const bool owns = InterlockedCompareExchange(&inputEnabled, 0, 0) &&
                              InterlockedCompareExchange(&mouseCaptured, 0, 0) && !cursorFreeNow();
            if (InterlockedCompareExchange(&rawRegistered, 0, 0))
                rawLook.event((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0, raw.data.mouse.lLastX,
                              raw.data.mouse.lLastY, owns);
            ReleaseSRWLockExclusive(&rawLock);
        }
        return CallWindowProcW(owner.previous, w, message, wp, lp); // DefWindowProc cleanup.
    }
    unsigned bit = keyMask(wp);
    if (message == WM_KEYDOWN && bit && InterlockedCompareExchange(&inputEnabled, 0, 0)) {
        inputOwner.movementDown(bit, !(lp & (1LL << 30)));
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && bit)
        inputOwner.movementUp(bit); // with Alt held a release comes as WM_SYSKEYUP
    // Held object: the wheel sets the hold distance and does nothing else meanwhile.
    if (message == WM_MOUSEWHEEL && closeViewContext()) {
        int kind = 0;
        __try {
            kind = dragKind();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            kind = 0;
        }
        if (kind == 1) {
            recordHeldThrow(GET_WHEEL_DELTA_WPARAM(wp), holdTarget.load());
            holdTarget = fp::holdTargetStep(holdTarget.load(), GET_WHEEL_DELTA_WPARAM(wp), fp::holdNear,
                                            float(grabRange.load()));
            ++holdNotches;
            return 0;
        }
        if (kind == 2 && doorDistance.load() > 0 &&
            heldDoorDrag()) { // the wheel's limits are from the eye (in third person the ray starts at the camera)
            const float offset = doorCameraOffset.load(), eye = doorDistance.load() - offset;
            doorDistance = fp::holdTargetStep(eye, GET_WHEEL_DELTA_WPARAM(wp), std::fmin(fp::holdNear, eye),
                                              float(grabRange.load())) +
                           offset;
            ++holdNotches;
            return 0;
        }
        if (*(int*)gameAt(fp::sig::uiOpenCount) <= 0) {
            ++overheadInputsHeld;
            return 0;
        } // the overhead camera's zoom: unseen here, and it would be changed on return
    }
    // F7 stops only when held (fp::stopKeyStep on the worker); never passed on.
    if (wp == VK_F7 && (message == WM_KEYDOWN || message == WM_KEYUP)) {
        if (message == WM_KEYDOWN && !(lp & (1LL << 30))) {
            stopRelease = 0;
            stopPress = GetTickCount64();
        } else if (message == WM_KEYUP && stopPress.load())
            stopRelease = GetTickCount64();
        return 0;
    }
    if (message == WM_KEYDOWN && !(lp & (1LL << 30)) && (wp == VK_F6 || wp == VK_F8 || wp == VK_ESCAPE))
        rmbEvent(fp::RmbEvent::lost, false);
    if (message == WM_KEYDOWN && !(lp & (1LL << 30))) {
        if (wp == VK_F6 && !fp::rearmF6Accepted(GetTickCount64(), rearmedAt.load()))
            return 0; // the polled press that re-armed
        if (wp == VK_F6) { ++inventoryOwnerEpoch; releaseHeldDoor(); }
        if (wp == VK_F6) {
            // F6 cycles first person -> third person -> the game's overhead.
            if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                viewMode.load() == 0) {
                viewMode = 1;
                releaseInput();
                return 0;
            } // same session, a fresh profile/input ownership boundary
            if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                viewMode.load() == 1) { // third -> overhead blends (0.3 s), then hands over
                tpBlendStart = GetTickCount64();
                viewMode = 2;
                releaseInput();
                return 0;
            }
            if (viewMode.load() == 2 && GetTickCount64() - tpBlendStart.load() < 600)
                return 0; // already blending out
            viewMode = 0;
            InterlockedExchange(&active, !InterlockedCompareExchange(&active, 0, 0));
            releaseInput();
            AcquireSRWLockExclusive(&aimLock);
            aim.valid = false;
            ReleaseSRWLockExclusive(&aimLock);
            return 0;
        }
        if (wp == VK_F8) {
            InterlockedExchange(&cursorFree, !InterlockedCompareExchange(&cursorFree, 0, 0));
            releaseInput();
            return 0;
        }
        if (wp == VK_ESCAPE) releaseInput();
    }
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || (message == WM_ACTIVATEAPP && !wp)) {
        ++inventoryOwnerEpoch;
        rmbEvent(fp::RmbEvent::focusLost, false);
        releaseInput();
    }
    return CallWindowProcW(owner.previous, w, message, wp, lp);
}
// A console (EMTK's framework opens one with AllocConsole) reports the game's process too: never the game's window.
static bool consoleWindow(HWND w) {
    wchar_t c[32]{};
    return GetClassNameW(w, c, 32) && !wcscmp(c, L"ConsoleWindowClass");
}
static HWND gameWindow() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId() && !consoleWindow(w) ? w : nullptr;
}
static BOOL CALLBACK findWindow(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && w != noticeWindow && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) &&
        !consoleWindow(w)) {
        *(HWND*)out = w;
        return FALSE;
    }
    return TRUE;
}
static bool playerContext(uintptr_t behavior) {
    if (!InterlockedCompareExchange(&cameraReady, 0, 0) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !gameWindow() ||
        *(int*)(gameAt(fp::sig::uiOpenCount)) > 0)
        return false;
    auto root = *(uintptr_t*)(behavior + field::behaviour::character);
    if (!root || root != *(uintptr_t*)(gameAt(fp::sig::playerRoot))) return false;
    if (*(unsigned*)(root + field::character::lifeFlags) & 0x100 ||
        *(unsigned char*)(behavior + field::behaviour::inactive))
        return false;
    auto view = *(uintptr_t*)(behavior + field::behaviour::view), ui = *(uintptr_t*)(gameAt(fp::sig::uiView)),
         manager = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    return (!ui || ui == view) && (!manager || !*(uintptr_t*)(manager + field::gui::capture) ||
                                   *(uintptr_t*)(manager + field::gui::capture) == view);
}
static bool controls(uintptr_t behavior) {
    return viewMode.load() < 2 && !cursorFreeNow() && playerContext(behavior);
}
// Window messages can precede the render hook's windowFree publication.
// Check the native view/capture owner too before taking a new binding press.
static bool bindingContextNow() {
    __try {
        return gameplayOwnsPointer() && closeViewContext();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Window messages can race scene teardown and native UI ownership changes.
static bool manualCombatNow() {
    if (!base || !InterlockedCompareExchange(&inputEnabled, 0, 0)) return false;
    __try {
        const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot),
                        bh = root ? *(uintptr_t*)(root + field::character::behaviour) : 0;
        return bh && controls(bh) && manualCombat(root, bh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The close view may release its pointer while native controller and GUI
// movement producers still run. This is only a request freeze: the native
// locomotion owner keeps its accepted steps, gait state and physical recovery.
// No Aim snapshot is required when input has just been handed to a window.
static bool movementFrozen(uintptr_t behavior) {
    if (!base || !behavior || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0))
        return false;
    __try {
        const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        if (!root || *(uintptr_t*)(root + field::character::behaviour) != behavior ||
            *(uintptr_t*)(behavior + field::behaviour::character) != root)
            return false;
        if (cursorFreeNow() || !gameWindow()) return true;
        if (!windowsFreeze.load()) return false;
        // Detect a new UI handoff before the render hook publishes windowFree.
        // A description-only popup retains windowFreeStep's settling policy.
        const uintptr_t view = *(uintptr_t*)(behavior + field::behaviour::view),
                        ui = *(uintptr_t*)gameAt(fp::sig::uiView),
                        manager = *(uintptr_t*)gameAt(fp::sig::guiManager);
        return *(int*)gameAt(fp::sig::uiOpenCount) > 0 || (ui && ui != view) ||
               (manager && *(uintptr_t*)(manager + field::gui::capture) &&
                *(uintptr_t*)(manager + field::gui::capture) != view);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false; // uncertain ownership stays native
    }
}
static bool closeViewContext() {
    if (viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0))
        return false;
    const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    const uintptr_t bh = root ? *(uintptr_t*)(root + field::character::behaviour) : 0;
    return bh && playerContext(bh);
}
// Native 0x11EF40 polls both configured camera bindings directly, including the
// body-turn variant. Window-message suppression alone cannot isolate it.
static char (*originalCameraMouse)();
static void (*copyCameraSnapshots)();
static void (*originalCameraOrbit)(uintptr_t, float, float, int);
static int (*originalCameraZoom)(uintptr_t, int);
// Controller paths 0x22E070 / 0x22E130 call these mutators without the
// mouse-mode callback. Their input still belongs to the overhead camera.
static void cameraOrbit(uintptr_t self, float x, float y, int flags) {
    if (!closeViewContext()) originalCameraOrbit(self, x, y, flags);
}
static int cameraZoom(uintptr_t self, int amount) {
    return closeViewContext() ? amount : originalCameraZoom(self, amount);
}
static char cameraMouse() {
    healthHit(fp::HealthHook::cameraMouse);
    if (!closeViewContext()) return originalCameraMouse();
    // End an overhead drag inherited on F6 so returning overhead starts a fresh
    // native gesture. Preserve the native cache publication used by picking.
    *(unsigned char*)gameAt(fp::sig::cameraMouseActive) = 0;
    *(unsigned char*)gameAt(fp::sig::cameraMouseTurn) = 0;
    *(unsigned*)gameAt(fp::sig::keyWord) &= ~0x300u;
    copyCameraSnapshots();
    // 0x263C0 calls the GUI mouse poll only when this callback returns zero.
    // Native orbit returns one after updating GUI coordinates itself. Our
    // camera leaves that work to the GUI, including inventory and free cursor.
    return 0;
}
// Where the physics head faces (its achieved pose's forward axis, head+0x2B8), as the aim's yaw and pitch.
static bool headFacing(uintptr_t root, float& yaw, float& pitch) {
    const uintptr_t head = readable(root, 0x11e8) ? *(uintptr_t*)(root + field::character::headPart) : 0;
    if (!readable(head, 0x2c4)) return false;
    const float* f = (const float*)(head + field::bodyPart::poseForward);
    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
    const float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 1e-4f)) return false;
    yaw = std::atan2(f[0], f[2]);
    pitch = std::asin(std::clamp(f[1] / n, -1.f, 1.f));
    return true;
}
static std::atomic<unsigned> aimHolds{0};
static uintptr_t tpAimSector(uintptr_t head) {
    __try {
        const uintptr_t mesh = *(uintptr_t*)(head + field::bodyPart::meshNode);
        return mesh ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// aimLock is held by the caller. Recheck lifecycle/settings too: a capture can
// be released or a projection changed while the camera's native pick runs.
static bool tpAimRequestCurrent(const TpAimRequest& request) {
    if (!sameTpAimRequest(request, tpAimRequest) || !aim.valid || aim.root != request.root ||
        aim.head != request.head || aim.yaw != request.yaw || aim.pitch != request.pitch ||
        viewMode.load() != 1 || !InterlockedCompareExchange(&inputEnabled, 0, 0) ||
        request.generation != inputOwner.generation() ||
        request.policy != inputOwner.policyRevision() || request.free != tpCombatAimFree.load())
        return false;
    const int thirdFov = tpFov.load();
    if (request.fov != float(thirdFov > 0 ? thirdFov : selectedFov.load())) return false;
    __try {
        if (request.root != *(uintptr_t*)gameAt(fp::sig::playerRoot) ||
            request.worldScene != *(uintptr_t*)gameAt(fp::sig::currentScene) ||
            request.head != *(uintptr_t*)(request.root + field::character::headPart) ||
            request.sector != tpAimSector(request.head))
            return false;
        const uintptr_t bh = *(uintptr_t*)(request.root + field::character::behaviour);
        return bh && controls(bh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// One lock supplies the orientation and box/projection descriptor together.
// No camera lock is acquired from input; nav already owns cameraLock here.
static TpAimRequest captureTpAimRequest(uintptr_t root, uintptr_t head, uintptr_t sector, float& yaw,
                                        float& pitch) {
    TpAimRequest request;
    AcquireSRWLockShared(&aimLock);
    if (aim.valid && aim.root == root && aim.head == head) {
        yaw = aim.yaw;
        pitch = aim.pitch;
        if (tpAimRequest.sector == sector && tpAimRequestCurrent(tpAimRequest)) request = tpAimRequest;
    }
    ReleaseSRWLockShared(&aimLock);
    return request;
}
static float heldAimGain(uintptr_t root, float width, float height);
static void updateAim(uintptr_t behavior) {
    if (!controls(behavior)) {
        releaseInput();
        inputClock = ~0u;
        return;
    }
    bool freeCursor = cursorFreeNow() != 0;
    InterlockedExchange(&inputEnabled, freeCursor ? 0 : 1);
    auto root = *(uintptr_t*)(behavior + field::behaviour::character);
    unsigned clock = *(unsigned*)(gameAt(fp::sig::gameClock));
    if (clock == inputClock) return;
    float dt = inputClock == ~0u ? 0.016f : float(clock - inputClock) * .001f;
    if (dt > 0.1f) dt = 0.1f;
    inputClock = clock;
    auto head = *(uintptr_t*)(root + field::character::headPart);
    if (!head) {
        releaseInput();
        return;
    }
    HWND w = gameWindow();
    RECT rect;
    POINT cursor, center;
    if (!w || !GetClientRect(w, &rect) || !GetCursorPos(&cursor)) {
        releaseInput();
        return;
    }
    center = {rect.right / 2, rect.bottom / 2};
    ClientToScreen(w, &center);
    // Centre off every monitor (window partly off-screen): recentring cannot work.
    if (!MonitorFromPoint(center, MONITOR_DEFAULTTONULL)) {
        releaseInput();
        return;
    }
    const float manipulationGain = freeCursor ? 1.f : heldAimGain(root, float(rect.right), float(rect.bottom));
    AcquireSRWLockExclusive(&aimLock);
    auto physical = (float*)(head + field::bodyPart::pose);
    auto anchor = (float*)(root + field::character::position);
    if (!aim.valid || aim.root != root || aim.head != head) {
        const bool entry = aim.entryPending && aim.root == root && aim.head == head;
        const float entryYaw = aim.entryYaw;
        InterlockedExchange(&mouseCaptured, 0);
        tpAimRequest.valid = false;
        tpAimSet = false;
        aim = {};
        aim.root = root;
        aim.head = head;
        auto direction = (float*)(root + field::character::facing);
        aim.yaw = atan2f(direction[0], direction[2]);
        aim.targetYaw = aim.yaw;
        if (entry) {
            aim.yaw = aim.targetYaw = entryYaw;
            aim.pitch = aim.targetPitch = 0.f;
        } else if (viewRoot.load() == root && InterlockedCompareExchange(&cameraReady, 0, 0)) {
            const float y = viewYaw.load(), p = viewPitch.load(); // continue from the view on screen
            if (std::isfinite(y) && std::isfinite(p)) {
                aim.yaw = aim.targetYaw = y;
                aim.pitch = aim.targetPitch = p > 1.15f ? 1.15f : (p < -1.15f ? -1.15f : p);
            }
        }
        fp::eyePoint(physical, aim.origin, float(eyeSetback.load()));
        for (int a = 0; a < 3; a++) aim.anchor[a] = anchor[a];
    }
    // Independent oval aim regions: a radial soft shoulder transfers turning gradually.
    // Recapture/mode/size changes reset the offset so an old region cannot shove the view.
    static int boxMode = -1, previousBoxX = -1, previousBoxY = -1;
    const int mode = viewMode.load();
    const bool combat = *(unsigned char*)(root + field::character::combatStance) != 0;
    const unsigned generation = inputOwner.generation(), policy = inputOwner.policyRevision();
    if (aim.swingView != mode || aim.swingCombat != combat || aim.swingGeneration != generation ||
        aim.swingPolicy != policy) {
        aim.swing = {};
        aim.swingView = mode;
        aim.swingCombat = combat;
        aim.swingGeneration = generation;
        aim.swingPolicy = policy;
    }
    const bool freeAim = mode == 0 || (mode == 1 && tpCombatAimFree.load());
    const int boxX = mode == 0 ? fpDeadzoneX.load() : tpDeadzoneX.load(),
              boxY = mode == 0 ? fpDeadzoneY.load() : tpDeadzoneY.load();
    if (!freeAim || boxMode != mode || boxX != previousBoxX || boxY != previousBoxY ||
        !InterlockedCompareExchange(&mouseCaptured, 0, 0))
        tpAimBox = fp::AimBox{};
    if (boxMode != mode) tpAimSet = false;
    boxMode = mode;
    previousBoxX = boxX;
    previousBoxY = boxY;
    if (!freeCursor && InterlockedCompareExchange(&mouseCaptured, 0, 0)) {
        float dx = float(cursor.x - center.x), dy = float(cursor.y - center.y);
        if (InterlockedCompareExchange(&rawRegistered, 0, 0)) {
            long rx = 0, ry = 0;
            AcquireSRWLockExclusive(&rawLock);
            const bool relative = InterlockedCompareExchange(&rawRegistered, 0, 0) && rawLook.take(rx, ry);
            ReleaseSRWLockExclusive(&rawLock);
            if (relative) {
                dx = float(rx);
                dy = float(ry);
            }
        }
        if (heldRotating.load()) {
            addTo(heldTurnX, float(dx));
            addTo(heldTurnY, float(dy));
            dx = dy = 0;
        } // turning a held item
        worldPointerMotion(dx, dy);
        if (combat) aim.swing.move(dx, dy, float(rect.right));
        // Gesture thresholds and explicit item rotation consume physical motion.
        // Only look/aim intent resists an off-centre held object.
        dx *= manipulationGain;
        dy *= manipulationGain;
        if (freeAim) {
            fp::aimBoxStep(tpAimBox, dx, dy, float(rect.right) * float(boxX) * 0.01f,
                           float(rect.bottom) * float(boxY) * 0.01f, aimEdgeSoftness.load());
        }
        float yawStep = -float(dx) * sensitivity,
              pitchStep = -float(dy) * sensitivity * (invertY ? -1.f : 1.f);
        if (viewMode.load() == 0) {
            float headYaw = 0, headPitch = 0;
            if (headFacing(root, headYaw, headPitch)) {
                const int s = flickCatchup.load();
                yawStep *= fp::flickGain(std::remainder(aim.targetYaw - headYaw, 6.28318530718f), yawStep, s);
                pitchStep *= fp::flickGain(aim.targetPitch - headPitch, pitchStep, s);
            }
        }
        aim.targetYaw = std::remainder(aim.targetYaw + yawStep, 6.28318530718f);
        aim.targetPitch += pitchStep;
    }
    if (aim.targetPitch > 1.15f) aim.targetPitch = 1.15f;
    if (aim.targetPitch < -1.15f) aim.targetPitch = -1.15f;
    // Requested combat intent is immediate; the physical head/weapon/body
    // still achieve it through native dynamics. Exploration retains its filter.
    aim.yaw = combat ? aim.targetYaw : fp::approachAngle(aim.yaw, aim.targetYaw, dt);
    aim.pitch = combat ? aim.targetPitch : fp::approachAngle(aim.pitch, aim.targetPitch, dt);
    // Rebase the ACTUAL aim ray, after exploration filtering. If only the target
    // direction were changed, the head filter would move the world-space point
    // away from the cursor while the onscreen reticle jumped to centre.
    const bool recenterNow = recenterClicks.exchange(0) != 0;
    if (freeAim && !freeCursor && InterlockedCompareExchange(&mouseCaptured, 0, 0) &&
        !heldRotating.load() && (recenterNow || aimAutoRecenter.load())) {
        const int thirdFov = tpFov.load();
        const float fov = float(mode == 1 && thirdFov > 0 ? thirdFov : selectedFov.load());
        float centeredYaw = aim.yaw, centeredPitch = aim.pitch;
        if (fp::recenterAimOnCursor(tpAimBox, centeredYaw, centeredPitch,
                                    float(rect.right), float(rect.bottom), fov, dt, recenterNow)) {
            aim.yaw = aim.targetYaw = centeredYaw;
            aim.pitch = aim.targetPitch = centeredPitch;
        }
    }
    tpAimX = tpAimBox.x;
    tpAimY = tpAimBox.y;
    tpAimFree = freeAim;
    float orientation[12]{}, direction[3];
    fp::basis(aim.yaw, aim.pitch, orientation);
    const int thirdFov = tpFov.load();
    const float aimFov = float(mode == 1 && thirdFov > 0 ? thirdFov : selectedFov.load());
    fp::cursorRay(orientation, freeAim ? tpAimBox.x : 0.f, freeAim ? tpAimBox.y : 0.f, float(rect.bottom),
                  aimFov, direction);
    aim.rayYaw = fp::cursorHeading(orientation, direction);
    aim.rayValid = true;
    // The ray starts at today's eye, not a mount cached before a crouch/lean/pitch change.
    fp::eyePoint(physical, aim.origin, float(eyeSetback.load()));
    for (int a = 0; a < 3; a++) {
        aim.anchor[a] = anchor[a];
        aim.point[a] = aim.origin[a] + direction[a] * 1000.f;
    }
    TpAimRequest request;
    request.root = root;
    request.head = head;
    request.sector = mode == 1 ? tpAimSector(head) : 0;
    request.worldScene = *(uintptr_t*)gameAt(fp::sig::currentScene);
    request.generation = inputOwner.generation();
    request.policy = inputOwner.policyRevision();
    request.yaw = aim.yaw;
    request.pitch = aim.pitch;
    request.x = freeAim ? tpAimBox.x : 0.f;
    request.y = freeAim ? tpAimBox.y : 0.f;
    request.fov = aimFov;
    request.width = rect.right;
    request.height = rect.bottom;
    request.free = freeAim;
    request.valid = mode == 1 && request.sector && request.worldScene && request.width > 40 &&
                    request.height > 40 && std::isfinite(request.yaw) && std::isfinite(request.pitch) &&
                    std::isfinite(request.x) && std::isfinite(request.y) && std::isfinite(request.fov) &&
                    request.fov > 1.f && request.fov < 170.f;
    request.sequence = tpAimRequest.sequence + (sameTpAimGeometry(request, tpAimRequest) ? 0 : 1);
    tpAimRequest = request;
    if (tpAimSet && sameTpAimRequest(tpAimSample, request))
        memcpy(aim.point, tpAimLocal, 12);
    else
        tpAimSet = false;
    // A cached world hit may refine the point only for matching input geometry.
    // Body/attack heading always retains this input sample's freshly computed ray.
    aim.attentionStamp = {root, head, request.worldScene, mode, generation, policy};
    aim.valid = true;
    ++aim.revision;
    ReleaseSRWLockExclusive(&aimLock);
    if (!freeCursor) {
        SetCursorPos(center.x, center.y);
        InterlockedExchange(&mouseCaptured, 1);
        if (!InterlockedCompareExchange(&cursorClipped, 0, 0)) {
            RECT clip = rect;
            MapWindowPoints(w, nullptr, (POINT*)&clip, 2);
            if (ClipCursor(&clip)) InterlockedExchange(&cursorClipped, 1);
        }
    } else
        InterlockedExchange(&mouseCaptured, 0);
    ++controlCalls;
}
// UI cursor hand-off: while a game window (inventory, containers, sheets,
// dialogue, menus: UI counter 0x499860 or another UI view) or the F8 cursor owns the mouse, the
// character keeps the last first-person aim instead of the native cursor-aim, so moving the
// mouse in the window does not turn it. Look itself is already frozen (no mouse delta is read
// without capture); on close the first frame recaptures without a delta.
static bool heldAim(uintptr_t behavior, Aim& a) {
    if (viewMode.load() >= 2 || !InterlockedCompareExchange(&cameraReady, 0, 0) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0))
        return false;
    const uintptr_t root = *(uintptr_t*)(behavior + field::behaviour::character);
    if (!root || root != *(uintptr_t*)(gameAt(fp::sig::playerRoot))) return false;
    // A window is open when the UI counter is up, or another UI view than the player's owns the
    // screen (live: the abilities (K) and skill-tree (P) screens leave the counter at 0
    // and switch the UI view 0x49AA60 away from the behaviour's view +0x898).
    const uintptr_t view = *(uintptr_t*)(behavior + field::behaviour::view),
                    ui = *(uintptr_t*)(gameAt(fp::sig::uiView)),
                    manager = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    const bool otherView = (ui && ui != view) || (manager && *(uintptr_t*)(manager + field::gui::capture) &&
                                                  *(uintptr_t*)(manager + field::gui::capture) != view);
    if (!(*(int*)(gameAt(fp::sig::uiOpenCount)) > 0 || otherView || cursorFreeNow() || !gameWindow()))
        return false;
    AcquireSRWLockShared(&aimLock);
    a = aim;
    ReleaseSRWLockShared(&aimLock);
    if (a.valid && a.root == root && a.head && a.head == *(uintptr_t*)(root + field::character::headPart)) {
        ++aimHolds;
        return true;
    }
    return false;
}
static bool getAim(uintptr_t behavior, Aim& a) {
    if (!controls(behavior)) return false;
    AcquireSRWLockShared(&aimLock);
    a = aim;
    ReleaseSRWLockShared(&aimLock);
    return a.valid && a.root == *(uintptr_t*)(behavior + field::behaviour::character) && a.head &&
           a.head == *(uintptr_t*)(a.root + field::character::headPart);
}
// Native controller providers flatten camera UP for forward movement. In a
// level/upward close view that collapses/reverses. Replace only the two movement
// composition operands per provider; native sampling, scalars and aim stay native.
using AnalogVectorOp = void (*)(float*, const float*, float);
static AnalogVectorOp originalAnalogScale, originalAnalogAdd;
// The native placement search was designed for an overhead surface query:
// target - 600*ray, then up to 1000 units forward. A close-view hold instead
// searches only from its real pick origin to its requested cursor target.
struct HeldPlacement {
    uintptr_t object = 0, viewport = 0, query = 0, root = 0, scene = 0, sector = 0;
    ULONGLONG generation = 0;
    int mode = 0;
    float distance = 0.f, values[6]{}, sectorOrigin[3]{};
    bool blocked = false;
};
static thread_local HeldPlacement pendingPlacement{}, boundedPlacement{}, updatePlacement{};
static thread_local unsigned heldUpdateDepth = 0;
static thread_local HeldPlacement acceptedHeldPlacement{};
static bool heldRecoveryPlacement(uintptr_t object, uintptr_t viewport, uintptr_t query,
                                  HeldPlacement& proposal);
static constexpr unsigned heldCollisionQueryBit = 0x200000;
static constexpr float nativeHeldSweepSpan = 1000.f;
static float heldPlacementStart(uintptr_t caller, const float* origin, const float* direction, float scale) {
    if (boundedPlacement.object &&
        (caller == gameAt(fp::sig::heldStartReturn) || caller == gameAt(fp::sig::heldChildStartReturn) ||
         caller == gameAt(fp::sig::inventoryStartReturn) ||
         caller == gameAt(fp::sig::inventoryChildStartReturn)) &&
        !memcmp(origin, boundedPlacement.values, 12) && !memcmp(direction, boundedPlacement.values + 3, 12))
        return -boundedPlacement.distance;
    return scale;
}
static void analogVectorFor(uintptr_t caller, bool forward, float* out, const float* axis, float amount) {
    const bool movement = forward ? caller == gameAt(fp::sig::controllerForwardReturn) ||
                                        caller == gameAt(fp::sig::xinputForwardReturn)
                                  : caller == gameAt(fp::sig::controllerRightReturn) ||
                                        caller == gameAt(fp::sig::xinputRightReturn);
    float planar[3];
    if (movement && out == (float*)gameAt(fp::sig::analogStick)) {
        __try {
            const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
            const uintptr_t bh = root ? *(uintptr_t*)(root + field::character::behaviour) : 0;
            Aim a;
            if (bh && getAim(bh, a) && std::isfinite(a.yaw)) {
                planar[0] = forward ? std::sin(a.yaw) : -std::cos(a.yaw);
                planar[1] = 0.f;
                planar[2] = forward ? std::cos(a.yaw) : std::sin(a.yaw);
                axis = planar;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Owner disappeared: retain the original operand.
        }
    }
    (forward ? originalAnalogAdd : originalAnalogScale)(out, axis, amount);
}
static void analogScale(float* out, const float* axis, float amount) {
    analogVectorFor((uintptr_t)_ReturnAddress(), false, out, axis, amount);
}
static void analogAdd(float* out, const float* axis, float amount) {
    analogVectorFor((uintptr_t)_ReturnAddress(), true, out, axis, amount);
}
// The body's heading on the ground is the mouse yaw itself. It was the direction from the body to the aim
// point, which in third person is where the screen centre meets the world: looking steeply down from
// behind the shoulder that point lies behind the body, and the body turned round to face the camera.
static void headingToPoint(const Aim& a, float* d) {
    const float yaw = a.rayValid ? a.rayYaw : a.yaw;
    d[0] = std::sin(yaw);
    d[1] = 0;
    d[2] = std::cos(yaw);
}
static void gazeToPoint(const Aim& a, float* d) {
    // Inside the first-person box the head keeps the look direction while body/weapon aim follows the cursor.
    // Otherwise aiming the head at that cursor would move the view before the box's edge.
    if (viewMode.load() == 0) {
        d[0] = std::sin(a.yaw) * std::cos(a.pitch);
        d[1] = std::sin(a.pitch);
        d[2] = std::cos(a.yaw) * std::cos(a.pitch);
        return;
    }
    auto head = *(uintptr_t*)(a.root + field::character::headPart);
    if (!head) {
        d[0] = sinf(a.yaw) * cosf(a.pitch);
        d[1] = sinf(a.pitch);
        d[2] = cosf(a.yaw) * cosf(a.pitch);
        return;
    }
    auto h = (float*)(head + field::bodyPart::pose);
    float eye[3];
    fp::eyePoint(h, eye, float(eyeSetback.load()));
    for (int i = 0; i < 3; i++) d[i] = a.point[i] - eye[i];
    normalize(d);
}
struct AttackHeadingPresentation {
    uintptr_t behavior = 0;
    float heading = 0, presented = 0;
    bool borrowed = false;
};
static thread_local AttackHeadingPresentation attackHeadingPresentation;
static float requestedAttackHeading(const Aim& a, float heading, float body, bool wideThreshold);
// Native C1CF0 polls first, then classifies a new press before cursorAim.
// Publish this sample's heading here; cursorAim alone advances its native filter.
static void prepareAttackHeading(uintptr_t bh) {
    Aim a;
    if (!getAim(bh, a)) return;
    const float heading = a.rayValid ? a.rayYaw : a.yaw;
    if (!std::isfinite(heading)) return;
    float presented = heading;
    bool borrowed = false;
    if (attackHeadingPresentation.behavior == bh && ordinaryCombat(a.root, bh) &&
        (*(unsigned*)gameAt(fp::sig::buttonWord) & 1u) &&
        !*(unsigned char*)(bh + field::behaviour::pressBlocked) &&
        !(*(unsigned*)(bh + field::behaviour::pressFlags) & 1u)) {
        const float body = *(float*)(a.root + field::character::bodyHeading);
        if (std::isfinite(body)) {
            const bool wide = *(int*)(a.root + field::character::gripFlags) != 0;
            presented = requestedAttackHeading(a, heading, body, wide);
            if (attackSideKey && (keyState(attackSideKey) & 0x8000)) {
                presented = fp::flippedSideHeading(presented, body, wide);
                ++sideFlips;
            }
            borrowed = presented != heading;
        }
    }
    *(float*)(bh + field::behaviour::aimHeading) = presented;
    if (attackHeadingPresentation.behavior == bh)
        attackHeadingPresentation = {bh, heading, presented, borrowed};
}
static fp::InputContext<field::bindings::count> inputContext(uintptr_t root, uintptr_t bh) {
    fp::InputContext<field::bindings::count> context;
    context.root = root;
    context.head = root ? *(uintptr_t*)(root + field::character::headPart) : 0;
    context.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
    context.mode = viewMode.load();
    context.combat = root && *(unsigned char*)(root + field::character::combatStance);
    context.ordinary = bh && ordinaryCombat(root, bh);
    context.policy = inputOwner.policyRevision();
    for (size_t i = 0; i < context.bindings.size(); ++i)
        context.bindings[i] = nativeBinding(i);
    return context;
}
static bool heldBinding(fp::HeldBinding role, int key, bool eligible, const InputOwner::Sample& sample) {
    const bool down = key > 0 && key < 256 && (keyState(key) & 0x8000);
    return inputOwner.held(role, key, down, eligible, sample);
}
static void sampleHeldBindings(bool owned, const InputOwner::Sample& sample) {
    const int rotate = rotateHeldKey.load();
    const bool turning = heldBinding(fp::HeldBinding::rotate, rotate, owned && holdingItem(), sample);
    heldRotating = turning;
    if (turning) inputOwner.consumeBinding(rotate);
    if (trackingEnabled.load() && inputOwner.windowOwns(trackingRecenterKey.load()))
        inputOwner.consumeBinding(trackingRecenterKey.load());
    // Keep an old physical press masked across a rebind or gameplay release.
    // Observing release retires polling ownership, not the WndProc's owed up.
    for (int bound = 1; bound < 256; ++bound)
        if (inputOwner.bindingOwned(bound) && inputOwner.maskBinding(bound, (keyState(bound) & 0x8000) != 0)) {
            consumeNativeBinding(bound);
            // A side-specific owned hold also masks its generic native modifier.
            if (alternateBindingKey(unsigned(bound))) *(unsigned*)gameAt(fp::sig::keyWord) &= ~0x80u;
        }
}
static void input() {
    healthHit(fp::HealthHook::pollInput);
    originalInput();
    auto root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    auto bh = root ? *(uintptr_t*)(root + field::character::behaviour) : 0;
    // Before ownership is decided, so a right mouse still physically held is blocked by the hand-off
    // rather than passed to the game. A release that never reached the window (let go outside it) ends
    // a hold too.
    if (InterlockedCompareExchange(&rmbFree, 0, 0) &&
        (!rmbEligible() || (rmbMode.load() == 1 && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)))) {
        InterlockedExchange(&rmbFree, 0);
        releaseInput();
    }
    const bool owned = bh && controls(bh);
    auto buttons = (unsigned*)(gameAt(fp::sig::buttonWord));
    const InputOwner::Sample sample = inputOwner.beginSample(inputContext(root, bh), owned, *buttons);
    if (owned) {
        // Movement only while its key is physically down (a tap within the frame still counts): a release that
        // never reached the window (an overlay ate it, focus moved between frames) left a held bit set, and
        // the character walked on by itself.
        const unsigned physical = physicalMovement();
        unsigned& keyWord = *(unsigned*)(gameAt(fp::sig::keyWord));
        keyWord = inputOwner.movementSample(keyWord, physical, movementBits);
        *buttons = inputOwner.bufferedButtons(sample);
    } else
        inputOwner.discardButtons();
    // Freeze each gameplay movement producer after polling and before native
    // behavior consumes it. 0xBFEA0 can assert moveIntent from analogStick even
    // when the keyboard is neutral; 0x11F610 can have cached cursor-walk intent.
    // Raw controller/UI channels and accepted native motion remain untouched.
    __try {
        if (movementFrozen(bh)) {
            *(unsigned*)gameAt(fp::sig::keyWord) &= ~movementBits;
            *(unsigned*)gameAt(fp::sig::buttonWord) = 0;
            float* stick = (float*)gameAt(fp::sig::analogStick);
            stick[0] = stick[1] = stick[2] = 0;
            *(unsigned char*)(bh + field::behaviour::moveIntent) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Scene teardown can invalidate the former controlled actor.
    }
    sampleHeldBindings(owned, sample);
    combatInput(root, bh, owned, sample);
    if (owned) prepareAttackHeading(bh);
    duckKey = (*(unsigned*)gameAt(fp::sig::keyWord) & duckKeyBit) != 0;
}
static void cursorAim(uintptr_t behavior) {
    Aim a;
    if (!getAim(behavior, a) && !heldAim(behavior, a)) {
        originalCursor(behavior);
        return;
    }
    auto root = a.root;
    float d[3];
    headingToPoint(a, d);
    memcpy((void*)(root + field::character::aimDirection), d, 12);
    *(float*)(behavior + field::behaviour::aimHeading) =
        ((float (*)(float*))(gameAt(fp::sig::headingFromDirection)))(d);
    ((void (*)(uintptr_t, float*))(gameAt(fp::sig::aimFilterPush)))(behavior + field::behaviour::aimFilter,
                                                                    d);
    ((void (*)(uintptr_t))(gameAt(fp::sig::aimFilterUpdate)))(behavior + field::behaviour::aimFilter);
    *(float*)(root + field::character::gazeDistance) = 1000.f;
    gazeToPoint(a, d);
    memcpy((void*)(root + field::character::gaze), d, 12);
}
static bool nativeAimPoint(uintptr_t behavior, float* out, const Aim& a);
static void nativePointUnscoped(uintptr_t behavior, float* out);
static void point(uintptr_t behavior, float* out) {
    Aim a;
    if (!getAim(behavior, a) && !heldAim(behavior, a)) {
        nativePointUnscoped(behavior, out);
        return;
    }
    if (nativeAimPoint(behavior, out, a)) {
        ++pointCalls;
        return;
    }
    // UI/held aim and an unavailable current camera retain the existing finite
    // handoff target. Ordinary owned aiming runs the native provider above.
    memcpy(out, a.point, 12);
    memcpy((void*)(behavior + field::behaviour::aimPoint), out, 12);
    ++pointCalls;
}
// Head stabilisation around the game's own head-target build (fp::steadySlackCap): the head part's
// slack cap is lowered for this call only and put back at once, so nothing persists and a park, F7 or
// another character sees the game's value. Turning still drops the slack to 0 (the game's rule), and
// hits still loosen the head (every drive term is scaled by balance).
static unsigned steadyCalls = 0;
static float lastHeadRoll = 0; // degrees, the last build call's
// put back in a __finally (also if the build call unwinds), and only while the cap
// still holds the value written here (a value the game wrote during the call is left as it is).
static void steadyHeadAround(uintptr_t root, void (*build)(uintptr_t, float, unsigned char), float time,
                             unsigned char mode) {
    const int strength = headSteady.load();
    const uintptr_t head = readable(root, 0x11e8) ? *(uintptr_t*)(root + field::character::headPart) : 0;
    float* cap = strength > 0 && readable(head, 0x780) ? (float*)(head + field::bodyPart::slackCap) : nullptr;
    const float kept = cap ? *cap : 0.f;
    if (cap && !(std::isfinite(kept) && kept > 0.f && kept <= 4.f))
        cap = nullptr; // not the value we know: leave it
    // roll comfort: beyond 5 degrees of head roll the cap is lowered further (fp::rollSlackScale).
    float roll = 0.f;
    {
        const uintptr_t mesh = readable(head, 0x20) ? *(uintptr_t*)(head + field::bodyPart::meshNode) : 0;
        if (cap && readable(mesh, 0x70)) {
            float m[9];
            memcpy(m, (const void*)(mesh + field::sceneNode::transform), 36);
            bool ok = true;
            for (float v : m)
                if (!std::isfinite(v)) ok = false;
            if (ok) roll = fp::headRollDegrees(m);
        }
    }
    lastHeadRoll = roll;
    float wobble = 1.f;
    {
        static fp::HeadWobble seen;
        static uintptr_t seenHead = 0;
        const uintptr_t mesh = readable(head, 0x20) ? *(uintptr_t*)(head + field::bodyPart::meshNode) : 0;
        if (head != seenHead) {
            seen = fp::HeadWobble();
            seenHead = head;
        }
        if (cap && readable(mesh, 0x70)) {
            float m[9];
            memcpy(m, (const void*)(mesh + field::sceneNode::transform), 36);
            const float* g = (const float*)(root + field::character::gaze);
            wobble = fp::wobbleSlackScale(seen, fp::headAngles(m), fp::gazeAngles(g), headSteadyRoll.load(),
                                          headSteadyPitch.load(), headSteadyYaw.load());
        }
    }
    const float lowered = cap ? fp::steadySlackCap(kept, strength) * fp::rollSlackScale(roll) * wobble : 0.f;
    if (cap) {
        *cap = lowered;
        ++steadyCalls;
    }
    __try {
        build(root, time, mode);
    } __finally {
        if (cap && *cap == lowered) *cap = kept;
    }
}
// Crouch in exploration (fp::crouchStep; [Input] Crouch). The game's duck (its duck key bit 0x10 in the key
// mask) sets the duck amount root+0x100C to 1 only while standing still, and it decays 0.02 a physics step;
// while it is up the game lowers the pelvis, the base height (x0.7) and the walk speed and step on its own.
// The crouch holds it at 1 every step, before the pose build that reads it.
static constexpr float crouchHeadroom =
    70.f; // cm above the head that must be clear to stand up (the head rises about this much)
// Spine follows aim: 0x1760F0 consumes each part's extra quaternion into its
// target matrix during this call. Borrow those inputs only for that build, then
// restore the exact previous quaternion and only the flag bit we changed.
static std::atomic<int> spineFollow{60};
static std::atomic<unsigned> spineBendTicks{0};
static constexpr unsigned partExtraRotation = 0x4000;
static bool bodyCalm(uintptr_t root, uintptr_t head);
struct SpinePoseBorrow {
    uintptr_t part = 0, root = 0;
    unsigned flag = 0;
    float kept[4]{}, written[4]{};
};
static void borrowSpinePart(uintptr_t root, uintptr_t part, const float* quat, SpinePoseBorrow& b) {
    if (!readable(part, 0x600) || *(uintptr_t*)(part + field::bodyPart::character) != root) return;
    b.root = root;
    b.flag = *(unsigned*)(part + field::bodyPart::flags) & partExtraRotation;
    memcpy(b.kept, (const void*)(part + field::bodyPart::extraRotation), 16);
    if (b.flag) {
        if (!fp::composeSpineRotation(quat, b.kept, b.written)) return;
    } else
        memcpy(b.written, quat, 16);
    b.part = part;
    memcpy((void*)(part + field::bodyPart::extraRotation), b.written, 16);
    *(unsigned*)(part + field::bodyPart::flags) |= partExtraRotation;
}
static void restoreSpinePart(const SpinePoseBorrow& b) {
    if (!b.part || !readable(b.part, 0x600) ||
        *(uintptr_t*)(b.part + field::bodyPart::character) != b.root) return;
    auto q = (float*)(b.part + field::bodyPart::extraRotation);
    auto flags = (unsigned*)(b.part + field::bodyPart::flags);
    // A native replacement is now its own value. Keep it, as with slackCap.
    if (memcmp(q, b.written, 16) != 0) return;
    memcpy(q, b.kept, 16);
    if (*flags & partExtraRotation) *flags = (*flags & ~partExtraRotation) | b.flag;
}
static void spineBendStep(uintptr_t root, SpinePoseBorrow (&borrowed)[2]) {
    const uintptr_t head = readable(root, 0x11e8) ? *(uintptr_t*)(root + field::character::headPart) : 0;
    const uintptr_t chest = readable(root, 0xd50) ? *(uintptr_t*)(root + field::character::chestPart) : 0;
    const uintptr_t rootPart = readable(chest, 0x430) ? *(uintptr_t*)(chest + field::bodyPart::rootPart) : 0,
                    lower = readable(chest, 0x430) ? *(uintptr_t*)(chest + field::bodyPart::parentPart) : 0;
    const uintptr_t parts[2] = {chest && chest != rootPart ? chest : 0,
                                lower && lower != rootPart ? lower : 0};
    const bool eligible = InterlockedCompareExchange(&active, 0, 0) &&
                          !InterlockedCompareExchange(&stopping, 0, 0) &&
                          InterlockedCompareExchange(&cameraReady, 0, 0) && viewMode.load() < 2 && head &&
                          bodyCalm(root, head);
    // Native attacks already consume their target through C0790/166580. Head
    // gaze is independent of that target in free aim; do not add an automatic
    // torso correction in combat.
    const int follow =
        eligible && !*(unsigned char*)(root + field::character::combatStance) ? spineFollow.load() : 0;
    const bool on = eligible && follow > 0;
    const float bend =
        follow > 0 ? fp::spineBend((const float*)(root + field::character::gaze), follow) : 0.f;
    for (int i = 0; i < 2; ++i) {
        float q[4];
        const float share = i == 0 ? 1.f - fp::spineLowerShare : fp::spineLowerShare;
        const uintptr_t parent =
            parts[i] && readable(parts[i], 0x430) ? *(uintptr_t*)(parts[i] + field::bodyPart::parentPart) : 0;
        const bool apply =
            on && parts[i] && readable(parts[i], 0x2d0) && readable(parent, 0x2d0) &&
            fp::spineBendQuat((const float*)(parts[i] + field::bodyPart::pose),
                             (const float*)(parent + field::bodyPart::pose),
                             (const float*)(root + field::character::gaze), bend * share, q);
        if (apply) borrowSpinePart(root, parts[i], q, borrowed[i]);
    }
    if (on) ++spineBendTicks;
}
// Increment before any stop/eligibility check: a callback already admitted when
// hooks are disabled must finish restoring its borrowed fields before teardown
// may report completion. Late callbacks see stopping and cannot borrow fields.
static volatile LONG poseBuildsInFlight = 0;
static bool awaitPoseBuilders(DWORD timeout = 2000) {
    const ULONGLONG started = GetTickCount64();
    while (InterlockedCompareExchange(&poseBuildsInFlight, 0, 0)) {
        if (GetTickCount64() - started >= timeout) return false;
        Sleep(1); // called without cameraLock, after native hooks are disabled
    }
    return true;
}
static void poseBuilderOwned(uintptr_t root, float time, unsigned char mode) {
    SpinePoseBorrow borrowed[2]{};
    uintptr_t* contextual = nullptr;
    uintptr_t savedContext = 0;
    __try {
        if (root == *(uintptr_t*)gameAt(fp::sig::playerRoot)) {
            ++playerSteps;
            if (InterlockedCompareExchange(&active, 0, 0) &&
                !InterlockedCompareExchange(&stopping, 0, 0) &&
                InterlockedCompareExchange(&cameraReady, 0, 0) && viewMode.load() < 2) {
                // The render request belongs to one character/head/scene. Native combat or
                // recovery may start between that frame and this physical pose build.
                if (crouchAppliesTo(root)) {
                    *(float*)(root + field::character::duckAmount) = 1.f;
                    ++crouchTicks;
                }
            }
            // Recheck the live character, head and stance for this physical pose build.
            __try {
                spineBendStep(root, borrowed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        const auto bh = *(uintptr_t*)(root + field::character::behaviour);
        Aim a;
        if (!bh || !getAim(bh, a))
            originalPose(root, time, mode);
        else {
            contextual = (uintptr_t*)(root + field::character::contextualTarget);
            savedContext = *contextual;
            *contextual = 0;
            float d[3];
            gazeToPoint(a, d);
            memcpy((void*)(root + field::character::gaze), d, 12);
            steadyHeadAround(root, originalPose, time, mode);
        }
    } __finally {
        __try {
            if (contextual && !*contextual) *contextual = savedContext;
        } __finally {
            // Restore even when the native builder changes the selected player:
            // the part's recorded owner, not today's playerRoot, owns this borrow.
            __try {
                restoreSpinePart(borrowed[1]);
            } __finally {
                restoreSpinePart(borrowed[0]);
            }
        }
    }
}
static void poseBuilder(uintptr_t root, float time, unsigned char mode) {
    InterlockedIncrement(&poseBuildsInFlight);
    __try {
        healthHit(fp::HealthHook::poseBuilder);
        poseBuilderOwned(root, time, mode);
    } __finally {
        InterlockedDecrement(&poseBuildsInFlight);
    }
}
// Active head stabiliser (fp::steadyHead): run on the physics thread for the player's head and
// the two parts under it (neck and upper chest), after the game's constraint passes and before it builds
// their poses from the points (0x89800 then 0x7ECD0 in the physics step 0x80CD0). First person only,
// and never while the character is knocked down, falling, dazed by a hit or holding a door or lid: the
// game's own physics alone then. The chest and neck are steadied only out of combat (swings turn them).
using PartSettle = void (*)(uintptr_t);
static PartSettle
    originalPartSettle; // RVA 0x89800 (an inverted tetrahedron's repair; then the pose is built)
static std::atomic<int> headBob{
    100};
static std::atomic<unsigned> steadyTicks{0};
static fp::HeadSteadyState headSteadyState;
static fp::BodySteadyState bodySteadyState[2];
static uintptr_t steadiedHead = 0, steadiedRoot = 0, steadyNeck = 0, steadyChest = 0; // physics thread only
static bool bodyCalm(uintptr_t root, uintptr_t head);
static bool steadyAllowed(uintptr_t root, uintptr_t head) {
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) || viewMode.load() != 0 || headSteady.load() <= 0)
        return false; // Head stabilisation 0: the game's own neck
    return bodyCalm(root, head);
}
static bool bodyCalm(uintptr_t root, uintptr_t head) {
    if (!readable(root, 0x1300) || *(uintptr_t*)(root + field::character::headPart) != head ||
        !*(unsigned char*)(root + field::character::physicsLive))
        return false;
    const unsigned state = *(unsigned*)(root + field::character::stateFlags);
    if ((state & (0xac0 | 0x40204 | 0xa00)) || (*(unsigned*)(root + field::character::lifeFlags) & 0x100))
        return false; // knocked down, falling, getting up, dead
    const float balance = *(float*)(root + field::character::balance),
                most = *(float*)(root + field::character::balanceMax);
    if (!(balance >= most - 0.05f)) return false; // a hit's stagger keeps the game's floppy head
    const uintptr_t drag = *(uintptr_t*)(gameAt(fp::sig::dragManager));
    if (readable(drag, 0x20) && (*(unsigned*)(drag + 8) & (0x4000 | 0x10000 | 0x80000)))
        return false; // a door or lid pulls on the body
    return true;
}
// The head's tick: steadies it and notes the two parts under it, neck and chest (the root part's own +0x428
// is itself, so the chain stops there).
static void steadyHeadPart(uintptr_t root, uintptr_t head) {
    if (!readable(head, 0x5c0) || !readable(root, 0x1300)) {
        steadyNeck = steadyChest = 0;
        return;
    }
    const uintptr_t rootPart = *(uintptr_t*)(head + field::bodyPart::rootPart);
    auto below = [&](uintptr_t p) {
        const uintptr_t q = p ? *(uintptr_t*)(p + field::bodyPart::parentPart) : 0;
        return q && q != rootPart && readable(q, 0x5c0) ? q : uintptr_t(0);
    };
    steadyNeck = below(head);
    steadyChest = below(steadyNeck);
    if (head != steadiedHead || root != steadiedRoot || !steadyAllowed(root, head)) {
        headSteadyState = fp::HeadSteadyState();
        bodySteadyState[0] = bodySteadyState[1] = fp::BodySteadyState();
        steadiedHead = head;
        steadiedRoot = root;
        return;
    }
    const fp::HeadSteadyStrength s{headSteadyRoll.load(), headSteadyPitch.load(), headSteadyYaw.load(),
                                   headBob.load()};
    if (fp::steadyHead(headSteadyState, (float(*)[3])(head + field::bodyPart::points),
                       (const float*)(head + field::bodyPart::poseCoefficients),
                       (const float*)(root + field::character::gaze), s))
        ++steadyTicks;
    headingTurn = holdHeadingOn.load()
                      ? fp::steadyHeading((float(*)[3])(head + field::bodyPart::points),
                                          (const float*)(head + field::bodyPart::poseCoefficients),
                                          (const float*)(root + field::character::gaze), headSteadyYaw.load())
                      : 0.f;
}
static void steadyUnderHead(uintptr_t root, int which, uintptr_t part) {
    if (*(unsigned char*)(root + field::character::combatStance)) {
        bodySteadyState[which].primed = false;
        return;
    } // in combat: the game's (swings turn them)
    fp::steadyBodyPart(bodySteadyState[which], (float(*)[3])(part + field::bodyPart::points),
                       (const float*)(part + field::bodyPart::poseCoefficients), headSteady.load());
}
static void partSettle(uintptr_t part) {
    healthHit(fp::HealthHook::partSettle);
    __try {
        const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
        const uintptr_t head = root ? *(uintptr_t*)(root + field::character::headPart)
                                    : 0; // two plain reads for every other part
        if (part && part == head)
            steadyHeadPart(root, head);
        else if (part && head == steadiedHead && (part == steadyNeck || part == steadyChest)) {
            if (steadyAllowed(root, head)) steadyUnderHead(root, part == steadyNeck ? 0 : 1, part);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        headSteadyState = fp::HeadSteadyState();
        steadyNeck = steadyChest = 0;
    }
    originalPartSettle(part);
}
static void step(uintptr_t bh) {
    Aim a;
    const bool frozen = movementFrozen(bh);
    const bool owned = !frozen && getAim(bh, a);
    if (!owned && !frozen) {
        originalStep(bh);
        return;
    }
    // 0xC13F0 cancels opposite pairs before selecting its request. Raw nonzero
    // key bits can still mean neutral (forward+back, left+right, or all four).
    unsigned digital = *(unsigned*)gameAt(fp::sig::keyWord) & movementBits;
    if ((digital & 5u) == 5u) digital &= ~5u;
    if ((digital & 10u) == 10u) digital &= ~10u;
    const float* stick = (const float*)gameAt(fp::sig::analogStick);
    const float magnitude = stick[0] * stick[0] + stick[1] * stick[1] + stick[2] * stick[2];
    // Match this owner's native threshold (squared magnitude > 0.01), not the
    // exploration helper's 0.04. Invalid native input also stays native.
    const bool analog = !std::isfinite(magnitude) || double(magnitude) > 0.01;
    auto& pending = *(unsigned*)(bh + field::behaviour::pendingMove);
    const bool pendingDigital = pending && !(pending & ~movementBits);
    if ((frozen || (!digital && !analog)) && (pendingDigital || (frozen && pending == 0x8000))) pending = 0;
    // A frozen input owner has no live Aim. Drop its unaccepted direction
    // memory, then leave hover/aim and the native physical step entirely alone.
    // The separate FE0/FEC correction stage remains owned by native physics.
    if (!owned) {
        originalStep(bh);
        return;
    }
    if (*(unsigned char*)(a.root + field::character::combatStance))
        ++combatSteps;
    else
        ++unexpectedRelaxedSteps;
    auto state = (unsigned*)(bh + field::behaviour::hoverState);
    const unsigned hovered = *state;
    float* facing = (float*)(a.root + field::character::facing);
    float savedFacing[3]{}, presentedFacing[3]{};
    const bool present = digital && !analog && std::isfinite(a.yaw) &&
                         *(unsigned char*)(a.root + field::character::combatStance);
    if (present) {
        memcpy(savedFacing, facing, sizeof savedFacing);
        presentedFacing[0] = std::sin(a.yaw);
        presentedFacing[2] = std::cos(a.yaw);
    }
    __try {
        if (hovered == 4) *state = 0;
        // C1CF0 copies gaze into CFC before this call. C12D0/C19D0 flatten that
        // facing and divide by its length: a vertical TP target has no planar
        // direction. Present finite look yaw only while the digital request is
        // built, before native threat-distance adjustment and environment
        // queries. Native lengths, diagonal weights, hold windows and accepted
        // motion remain unchanged; cursor/ray yaw does not steer the feet.
        if (present) memcpy(facing, presentedFacing, sizeof presentedFacing);
        originalStep(bh);
    } __finally {
        // Restore only fields that still contain our exact borrowed value.
        // Native changes win, including when it raises an exception or stops.
        if (present && !memcmp(facing, presentedFacing, sizeof presentedFacing))
            memcpy(facing, savedFacing, sizeof savedFacing);
        if (hovered == 4 && *state == 0) *state = hovered;
    }
}
static void travelTarget(float* out, const float* origin, const float* direction, float scale) {
    auto caller = (uintptr_t)_ReturnAddress();
    scale = heldPlacementStart(caller, origin, direction, scale);
    if (exploringRoot && out == (float*)(exploringRoot + field::character::travelTarget) &&
        origin == (float*)(exploringRoot + field::character::position) &&
        (caller == gameAt(fp::sig::exploreTravelReturnA) ||
         caller == gameAt(fp::sig::exploreTravelReturnB))) {
        // Redirect only exploration's native continuous travel target BEFORE its
        // environment query. All other callers, including braking, keep their input.
        originalTravelTarget(out, origin, exploringDirection, scale);
        ++walkTargets;
        return;
    }
    originalTravelTarget(out, origin, direction, scale);
}
static void walkGait(uintptr_t root, const float* direction) {
    auto bh = *(uintptr_t*)(root + field::character::behaviour);
    Aim a;
    if (!*(unsigned char*)(root + field::character::combatStance) && bh && getAim(bh, a) && a.root == root &&
        a.travelValid && *(unsigned char*)(root + field::character::walking)) {
        // 163EF0 expects a gait axis, not a signed travel vector. Its backward
        // gait (10F4 bit 2) already uses the negative thrust at 12C0+3C. Passing
        // backward intent unchanged reverses twice and walks forward on S.
        float axis[3];
        const float sign = (*(unsigned*)(root + field::character::gaitFlags) & 2) ? -1.f : 1.f;
        for (int i = 0; i < 3; ++i) axis[i] = a.travel[i] * sign;
        originalWalkGait(root, axis);
        return;
    }
    originalWalkGait(root, direction);
}
static void explore(uintptr_t behavior) {
    Aim a;
    if (!getAim(behavior, a)) {
        originalExplore(behavior);
        return;
    }
    auto mask = (unsigned*)gameAt(fp::sig::keyWord);
    auto nativeAnalog = (float*)gameAt(fp::sig::analogStick);
    const unsigned saved = *mask;
    float travel[3], analog[3];
    const bool moving = fp::travel(saved, a.yaw, travel);
    memcpy(analog, nativeAnalog, sizeof analog);
    const bool stick = !moving && fp::analogActive(analog);
    const bool walking = *(unsigned char*)(a.root + field::character::walking) != 0;
    AcquireSRWLockExclusive(&aimLock);
    if (aim.root == a.root && aim.head == a.head) {
        if (moving) {
            memcpy(aim.travel, travel, sizeof travel);
            aim.travelValid = true;
        } else if (stick || !walking)
            // A controller owns its native gait axis immediately. With neither input,
            // retain the last digital axis only while native walking brakes to a stop.
            aim.travelValid = false;
    }
    ReleaseSRWLockExclusive(&aimLock);
    auto intent = (unsigned char*)(behavior + field::behaviour::moveIntent);
    const unsigned char savedIntent = *intent;
    const uintptr_t previousRoot = exploringRoot;
    float previousDirection[3];
    memcpy(previousDirection, exploringDirection, sizeof previousDirection);
    // Native analog exploration still owns walking, running, collision queries,
    // braking and the carried-item veto. Borrow only its directional input.
    __try {
        *mask &= ~movementBits;
        if (!stick) memcpy(nativeAnalog, travel, sizeof travel);
        *intent = 0;
        exploringRoot = moving ? a.root : 0;
        memcpy(exploringDirection, travel, sizeof travel);
        originalExplore(behavior);
        if (moving) ++relaxedSteps;
    } __finally {
        // Always pop this thread's caller scope, including native exceptions and
        // nested exploration. Native replacement publications keep their values.
        exploringRoot = previousRoot;
        memcpy(exploringDirection, previousDirection, sizeof previousDirection);
        if (!stick && !memcmp(nativeAnalog, travel, sizeof travel))
            memcpy(nativeAnalog, analog, sizeof analog);
        if (!(*mask & movementBits)) *mask = (*mask & ~movementBits) | (saved & movementBits);
        if (readable(behavior, field::behaviour::moveIntent + sizeof *intent) &&
            *(uintptr_t*)(behavior + field::behaviour::character) == a.root && !*intent)
            *intent = savedIntent;
    }
}
static bool readable(uintptr_t address, size_t size);
// Hand of the weapon that swings for this cue (see fp::SwingHands). Read-only.
// Accepted families name the weapon the start routine chose (root+0x988);
// a prediction takes the slot the native start routine would.
static fp::SwingHands swingHands(uintptr_t root, fp::OriginCue cue, int motion) {
    fp::SwingHands s;
    if (cue != fp::OriginCue::defaultSide && cue != fp::OriginCue::oppositeSide) return s;
    const int count = *(int*)(root + field::character::weaponCount);
    const unsigned grip = *(unsigned*)(root + field::character::gripFlags);
    handCount = count;
    handGrip = int(grip);
    if (count < 1 || count > 2) return s;
    s.alternate = (grip & 0x80) != 0;
    const uintptr_t weapons[2] = {*(uintptr_t*)(root + field::character::weapons),
                                  count > 1 ? *(uintptr_t*)(root + field::character::weapons + 8) : 0};
    const uintptr_t held[2] = {*(uintptr_t*)(root + field::character::heldItems),
                               *(uintptr_t*)(root + field::character::heldItems + 8)};
    const uintptr_t parts[2] = {*(uintptr_t*)(root + field::character::handParts),
                                *(uintptr_t*)(root + field::character::handParts + 8)};
    uintptr_t w = 0;
    if (motion >= 5 && motion <= 10) {
        w = *(uintptr_t*)(root + field::character::swingWeapon);
        s.slot = w && w == weapons[0] ? 0 : (w && w == weapons[1] ? 1 : -1);
    } else {
        s.slot = fp::swingSlot(cue == fp::OriginCue::oppositeSide, s.alternate);
        w = weapons[s.slot];
        if (!w) s.slot = -1;
    }
    // Grip entry hand: slot 0 from root+0x1070 (must point at an entry of the
    // native table), slot 1 always entry 5, as 0x155250 selects them.
    const uintptr_t table = gameAt(fp::sig::gripTable);
    constexpr uintptr_t stride = 0xb8;
    uintptr_t entry = s.slot == 1 ? table + 5 * stride
                                  : (s.slot == 0 ? *(uintptr_t*)(root + field::character::gripEntry) : 0);
    if (entry < table || entry >= table + 6 * stride || (entry - table) % stride) entry = 0;
    if (entry) {
        const int h = *(int*)(entry + 8);
        s.expectedHand = h == 0 || h == 1 ? h : -1;
    }
    if (s.slot >= 0) s.hand = fp::weaponHand(w, held, parts, s.expectedHand);
    // Both hand names are read (status shows them); the swinging hand decides.
    int sides[2]{};
    HandNamesStatus names;
    for (int h = 0; h < 2; h++) {
        const uintptr_t part = parts[h];
        const uintptr_t mesh = readable(part, 0x20) ? *(uintptr_t*)(part + field::bodyPart::meshNode) : 0;
        if (!mesh || !readable(mesh, 0x168) || *(uintptr_t*)(mesh + field::sceneNode::bodyPart) != part) {
            names.names[h][0] = 0;
            continue;
        }
        char name[17]{};
        memcpy(name, (void*)(mesh + field::sceneNode::name), 16);
        sides[h] = fp::handSideFromName(name);
        // Status JSON only: keep letters, digits and '_'.
        for (int i = 0; i < 16; i++) {
            const char c = i < 15 ? name[i] : 0;
            names.names[h][i] =
                !c || ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')
                    ? c
                    : '_';
        }
    }
    publishHandNames(names);
    if (s.hand >= 0) s.handSide = sides[s.hand];
    handSlot = s.slot;
    handIndex = s.hand;
    handExpected = s.expectedHand;
    handSideSeen = s.handSide;
    return s;
}
static float requestedAttackHeading(const Aim& a, float heading, float body, bool wideThreshold) {
    const bool directional = viewMode.load() == 0 ? fpSwingSelection.load() != 0
                                                : tpSwingSelection.load() != 0;
    if (!directional || !a.swing.side || a.swingGeneration != inputOwner.generation() ||
        a.swingPolicy != inputOwner.policyRevision() || a.swingView != viewMode.load())
        return heading;
    const fp::CueSide wanted = a.swing.side < 0 ? fp::CueSide::left : fp::CueSide::right;
    const fp::CueSide natural = fp::originSide(fp::OriginCue::defaultSide,
                                             swingHands(a.root, fp::OriginCue::defaultSide, 0));
    const fp::CueSide opposite = fp::originSide(fp::OriginCue::oppositeSide,
                                              swingHands(a.root, fp::OriginCue::oppositeSide, 0));
    // Resolve the physical side through the native grip. This also covers an
    // offhand forehand, a same-weapon backhand and a left-leading two-hander.
    // Unknown or ambiguous equipment retains the game's ordinary classification.
    if (wanted == natural && wanted != opposite) return body;
    if (wanted == opposite && wanted != natural) {
        const float threshold = (wideThreshold ? 20.f : 15.f) * 0.0174532925f;
        return fp::wrapHeading(body + threshold + 0.174532925f);
    }
    return heading;
}
// After the native step: +0x1B0 now holds the heading the next new press's
// classifier will read (cursorAim rewrites it later in that call), so the
// preview uses exactly the native inputs. Read-only; never calls attack code.
static void publishCue(uintptr_t self) {
    const uintptr_t root = *(uintptr_t*)(self + field::behaviour::character);
    if (!root || root != *(uintptr_t*)(gameAt(fp::sig::playerRoot)))
        return; // Only the controlled player's behaviour.
    fp::CueInputs in;
    Aim a;
    if (attackCue && getAim(self, a)) {
        in.owned = true;
        in.combat = *(unsigned char*)(root + field::character::combatStance) != 0;
        in.upright = (*(unsigned*)(root + field::character::stateFlags) & 0x40204) == 0;
        in.attackHeld = (*(unsigned*)(gameAt(fp::sig::buttonWord)) & 1) != 0;
        in.alternateHeld = (*(unsigned*)(gameAt(fp::sig::keyWord)) & 0x80) != 0;
        in.pressBlocked = *(unsigned char*)(self + field::behaviour::pressBlocked) != 0;
        in.modifierHeld = attackSideKey && (GetAsyncKeyState(attackSideKey) & 0x8000);
        in.wideThreshold = *(int*)(root + field::character::gripFlags) != 0;
        in.motion = *(int*)(root + field::character::motion);
        in.clock = *(unsigned*)(gameAt(fp::sig::gameClock));
        in.lastPress = *(unsigned*)(self + field::behaviour::lastPressTime);
        in.aimHeading = *(float*)(self + field::behaviour::aimHeading);
        in.bodyHeading = *(float*)(root + field::character::bodyHeading);
        in.aimHeading = requestedAttackHeading(a, in.aimHeading, in.bodyHeading, in.wideThreshold);
    }
    const fp::OriginCue cue = fp::originCue(in);
    cueState.store(int(cue));
    {
        const int motion = *(int*)(root + field::character::motion);
        swinging.store(*(unsigned char*)(root + field::character::combatStance) != 0 && motion >= 5 &&
                       motion <= 12);
    } // accepted motions 5-12 (0x179EE0), with or without the cue
    cueOrigin.store(int(fp::originSide(cue, swingHands(root, cue, in.motion))));
    {
        static fp::Windup w;
        const int motion = *(int*)(root + field::character::motion);
        const bool on = attackCue && in.owned && in.combat && motion >= 5 && motion <= 12 &&
                        (motion != 11 || cueThrust.load());
        fp::windupStep(w, on, motion, *(float*)(root + field::character::motionProgress),
                       *(unsigned*)(root + field::character::swingBits), GetTickCount64());
        if (on)
            cueArcSide.store(motion == 11 ? int(fp::CueSide::bottom)
                                          : cueOrigin.load()); // kept through the fade
        cueArcFill.store(w.fill);
        cueArcFade.store(w.fadeFrom);
        cueArcPublished.store(GetTickCount64());
        cueArcPhase.store(w.phase);
    }
}
static void behavior(uintptr_t self) {
    healthHit(fp::HealthHook::playerBehavior);
    updateAim(self);
    const AttackHeadingPresentation previous = attackHeadingPresentation;
    attackHeadingPresentation = {self};
    __try {
        originalBehavior(self);
    } __finally {
        const AttackHeadingPresentation shown = attackHeadingPresentation;
        if (shown.behavior == self && shown.borrowed &&
            *(float*)(self + field::behaviour::aimHeading) == shown.presented)
            *(float*)(self + field::behaviour::aimHeading) = shown.heading;
        attackHeadingPresentation = previous;
    }
    Aim a;
    if (getAim(self, a)) {
        // The original behavior already calls cursorAim. Do not advance its native
        // smoothing history a second time after it returns.
        float heading[3];
        headingToPoint(a, heading);
        if ((*(unsigned*)(a.root + field::character::stateFlags) & 0x40204) == 0) {
            if (!*(unsigned char*)(a.root + field::character::combatStance) ||
                !*(unsigned*)(self + field::behaviour::swingState)) {
                memcpy((void*)(a.root + field::character::facing), heading, 12);
                memcpy((void*)(a.root + field::character::movementFacing), heading, 12);
            }
        }
    }
    publishCue(self);
}
// World picks follow the rendered reticle. Native GUI press/drag classification
// separately receives physical mouse displacement below, so a fixed reticle can
// still start a drag and head motion cannot invalidate a double-click.
static constexpr ULONGLONG reticleFreshMs = 250;
static bool gameplayOwnsPointer() {
    return base && viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) && InterlockedCompareExchange(&inputEnabled, 0, 0) &&
           !cursorFreeNow() && *(int*)(gameAt(fp::sig::uiOpenCount)) <= 0;
}
// 0x4996B0 > 0: the game's own temporary pointer override (0x22710) wins.
static bool reticleForUi(LONG* x, LONG* y) {
    if (!gameplayOwnsPointer() || *(int*)(gameAt(fp::sig::pointerOverride)) >= 1) return false;
    AcquireSRWLockShared(&reticleLock);
    const bool fresh = reticleFrame >= 0 && GetTickCount64() - reticleTick <= reticleFreshMs;
    LONG rx = reticleX, ry = reticleY;
    const int clear = reticleClear;
    ReleaseSRWLockShared(&reticleLock);
    if (!fresh) return false;
    if (const uintptr_t kept = heldOffsetObject.load()) {
        if (heldThing() == kept) {
            rx += heldOffsetX.load();
            ry += heldOffsetY.load();
        } else
            heldOffsetObject = 0;
    }
    // Level geometry between the head and the pick origin: the eye is through a
    // wall. Report a pointer outside the viewport so the native picks (bounds
    // checked first in 0x8F720/0x8FB30) find nothing, as with a cursor off the
    // view. Undetermined clearance keeps the native result.
    if (clear == 0) {
        *x = -1;
        *y = -1;
        ++pickBlocked;
        return true;
    }
    *x = rx;
    *y = ry;
    ++uiReticleReads;
    return true;
}
// A borrowed comparison operand is never a world-pick coordinate. Keep the
// admitted real point for this synchronous call, but never lend it to a new owner.
static bool guiPointerOwned() {
    auto& point = guiPointerPresentation;
    if (!point.active || point.rejected) return false;
    __try {
        const fp::InputStamp now = worldPointerStamp();
        if (gameplayOwnsPointer() && closeViewContext() &&
            *(int*)gameAt(fp::sig::pointerOverride) < 1 &&
            point.gui == *(uintptr_t*)gameAt(fp::sig::guiManager) &&
            point.viewport == *(uintptr_t*)gameAt(fp::sig::mainViewport) && point.camera == savedCamera &&
            point.controller == savedController && point.view == savedView && savedCamera && savedView &&
            savedRoot == now.root && savedScene == now.scene &&
            *(uintptr_t*)(point.viewport + field::viewport::pickCamera) == savedCamera &&
            *(uintptr_t*)(point.viewport + field::viewport::view) == savedView &&
            *(uintptr_t*)(savedView + field::view::camera) == savedCamera &&
            *(uintptr_t*)(savedCamera + field::camera::controller) == point.controller &&
            point.stamp.root == now.root && point.stamp.head == now.head && point.stamp.scene == now.scene &&
            point.stamp.mode == now.mode && point.stamp.generation == now.generation &&
            point.stamp.policyRevision == now.policyRevision)
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    point.rejected = true;
    return false;
}
using GuiMousePress = void (*)(uintptr_t, unsigned short);
using GuiMousePoll = char (*)(uintptr_t, char);
static GuiMousePress originalGuiMousePress;
static GuiMousePoll originalGuiMousePoll;
struct WorldPointerSample {
    fp::InputStamp stamp;
    fp::PointerGesture::Point current, polled;
    LONG x = 0, y = 0, centerX = 0, centerY = 0, limitX = 0, limitY = 0;
    bool havePress = false;
};
static bool sampleWorldPointer(uintptr_t gui, WorldPointerSample& sample) {
    if (guiPointerPresentation.active || guiPointerPresentation.nativeOnly ||
        !gameplayOwnsPointer() || !closeViewContext()) return false;
    __try {
        if (gui != *(uintptr_t*)gameAt(fp::sig::guiManager) ||
            !readable(gui, field::gui::capture + sizeof(uintptr_t)) ||
            *(uintptr_t*)(gui + field::gui::capture) ||
            *(uintptr_t*)(gui + field::gui::hoverView) != *(uintptr_t*)gameAt(fp::sig::mainViewport))
            return false;
        sample.stamp = worldPointerStamp();
        if (*(unsigned char*)(sample.stamp.root + field::character::combatStance)) {
            const uintptr_t bh = *(uintptr_t*)(sample.stamp.root + field::character::behaviour);
            const unsigned interact = nativeBinding(field::bindings::interact / field::bindings::stride);
            if (!*(unsigned char*)(bh + field::behaviour::interactionLatch) &&
                !(interact > 0 && interact < 256 && (keyState(int(interact)) & 0x8000)))
                return false; // ordinary combat keeps its native message owner
        }
        RECT rect;
        if (!reticleForUi(&sample.x, &sample.y) || !GetClientRect(gameWindow(), &rect) ||
            rect.right <= 40 || rect.bottom <= 40 || sample.x < 0 || sample.y < 0 ||
            sample.x >= rect.right || sample.y >= rect.bottom)
            return false;
        sample.centerX = rect.right / 2;
        sample.centerY = rect.bottom / 2;
        sample.limitX = rect.right / 4;
        sample.limitY = rect.bottom / 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    AcquireSRWLockExclusive(&worldPointerLock);
    worldPointer.sync(sample.stamp);
    sample.current = worldPointer.current;
    sample.polled = worldPointer.polled;
    sample.havePress = worldPointer.havePress;
    ReleaseSRWLockExclusive(&worldPointerLock);
    return true;
}
static LONG gestureCoordinate(LONG center, LONG limit, double movement) {
    return center + LONG(std::lround(std::fmax(-double(limit), std::fmin(double(limit), movement))));
}
static void restoreGuiPointer(uintptr_t gui, size_t offset, LONG fromX, LONG fromY, LONG toX, LONG toY) {
    auto* point = (LONG*)(gui + offset);
    if (point[0] == fromX && point[1] == fromY) {
        point[0] = toX;
        point[1] = toY;
    }
}
static void guiMousePress(uintptr_t gui, unsigned short buttons) {
    if (guiPointerPresentation.active &&
        (gui != guiPointerPresentation.gui || guiPointerPresentation.nativeOnly)) {
        const GuiPointerPresentation previous = guiPointerPresentation;
        // Retain the borrowed owner's identity: a foreign callback may reenter
        // that GUI before its translated press operand has been restored.
        guiPointerPresentation.nativeOnly = gui != guiPointerPresentation.gui;
        __try {
            originalGuiMousePress(gui, buttons);
        } __finally {
            const bool read = guiPointerPresentation.read, rejected = guiPointerPresentation.rejected;
            guiPointerPresentation = previous;
            guiPointerPresentation.read |= read;
            guiPointerPresentation.rejected |= rejected;
        }
        return;
    }
    WorldPointerSample sample;
    if ((buttons & 7) != 1 || !sampleWorldPointer(gui, sample)) {
        originalGuiMousePress(gui, buttons);
        return;
    }
    const LONG x = sample.x, y = sample.y;
    LONG priorX = x - (gestureCoordinate(sample.centerX, sample.limitX, sample.current.x) - sample.centerX),
         priorY = y - (gestureCoordinate(sample.centerY, sample.limitY, sample.current.y) - sample.centerY);
    // A first press in this input owner cannot double-click an older UI/profile press.
    if (!sample.havePress && std::abs(priorX - x) < 2 && std::abs(priorY - y) < 2) priorX = x - 2;
    auto* current = (LONG*)(gui + field::gui::pointerX);
    auto* pressed = (LONG*)(gui + field::gui::pressX);
    const LONG oldPressX = pressed[0], oldPressY = pressed[1];
    const GuiPointerPresentation previous = guiPointerPresentation;
    guiPointerPresentation = {true, false, x, y};
    guiPointerPresentation.gui = gui;
    guiPointerPresentation.viewport = *(uintptr_t*)gameAt(fp::sig::mainViewport);
    guiPointerPresentation.camera = savedCamera;
    guiPointerPresentation.controller = savedController;
    guiPointerPresentation.view = savedView;
    guiPointerPresentation.stamp = sample.stamp;
    bool accepted = false;
    __try {
        current[0] = x;
        current[1] = y;
        // Preserve real current coordinates: native hit-testing and its +28/+2C
        // press snapshot must stay on the rendered reticle. Only the comparison
        // with the previous press uses deliberate physical mouse displacement.
        pressed[0] = priorX;
        pressed[1] = priorY;
        originalGuiMousePress(gui, buttons);
        accepted = (*(unsigned*)(gui + field::gui::buttons) & 7) == 1;
    } __finally {
        restoreGuiPointer(gui, field::gui::pointerX, x, y, sample.x, sample.y);
        const LONG restoreX = accepted ? sample.x : oldPressX,
                   restoreY = accepted ? sample.y : oldPressY;
        restoreGuiPointer(gui, field::gui::pressX, x, y, restoreX, restoreY);
        restoreGuiPointer(gui, field::gui::pressX, priorX, priorY, restoreX, restoreY);
        guiPointerPresentation = previous;
    }
    if (accepted) {
        AcquireSRWLockExclusive(&worldPointerLock);
        if (worldPointer.ownedBy(sample.stamp)) worldPointer.press(sample.current);
        ReleaseSRWLockExclusive(&worldPointerLock);
    }
}
static char guiMousePoll(uintptr_t gui, char force) {
    if (guiPointerPresentation.active &&
        (gui != guiPointerPresentation.gui || guiPointerPresentation.nativeOnly)) {
        const GuiPointerPresentation previous = guiPointerPresentation;
        // Retain the borrowed owner's identity: a foreign callback may reenter
        // that GUI before its translated press operand has been restored.
        guiPointerPresentation.nativeOnly = gui != guiPointerPresentation.gui;
        __try {
            return originalGuiMousePoll(gui, force);
        } __finally {
            const bool read = guiPointerPresentation.read, rejected = guiPointerPresentation.rejected;
            guiPointerPresentation = previous;
            guiPointerPresentation.read |= read;
            guiPointerPresentation.rejected |= rejected;
        }
    }
    WorldPointerSample sample;
    const unsigned buttons = readable(gui, field::gui::buttons + sizeof(unsigned))
                                 ? *(unsigned*)(gui + field::gui::buttons) : 0;
    if ((buttons & 7) != 1 || (buttons & 0x6100) || !sampleWorldPointer(gui, sample) || !sample.havePress)
        return originalGuiMousePoll(gui, force);
    const LONG x = sample.x, y = sample.y,
               totalX = gestureCoordinate(sample.centerX, sample.limitX, sample.current.x) - sample.centerX,
               totalY = gestureCoordinate(sample.centerY, sample.limitY, sample.current.y) - sample.centerY,
               deltaX = totalX - (gestureCoordinate(sample.centerX, sample.limitX, sample.polled.x) - sample.centerX),
               deltaY = totalY - (gestureCoordinate(sample.centerY, sample.limitY, sample.polled.y) - sample.centerY),
               priorX = x - deltaX, priorY = y - deltaY, pressX = x - totalX, pressY = y - totalY;
    auto* current = (LONG*)(gui + field::gui::pointerX);
    auto* pressed = (LONG*)(gui + field::gui::pressX);
    const LONG oldPressX = pressed[0], oldPressY = pressed[1];
    const GuiPointerPresentation previous = guiPointerPresentation;
    guiPointerPresentation = {true, false, x, y};
    guiPointerPresentation.gui = gui;
    guiPointerPresentation.viewport = *(uintptr_t*)gameAt(fp::sig::mainViewport);
    guiPointerPresentation.camera = savedCamera;
    guiPointerPresentation.controller = savedController;
    guiPointerPresentation.view = savedView;
    guiPointerPresentation.stamp = sample.stamp;
    guiPointerPresentation.priorX = priorX;
    guiPointerPresentation.priorY = priorY;
    guiPointerPresentation.compare = true;
    char result = 0;
    bool sampled = false;
    __try {
        current[0] = x;
        current[1] = y;
        pressed[0] = pressX;
        pressed[1] = pressY;
        result = originalGuiMousePoll(gui, force);
        sampled = guiPointerPresentation.read;
    } __finally {
        restoreGuiPointer(gui, field::gui::pointerX, x, y, sample.x, sample.y);
        restoreGuiPointer(gui, field::gui::pointerX, priorX, priorY, sample.x, sample.y);
        restoreGuiPointer(gui, field::gui::pressX, pressX, pressY, oldPressX, oldPressY);
        guiPointerPresentation = previous;
    }
    if (sampled) {
        AcquireSRWLockExclusive(&worldPointerLock);
        if (worldPointer.ownedBy(sample.stamp)) worldPointer.poll(sample.current);
        ReleaseSRWLockExclusive(&worldPointerLock);
    }
    return result;
}
// Level grid of the viewport's scene manager (+0x1F0), present when its +0xC
// carries 0x2000000 (the guard 0x2462F0 and 0x117AA0 use). 0 when absent.
static uintptr_t levelGrid() {
    const uintptr_t viewport = *(uintptr_t*)(gameAt(fp::sig::mainViewport));
    const uintptr_t manager = viewport && readable(viewport + field::viewport::sceneManager, 8)
                                  ? *(uintptr_t*)(viewport + field::viewport::sceneManager)
                                  : 0;
    if (!manager || !readable(manager, 0x328) ||
        !(*(unsigned*)(manager + field::sceneManager::flags) & 0x2000000))
        return 0;
    const uintptr_t level = *(uintptr_t*)(manager + field::sceneManager::levelGrid);
    return level && readable(level, 0x28) ? level : 0;
}
static int levelBlocked(uintptr_t level, const float* origin, const float* dir, float length) {
    __try {
        return levelRay(level, origin, dir, length) || levelRay2(level, origin, dir, length) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// Called with cameraLock held (lastCamera/lastHeadGlobal/renderedFov pairing).
// Only meaningful while picks unproject from the camera first person wrote
// (pick_camera 1, near renderedNear).
static int pickClearance(float px, float py, float width, float height) {
    const uintptr_t viewport = *(uintptr_t*)(gameAt(fp::sig::mainViewport));
    if (!levelRay || !levelRay2 || !viewport || !savedCamera ||
        *(uintptr_t*)(viewport + field::viewport::pickCamera) != savedCamera)
        return -1;
    const uintptr_t level = levelGrid();
    if (!level) return -1;
    float origin[3];
    if (!fp::pickOrigin(lastCamera, px, py, width, height, fp::projectionScale(height, renderedFov),
                        renderedNear, origin))
        return -1;
    int fault = 0;
    const int clear = fp::pickClear(lastHeadGlobal, origin, [&](const float* o, const float* d, float len) {
        ++clearanceQueries;
        const int r = levelBlocked(level, o, d, len);
        if (r < 0) fault = 1;
        return r == 1;
    });
    return fault ? -1 : clear;
}
// Held-object placement. 0x1183C0(object, camera, h) unprojects the pointer
// with 0x8F720(camera, x, y, out) and places the object at out's point (+8): the first hit, or
// with no hit and a downward ray the crossing of height h (the constant -100000 at 0x3125C0).
// out: +0x14 unit direction, +0x20 origin, +0x2C distance; returns whether something was hit.
// While first person holds an object, the point becomes the hold distance along that ray,
// clamped by the real hit (the native pull-back by object size still applies), a level wall
// and the reach sphere around the player; a new object starts at its native distance.
using DragPlace = int (*)(uintptr_t, uintptr_t, float);
static DragPlace originalDragPlace; // RVA 0x1183C0
using PickRay = char (*)(uintptr_t, float, float, uintptr_t);
static PickRay originalPickRay; // RVA 0x8F720
// Only mod-owned full pick buffers use this path. Native forwarding retains its
// caller's selector. 0x8F720 consumes/clears the selector, so prepare every call.
static char ownedPickRay(uintptr_t viewport, float x, float y, uintptr_t out) {
    const std::uint32_t bank = fp::game::viewQueryScratchBank;
    memcpy((void*)(out + field::pick::scratchBank), &bank, sizeof bank);
    return originalPickRay(viewport, x, y, out);
}
// Placement scope follows the native call stack; callers on another thread cannot overwrite it.
static thread_local DWORD placingThread = 0;
static thread_local bool placingReady = false;
static thread_local uintptr_t placingViewport = 0;
static thread_local unsigned char placingRay[0x34]{};
static bool prepareHeldPlacement(uintptr_t object, uintptr_t camera, float height, unsigned char* ray);
// Ordinary gameplay holds use 11F610 -> 117AA0 -> 118000. The separate
// 1183C0 placement entry is not called by that loop. Admit only the current
// world viewport's movable item; UI, park, focus and native drag owners win.
static uintptr_t closeWorldHeldObject(uintptr_t viewport) {
    if (!base || !viewport || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0))
        return 0;
    __try {
        if (viewport != *(uintptr_t*)gameAt(fp::sig::mainViewport) ||
            !*(uintptr_t*)gameAt(fp::sig::playerRoot))
            return 0;
        const uintptr_t drag = *(uintptr_t*)gameAt(fp::sig::dragManager);
        if (!readable(drag, 0x20) || *(unsigned*)(drag + field::drag::flags) != 0x2000) return 0;
        const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
        return readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t)) ? object : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
static uintptr_t gameplayHeldObject(uintptr_t viewport) {
    const uintptr_t object = closeWorldHeldObject(viewport);
    if (!object ||
        (!InterlockedCompareExchange(&inputEnabled, 0, 0) && !InterlockedCompareExchange(&rmbFree, 0, 0)) ||
        InterlockedCompareExchange(&cursorFree, 0, 0) || InterlockedCompareExchange(&windowFree, 0, 0))
        return 0;
    __try {
        return closeViewContext() && *(int*)gameAt(fp::sig::pointerOverride) < 1 ? object : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
using HeldQuery = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, float);
static HeldQuery originalHeldQuery; // RVA 0x117AA0
using HeldPlace = bool (*)(uintptr_t, uintptr_t, uintptr_t);
static HeldPlace originalHeldPlace; // RVA 0x118000
using HeldSweep = float (*)(uintptr_t, const float*, uintptr_t, uintptr_t);
static HeldSweep originalHeldSweep; // RVA 0x1170F0
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity);
static float heldSweepFor(uintptr_t caller, uintptr_t body, const float* direction, uintptr_t viewport,
                          uintptr_t pose) {
    if (!boundedPlacement.object || viewport != boundedPlacement.viewport ||
        (caller != gameAt(fp::sig::heldSweepReturn) && caller != gameAt(fp::sig::inventorySweepReturn)) ||
        memcmp(direction, boundedPlacement.values + 3, 12))
        return originalHeldSweep(body, direction, viewport, pose);
    // 1170F0 uses node+C bit 200000, not the pick's node+8 skip bit.
    // Exclude the player's mesh tree during this query only, including with
    // EyeSetback/third-person cameras. All other objects remain native blockers.
    uintptr_t nodes[1024]{};
    unsigned parents[1024]{}, changed = 0;
    float distance = 0.f;
    __try {
        const uintptr_t root = boundedPlacement.root;
        const uintptr_t mesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
        const unsigned count = mesh ? walkScene(mesh, nodes, parents, 1024) : 0;
        for (unsigned i = 0; i < count; ++i) {
            const uintptr_t node = nodes[i];
            if (!readable(node, field::sceneNode::queryFlags + sizeof(unsigned))) continue;
            auto& flags = *(unsigned*)(node + field::sceneNode::queryFlags);
            if (flags & heldCollisionQueryBit) {
                flags &= ~heldCollisionQueryBit;
                nodes[changed++] = node;
            }
        }
        // Native 1170F0 searches at most 1000 units. Distant third-person
        // cameras need consecutive native sweeps, each with the same body
        // orientation and shape, so an empty first segment cannot stop the hold.
        float walked = 0.f;
        do {
            const float segment = std::fmin(nativeHeldSweepSpan, boundedPlacement.distance - walked);
            float shifted[12];
            uintptr_t transform = pose;
            if (walked > 0.f) {
                memcpy(shifted, (void*)pose, sizeof shifted);
                for (int i = 0; i < 3; ++i) shifted[9 + i] += direction[i] * walked;
                transform = (uintptr_t)shifted;
            }
            const float hit = originalHeldSweep(body, direction, viewport, transform);
            if (!std::isfinite(hit) || hit < 0.f) {
                boundedPlacement.blocked = true;
                distance = 0.f;
                break;
            }
            distance = walked + std::fmin(hit, segment);
            if (hit < segment) break;
            walked += segment;
        } while (distance < boundedPlacement.distance);
    } __finally {
        for (unsigned i = 0; i < changed; ++i)
            if (readable(nodes[i], field::sceneNode::queryFlags + sizeof(unsigned)))
                *(unsigned*)(nodes[i] + field::sceneNode::queryFlags) |= heldCollisionQueryBit;
    }
    if (!std::isfinite(distance) || distance < 0.f) {
        boundedPlacement.blocked = true;
        return 0.f;
    }
    return std::fmin(distance, boundedPlacement.distance);
}
static float heldSweep(uintptr_t body, const float* direction, uintptr_t viewport, uintptr_t pose) {
    return heldSweepFor((uintptr_t)_ReturnAddress(), body, direction, viewport, pose);
}
static bool heldPlace(uintptr_t object, uintptr_t viewport, uintptr_t query) {
    HeldPlacement proposal = pendingPlacement;
    pendingPlacement = {};
    if (!object || closeWorldHeldObject(viewport) != object) {
        // A close-view query cannot become an overhead placement after handoff.
        if (proposal.object) return false;
        return originalHeldPlace(object, viewport, query);
    }
    // 119CB0 revalidates its saved native query without calling 117AA0 again.
    // Only that exact call may borrow the last native-confirmed bounded proposal.
    const bool recovery = (uintptr_t)_ReturnAddress() == gameAt(fp::sig::heldRecoveryPlaceReturn) &&
                          heldRecoveryPlacement(object, viewport, query, proposal);
    if ((!recovery && gameplayHeldObject(viewport) != object) || proposal.object != object ||
        proposal.viewport != viewport || proposal.query != query ||
        proposal.generation != holdGeneration.load() || proposal.mode != viewMode.load() ||
        proposal.root != *(uintptr_t*)gameAt(fp::sig::playerRoot) ||
        proposal.scene != *(uintptr_t*)gameAt(fp::sig::currentScene) || !std::isfinite(proposal.distance) ||
        proposal.distance < 1.f || !query || memcmp((void*)query, proposal.values, sizeof proposal.values))
        return false;
    const HeldPlacement previous = boundedPlacement;
    bool result = false;
    boundedPlacement = proposal;
    __try {
        result = originalHeldPlace(object, viewport, query);
        result = result && !boundedPlacement.blocked && proposal.generation == holdGeneration.load() &&
                 proposal.mode == viewMode.load() &&
                 proposal.root == *(uintptr_t*)gameAt(fp::sig::playerRoot) &&
                 proposal.scene == *(uintptr_t*)gameAt(fp::sig::currentScene) &&
                 (recovery ? closeWorldHeldObject(viewport) : gameplayHeldObject(viewport)) == object;
        if (result && !recovery && heldUpdateDepth == 1) updatePlacement = proposal;
    } __finally {
        boundedPlacement = previous;
    }
    return result;
}
// Close views supply a view-ray point/direction to the existing 11F610 caller.
// 118000 still owns placement, collision and physical history. The overhead
// query's downward-only and waist-plane policy is not used by this path.
static uintptr_t heldQuery(uintptr_t object, uintptr_t viewport, uintptr_t out, float height);
static std::atomic<unsigned> dropSpots{0}; // accepted close-view query outputs
using DragRotate = bool (*)(uintptr_t, uintptr_t);
static DragRotate originalDragRotate; // Native 0x118920, before both world placement routes.
using NodePose = void (*)(uintptr_t);
static void callPropagateHeldPose(uintptr_t object) {
    ((NodePose)gameAt(fp::sig::propagateNodePose))(object);
}
static NodePose propagateHeldPose = callPropagateHeldPose;
static bool heldUnit(float* axis) {
    const float length = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if (!std::isfinite(length) || length < 1.e-8f) return false;
    const float scale = 1.f / std::sqrt(length);
    for (int i = 0; i < 3; ++i) axis[i] *= scale;
    return true;
}
// 0x118920 rotates node+70, then copies it to body+2A0 and propagates the node.
// 0x118000/0x1183C0 subsequently place that orientation, rebuilding the physical
// points and their histories. drag+40 is the original grab's recovery snapshot;
// changing it neither rotates the held item nor preserves cancellation correctly.
static bool turnHeld(uintptr_t object) {
    const float tx = heldTurnX.exchange(0.f), ty = heldTurnY.exchange(0.f);
    if (!readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t))) return false;
    const uintptr_t body = *(uintptr_t*)(object + field::sceneNode::bodyPart);
    if (!readable(body, field::bodyPart::pose + 48) ||
        *(uintptr_t*)(body + field::bodyPart::meshNode) != object)
        return false;
    float pose[12];
    memcpy(pose, (const void*)(object + field::sceneNode::localTransform), sizeof pose);
    for (float value : pose)
        if (!std::isfinite(value)) return false;
    const bool turning = std::isfinite(tx) && std::isfinite(ty) && (tx != 0.f || ty != 0.f);
    if (turning) {
        float view[12], right[3], up[3];
        fp::basis(viewYaw.load(), viewPitch.load(), view);
        const uintptr_t parent = *(uintptr_t*)(object + field::sceneNode::parent);
        if (parent) {
            if (!readable(parent, field::sceneNode::transform + 48)) return false;
            const float* parentPose = (const float*)(parent + field::sceneNode::transform);
            fp::worldToLocalDirection(parentPose, view, right);
            fp::worldToLocalDirection(parentPose, view + 3, up);
        } else {
            memcpy(right, view, sizeof right);
            memcpy(up, view + 3, sizeof up);
        }
        if (!heldUnit(right) || !heldUnit(up)) return false;
        fp::rotateAxes(pose, up, -tx * fp::heldRotateRadiansPerPixel);
        fp::rotateAxes(pose, right, -ty * fp::heldRotateRadiansPerPixel);
        // The same Gram-Schmidt order as native 0x1188D0 keeps repeated turns rigid.
        if (!heldUnit(pose + 3)) return false;
        const float dot = pose[0] * pose[3] + pose[1] * pose[4] + pose[2] * pose[5];
        for (int i = 0; i < 3; ++i) pose[i] -= dot * pose[3 + i];
        if (!heldUnit(pose)) return false;
        pose[6] = pose[1] * pose[5] - pose[2] * pose[4];
        pose[7] = pose[2] * pose[3] - pose[0] * pose[5];
        pose[8] = pose[0] * pose[4] - pose[1] * pose[3];
        if (!heldUnit(pose + 6)) return false;
        memcpy((void*)(object + field::sceneNode::localTransform), pose, 36);
        ++heldTurns;
    }
    // Match the native dynamic-body guard and its complete affine copy. Placement
    // owns point/history updates, including restoring the last valid pose if blocked.
    if (*(unsigned*)(body + field::bodyPart::flags) & 0x10000000)
        memcpy((void*)(body + field::bodyPart::pose), pose, sizeof pose);
    propagateHeldPose(object);
    return turning;
}
static bool dragRotate(uintptr_t object, uintptr_t viewport) {
    if (!object || closeWorldHeldObject(viewport) != object) return originalDragRotate(object, viewport);
    if (gameplayHeldObject(viewport) != object) return false;
    // Native 0x118920 polls movement keys directly. Owned play keeps those keys
    // for locomotion; only this grab's rotate-key mouse deltas turn the item.
    return turnHeld(object);
}
static ULONGLONG inventoryPlacementBegin(uintptr_t object, uintptr_t viewport);
static void inventoryPlacementEnd(ULONGLONG generation, uintptr_t object, int result);
static void inventoryPlacementHandoff();
static int dragPlace(uintptr_t object, uintptr_t camera, float h) {
    // Inventory materialization (11D1D0) enters 1183C0 before flags become
    // 0x2000. Its -100000 height fallback is an overhead policy too. Preflight
    // a bounded actual-view pick, then let 1183C0 initialize native physics.
    const uintptr_t manager = *(uintptr_t*)gameAt(fp::sig::dragManager);
    const unsigned flags = readable(manager, 0x20) ? *(unsigned*)(manager + field::drag::flags) : 0;
    const bool closePlacement =
        camera && camera == *(uintptr_t*)gameAt(fp::sig::mainViewport) && viewMode.load() < 2 &&
        InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
        InterlockedCompareExchange(&cameraReady, 0, 0) && *(uintptr_t*)gameAt(fp::sig::playerRoot) &&
        (flags == 0x1000 || flags == 0x2000) && *(uintptr_t*)(manager + field::drag::object) == object;
    if (closePlacement) {
        const ULONGLONG inventory = inventoryPlacementBegin(object, camera);
        if (flags == 0x1000) resetHeldGrab();
        alignas(8) unsigned char ray[0x40]{};
        if (!prepareHeldPlacement(object, camera, h, ray)) {
            inventoryPlacementEnd(inventory, object, 0);
            return 0;
        }
        const DWORD previousThread = placingThread;
        const bool previousReady = placingReady;
        const uintptr_t previousViewport = placingViewport;
        unsigned char previousRay[0x34];
        memcpy(previousRay, placingRay, sizeof previousRay);
        placingThread = GetCurrentThreadId();
        placingReady = true;
        placingViewport = camera;
        memcpy(placingRay, ray, sizeof placingRay);
        const HeldPlacement previousBound = boundedPlacement;
        boundedPlacement = {};
        boundedPlacement.object = object;
        boundedPlacement.viewport = camera;
        boundedPlacement.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        boundedPlacement.distance = *(float*)(ray + field::pick::distance);
        memcpy(boundedPlacement.values, ray + 8, 12);
        memcpy(boundedPlacement.values + 3, ray + field::pick::direction, 12);
        int result = 0;
        __try {
            result = originalDragPlace(object, camera, h);
            if (boundedPlacement.blocked) result = 0;
        } __finally {
            boundedPlacement = previousBound;
            placingThread = previousThread;
            placingReady = previousReady;
            placingViewport = previousViewport;
            memcpy(placingRay, previousRay, sizeof placingRay);
        }
        inventoryPlacementEnd(inventory, object, result);
        return result;
    }
    inventoryPlacementHandoff(); // native-only placement cannot retain an old owned failure
    if (holdObject.load() || grabStartDistance.load() != 0.f) resetHeldGrab();
    return originalDragPlace(object, camera, h);
}
// Reach sphere round the body: root+0xAB4 is scene-local; the ray is global (scene +0x64 offset).
static float holdReachClamp(const float* origin, const float* dir, float t) {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t mesh = root ? *(uintptr_t*)(root + field::character::sceneNode) : 0,
                    scene = mesh ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
    if (scene) {
        float body[3];
        for (int i = 0; i < 3; i++)
            body[i] = *(float*)(root + field::character::position + i * 4) +
                      *(float*)(scene + field::sector::origin + i * 4);
        const float reach = fp::rayInSphere(origin, dir, body, fp::holdSphere(float(grabRange.load())));
        if (reach >= 0) t = std::fmin(t, reach);
    }
    return t;
}
static float levelClear(uintptr_t level, const float* origin, const float* dir, float length) {
    const int initial = levelBlocked(level, origin, dir, length);
    if (initial < 0) return 0.f; // unknown is not clear space
    if (initial == 0) return length;
    float lo = 0, hi = length;
    for (int i = 0; i < 7; i++) {
        const float mid = (lo + hi) * .5f;
        const int blocked = levelBlocked(level, origin, dir, mid);
        if (blocked < 0) return 0.f; // no safe clearance established by a fault
        if (blocked == 1)
            hi = mid;
        else
            lo = mid;
    }
    return std::fmax(lo - 8.f, 0.f);
}
// dynamic near plane: the view depth to the nearest level geometry inside the view, from the
// eye along the view centre and the four frustum corners (16 units, the level ray queries the
// pick clearance uses). Every frame; -1 unknown (no level grid, a fault, a bad pose), 1e9 nothing near.
// Level geometry only: props and characters are not in the level grid.
static float viewClearance(const float* pose, float fov) {
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(pose[i])) return -1;
    const uintptr_t level = levelGrid();
    if (!level || !levelRay || !levelRay2) return -1;
    float aspect = 16.f / 9.f;
    RECT r;
    if (hookedWindow && GetClientRect(hookedWindow, &r) && r.bottom > 0 && r.right > 0)
        aspect = float(r.right) / float(r.bottom);
    const float tv = std::tan(fov * 3.14159265f / 360.f), th = tv * aspect;
    float best = 1e9f;
    for (int k = 0; k < 5; ++k) {
        const float sx = k == 0 ? 0 : ((k & 1) ? th : -th), sy = k == 0 ? 0 : ((k & 2) ? tv : -tv);
        float d[3];
        double n = 0;
        for (int i = 0; i < 3; i++) {
            d[i] = -pose[6 + i] + pose[i] * sx + pose[3 + i] * sy;
            n += double(d[i]) * d[i];
        }
        n = std::sqrt(n);
        if (!(n > 1e-6)) continue;
        for (float& v : d) v = float(v / n);
        const float length = std::fmax(
            16.f,
            9.f * float(n)); // a corner ray reaches about 9 deep at any field of view (wide views slant it)
        const int hit = levelBlocked(level, pose + 9, d, length);
        if (hit < 0) return -1;
        if (hit == 0) continue;
        float lo = 0, hi = length;
        for (int i = 0; i < 6; i++) {
            const float mid = (lo + hi) * .5f;
            const int m = levelBlocked(level, pose + 9, d, mid);
            if (m < 0) return -1;
            if (m == 1)
                hi = mid;
            else
                lo = mid;
        }
        best = std::fmin(best, lo * float(1.0 / n)); // along the ray -> view depth
    }
    return best;
}
// The close-view hold is a distance from the eye along the actual pointer ray,
// limited by the nearer hit, native level geometry and the player's reach.
// Neither the ray's origin nor its direction is ever rewritten.
static char heldPointAlongViewDistance(char hit, uintptr_t out, float distance) {
    const float* dir = (const float*)(out + field::pick::direction);
    const float* origin = (const float*)(out + field::pick::origin);
    const float length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!std::isfinite(length) || length < 0.5f || !std::isfinite(origin[0] + origin[1] + origin[2]))
        return 0;
    const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
    float t = fp::holdAlongRay(origin, dir, eye, distance);
    if (t < 0.f) return 0; // pointing away from the character
    t = holdReachClamp(origin, dir, t);
    const float reached = *(float*)(out + field::pick::distance);
    if (hit && (!std::isfinite(reached) || reached < 0.f)) return 0;
    if (hit) t = std::fmin(t, reached);
    const uintptr_t level = levelGrid();
    if (level) {
        if (!levelRay || !levelRay2 || levelBlocked(level, origin, dir, t) < 0) return 0;
        t = levelClear(level, origin, dir, t);
        if (levelBlocked(level, origin, dir, t) != 0) return 0;
    }
    if (!(t >= 1.f)) return 0;
    for (int i = 0; i < 3; i++) ((float*)(out + 8))[i] = origin[i] + dir[i] * t;
    *(float*)(out + field::pick::distance) = t;
    return 1;
}
static char heldPointAlongView(char hit, uintptr_t out) {
    return heldPointAlongViewDistance(hit, out, holdApplied.load());
}
// The game's picks (0x8F720, 0x8FB30) keep the nearest mesh the ray meets and skip any whose flags +8 carry
// 0x4000000. In first and third person the ray starts at (or near) your own head, so your arms, hands, what
// you hold and your body were met first: a grab or the held item's placement landed on yourself. For the
// game's own picks there, the player's meshes carry that bit for the call only; only the bits set here are
// cleared again (__finally), on the game's thread that runs the pick.
static constexpr unsigned pickSkipBit = 0x4000000;
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity);
static uintptr_t selfPickNodes[1024];
static unsigned selfPickParents[1024], selfPickSet = 0;
static std::atomic<unsigned> selfPicksMasked{0};
static bool onGameThread();
static bool selfPickWanted() {
    return InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
           viewMode.load() < 2 && onGameThread();
} // one thread owns the mask
// held: the scene node of an object being held, skipped as well (else the placement's pick met the object's
// own front every frame and pulled it toward the eye).
static uintptr_t heldPickNodes[256];
static unsigned heldPickParents[256];
static void selfPickMask(uintptr_t held = 0) {
    selfPickSet = 0;
    auto mask = [](uintptr_t node) {
        if (!readable(node, 0x10) || selfPickSet >= 1024) return;
        unsigned& flags = *(unsigned*)(node + 8);
        if (!(flags & pickSkipBit)) {
            flags |= pickSkipBit;
            selfPickNodes[selfPickSet++] = node;
        }
    }; // compacted in place: only the ones changed here
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t mesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    if (mesh) {
        const unsigned count = walkScene(mesh, selfPickNodes, selfPickParents, 1024);
        for (unsigned i = 0; i < count; ++i) mask(selfPickNodes[i]);
    }
    if (held) {
        const unsigned count = walkScene(held, heldPickNodes, heldPickParents, 256);
        for (unsigned i = 0; i < count; ++i) mask(heldPickNodes[i]);
    }
    if (selfPickSet) ++selfPicksMasked;
}
static void selfPickRestore() {
    for (unsigned i = 0; i < selfPickSet; ++i)
        if (readable(selfPickNodes[i], 0x10)) *(unsigned*)(selfPickNodes[i] + 8) &= ~pickSkipBit;
    selfPickSet = 0;
}
static char pickPastSelf(PickRay pick, uintptr_t viewport, float x, float y, uintptr_t out,
                         uintptr_t held = 0) {
    if (!viewport || viewport != *(uintptr_t*)gameAt(fp::sig::mainViewport) ||
        !selfPickWanted() || selfPickSet)
        return pick(viewport, x, y, out); // nested: the outer call already masked
    char hit = 0;
    __try {
        selfPickMask(held);
        hit = pick(viewport, x, y, out);
    } __finally {
        selfPickRestore();
    }
    return hit;
}
