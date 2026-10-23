// Input: the game window procedure (F6-F11, right mouse, raw mouse), mouse-look aim, the input and behaviour hooks, head steadying, movement, the attack cue's state.
// Part of first_person.cpp (one translation unit): included there, in order.
static void fingerPosesTick(uintptr_t root); // xr_bones.inl: the held-item finger poses, after the drive
static void motionKeysTick(uintptr_t root);  // motions.inl: the kick and jump keys

static std::atomic<unsigned> dropSpots{0}; // accepted close-view query outputs

static bool closeViewContext();
static void recordHeldThrow(int wheel, float distance);
static bool manualCombatNow();
static bool bindingContextNow();
static bool nativeSettingsBindingInput(UINT message, WPARAM wp, LPARAM lp);

static inline bool alternateBindingKey(unsigned key) {
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

static inline int bindingMessageKey(UINT message, WPARAM wp) {
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

static inline int cursorLookPhysicalKey(int key, LPARAM lp) {
    if (key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU) return key;
    unsigned scan = unsigned((lp >> 16) & 0xff);
    if (lp & (1LL << 24)) scan |= 0xe000;
    const unsigned physical = MapVirtualKeyW(scan, 3);
    return physical > 0 && physical < 256 ? int(physical) : key;
}

static inline int rotateKeyMessage(UINT message, WPARAM wp, int key) {
    if (!key || bindingMessageKey(message, wp) != key) return -1;
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                   message == WM_XBUTTONUP || message == WM_KEYUP || message == WM_SYSKEYUP
               ? 0
               : 1;
}

static void resetBindingWindowOwnership() {
    rmbEvent(fp::RmbEvent::focusLost, false);
    inputOwner.newWindowSession();
}

static void combatMouseMessage(UINT message, WPARAM wp) {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        const bool overlap = manualCombatNow() && (wp & MK_RBUTTON);
        inputOwner.attackDown(overlap);
    } else if ((message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) && manualCombatNow() &&
               (wp & MK_LBUTTON)) {
        inputOwner.suppressAttack();
    }
}

static void consumeNativeBinding(int key) {
    *reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord)) &= ~actionMask(WPARAM(key));
    if (key == VK_LBUTTON) *reinterpret_cast<unsigned*>(gameAt(fp::sig::buttonWord)) &= ~1u;
    if (key == VK_RBUTTON) *reinterpret_cast<unsigned*>(gameAt(fp::sig::buttonWord)) &= ~2u;
}

static void addTo(std::atomic<float>& a, float v) {
    float o = a.load();
    while (!a.compare_exchange_weak(o, o + v)) {}
}

static SRWLOCK worldPointerLock = SRWLOCK_INIT;
static fp::PointerGesture worldPointer;

static fp::InputStamp worldPointerStamp() {
    fp::InputStamp stamp;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    stamp.root = root;
    stamp.head = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
    stamp.scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
    stamp.mode = viewMode.load();
    stamp.generation = inputOwner.generation();
    stamp.policyRevision = inputOwner.policyRevision();
    return stamp;
}

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
static std::atomic<unsigned> guiPointerNativeFallbacks{0};

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
    if (owner.window != w || !owner.previous || owner.retired)
        return DefWindowProcW(w, message, wp, lp);
    if (message == WM_NCDESTROY) {
        const LRESULT result = CallWindowProcW(owner.previous, w, message, wp, lp);
        retireWindowInstallation(owner);
        return result;
    }
    if (InterlockedCompareExchange(&stopping, 0, 0))
        return CallWindowProcW(owner.previous, w, message, wp, lp);

    if (nativeSettingsBindingInput(message, wp, lp)) {
        return (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                message == WM_XBUTTONDBLCLK) ? TRUE : 0;
    }

    const bool fovKey = wp == VK_F9 || wp == VK_F10 || wp == VK_F11;
    const bool fovMessage = message == WM_KEYDOWN || message == WM_KEYUP ||
                            (wp == VK_F10 && (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP));
    if (InterlockedCompareExchange(&active, 0, 0) && viewMode.load() < 2 && fovKey && fovMessage &&
        !(lp & (1LL << 29))) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lp & (1LL << 30)))
            adjustFov(wp == VK_F10 ? 5 : -5, wp == VK_F11);
        return 0;
    }

    const bool keyMessage = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
    const unsigned bit = keyMessage ? keyMask(wp) : 0;
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && bit)
        inputOwner.movementUp(bit);

    const int bindingKey = bindingMessageKey(message, wp);
    if (bindingKey) {
        const int edge = rotateKeyMessage(message, wp, bindingKey);
        const int cursorKey = cursorLookPhysicalKey(bindingKey, lp);
        const bool xbutton =
            message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK || message == WM_XBUTTONUP;
        if (edge == 0) {
            bool cursorUp = cursorLookMessage(cursorKey, false, false);
            if (cursorKey != bindingKey) cursorUp |= cursorLookMessage(bindingKey, false, false);
            const bool physicalUp = inputOwner.windowRelease(cursorKey);
            if (inputOwner.windowRelease(bindingKey) || physicalUp || cursorUp) return xbutton ? TRUE : 0;
        }
        if (edge == 1) {
            const bool repeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (lp & (1LL << 30));
            if (repeat && (inputOwner.windowOwns(bindingKey) || inputOwner.windowOwns(cursorKey))) return 0;
            if (!repeat) {
                inputOwner.windowBegin(bindingKey);
                if (cursorKey != bindingKey) inputOwner.windowBegin(cursorKey);
            }
            const bool cursorToggleActive = !repeat && bindingKey == static_cast<int>(nativeBinding(8)) &&
                                            InterlockedCompareExchange(&rmbFree, 0, 0);
            const bool canTake = !repeat && (bindingContextNow() || cursorToggleActive);
            if (canTake && bindingKey == rotateHeldKey.load() && holdingItem()) {
                inputOwner.windowClaim(bindingKey);
                return xbutton ? TRUE : 0;
            }
            if (canTake && bindingKey && (bindingKey == kickKey.load() || bindingKey == jumpKey.load())) { // the kick and jump keys: the game never sees them
                inputOwner.windowClaim(bindingKey);
                if (bindingKey == kickKey.load()) motionKickRequested = true;
                if (bindingKey == jumpKey.load()) motionJumpRequested = true;
                return xbutton ? TRUE : 0;
            }
            if (canTake && trackingEnabled.load() && bindingKey == trackingRecenterKey.load()) {
                inputOwner.windowClaim(bindingKey);
                ++trackingReset;
                return xbutton ? TRUE : 0;
            }
            if (canTake && cursorLookMessage(cursorKey, true, repeat)) {
                inputOwner.windowClaim(cursorKey);
                return xbutton ? TRUE : 0;
            }
            if (canTake && cursorKey != bindingKey && cursorLookMessage(bindingKey, true, repeat)) {
                inputOwner.windowClaim(bindingKey);
                return xbutton ? TRUE : 0;
            }
        }
    }

    combatMouseMessage(message, wp);

    if ((message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) &&
        bindingContextNow() && !holdingItem()) {
        inputOwner.windowClaim(VK_MBUTTON);
        recenterClicks.fetch_add(1);
        return 0;
    }

    if ((message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) && closeViewContext()) return 0;

    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN) &&
        gameplayOwnsPointer()) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) inputOwner.buttonDown(1);
        if (message == WM_RBUTTONDOWN) inputOwner.buttonDown(2);
    }

    if (message == WM_INPUT && InterlockedCompareExchange(&rawRegistered, 0, 0)) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            AcquireSRWLockExclusive(&rawLock);
            const bool owns = InterlockedCompareExchange(&inputEnabled, 0, 0) &&
                              InterlockedCompareExchange(&mouseCaptured, 0, 0) && !cursorFreeNow();
            if (InterlockedCompareExchange(&rawRegistered, 0, 0))
                rawLook.event((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0, raw.data.mouse.lLastX,
                              raw.data.mouse.lLastY, owns);
            ReleaseSRWLockExclusive(&rawLock);
        }
        return CallWindowProcW(owner.previous, w, message, wp, lp);
    }

    if (message == WM_KEYDOWN && bit && InterlockedCompareExchange(&inputEnabled, 0, 0)) {
        inputOwner.movementDown(bit, !(lp & (1LL << 30)));
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && bit)
        inputOwner.movementUp(bit);

    // The Settings window scrolls with the wheel; nothing of the camera's takes it while the window is up.
    const uintptr_t optionsForm = message == WM_MOUSEWHEEL && base ? *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiOptionsForm)) : 0;
    const bool optionsShown = optionsForm && readable(optionsForm + field::form::visible, 1) &&
                              *reinterpret_cast<const unsigned char*>(optionsForm + field::form::visible);
    if (optionsShown) {
        // The game's scroll panes take no wheel: scroll the Settings window's scrolling Controls pane by three
        // rows a notch, as its own scroll bar would.
        namespace F = field::nativeForm;
        const uintptr_t pane = readable(optionsForm + field::optionsForm::controlsPane, 8)
                                   ? *reinterpret_cast<const uintptr_t*>(optionsForm + field::optionsForm::controlsPane) : 0;
        if (pane && readable(pane, F::verticalScroll + sizeof(float)) &&
            *reinterpret_cast<const unsigned char*>(pane + field::form::visible) &&
            (*reinterpret_cast<const unsigned*>(pane + F::frameFlags) & 0x200)) {
            const int extent = *reinterpret_cast<const int*>(pane + F::scrollExtent);
            const int shown = *reinterpret_cast<const int*>(pane + F::contentBottom) - *reinterpret_cast<const int*>(pane + F::contentTop);
            float& fraction = *reinterpret_cast<float*>(pane + F::verticalScroll);
            if (extent > shown && shown > 0 && std::isfinite(fraction)) {
                const float row = 24.f * float(*reinterpret_cast<const int*>(pane + F::height)) / 416.f; // a native row at this scale
                const float step = 3.f * row * float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                fraction = std::clamp(fraction - step / float(extent - shown), 0.f, 1.f);
                return 0;
            }
        }
    }
    if (message == WM_MOUSEWHEEL && !optionsShown && closeViewContext()) {
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
        if (kind == 2 && doorDistance.load() > 0 && heldDoorDrag()) {
            const float offset = doorCameraOffset.load(), eye = doorDistance.load() - offset;
            doorDistance = fp::holdTargetStep(eye, GET_WHEEL_DELTA_WPARAM(wp), std::fmin(fp::holdNear, eye),
                                              float(grabRange.load())) +
                           offset;
            ++holdNotches;
            return 0;
        }
        if (*reinterpret_cast<const int*>(gameAt(fp::sig::uiOpenCount)) <= 0) {
            ++overheadInputsHeld;
            return 0;
        }
    }

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
            return 0;
        if (wp == VK_F6) { ++inventoryOwnerEpoch; releaseHeldDoor(); }
        if (wp == VK_F6) {
            if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                viewMode.load() == 0) {
                viewMode = 1;
                releaseInput();
                return 0;
            }
            if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                viewMode.load() == 1) {
                tpBlendStart = GetTickCount64();
                viewMode = 2;
                releaseInput();
                return 0;
            }
            if (viewMode.load() == 2 && GetTickCount64() - tpBlendStart.load() < 600)
                return 0;
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
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

static bool playerContext(uintptr_t behavior) {
    if (!InterlockedCompareExchange(&cameraReady, 0, 0) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !gameWindow() ||
        *reinterpret_cast<const int*>(gameAt(fp::sig::uiOpenCount)) > 0)
        return false;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::character);
    if (!root || root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot))) return false;
    if (*reinterpret_cast<const unsigned*>(root + field::character::lifeFlags) & 0x100 ||
        *reinterpret_cast<const unsigned char*>(behavior + field::behaviour::inactive))
        return false;
    const uintptr_t view = *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::view),
                    ui = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::uiView)),
                    manager = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiManager));
    return (!ui || ui == view) && (!manager || !*reinterpret_cast<const uintptr_t*>(manager + field::gui::capture) ||
                                   *reinterpret_cast<const uintptr_t*>(manager + field::gui::capture) == view);
}

static bool controls(uintptr_t behavior) {
    return viewMode.load() < 2 && !cursorFreeNow() && playerContext(behavior);
}

static bool bindingContextNow() {
    __try {
        return gameplayOwnsPointer() && closeViewContext();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool manualCombatNow() {
    if (!base || !InterlockedCompareExchange(&inputEnabled, 0, 0)) return false;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)),
                        bh = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour) : 0;
        return bh && controls(bh) && manualCombat(root, bh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool closeViewContext() {
    if (viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0))
        return false;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t bh = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour) : 0;
    return bh && playerContext(bh);
}

static char (*originalCameraMouse)();
static void (*copyCameraSnapshots)();
static void (*originalCameraOrbit)(uintptr_t, float, float, int);
static int (*originalCameraZoom)(uintptr_t, int);

static void cameraOrbit(uintptr_t self, float x, float y, int flags) {
    if (!closeViewContext()) originalCameraOrbit(self, x, y, flags);
}

static int cameraZoom(uintptr_t self, int amount) {
    return closeViewContext() ? amount : originalCameraZoom(self, amount);
}

static char cameraMouse() {
    healthHit(fp::HealthHook::cameraMouse);
    if (!closeViewContext()) return originalCameraMouse();
    *reinterpret_cast<unsigned char*>(gameAt(fp::sig::cameraMouseActive)) = 0;
    *reinterpret_cast<unsigned char*>(gameAt(fp::sig::cameraMouseTurn)) = 0;
    *reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord)) &= ~0x300u;
    copyCameraSnapshots();
    return 0;
}

static bool headFacing(uintptr_t root, float& yaw, float& pitch) {
    const uintptr_t head = readable(root, 0x11e8) ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
    if (!readable(head, 0x2c4)) return false;
    const float* f = reinterpret_cast<const float*>(head + field::bodyPart::poseForward);
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
        const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(head + field::bodyPart::meshNode);
        return mesh ? *reinterpret_cast<const uintptr_t*>(mesh + field::sceneNode::sector) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

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
        if (request.root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) ||
            request.worldScene != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) ||
            request.head != *reinterpret_cast<const uintptr_t*>(request.root + field::character::headPart) ||
            request.sector != tpAimSector(request.head))
            return false;
        const uintptr_t bh = *reinterpret_cast<const uintptr_t*>(request.root + field::character::behaviour);
        return bh && controls(bh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

static thread_local bool updateAimLocked = false; // behavior() releases aimLock if a fault unwinds updateAim

static void updateAim(uintptr_t behavior) {
    if (!controls(behavior)) {
        releaseInput();
        inputClock = ~0u;
        return;
    }
    bool freeCursor = cursorFreeNow() != 0;
    InterlockedExchange(&inputEnabled, freeCursor ? 0 : 1);
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::character);
    const unsigned clock = *reinterpret_cast<const unsigned*>(gameAt(fp::sig::gameClock));
    if (clock == inputClock) return;
    float dt = inputClock == ~0u ? 0.016f : float(clock - inputClock) * .001f;
    if (dt > 0.1f) dt = 0.1f;
    inputClock = clock;
    const uintptr_t head = *reinterpret_cast<const uintptr_t*>(root + field::character::headPart);
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
    if (!MonitorFromPoint(center, MONITOR_DEFAULTTONULL)) {
        releaseInput();
        return;
    }
    const float manipulationGain = freeCursor ? 1.f : heldAimGain(root, float(rect.right), float(rect.bottom));
    AcquireSRWLockExclusive(&aimLock);
    updateAimLocked = true;
    auto physical = reinterpret_cast<float*>(head + field::bodyPart::pose);
    auto anchor = reinterpret_cast<float*>(root + field::character::position);
    if (!aim.valid || aim.root != root || aim.head != head) {
        const bool entry = aim.entryPending && aim.root == root && aim.head == head;
        const float entryYaw = aim.entryYaw;
        InterlockedExchange(&mouseCaptured, 0);
        tpAimRequest.valid = false;
        tpAimSet = false;
        aim = {};
        aim.root = root;
        aim.head = head;
        const auto direction = reinterpret_cast<const float*>(root + field::character::facing);
        aim.yaw = std::atan2(direction[0], direction[2]);
        aim.targetYaw = aim.yaw;
        if (entry) {
            aim.yaw = aim.targetYaw = entryYaw;
            aim.pitch = aim.targetPitch = 0.f;
        } else if (viewRoot.load() == root && InterlockedCompareExchange(&cameraReady, 0, 0)) {
            const float y = viewYaw.load(), p = viewPitch.load();
            if (std::isfinite(y) && std::isfinite(p)) {
                aim.yaw = aim.targetYaw = y;
                aim.pitch = aim.targetPitch = p > 1.15f ? 1.15f : (p < -1.15f ? -1.15f : p);
            }
        }
        fp::eyePoint(physical, aim.origin, float(eyeSetback.load()));
        for (int a = 0; a < 3; a++) aim.anchor[a] = anchor[a];
    }

    static int boxMode = -1, previousBoxX = -1, previousBoxY = -1;
    const int mode = viewMode.load();
    const bool combat = *reinterpret_cast<const unsigned char*>(root + field::character::combatStance) != 0;
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
        }
        worldPointerMotion(dx, dy);
        if (combat) aim.swing.move(dx, dy, float(rect.right));
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
    aim.targetYaw = std::remainder(aim.targetYaw + trackedTurnTake(), 6.28318530718f); // a VR turn is a mouse turn
    if (aim.targetPitch > 1.15f) aim.targetPitch = 1.15f;
    if (aim.targetPitch < -1.15f) aim.targetPitch = -1.15f;
    aim.yaw = combat ? aim.targetYaw : fp::approachAngle(aim.yaw, aim.targetYaw, dt);
    aim.pitch = combat ? aim.targetPitch : fp::approachAngle(aim.pitch, aim.targetPitch, dt);

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
    fp::eyePoint(physical, aim.origin, float(eyeSetback.load()));
    for (int a = 0; a < 3; a++) {
        aim.anchor[a] = anchor[a];
        aim.point[a] = aim.origin[a] + direction[a] * 1000.f;
    }
    TpAimRequest request;
    request.root = root;
    request.head = head;
    request.sector = mode == 1 ? tpAimSector(head) : 0;
    request.worldScene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
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
        std::memcpy(aim.point, tpAimLocal, 12);
    else
        tpAimSet = false;
    aim.attentionStamp = {root, head, request.worldScene, mode, generation, policy};
    aim.valid = true;
    ++aim.revision;
    updateAimLocked = false;
    ReleaseSRWLockExclusive(&aimLock);
    if (!freeCursor) {
        SetCursorPos(center.x, center.y);
        InterlockedExchange(&mouseCaptured, 1);
        if (!InterlockedCompareExchange(&cursorClipped, 0, 0)) {
            RECT clip = rect;
            MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&clip), 2);
            if (ClipCursor(&clip)) InterlockedExchange(&cursorClipped, 1);
        }
    } else
        InterlockedExchange(&mouseCaptured, 0);
    ++controlCalls;
}

static bool heldAim(uintptr_t behavior, Aim& a) {
    if (viewMode.load() >= 2 || !InterlockedCompareExchange(&cameraReady, 0, 0) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0))
        return false;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::character);
    if (!root || root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot))) return false;
    const uintptr_t view = *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::view),
                    ui = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::uiView)),
                    manager = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiManager));
    const bool otherView = (ui && ui != view) || (manager && *reinterpret_cast<const uintptr_t*>(manager + field::gui::capture) &&
                                                  *reinterpret_cast<const uintptr_t*>(manager + field::gui::capture) != view);
    if (!(*reinterpret_cast<const int*>(gameAt(fp::sig::uiOpenCount)) > 0 || otherView || cursorFreeNow() || !gameWindow()))
        return false;
    AcquireSRWLockShared(&aimLock);
    a = aim;
    ReleaseSRWLockShared(&aimLock);
    if (a.valid && a.root == root && a.head && a.head == *reinterpret_cast<const uintptr_t*>(root + field::character::headPart)) {
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
    return a.valid && a.root == *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::character) && a.head &&
           a.head == *reinterpret_cast<const uintptr_t*>(a.root + field::character::headPart);
}

using AnalogVectorOp = void (*)(float*, const float*, float);
static AnalogVectorOp originalAnalogScale, originalAnalogAdd;

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
        !std::memcmp(origin, boundedPlacement.values, 12) && !std::memcmp(direction, boundedPlacement.values + 3, 12))
        return -boundedPlacement.distance;
    return scale;
}

static void analogVectorFor(uintptr_t caller, bool forward, float* out, const float* axis, float amount) {
    const bool movement = forward ? caller == gameAt(fp::sig::controllerForwardReturn) ||
                                        caller == gameAt(fp::sig::xinputForwardReturn)
                                  : caller == gameAt(fp::sig::controllerRightReturn) ||
                                        caller == gameAt(fp::sig::xinputRightReturn);
    float planar[3];
    if (movement && out == reinterpret_cast<float*>(gameAt(fp::sig::analogStick))) {
        __try {
            const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
            const uintptr_t bh = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour) : 0;
            Aim a;
            if (bh && getAim(bh, a) && std::isfinite(a.yaw)) {
                planar[0] = forward ? std::sin(a.yaw) : -std::cos(a.yaw);
                planar[1] = 0.f;
                planar[2] = forward ? std::cos(a.yaw) : std::sin(a.yaw);
                axis = planar;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    (forward ? originalAnalogAdd : originalAnalogScale)(out, axis, amount);
}

static void analogScale(float* out, const float* axis, float amount) {
    analogVectorFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), false, out, axis, amount);
}

static void analogAdd(float* out, const float* axis, float amount) {
    analogVectorFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), true, out, axis, amount);
}

static void headingToPoint(const Aim& a, float* d) {
    const float yaw = a.rayValid ? a.rayYaw : a.yaw;
    d[0] = std::sin(yaw);
    d[1] = 0;
    d[2] = std::cos(yaw);
}

static void gazeToPoint(const Aim& a, float* d) {
    if (viewMode.load() == 0) {
        d[0] = std::sin(a.yaw) * std::cos(a.pitch);
        d[1] = std::sin(a.pitch);
        d[2] = std::cos(a.yaw) * std::cos(a.pitch);
        return;
    }
    const uintptr_t head = *reinterpret_cast<const uintptr_t*>(a.root + field::character::headPart);
    if (!head) {
        d[0] = std::sin(a.yaw) * std::cos(a.pitch);
        d[1] = std::sin(a.pitch);
        d[2] = std::cos(a.yaw) * std::cos(a.pitch);
        return;
    }
    const auto h = reinterpret_cast<const float*>(head + field::bodyPart::pose);
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

static void prepareAttackHeading(uintptr_t bh) {
    Aim a;
    if (!getAim(bh, a)) return;
    const float heading = a.rayValid ? a.rayYaw : a.yaw;
    if (!std::isfinite(heading)) return;
    float presented = heading;
    bool borrowed = false;
    if (attackHeadingPresentation.behavior == bh && ordinaryCombat(a.root, bh) &&
        (*reinterpret_cast<const unsigned*>(gameAt(fp::sig::buttonWord)) & 1u) &&
        !*reinterpret_cast<const unsigned char*>(bh + field::behaviour::pressBlocked) &&
        !(*reinterpret_cast<const unsigned*>(bh + field::behaviour::pressFlags) & 1u)) {
        const float body = *reinterpret_cast<const float*>(a.root + field::character::bodyHeading);
        if (std::isfinite(body)) {
            const bool wide = *reinterpret_cast<const int*>(a.root + field::character::gripFlags) != 0;
            presented = requestedAttackHeading(a, heading, body, wide);
            if (attackSideKey && (keyState(attackSideKey) & 0x8000)) {
                presented = fp::flippedSideHeading(presented, body, wide);
                ++sideFlips;
            }
            borrowed = presented != heading;
        }
    }
    *reinterpret_cast<float*>(bh + field::behaviour::aimHeading) = presented;
    if (attackHeadingPresentation.behavior == bh)
        attackHeadingPresentation = {bh, heading, presented, borrowed};
}

static fp::InputContext<field::bindings::count> inputContext(uintptr_t root, uintptr_t bh) {
    fp::InputContext<field::bindings::count> context;
    context.root = root;
    context.head = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
    context.scene = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene));
    context.mode = viewMode.load();
    context.combat = root && *reinterpret_cast<const unsigned char*>(root + field::character::combatStance);
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
    for (int bound = 1; bound < 256; ++bound)
        if (inputOwner.bindingOwned(bound) && inputOwner.maskBinding(bound, (keyState(bound) & 0x8000) != 0)) {
            consumeNativeBinding(bound);
            if (alternateBindingKey(unsigned(bound))) *reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord)) &= ~0x80u;
        }
}

static void inputProfileBody() {
    healthHit(fp::HealthHook::pollInput);
    fp::profile::invoke<fp::profile::Hook::inputNative>(originalInput);
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t bh = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour) : 0;
    cursorLookPoll();
    const bool owned = bh && controls(bh);
    auto buttons = reinterpret_cast<unsigned*>(gameAt(fp::sig::buttonWord));
    const InputOwner::Sample sample = inputOwner.beginSample(inputContext(root, bh), owned, *buttons);
    if (owned) {
        const unsigned physical = physicalMovement();
        unsigned& keyWord = *reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord));
        keyWord = inputOwner.movementSample(keyWord, physical, movementBits);
        *buttons = inputOwner.bufferedButtons(sample);
    } else
        inputOwner.discardButtons();
    sampleHeldBindings(owned, sample);
    trackedControllerInput(root, bh, owned);
    fingerPosesTick(root);
    if (owned) motionKeysTick(root);
    combatInput(root, bh, owned, sample);
    if (owned) prepareAttackHeading(bh);
    duckKey = (*reinterpret_cast<const unsigned*>(gameAt(fp::sig::keyWord)) & duckKeyBit) != 0;
}

static void input() {
    return fp::profile::invoke<fp::profile::Hook::input>(inputProfileBody);
}

static void cursorAim(uintptr_t behavior) {
    Aim a;
    if (!getAim(behavior, a) && !heldAim(behavior, a)) {
        originalCursor(behavior);
        return;
    }
    const uintptr_t root = a.root;
    float d[3];
    headingToPoint(a, d);
    std::memcpy(reinterpret_cast<void*>(root + field::character::aimDirection), d, 12);
    *reinterpret_cast<float*>(behavior + field::behaviour::aimHeading) =
        reinterpret_cast<float (*)(float*)>(gameAt(fp::sig::headingFromDirection))(d);
    reinterpret_cast<void (*)(uintptr_t, float*)>(gameAt(fp::sig::aimFilterPush))(behavior + field::behaviour::aimFilter, d);
    reinterpret_cast<void (*)(uintptr_t)>(gameAt(fp::sig::aimFilterUpdate))(behavior + field::behaviour::aimFilter);
    *reinterpret_cast<float*>(root + field::character::gazeDistance) = 1000.f;
    gazeToPoint(a, d);
    std::memcpy(reinterpret_cast<void*>(root + field::character::gaze), d, 12);
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
    std::memcpy(out, a.point, 12);
    std::memcpy(reinterpret_cast<void*>(behavior + field::behaviour::aimPoint), out, 12);
    ++pointCalls;
}

static unsigned steadyCalls = 0;
static float lastHeadRoll = 0;

static void steadyHeadAround(uintptr_t root, void (*build)(uintptr_t, float, unsigned char), float time,
                             unsigned char mode) {
    const int strength = headSteady.load();
    const uintptr_t head = readable(root, 0x11e8) ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
    float* cap = strength > 0 && readable(head, 0x780) ? reinterpret_cast<float*>(head + field::bodyPart::slackCap) : nullptr;
    const float kept = cap ? *cap : 0.f;
    if (cap && !(std::isfinite(kept) && kept > 0.f && kept <= 4.f))
        cap = nullptr;
    // cap implies readable(head, 0x780), which covers the mesh pointer.
    const uintptr_t mesh = cap ? *reinterpret_cast<const uintptr_t*>(head + field::bodyPart::meshNode) : 0;
    const bool meshReadable = cap && readable(mesh, 0x70);
    float roll = 0.f;
    {
        if (meshReadable) {
            float m[9];
            std::memcpy(m, reinterpret_cast<const void*>(mesh + field::sceneNode::transform), 36);
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
        if (head != seenHead) {
            seen = fp::HeadWobble();
            seenHead = head;
        }
        if (meshReadable) {
            float m[9];
            std::memcpy(m, reinterpret_cast<const void*>(mesh + field::sceneNode::transform), 36);
            const auto g = reinterpret_cast<const float*>(root + field::character::gaze);
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
        fp::profile::invoke<fp::profile::Hook::poseNative>(build, root, time, mode);
    } __finally {
        if (cap && *cap == lowered) *cap = kept;
    }
}

static constexpr float crouchHeadroom = 70.f;
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
    if (!readable(part, 0x600) || *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character) != root) return;
    b.root = root;
    b.flag = *reinterpret_cast<const unsigned*>(part + field::bodyPart::flags) & partExtraRotation;
    std::memcpy(b.kept, reinterpret_cast<const void*>(part + field::bodyPart::extraRotation), 16);
    if (b.flag) {
        if (!fp::composeSpineRotation(quat, b.kept, b.written)) return;
    } else
        std::memcpy(b.written, quat, 16);
    b.part = part;
    std::memcpy(reinterpret_cast<void*>(part + field::bodyPart::extraRotation), b.written, 16);
    *reinterpret_cast<unsigned*>(part + field::bodyPart::flags) |= partExtraRotation;
}

static void restoreSpinePart(const SpinePoseBorrow& b) {
    if (!b.part || !readable(b.part, 0x600) ||
        *reinterpret_cast<const uintptr_t*>(b.part + field::bodyPart::character) != b.root) return;
    auto q = reinterpret_cast<float*>(b.part + field::bodyPart::extraRotation);
    auto flags = reinterpret_cast<unsigned*>(b.part + field::bodyPart::flags);
    if (std::memcmp(q, b.written, 16) != 0) return;
    std::memcpy(q, b.kept, 16);
    if (*flags & partExtraRotation) *flags = (*flags & ~partExtraRotation) | b.flag;
}

static void spineBendStep(uintptr_t root, SpinePoseBorrow (&borrowed)[2]) {
    const uintptr_t head = readable(root, 0x11e8) ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
    const uintptr_t chest = readable(root, 0xd50) ? *reinterpret_cast<const uintptr_t*>(root + field::character::chestPart) : 0;
    const bool chestReadable = readable(chest, 0x430);
    const uintptr_t rootPart = chestReadable ? *reinterpret_cast<const uintptr_t*>(chest + field::bodyPart::rootPart) : 0,
                    lower = chestReadable ? *reinterpret_cast<const uintptr_t*>(chest + field::bodyPart::parentPart) : 0;
    const uintptr_t parts[2] = {chest && chest != rootPart ? chest : 0,
                                lower && lower != rootPart ? lower : 0};
    const bool eligible = InterlockedCompareExchange(&active, 0, 0) &&
                          !InterlockedCompareExchange(&stopping, 0, 0) &&
                          InterlockedCompareExchange(&cameraReady, 0, 0) && viewMode.load() < 2 && head &&
                          bodyCalm(root, head);
    const int follow =
        eligible && !*reinterpret_cast<const unsigned char*>(root + field::character::combatStance) ? spineFollow.load() : 0;
    const bool on = eligible && follow > 0;
    const float bend =
        follow > 0 ? fp::spineBend(reinterpret_cast<const float*>(root + field::character::gaze), follow) : 0.f;
    for (int i = 0; i < 2; ++i) {
        float q[4];
        const float share = i == 0 ? 1.f - fp::spineLowerShare : fp::spineLowerShare;
        const uintptr_t parent =
            on && parts[i] && readable(parts[i], 0x430) ? *reinterpret_cast<const uintptr_t*>(parts[i] + field::bodyPart::parentPart) : 0;
        const bool apply =
            on && parts[i] && readable(parts[i], 0x2d0) && readable(parent, 0x2d0) &&
            fp::spineBendQuat(reinterpret_cast<const float*>(parts[i] + field::bodyPart::pose),
                             reinterpret_cast<const float*>(parent + field::bodyPart::pose),
                             reinterpret_cast<const float*>(root + field::character::gaze), bend * share, q);
        if (apply) borrowSpinePart(root, parts[i], q, borrowed[i]);
    }
    if (on) ++spineBendTicks;
}

static volatile LONG poseBuildsInFlight = 0;
static bool awaitPoseBuilders(DWORD timeout = 2000) {
    const ULONGLONG started = GetTickCount64();
    while (InterlockedCompareExchange(&poseBuildsInFlight, 0, 0)) {
        if (GetTickCount64() - started >= timeout) return false;
        Sleep(1);
    }
    return true;
}

static void poseBuilderOwned(uintptr_t root, float time, unsigned char mode) {
    if (root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot))) {
        fp::profile::invoke<fp::profile::Hook::poseNative>(originalPose, root, time, mode);
        return;
    }
    SpinePoseBorrow borrowed[2]{};
    uintptr_t* contextual = nullptr;
    uintptr_t savedContext = 0;
    const bool trackedMotor = trackedMotorBegin(root);
    __try {
        if (root == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot))) {
            ++playerSteps;
            if (InterlockedCompareExchange(&active, 0, 0) &&
                !InterlockedCompareExchange(&stopping, 0, 0) &&
                InterlockedCompareExchange(&cameraReady, 0, 0) && viewMode.load() < 2) {
                if (crouchAppliesTo(root)) {
                    *reinterpret_cast<float*>(root + field::character::duckAmount) = 1.f;
                    ++crouchTicks;
                }
            }
            // A tracked body owns the spine instead of the look-driven bend.
            __try {
                if (!trackedMotor) spineBendStep(root, borrowed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        const uintptr_t bh = *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour);
        Aim a;
        if (!bh || !getAim(bh, a))
            fp::profile::invoke<fp::profile::Hook::poseNative>(originalPose, root, time, mode);
        else {
            contextual = reinterpret_cast<uintptr_t*>(root + field::character::contextualTarget);
            savedContext = *contextual;
            *contextual = 0;
            float d[3];
            gazeToPoint(a, d);
            trackedMotorGaze(root, d);
            std::memcpy(reinterpret_cast<void*>(root + field::character::gaze), d, 12);
            steadyHeadAround(root, originalPose, time, mode);
        }
    } __finally {
        if (AbnormalTermination()) trackedMotorFault();
        __try {
            if (contextual && !*contextual) *contextual = savedContext;
        } __finally {
            __try {
                restoreSpinePart(borrowed[1]);
            } __finally {
                __try { restoreSpinePart(borrowed[0]); }
                __finally { trackedMotorEnd(); }
            }
        }
    }
}

static void poseBuilderProfileBody(uintptr_t root, float time, unsigned char mode) {
    InterlockedIncrement(&poseBuildsInFlight);
    __try {
        healthHit(fp::HealthHook::poseBuilder);
        poseBuilderOwned(root, time, mode);
    } __finally {
        InterlockedDecrement(&poseBuildsInFlight);
    }
}

static void poseBuilder(uintptr_t root, float time, unsigned char mode) {
    return fp::profile::invoke<fp::profile::Hook::pose>(poseBuilderProfileBody, root, time, mode);
}

using PartSettle = void (*)(uintptr_t);
static PartSettle originalPartSettle;
static std::atomic<int> headBob{100};
static std::atomic<unsigned> steadyTicks{0};
static fp::HeadSteadyState headSteadyState;
static fp::BodySteadyState bodySteadyState[2];
static uintptr_t steadiedHead = 0, steadiedRoot = 0, steadyNeck = 0, steadyChest = 0;
static bool bodyCalm(uintptr_t root, uintptr_t head);

static bool steadyAllowed(uintptr_t root, uintptr_t head) {
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) || viewMode.load() != 0 || headSteady.load() <= 0)
        return false;
    return bodyCalm(root, head);
}

static bool bodyCalm(uintptr_t root, uintptr_t head) {
    if (!readable(root, 0x1300) || *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) != head ||
        !*reinterpret_cast<const unsigned char*>(root + field::character::physicsLive))
        return false;
    const unsigned state = *reinterpret_cast<const unsigned*>(root + field::character::stateFlags);
    if ((state & (0xac0 | 0x40204 | 0xa00)) || (*reinterpret_cast<const unsigned*>(root + field::character::lifeFlags) & 0x100))
        return false;
    const float balance = *reinterpret_cast<const float*>(root + field::character::stamina),
                most = *reinterpret_cast<const float*>(root + field::character::staminaMax);
    if (!(balance >= most - 0.05f)) return false;
    const uintptr_t drag = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::dragManager));
    if (readable(drag, 0x20) && (*reinterpret_cast<const unsigned*>(drag + 8) & (0x4000 | 0x10000 | 0x80000)))
        return false;
    return true;
}

static void steadyHeadPart(uintptr_t root, uintptr_t head) {
    if (!readable(head, 0x5c0) || !readable(root, 0x1300)) {
        steadyNeck = steadyChest = 0;
        return;
    }
    const uintptr_t rootPart = *reinterpret_cast<const uintptr_t*>(head + field::bodyPart::rootPart);
    auto below = [&](uintptr_t p) {
        const uintptr_t q = p ? *reinterpret_cast<const uintptr_t*>(p + field::bodyPart::parentPart) : 0;
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
    if (fp::steadyHead(headSteadyState, reinterpret_cast<float(*)[3]>(head + field::bodyPart::points),
                       reinterpret_cast<const float*>(head + field::bodyPart::poseCoefficients),
                       reinterpret_cast<const float*>(root + field::character::gaze), s))
        ++steadyTicks;
    headingTurn = holdHeadingOn.load()
                      ? fp::steadyHeading(reinterpret_cast<float(*)[3]>(head + field::bodyPart::points),
                                          reinterpret_cast<const float*>(head + field::bodyPart::poseCoefficients),
                                          reinterpret_cast<const float*>(root + field::character::gaze), headSteadyYaw.load())
                      : 0.f;
}

static void steadyUnderHead(uintptr_t root, int which, uintptr_t part) {
    if (*reinterpret_cast<const unsigned char*>(root + field::character::combatStance)) {
        bodySteadyState[which].primed = false;
        return;
    }
    fp::steadyBodyPart(bodySteadyState[which], reinterpret_cast<float(*)[3]>(part + field::bodyPart::points),
                       reinterpret_cast<const float*>(part + field::bodyPart::poseCoefficients), headSteady.load());
}

static void partSettleProfileBody(uintptr_t part) {
    healthHit(fp::HealthHook::partSettle);
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
        const uintptr_t head = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::headPart) : 0;
        if (part && (part == head || part == steadyNeck || part == steadyChest) && trackedMotorOwns(root)) {
            // Native XR motors own head and chest; the stabiliser's history must not resume stale.
            headSteadyState = {};
            bodySteadyState[0] = {};
            bodySteadyState[1] = {};
            steadiedHead = steadiedRoot = steadyNeck = steadyChest = 0;
            headingTurn = 0;
        } else if (part && part == head)
            steadyHeadPart(root, head);
        else if (part && head == steadiedHead && (part == steadyNeck || part == steadyChest)) {
            if (steadyAllowed(root, head)) steadyUnderHead(root, part == steadyNeck ? 0 : 1, part);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        headSteadyState = fp::HeadSteadyState();
        steadyNeck = steadyChest = 0;
    }
    fp::profile::invoke<fp::profile::Hook::partNative>(originalPartSettle, part);
}

static void partSettle(uintptr_t part) {
    return fp::profile::invoke<fp::profile::Hook::part>(partSettleProfileBody, part);
}

static void step(uintptr_t bh) {
    Aim a;
    const bool owned = getAim(bh, a);
    if (!owned) {
        originalStep(bh);
        return;
    }
    unsigned digital = *reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord)) & movementBits;
    if ((digital & 5u) == 5u) digital &= ~5u;
    if ((digital & 10u) == 10u) digital &= ~10u;
    const auto stick = reinterpret_cast<const float*>(gameAt(fp::sig::analogStick));
    const float magnitude = stick[0] * stick[0] + stick[1] * stick[1] + stick[2] * stick[2];
    const bool analog = !std::isfinite(magnitude) || double(magnitude) > 0.01;
    auto pending = reinterpret_cast<unsigned*>(bh + field::behaviour::pendingMove);
    const bool pendingDigital = *pending && !(*pending & ~movementBits);
    if (!digital && !analog && pendingDigital) *pending = 0;
    if (!owned) {
        originalStep(bh);
        return;
    }
    if (*reinterpret_cast<const unsigned char*>(a.root + field::character::combatStance))
        ++combatSteps;
    else
        ++unexpectedRelaxedSteps;
    auto state = reinterpret_cast<unsigned*>(bh + field::behaviour::hoverState);
    const unsigned hovered = *state;
    auto facing = reinterpret_cast<float*>(a.root + field::character::facing);
    float savedFacing[3]{}, presentedFacing[3]{};
    const bool present = digital && !analog && std::isfinite(a.yaw) &&
                         *reinterpret_cast<const unsigned char*>(a.root + field::character::combatStance);
    if (present) {
        std::memcpy(savedFacing, facing, sizeof savedFacing);
        presentedFacing[0] = std::sin(a.yaw);
        presentedFacing[2] = std::cos(a.yaw);
    }
    __try {
        if (hovered == 4) *state = 0;
        if (present) std::memcpy(facing, presentedFacing, sizeof presentedFacing);
        originalStep(bh);
    } __finally {
        if (present && !std::memcmp(facing, presentedFacing, sizeof presentedFacing))
            std::memcpy(facing, savedFacing, sizeof savedFacing);
        if (hovered == 4 && *state == 0) *state = hovered;
    }
}

static thread_local float exploringTrackedDistance = 0; // a tracked walk stops at its room goal
static void travelTargetFor(uintptr_t caller, float* out, const float* origin, const float* direction, float scale) {
    scale = heldPlacementStart(caller, origin, direction, scale);
    if (exploringRoot && out == reinterpret_cast<float*>(exploringRoot + field::character::travelTarget) &&
        origin == reinterpret_cast<float*>(exploringRoot + field::character::position) &&
        (caller == gameAt(fp::sig::exploreTravelReturnA) ||
         caller == gameAt(fp::sig::exploreTravelReturnB))) {
        originalTravelTarget(out, origin, exploringDirection,
                             exploringTrackedDistance > 0 ? (std::min)(scale, exploringTrackedDistance) : scale);
        ++walkTargets;
        return;
    }
    originalTravelTarget(out, origin, direction, scale);
}
static void travelTarget(float* out, const float* origin, const float* direction, float scale) {
    travelTargetFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), out, origin, direction, scale);
}

static void walkGait(uintptr_t root, const float* direction) {
    const uintptr_t bh = *reinterpret_cast<const uintptr_t*>(root + field::character::behaviour);
    Aim a;
    if (!*reinterpret_cast<const unsigned char*>(root + field::character::combatStance) && bh && getAim(bh, a) && a.root == root &&
        a.travelValid && *reinterpret_cast<const unsigned char*>(root + field::character::walking)) {
        float axis[3];
        const float sign = (*reinterpret_cast<const unsigned*>(root + field::character::gaitFlags) & 2) ? -1.f : 1.f;
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
    auto mask = reinterpret_cast<unsigned*>(gameAt(fp::sig::keyWord));
    auto nativeAnalog = reinterpret_cast<float*>(gameAt(fp::sig::analogStick));
    const unsigned saved = *mask;
    float travel[3], analog[3];
    const bool digital = fp::travel(saved, a.yaw, travel);
    std::memcpy(analog, nativeAnalog, sizeof analog);
    // Keys win, then a stick, then real walking: the tracked pelvis supplies a direction and
    // a bounded distance that native exploration still queries against the environment.
    float trackedDistance = 0;
    const bool tracked = !digital && !fp::analogActive(analog) && trackedWalkInput(a.root, travel, trackedDistance);
    const bool moving = digital || (tracked && fp::analogActive(travel));
    const bool stick = !moving && !tracked && fp::analogActive(analog);
    const bool walking = *reinterpret_cast<const unsigned char*>(a.root + field::character::walking) != 0;
    AcquireSRWLockExclusive(&aimLock);
    if (aim.root == a.root && aim.head == a.head) {
        if (moving) {
            std::memcpy(aim.travel, travel, sizeof travel);
            aim.travelValid = true;
        } else if (stick || !walking)
            aim.travelValid = false;
    }
    ReleaseSRWLockExclusive(&aimLock);
    auto intent = reinterpret_cast<unsigned char*>(behavior + field::behaviour::moveIntent);
    const unsigned char savedIntent = *intent;
    const uintptr_t previousRoot = exploringRoot;
    const float previousTrackedDistance = exploringTrackedDistance;
    float previousDirection[3];
    std::memcpy(previousDirection, exploringDirection, sizeof previousDirection);
    __try {
        *mask &= ~movementBits;
        if (!stick) std::memcpy(nativeAnalog, travel, sizeof travel);
        *intent = 0;
        exploringRoot = moving ? a.root : 0;
        exploringTrackedDistance = tracked && moving ? trackedDistance : 0;
        std::memcpy(exploringDirection, travel, sizeof travel);
        originalExplore(behavior);
        if (moving) ++relaxedSteps;
    } __finally {
        if (tracked && AbnormalTermination()) trackedLocomotionFault();
        exploringRoot = previousRoot;
        exploringTrackedDistance = previousTrackedDistance;
        std::memcpy(exploringDirection, previousDirection, sizeof previousDirection);
        if (!stick && !std::memcmp(nativeAnalog, travel, sizeof travel))
            std::memcpy(nativeAnalog, analog, sizeof analog);
        if (!(*mask & movementBits)) *mask = (*mask & ~movementBits) | (saved & movementBits);
        if (readable(behavior, field::behaviour::moveIntent + sizeof *intent) &&
            *reinterpret_cast<const uintptr_t*>(behavior + field::behaviour::character) == a.root && !*intent)
            *intent = savedIntent;
    }
}

static bool readable(uintptr_t address, size_t size);
static bool readable(uintptr_t address, size_t size, MEMORY_BASIC_INFORMATION& region);
static inline bool readableRegionContains(const MEMORY_BASIC_INFORMATION& region, uintptr_t address, size_t size);

static fp::SwingHands swingHands(uintptr_t root, fp::OriginCue cue, int motion) {
    fp::SwingHands s;
    if (cue != fp::OriginCue::defaultSide && cue != fp::OriginCue::oppositeSide) return s;
    const int count = *reinterpret_cast<const int*>(root + field::character::weaponCount);
    const unsigned grip = *reinterpret_cast<const unsigned*>(root + field::character::gripFlags);
    handCount = count;
    handGrip = int(grip);
    if (count < 1 || count > 2) return s;
    s.alternate = (grip & 0x80) != 0;
    const uintptr_t weapons[2] = {*reinterpret_cast<const uintptr_t*>(root + field::character::weapons),
                                  count > 1 ? *reinterpret_cast<const uintptr_t*>(root + field::character::weapons + 8) : 0};
    const uintptr_t held[2] = {*reinterpret_cast<const uintptr_t*>(root + field::character::heldItems),
                               *reinterpret_cast<const uintptr_t*>(root + field::character::heldItems + 8)};
    const uintptr_t parts[2] = {*reinterpret_cast<const uintptr_t*>(root + field::character::handParts),
                                *reinterpret_cast<const uintptr_t*>(root + field::character::handParts + 8)};
    uintptr_t w = 0;
    if (motion >= 5 && motion <= 10) {
        w = *reinterpret_cast<const uintptr_t*>(root + field::character::swingWeapon);
        s.slot = w && w == weapons[0] ? 0 : (w && w == weapons[1] ? 1 : -1);
    } else {
        s.slot = fp::swingSlot(cue == fp::OriginCue::oppositeSide, s.alternate);
        w = weapons[s.slot];
        if (!w) s.slot = -1;
    }
    const uintptr_t table = gameAt(fp::sig::gripTable);
    constexpr uintptr_t stride = 0xb8;
    uintptr_t entry = s.slot == 1 ? table + 5 * stride
                                  : (s.slot == 0 ? *reinterpret_cast<const uintptr_t*>(root + field::character::gripEntry) : 0);
    if (entry < table || entry >= table + 6 * stride || (entry - table) % stride) entry = 0;
    if (entry) {
        const int h = *reinterpret_cast<const int*>(entry + 8);
        s.expectedHand = h == 0 || h == 1 ? h : -1;
    }
    if (s.slot >= 0) s.hand = fp::weaponHand(w, held, parts, s.expectedHand);
    int sides[2]{};
    HandNamesStatus names;
    for (int h = 0; h < 2; h++) {
        const uintptr_t part = parts[h];
        const uintptr_t mesh = readable(part, 0x20) ? *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::meshNode) : 0;
        if (!mesh || !readable(mesh, 0x168) || *reinterpret_cast<const uintptr_t*>(mesh + field::sceneNode::bodyPart) != part) {
            names.names[h][0] = 0;
            continue;
        }
        char name[17]{};
        std::memcpy(name, reinterpret_cast<void*>(mesh + field::sceneNode::name), 16);
        sides[h] = fp::handSideFromName(name);
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
    if (wanted == natural && wanted != opposite) return body;
    if (wanted == opposite && wanted != natural) {
        const float threshold = (wideThreshold ? 20.f : 15.f) * 0.0174532925f;
        return fp::wrapHeading(body + threshold + 0.174532925f);
    }
    return heading;
}

static void publishCue(uintptr_t self) {
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(self + field::behaviour::character);
    if (!root || root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)))
        return;
    fp::CueInputs in;
    Aim a;
    if (attackCue && getAim(self, a)) {
        in.owned = true;
        in.combat = *reinterpret_cast<const unsigned char*>(root + field::character::combatStance) != 0;
        in.upright = (*reinterpret_cast<const unsigned*>(root + field::character::stateFlags) & 0x40204) == 0;
        in.attackHeld = (*reinterpret_cast<const unsigned*>(gameAt(fp::sig::buttonWord)) & 1) != 0;
        in.alternateHeld = (*reinterpret_cast<const unsigned*>(gameAt(fp::sig::keyWord)) & 0x80) != 0;
        in.pressBlocked = *reinterpret_cast<const unsigned char*>(self + field::behaviour::pressBlocked) != 0;
        in.modifierHeld = attackSideKey && (GetAsyncKeyState(attackSideKey) & 0x8000);
        in.wideThreshold = *reinterpret_cast<const int*>(root + field::character::gripFlags) != 0;
        in.motion = *reinterpret_cast<const int*>(root + field::character::motion);
        in.clock = *reinterpret_cast<const unsigned*>(gameAt(fp::sig::gameClock));
        in.lastPress = *reinterpret_cast<const unsigned*>(self + field::behaviour::lastPressTime);
        in.aimHeading = *reinterpret_cast<const float*>(self + field::behaviour::aimHeading);
        in.bodyHeading = *reinterpret_cast<const float*>(root + field::character::bodyHeading);
        in.aimHeading = requestedAttackHeading(a, in.aimHeading, in.bodyHeading, in.wideThreshold);
    }
    const fp::OriginCue cue = fp::originCue(in);
    cueState.store(int(cue));
    {
        const int motion = *reinterpret_cast<const int*>(root + field::character::motion);
        swinging.store(*reinterpret_cast<const unsigned char*>(root + field::character::combatStance) != 0 && motion >= 5 &&
                       motion <= 12);
    }
    cueOrigin.store(int(fp::originSide(cue, swingHands(root, cue, in.motion))));
    {
        static fp::Windup w;
        const int motion = *reinterpret_cast<const int*>(root + field::character::motion);
        const bool on = attackCue && in.owned && in.combat && motion >= 5 && motion <= 12 &&
                        (motion != 11 || cueThrust.load());
        fp::windupStep(w, on, motion, *reinterpret_cast<const float*>(root + field::character::motionProgress),
                       *reinterpret_cast<const unsigned*>(root + field::character::swingBits), GetTickCount64());
        if (on)
            cueArcSide.store(motion == 11 ? int(fp::CueSide::bottom)
                                          : cueOrigin.load());
        cueArcFill.store(w.fill);
        cueArcFade.store(w.fadeFrom);
        cueArcPublished.store(GetTickCount64());
        cueArcPhase.store(w.phase);
    }
}

static void behavior(uintptr_t self) {
    healthHit(fp::HealthHook::playerBehavior);
    __try {
        updateAim(self);
    } __finally {
        if (updateAimLocked) {
            updateAimLocked = false;
            ReleaseSRWLockExclusive(&aimLock);
        }
    }
    const AttackHeadingPresentation previous = attackHeadingPresentation;
    attackHeadingPresentation = {self};
    __try {
        originalBehavior(self);
    } __finally {
        const AttackHeadingPresentation shown = attackHeadingPresentation;
        if (shown.behavior == self && shown.borrowed &&
            *reinterpret_cast<float*>(self + field::behaviour::aimHeading) == shown.presented)
            *reinterpret_cast<float*>(self + field::behaviour::aimHeading) = shown.heading;
        attackHeadingPresentation = previous;
    }
    Aim a;
    if (getAim(self, a)) {
        float heading[3];
        headingToPoint(a, heading);
        if ((*reinterpret_cast<const unsigned*>(a.root + field::character::stateFlags) & 0x40204) == 0) {
            if (!*reinterpret_cast<const unsigned char*>(a.root + field::character::combatStance) ||
                !*reinterpret_cast<const unsigned*>(self + field::behaviour::swingState)) {
                std::memcpy(reinterpret_cast<void*>(a.root + field::character::facing), heading, 12);
                std::memcpy(reinterpret_cast<void*>(a.root + field::character::movementFacing), heading, 12);
            }
        }
    }
    publishCue(self);
}

static constexpr ULONGLONG reticleFreshMs = 250;
static bool gameplayOwnsPointer() {
    return base && viewMode.load() < 2 && InterlockedCompareExchange(&active, 0, 0) &&
           !InterlockedCompareExchange(&stopping, 0, 0) && InterlockedCompareExchange(&inputEnabled, 0, 0) &&
           !cursorFreeNow() && *reinterpret_cast<const int*>(gameAt(fp::sig::uiOpenCount)) <= 0;
}

static bool reticleForUi(LONG* x, LONG* y) {
    if (!gameplayOwnsPointer() || *reinterpret_cast<const int*>(gameAt(fp::sig::pointerOverride)) >= 1) return false;
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

static bool guiPointerOwned() {
    auto& point = guiPointerPresentation;
    if (!point.active || point.rejected) return false;
    __try {
        const fp::InputStamp now = worldPointerStamp();
        if (gameplayOwnsPointer() && closeViewContext() &&
            *reinterpret_cast<const int*>(gameAt(fp::sig::pointerOverride)) < 1 &&
            point.gui == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiManager)) &&
            point.viewport == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)) && point.camera == savedCamera &&
            point.controller == savedController && point.view == savedView && savedCamera && savedView &&
            savedRoot == now.root && savedScene == now.scene &&
            *reinterpret_cast<const uintptr_t*>(point.viewport + field::viewport::pickCamera) == savedCamera &&
            *reinterpret_cast<const uintptr_t*>(point.viewport + field::viewport::view) == savedView &&
            *reinterpret_cast<const uintptr_t*>(savedView + field::view::camera) == savedCamera &&
            *reinterpret_cast<const uintptr_t*>(savedCamera + field::camera::controller) == point.controller &&
            point.stamp.root == now.root && point.stamp.head == now.head && point.stamp.scene == now.scene &&
            point.stamp.mode == now.mode && point.stamp.generation == now.generation &&
            point.stamp.policyRevision == now.policyRevision)
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
        if (gui != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiManager)) ||
            !readable(gui, field::gui::capture + sizeof(uintptr_t)) ||
            *reinterpret_cast<const uintptr_t*>(gui + field::gui::capture) ||
            *reinterpret_cast<const uintptr_t*>(gui + field::gui::hoverView) != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)))
            return false;
        sample.stamp = worldPointerStamp();
        if (*reinterpret_cast<const unsigned char*>(sample.stamp.root + field::character::combatStance)) {
            const uintptr_t bh = *reinterpret_cast<const uintptr_t*>(sample.stamp.root + field::character::behaviour);
            const unsigned interact = nativeBinding(field::bindings::interact / field::bindings::stride);
            if (!*reinterpret_cast<const unsigned char*>(bh + field::behaviour::interactionLatch) &&
                !(interact > 0 && interact < 256 && (keyState(int(interact)) & 0x8000)))
                return false;
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
    auto point = reinterpret_cast<LONG*>(gui + offset);
    if (point[0] == fromX && point[1] == fromY) {
        point[0] = toX;
        point[1] = toY;
    }
}

static void guiMousePress(uintptr_t gui, unsigned short buttons) {
    if (guiPointerPresentation.active &&
        (gui != guiPointerPresentation.gui || guiPointerPresentation.nativeOnly)) {
        const GuiPointerPresentation previous = guiPointerPresentation;
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
    if (!sample.havePress && std::abs(priorX - x) < 2 && std::abs(priorY - y) < 2) priorX = x - 2;
    auto current = reinterpret_cast<LONG*>(gui + field::gui::pointerX);
    auto pressed = reinterpret_cast<LONG*>(gui + field::gui::pressX);
    const LONG oldPressX = pressed[0], oldPressY = pressed[1];
    const GuiPointerPresentation previous = guiPointerPresentation;
    guiPointerPresentation = {true, false, x, y};
    guiPointerPresentation.gui = gui;
    guiPointerPresentation.viewport = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport));
    guiPointerPresentation.camera = savedCamera;
    guiPointerPresentation.controller = savedController;
    guiPointerPresentation.view = savedView;
    guiPointerPresentation.stamp = sample.stamp;
    bool accepted = false;
    __try {
        current[0] = x;
        current[1] = y;
        pressed[0] = priorX;
        pressed[1] = priorY;
        originalGuiMousePress(gui, buttons);
        accepted = (*reinterpret_cast<const unsigned*>(gui + field::gui::buttons) & 7) == 1;
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
                                 ? *reinterpret_cast<const unsigned*>(gui + field::gui::buttons) : 0;
    if ((buttons & 7) != 1 || (buttons & 0x6100) || !sampleWorldPointer(gui, sample) || !sample.havePress)
        return originalGuiMousePoll(gui, force);
    const LONG x = sample.x, y = sample.y,
               totalX = gestureCoordinate(sample.centerX, sample.limitX, sample.current.x) - sample.centerX,
               totalY = gestureCoordinate(sample.centerY, sample.limitY, sample.current.y) - sample.centerY,
               deltaX = totalX - (gestureCoordinate(sample.centerX, sample.limitX, sample.polled.x) - sample.centerX),
               deltaY = totalY - (gestureCoordinate(sample.centerY, sample.limitY, sample.polled.y) - sample.centerY),
               priorX = x - deltaX, priorY = y - deltaY, pressX = x - totalX, pressY = y - totalY;
    auto current = reinterpret_cast<LONG*>(gui + field::gui::pointerX);
    auto pressed = reinterpret_cast<LONG*>(gui + field::gui::pressX);
    const LONG oldPressX = pressed[0], oldPressY = pressed[1];
    const GuiPointerPresentation previous = guiPointerPresentation;
    guiPointerPresentation = {true, false, x, y};
    guiPointerPresentation.gui = gui;
    guiPointerPresentation.viewport = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport));
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

static uintptr_t levelGrid() {
    const uintptr_t viewport = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport));
    const uintptr_t manager = viewport && readable(viewport + field::viewport::sceneManager, 8)
                                  ? *reinterpret_cast<const uintptr_t*>(viewport + field::viewport::sceneManager)
                                  : 0;
    if (!manager || !readable(manager, 0x328) ||
        !(*reinterpret_cast<const unsigned*>(manager + field::sceneManager::flags) & 0x2000000))
        return 0;
    const uintptr_t level = *reinterpret_cast<const uintptr_t*>(manager + field::sceneManager::levelGrid);
    return level && readable(level, 0x28) ? level : 0;
}

static int levelBlockedProfileBody(uintptr_t level, const float* origin, const float* dir, float length) {
    __try {
        return fp::profile::invoke<fp::profile::Hook::levelNative>(levelRay, level, origin, dir, length) || fp::profile::invoke<fp::profile::Hook::levelNative>(levelRay2, level, origin, dir, length) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int levelBlocked(uintptr_t level, const float* origin, const float* dir, float length) {
    return fp::profile::invoke<fp::profile::Hook::level>(levelBlockedProfileBody, level, origin, dir, length);
}

static int pickClearance(float px, float py, float width, float height) {
    const uintptr_t viewport = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport));
    if (!levelRay || !levelRay2 || !viewport || !savedCamera ||
        *reinterpret_cast<const uintptr_t*>(viewport + field::viewport::pickCamera) != savedCamera)
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

using DragPlace = int (*)(uintptr_t, uintptr_t, float);
static DragPlace originalDragPlace;
using PickRay = char (*)(uintptr_t, float, float, uintptr_t);
static PickRay originalPickRay;

static char ownedPickRay(uintptr_t viewport, float x, float y, uintptr_t out) {
    const std::uint32_t bank = fp::game::viewQueryScratchBank;
    std::memcpy(reinterpret_cast<void*>(out + field::pick::scratchBank), &bank, sizeof bank);
    return fp::profile::invoke<fp::profile::Hook::pickNative>(originalPickRay, viewport, x, y, out);
}

static thread_local DWORD placingThread = 0;
static thread_local bool placingReady = false;
static thread_local uintptr_t placingViewport = 0;
static thread_local unsigned char placingRay[0x34]{};
static bool prepareHeldPlacement(uintptr_t object, uintptr_t camera, float height, unsigned char* ray);

static uintptr_t closeWorldHeldObject(uintptr_t viewport) {
    if (!base || !viewport || viewMode.load() >= 2 || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0))
        return 0;
    __try {
        if (viewport != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)) ||
            !*reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)))
            return 0;
        const uintptr_t drag = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::dragManager));
        if (!readable(drag, 0x20) || *reinterpret_cast<const unsigned*>(drag + field::drag::flags) != 0x2000) return 0;
        const uintptr_t object = *reinterpret_cast<const uintptr_t*>(drag + field::drag::object);
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
        return closeViewContext() && *reinterpret_cast<const int*>(gameAt(fp::sig::pointerOverride)) < 1 ? object : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

using HeldQuery = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, float);
static HeldQuery originalHeldQuery;
using HeldPlace = bool (*)(uintptr_t, uintptr_t, uintptr_t);
static HeldPlace originalHeldPlace;
using HeldSweep = float (*)(uintptr_t, const float*, uintptr_t, uintptr_t);
static HeldSweep originalHeldSweep;
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity);
static unsigned walkSceneVisit(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity,
                               void (*visit)(uintptr_t, bool));

static float heldSweepFor(uintptr_t caller, uintptr_t body, const float* direction, uintptr_t viewport,
                          uintptr_t pose) {
    if (!boundedPlacement.object || viewport != boundedPlacement.viewport ||
        (caller != gameAt(fp::sig::heldSweepReturn) && caller != gameAt(fp::sig::inventorySweepReturn)) ||
        std::memcmp(direction, boundedPlacement.values + 3, 12))
        return originalHeldSweep(body, direction, viewport, pose);
    uintptr_t nodes[1024]{};
    unsigned parents[1024]{}, changed = 0;
    float distance = 0.f;
    __try {
        const uintptr_t root = boundedPlacement.root;
        const uintptr_t mesh = readable(root, 0x20) ? *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode) : 0;
        const unsigned count = mesh ? walkScene(mesh, nodes, parents, 1024) : 0;
        for (unsigned i = 0; i < count; ++i) {
            const uintptr_t node = nodes[i];
            if (!readable(node, field::sceneNode::queryFlags + sizeof(unsigned))) continue;
            auto flags = reinterpret_cast<unsigned*>(node + field::sceneNode::queryFlags);
            if (*flags & heldCollisionQueryBit) {
                *flags &= ~heldCollisionQueryBit;
                nodes[changed++] = node;
            }
        }
        float walked = 0.f;
        do {
            const float segment = std::fmin(nativeHeldSweepSpan, boundedPlacement.distance - walked);
            float shifted[12];
            uintptr_t transform = pose;
            if (walked > 0.f) {
                std::memcpy(shifted, reinterpret_cast<void*>(pose), sizeof shifted);
                for (int i = 0; i < 3; ++i) shifted[9 + i] += direction[i] * walked;
                transform = reinterpret_cast<uintptr_t>(shifted);
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
                *reinterpret_cast<unsigned*>(nodes[i] + field::sceneNode::queryFlags) |= heldCollisionQueryBit;
    }
    if (!std::isfinite(distance) || distance < 0.f) {
        boundedPlacement.blocked = true;
        return 0.f;
    }
    return std::fmin(distance, boundedPlacement.distance);
}

static float heldSweep(uintptr_t body, const float* direction, uintptr_t viewport, uintptr_t pose) {
    return heldSweepFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), body, direction, viewport, pose);
}

static bool heldPlace(uintptr_t object, uintptr_t viewport, uintptr_t query) {
    HeldPlacement proposal = pendingPlacement;
    pendingPlacement = {};
    if (!object || closeWorldHeldObject(viewport) != object) {
        if (proposal.object) return false;
        return originalHeldPlace(object, viewport, query);
    }
    const bool recovery = reinterpret_cast<uintptr_t>(_ReturnAddress()) == gameAt(fp::sig::heldRecoveryPlaceReturn) &&
                          heldRecoveryPlacement(object, viewport, query, proposal);
    if ((!recovery && gameplayHeldObject(viewport) != object) || proposal.object != object ||
        proposal.viewport != viewport || proposal.query != query ||
        proposal.generation != holdGeneration.load() || proposal.mode != viewMode.load() ||
        proposal.root != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) ||
        proposal.scene != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) || !std::isfinite(proposal.distance) ||
        proposal.distance < 1.f || !query || std::memcmp(reinterpret_cast<void*>(query), proposal.values, sizeof proposal.values))
        return false;
    const HeldPlacement previous = boundedPlacement;
    bool result = false;
    boundedPlacement = proposal;
    __try {
        result = originalHeldPlace(object, viewport, query);
        result = result && !boundedPlacement.blocked && proposal.generation == holdGeneration.load() &&
                 proposal.mode == viewMode.load() &&
                 proposal.root == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) &&
                 proposal.scene == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::currentScene)) &&
                 (recovery ? closeWorldHeldObject(viewport) : gameplayHeldObject(viewport)) == object;
        if (result && !recovery && heldUpdateDepth == 1) updatePlacement = proposal;
    } __finally {
        boundedPlacement = previous;
    }
    return result;
}

static uintptr_t heldQuery(uintptr_t object, uintptr_t viewport, uintptr_t out, float height);
using DragRotate = bool (*)(uintptr_t, uintptr_t);
static DragRotate originalDragRotate;
using NodePose = void (*)(uintptr_t);

static void callPropagateHeldPose(uintptr_t object) {
    reinterpret_cast<NodePose>(gameAt(fp::sig::propagateNodePose))(object);
}
static NodePose propagateHeldPose = callPropagateHeldPose;

static bool heldUnit(float* axis) {
    const float length = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if (!std::isfinite(length) || length < 1.e-8f) return false;
    const float scale = 1.f / std::sqrt(length);
    for (int i = 0; i < 3; ++i) axis[i] *= scale;
    return true;
}

static bool turnHeld(uintptr_t object) {
    const float tx = heldTurnX.exchange(0.f), ty = heldTurnY.exchange(0.f);
    if (!readable(object, field::sceneNode::bodyPart + sizeof(uintptr_t))) return false;
    const uintptr_t body = *reinterpret_cast<const uintptr_t*>(object + field::sceneNode::bodyPart);
    if (!readable(body, field::bodyPart::pose + 48) ||
        *reinterpret_cast<const uintptr_t*>(body + field::bodyPart::meshNode) != object)
        return false;
    float pose[12];
    std::memcpy(pose, reinterpret_cast<const void*>(object + field::sceneNode::localTransform), sizeof pose);
    for (float value : pose)
        if (!std::isfinite(value)) return false;
    float trackedPoint[3], trackedAxes[9];
    const int tracked = trackedHeldTarget(object, *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)), trackedPoint, trackedAxes);
    if (tracked == 1) return false;
    if (tracked == 2) {
        if (*reinterpret_cast<const uintptr_t*>(object + field::sceneNode::parent)) return false; // parent-space controller rotation is not admitted
        std::memcpy(pose, trackedAxes, 36);
        std::memcpy(reinterpret_cast<void*>(object + field::sceneNode::localTransform), pose, 36);
        if (*reinterpret_cast<const unsigned*>(body + field::bodyPart::flags) & 0x10000000) std::memcpy(reinterpret_cast<void*>(body + field::bodyPart::pose), pose, sizeof pose);
        propagateHeldPose(object);
        return true;
    }
    const bool turning = std::isfinite(tx) && std::isfinite(ty) && (tx != 0.f || ty != 0.f);
    if (turning) {
        float view[12], right[3], up[3];
        fp::basis(viewYaw.load(), viewPitch.load(), view);
        const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(object + field::sceneNode::parent);
        if (parent) {
            if (!readable(parent, field::sceneNode::transform + 48)) return false;
            const auto parentPose = reinterpret_cast<const float*>(parent + field::sceneNode::transform);
            fp::worldToLocalDirection(parentPose, view, right);
            fp::worldToLocalDirection(parentPose, view + 3, up);
        } else {
            std::memcpy(right, view, sizeof right);
            std::memcpy(up, view + 3, sizeof up);
        }
        if (!heldUnit(right) || !heldUnit(up)) return false;
        fp::rotateAxes(pose, up, -tx * fp::heldRotateRadiansPerPixel);
        fp::rotateAxes(pose, right, -ty * fp::heldRotateRadiansPerPixel);
        if (!heldUnit(pose + 3)) return false;
        const float dot = pose[0] * pose[3] + pose[1] * pose[4] + pose[2] * pose[5];
        for (int i = 0; i < 3; ++i) pose[i] -= dot * pose[3 + i];
        if (!heldUnit(pose)) return false;
        pose[6] = pose[1] * pose[5] - pose[2] * pose[4];
        pose[7] = pose[2] * pose[3] - pose[0] * pose[5];
        pose[8] = pose[0] * pose[4] - pose[1] * pose[3];
        if (!heldUnit(pose + 6)) return false;
        std::memcpy(reinterpret_cast<void*>(object + field::sceneNode::localTransform), pose, 36);
        ++heldTurns;
    }
    if (*reinterpret_cast<const unsigned*>(body + field::bodyPart::flags) & 0x10000000)
        std::memcpy(reinterpret_cast<void*>(body + field::bodyPart::pose), pose, sizeof pose);
    propagateHeldPose(object);
    return turning;
}

static bool dragRotate(uintptr_t object, uintptr_t viewport) {
    if (!object || closeWorldHeldObject(viewport) != object) return originalDragRotate(object, viewport);
    if (gameplayHeldObject(viewport) != object) return false;
    return turnHeld(object);
}

static ULONGLONG inventoryPlacementBegin(uintptr_t object, uintptr_t viewport);
static void inventoryPlacementEnd(ULONGLONG generation, uintptr_t object, int result);
static void inventoryPlacementHandoff();

static int dragPlace(uintptr_t object, uintptr_t camera, float h) {
    const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::dragManager));
    const unsigned flags = readable(manager, 0x20) ? *reinterpret_cast<const unsigned*>(manager + field::drag::flags) : 0;
    const bool closePlacement =
        camera && camera == *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)) && viewMode.load() < 2 &&
        InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
        InterlockedCompareExchange(&cameraReady, 0, 0) && *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) &&
        (flags == 0x1000 || flags == 0x2000) && *reinterpret_cast<const uintptr_t*>(manager + field::drag::object) == object;
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
        std::memcpy(previousRay, placingRay, sizeof previousRay);
        placingThread = GetCurrentThreadId();
        placingReady = true;
        placingViewport = camera;
        std::memcpy(placingRay, ray, sizeof placingRay);
        const HeldPlacement previousBound = boundedPlacement;
        boundedPlacement = {};
        boundedPlacement.object = object;
        boundedPlacement.viewport = camera;
        boundedPlacement.root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
        boundedPlacement.distance = *reinterpret_cast<const float*>(ray + field::pick::distance);
        std::memcpy(boundedPlacement.values, ray + 8, 12);
        std::memcpy(boundedPlacement.values + 3, ray + field::pick::direction, 12);
        int result = 0;
        __try {
            result = originalDragPlace(object, camera, h);
            if (boundedPlacement.blocked) result = 0;
        } __finally {
            boundedPlacement = previousBound;
            placingThread = previousThread;
            placingReady = previousReady;
            placingViewport = previousViewport;
            std::memcpy(placingRay, previousRay, sizeof placingRay);
        }
        inventoryPlacementEnd(inventory, object, result);
        return result;
    }
    inventoryPlacementHandoff();
    if (holdObject.load() || grabStartDistance.load() != 0.f) resetHeldGrab();
    return originalDragPlace(object, camera, h);
}

static float holdReachClamp(const float* origin, const float* dir, float t) {
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t mesh = root ? *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode) : 0,
                    scene = mesh ? *reinterpret_cast<const uintptr_t*>(mesh + field::sceneNode::sector) : 0;
    if (scene) {
        float body[3];
        for (int i = 0; i < 3; i++)
            body[i] = *reinterpret_cast<const float*>(root + field::character::position + i * 4) +
                      *reinterpret_cast<const float*>(scene + field::sector::origin + i * 4);
        const float reach = fp::rayInSphere(origin, dir, body, fp::holdSphere(float(grabRange.load())));
        if (reach >= 0) t = std::fmin(t, reach);
    }
    return t;
}

static float levelClearAfterProbe(uintptr_t level, const float* origin, const float* dir, float length,
                                 int initial) {
    if (initial < 0) return 0.f;
    if (initial == 0) return length;
    float lo = 0, hi = length;
    for (int i = 0; i < 7; i++) {
        const float mid = (lo + hi) * .5f;
        const int blocked = levelBlocked(level, origin, dir, mid);
        if (blocked < 0) return 0.f;
        if (blocked == 1)
            hi = mid;
        else
            lo = mid;
    }
    return std::fmax(lo - 8.f, 0.f);
}

static float levelClear(uintptr_t level, const float* origin, const float* dir, float length) {
    return levelClearAfterProbe(level, origin, dir, length, levelBlocked(level, origin, dir, length));
}

static float viewClearanceProfileBody(const float* pose, float fov, float maxClearance = 0.f) {
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
        const float length = std::fmax(16.f, 9.f * float(n));
        const float depthScale = float(1.0 / n);
        float probe = length;
        if (std::isfinite(maxClearance) && maxClearance > 0.f && length * depthScale > maxClearance) {
            float lo = 0.f, hi = length;
            for (int i = 0; i < 6; ++i) {
                const float mid = (lo + hi) * .5f;
                if (mid * depthScale >= maxClearance) hi = mid;
                else lo = mid;
            }
            probe = hi;
        }
        const int hit = levelBlocked(level, pose + 9, d, probe);
        if (hit < 0) return -1;
        if (hit == 0) {
            if (probe < length) best = std::fmin(best, maxClearance);
            continue;
        }
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
        best = std::fmin(best, lo * depthScale);
    }
    return best;
}

static float viewClearance(const float* pose, float fov, float maxClearance = 0.f) {
    return fp::profile::invoke<fp::profile::Hook::viewClearance>(viewClearanceProfileBody, pose, fov, maxClearance);
}

static char heldPointAlongViewDistance(char hit, uintptr_t out, float distance) {
    const auto dir = reinterpret_cast<const float*>(out + field::pick::direction);
    const auto origin = reinterpret_cast<const float*>(out + field::pick::origin);
    const float length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!std::isfinite(length) || length < 0.5f || !std::isfinite(origin[0] + origin[1] + origin[2]))
        return 0;
    const float eye[3] = {eyeGlobal[0].load(), eyeGlobal[1].load(), eyeGlobal[2].load()};
    float t = fp::holdAlongRay(origin, dir, eye, distance);
    if (t < 0.f) return 0;
    t = holdReachClamp(origin, dir, t);
    const float reached = *reinterpret_cast<const float*>(out + field::pick::distance);
    if (hit && (!std::isfinite(reached) || reached < 0.f)) return 0;
    if (hit) t = std::fmin(t, reached);
    const uintptr_t level = levelGrid();
    if (level) {
        if (!levelRay || !levelRay2) return 0;
        const int initial = levelBlocked(level, origin, dir, t);
        if (initial < 0) return 0;
        t = levelClearAfterProbe(level, origin, dir, t, initial);
        if (levelBlocked(level, origin, dir, t) != 0) return 0;
    }
    if (!(t >= 1.f)) return 0;
    for (int i = 0; i < 3; i++) *reinterpret_cast<float*>(out + 8 + i * sizeof(float)) = origin[i] + dir[i] * t;
    *reinterpret_cast<float*>(out + field::pick::distance) = t;
    return 1;
}

static char heldPointAlongView(char hit, uintptr_t out) {
    return heldPointAlongViewDistance(hit, out, holdApplied.load());
}

static constexpr unsigned pickSkipBit = 0x4000000;
static unsigned walkScene(uintptr_t scene, uintptr_t* nodes, unsigned* parents, unsigned capacity);
static uintptr_t selfPickNodes[1024];
static unsigned selfPickParents[1024], selfPickSet = 0;
static std::atomic<unsigned> selfPicksMasked{0};
static bool onGameThread();

static bool selfPickWanted() {
    return InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
           viewMode.load() < 2 && onGameThread();
}

static uintptr_t heldPickNodes[256];
static unsigned heldPickParents[256];

static void maskSelfPickNode(uintptr_t node, bool treeReadable) {
    if (selfPickSet >= 1024 || (!treeReadable && !readable(node, 0x10))) return;
    auto flags = reinterpret_cast<unsigned*>(node + 8);
    if (!(*flags & pickSkipBit)) {
        *flags |= pickSkipBit;
        selfPickNodes[selfPickSet++] = node;
    }
}

static void selfPickMaskProfileBody(uintptr_t held = 0) {
    selfPickSet = 0;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
    const uintptr_t mesh = readable(root, 0x20) ? *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode) : 0;
    if (mesh) walkSceneVisit(mesh, selfPickNodes, selfPickParents, 1024, maskSelfPickNode);
    if (held) walkSceneVisit(held, heldPickNodes, heldPickParents, 256, maskSelfPickNode);
    if (selfPickSet) ++selfPicksMasked;
}

static void selfPickMask(uintptr_t held = 0) {
    return fp::profile::invoke<fp::profile::Hook::selfMask>(selfPickMaskProfileBody, held);
}

static void selfPickRestoreProfileBody() {
    MEMORY_BASIC_INFORMATION region{};
    for (unsigned i = 0; i < selfPickSet; ++i)
        if (readableRegionContains(region, selfPickNodes[i], 0x10) || readable(selfPickNodes[i], 0x10, region))
            *reinterpret_cast<unsigned*>(selfPickNodes[i] + 8) &= ~pickSkipBit;
    selfPickSet = 0;
}

static void selfPickRestore() {
    return fp::profile::invoke<fp::profile::Hook::selfRestore>(selfPickRestoreProfileBody);
}

static char pickPastSelf(PickRay pick, uintptr_t viewport, float x, float y, uintptr_t out,
                         uintptr_t held = 0) {
    if (!viewport || viewport != *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::mainViewport)) ||
        !selfPickWanted() || selfPickSet)
        return pick == originalPickRay ? fp::profile::invoke<fp::profile::Hook::pickNative>(pick, viewport, x, y, out) : pick(viewport, x, y, out);
    char hit = 0;
    __try {
        selfPickMask(held);
        hit = pick == originalPickRay ? fp::profile::invoke<fp::profile::Hook::pickNative>(pick, viewport, x, y, out) : pick(viewport, x, y, out);
    } __finally {
        selfPickRestore();
    }
    return hit;
}
