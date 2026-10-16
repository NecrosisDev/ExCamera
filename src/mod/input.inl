// Input: the game window procedure (F6-F11, right mouse, raw mouse), mouse-look aim, the input and behaviour hooks, head steadying, movement, the attack cue's state.
// Part of first_person.cpp (one translation unit): included there, in order.
static LRESULT CALLBACK windowProc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    if (InterlockedCompareExchange(&stopping, 0, 0))
        return CallWindowProcW(originalWindowProc, w, message, wp, lp);
    // Plain F10 is a system key on Windows; leave Alt shortcuts with Exanima.
    // Consume owned presses, repeats and releases so F10 cannot activate a menu.
    const bool fovKey = wp == VK_F9 || wp == VK_F10 || wp == VK_F11;
    const bool fovMessage = message == WM_KEYDOWN || message == WM_KEYUP ||
                            (wp == VK_F10 && (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP));
    if (fovKey && fovMessage && !(lp & (1LL << 29))) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lp & (1LL << 30)))
            selectedFov.store(wp == VK_F11 ? 75 : fp::clampFov(selectedFov.load() + (wp == VK_F10 ? 5 : -5)));
        return 0;
    }
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP) {
        bool eaten = false;
        rmbEvent(message == WM_RBUTTONUP ? fp::RmbEvent::up : fp::RmbEvent::down,
                 message != WM_RBUTTONUP && rmbEligible(), &eaten);
        if (eaten) return 0;
    }
    if (InterlockedCompareExchange(&inputEnabled, 0, 0)) {
        // Free mouse-look replaces overhead rotation. Never start the native
        // camera's middle-button drag while this input context owns the mouse.
        if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) return 0;
        if (message == WM_LBUTTONDOWN) InterlockedOr(&tappedButtons, 1);
        if (message == WM_RBUTTONDOWN) InterlockedOr(&tappedButtons, 2);
    }
    if (message == WM_INPUT && InterlockedCompareExchange(&rawRegistered, 0, 0)) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            const bool owns = InterlockedCompareExchange(&inputEnabled, 0, 0) &&
                              InterlockedCompareExchange(&mouseCaptured, 0, 0) && !cursorFreeNow();
            AcquireSRWLockExclusive(&rawLock);
            rawLook.event((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0, raw.data.mouse.lLastX,
                          raw.data.mouse.lLastY, owns);
            ReleaseSRWLockExclusive(&rawLock);
        }
        return CallWindowProcW(originalWindowProc, w, message, wp, lp); // DefWindowProc cleanup.
    }
    unsigned bit = keyMask(wp);
    if (message == WM_KEYDOWN && bit && InterlockedCompareExchange(&inputEnabled, 0, 0)) {
        InterlockedOr(&heldKeys, bit);
        if (!(lp & (1LL << 30))) InterlockedOr(&tappedKeys, bit);
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && bit)
        InterlockedAnd(&heldKeys, ~bit); // with Alt held a release comes as WM_SYSKEYUP
    // Held object: the wheel sets the hold distance and does nothing else meanwhile.
    if (message == WM_MOUSEWHEEL && InterlockedCompareExchange(&active, 0, 0) &&
        (InterlockedCompareExchange(&inputEnabled, 0, 0) || InterlockedCompareExchange(&rmbFree, 0, 0))) {
        int kind = 0;
        __try {
            kind = dragKind();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            kind = 0;
        }
        if (kind == 1) {
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
        if (wp == VK_F6) releaseHeldDoor();
        if (wp == VK_F6) {
            // F6 cycles first person -> third person -> the game's overhead.
            if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                viewMode.load() == 0) {
                viewMode = 1;
                return 0;
            } // first -> third: the same session, the camera only
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
        if (probeKey && wp == WPARAM(probeKey) && InterlockedCompareExchange(&active, 0, 0)) {
            InterlockedExchange(&probeRequest, 1);
            return 0;
        }
        if (wp == VK_ESCAPE) releaseInput();
    }
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || (message == WM_ACTIVATEAPP && !wp)) {
        rmbEvent(fp::RmbEvent::focusLost, false);
        releaseInput();
    }
    return CallWindowProcW(originalWindowProc, w, message, wp, lp);
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
    return !cursorFreeNow() && playerContext(behavior);
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
    AcquireSRWLockExclusive(&aimLock);
    auto physical = (float*)(head + field::bodyPart::pose);
    auto anchor = (float*)(root + field::character::position);
    if (!aim.valid || aim.root != root || aim.head != head) {
        InterlockedExchange(&mouseCaptured, 0);
        aim = {};
        aim.root = root;
        aim.head = head;
        auto direction = (float*)(root + field::character::facing);
        aim.yaw = atan2f(direction[0], direction[2]);
        aim.targetYaw = aim.yaw;
        if (viewRoot.load() == root && InterlockedCompareExchange(&cameraReady, 0, 0)) {
            const float y = viewYaw.load(), p = viewPitch.load(); // continue from the view on screen
            if (std::isfinite(y) && std::isfinite(p)) {
                aim.yaw = aim.targetYaw = y;
                aim.pitch = aim.targetPitch = p > 1.15f ? 1.15f : (p < -1.15f ? -1.15f : p);
            }
        }
        fp::eyePoint(physical, aim.origin);
        for (int a = 0; a < 3; a++) aim.anchor[a] = anchor[a];
    }
    if (!freeCursor && InterlockedCompareExchange(&mouseCaptured, 0, 0)) {
        long dx = cursor.x - center.x, dy = cursor.y - center.y;
        if (InterlockedCompareExchange(&rawRegistered, 0, 0)) {
            long rx = 0, ry = 0;
            AcquireSRWLockExclusive(&rawLock);
            const bool relative = rawLook.take(rx, ry);
            ReleaseSRWLockExclusive(&rawLock);
            if (relative) {
                dx = rx;
                dy = ry;
            }
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
    aim.yaw = fp::approachAngle(aim.yaw, aim.targetYaw, dt);
    aim.pitch = fp::approachAngle(aim.pitch, aim.targetPitch, dt);
    float direction[3] = {sinf(aim.yaw) * cosf(aim.pitch), sinf(aim.pitch), cosf(aim.yaw) * cosf(aim.pitch)};
    // Translate the input ray with locomotion; head animation never rotates or recenters it.
    for (int a = 0; a < 3; a++) {
        aim.origin[a] += anchor[a] - aim.anchor[a];
        aim.anchor[a] = anchor[a];
        aim.point[a] = aim.origin[a] + direction[a] * 1000.f;
    }
    if (viewMode.load() == 1 && tpAimSet)
        memcpy(aim.point, tpAimLocal, 12); // third person aims where the crosshair is
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
    if (!InterlockedCompareExchange(&cameraReady, 0, 0) || !InterlockedCompareExchange(&active, 0, 0) ||
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
// The body's heading on the ground is the mouse yaw itself. It was the direction from the body to the aim
// point, which in third person is where the screen centre meets the world: looking steeply down from
// behind the shoulder that point lies behind the body, and the body turned round to face the camera.
static void headingToPoint(const Aim& a, float* d) {
    d[0] = std::sin(a.yaw);
    d[1] = 0;
    d[2] = std::cos(a.yaw);
}
static void gazeToPoint(const Aim& a, float* d) {
    auto head = *(uintptr_t*)(a.root + field::character::headPart);
    if (!head) {
        d[0] = sinf(a.yaw) * cosf(a.pitch);
        d[1] = sinf(a.pitch);
        d[2] = cosf(a.yaw) * cosf(a.pitch);
        return;
    }
    auto h = (float*)(head + field::bodyPart::pose);
    float eye[3];
    fp::eyePoint(h, eye);
    for (int i = 0; i < 3; i++) d[i] = a.point[i] - eye[i];
    normalize(d);
}
static void input() {
    healthHit(fp::HealthHook::pollInput);
    originalInput();
    auto root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    auto bh = root ? *(uintptr_t*)(root + field::character::behaviour) : 0;
    duckKey = (*(unsigned*)(gameAt(fp::sig::keyWord)) & duckKeyBit) !=
              0; // the game's own duck binding, as it polled it
    // Before ownership is decided, so a right mouse still physically held is blocked by the hand-off
    // rather than passed to the game. A release that never reached the window (let go outside it) ends
    // a hold too.
    if (InterlockedCompareExchange(&rmbFree, 0, 0) &&
        (!rmbEligible() || (rmbMode.load() == 1 && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)))) {
        InterlockedExchange(&rmbFree, 0);
        releaseInput();
    }
    const bool owned = bh && controls(bh);
    const LONG generation = InterlockedCompareExchange(&inputGeneration, 0, 0);
    if (generation != sampledInputGeneration) {
        buttonHandoff.reset();
        sampledInputGeneration = generation;
    }
    auto buttons = (unsigned*)(gameAt(fp::sig::buttonWord));
    const bool continuing = buttonHandoff.owned;
    const unsigned safeButtons = buttonHandoff.sample(owned, *buttons);
    if (owned) {
        // Movement only while its key is physically down (a tap within the frame still counts): a release that
        // never reached the window (an overlay ate it, focus moved between frames) left a held bit set, and
        // the character walked on by itself.
        const unsigned physical = physicalMovement();
        InterlockedAnd(&heldKeys, LONG(physical));
        unsigned& keyWord = *(unsigned*)(gameAt(fp::sig::keyWord));
        keyWord =
            (keyWord & ~movementBits) |
            ((keyWord | unsigned(InterlockedCompareExchange(&heldKeys, 0, 0))) & physical & movementBits) |
            unsigned(InterlockedExchange(&tappedKeys, 0));
        const unsigned taps = unsigned(InterlockedExchange(&tappedButtons, 0));
        *buttons = (safeButtons | (continuing ? taps : 0)) & ~buttonHandoff.blocked;
    } else
        InterlockedExchange(&tappedButtons, 0);
    // A game window's freeze holds movement wherever the pointer is: over the window the game's own view
    // is not the hovered one (playerContext fails), yet it still reads the movement keys.
    if (bh && cursorFreeNow() && (playerContext(bh) || InterlockedCompareExchange(&windowFree, 0, 0))) {
        // HUD still receives native window messages. Suppress only gameplay actions
        // so clicking inventory does not also swing a weapon or start walking.
        *(unsigned*)(gameAt(fp::sig::keyWord)) &= ~15u;
        *(unsigned*)(gameAt(fp::sig::buttonWord)) = 0;
    }
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
static void point(uintptr_t behavior, float* out) {
    Aim a;
    if (!getAim(behavior, a) && !heldAim(behavior, a)) {
        originalPoint(behavior, out);
        return;
    }
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
// Spine follows aim (fp::spineBend; [Camera] SpineFollowsAim): written into the chest (root+0xD48, the part
// the arms hang from) and the spine part below it before the game builds this step's pose targets. First
// and third person, combat included (it aims the swing); identity whenever it does not apply, so the game's
// pose is untouched then. Never the root part (the pelvis carries the legs).
static std::atomic<int> spineFollow{60};
static std::atomic<unsigned> spineBendTicks{0};
static constexpr unsigned partExtraRotation = 0x4000; // part+0xC: 0x1760F0 multiplies +0x5EC (and +0x5CC) in
static bool bodyCalm(uintptr_t root, uintptr_t head);
static uintptr_t spineBent[2]{},
    spineBentRoot =
        0; // the parts given a rotation, and whose (physics thread; the stop path once the hooks are off)
static constexpr float spineIdentity[4] = {0, 0, 0, 1};
static void spineBendPart(uintptr_t part, const float* quat) {
    if (!readable(part, 0x600)) return;
    *(unsigned*)(part + field::bodyPart::flags) |= partExtraRotation;
    memcpy((void*)(part + field::bodyPart::extraRotation), quat, 16);
}
// Back to the game's pose, only on parts still the same character's (part+0x580): a freed or reused part is
// never written.
static void spineBendRelease() {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    for (uintptr_t& p : spineBent) {
        if (p && root == spineBentRoot && readable(p, 0x600) &&
            *(uintptr_t*)(p + field::bodyPart::character) == root)
            spineBendPart(p, spineIdentity);
        p = 0;
    }
    spineBentRoot = 0;
}
static void spineBendStep(uintptr_t root) {
    uintptr_t* bent = spineBent;
    const float* identity = spineIdentity;
    if (root != spineBentRoot) {
        spineBent[0] = spineBent[1] = 0;
        spineBentRoot = root;
    } // another body: what was bent belongs to the old one (never written)
    const uintptr_t head = readable(root, 0x11e8) ? *(uintptr_t*)(root + field::character::headPart) : 0;
    const uintptr_t chest = readable(root, 0xd50) ? *(uintptr_t*)(root + field::character::chestPart) : 0;
    const uintptr_t rootPart = readable(chest, 0x430) ? *(uintptr_t*)(chest + field::bodyPart::rootPart) : 0,
                    lower = readable(chest, 0x430) ? *(uintptr_t*)(chest + field::bodyPart::parentPart) : 0;
    const uintptr_t parts[2] = {chest && chest != rootPart ? chest : 0,
                                lower && lower != rootPart ? lower : 0};
    const bool on = InterlockedCompareExchange(&active, 0, 0) &&
                    !InterlockedCompareExchange(&stopping, 0, 0) && viewMode.load() < 2 &&
                    spineFollow.load() > 0 && head && bodyCalm(root, head);
    const float bend =
        on ? fp::spineBend((const float*)(root + field::character::gaze), spineFollow.load()) : 0.f;
    for (int i = 0; i < 2; ++i) {
        float q[4];
        const float share = i == 0 ? 1.f - fp::spineLowerShare : fp::spineLowerShare;
        const uintptr_t parent =
            parts[i] && readable(parts[i], 0x430) ? *(uintptr_t*)(parts[i] + field::bodyPart::parentPart) : 0;
        const bool apply = on && parts[i] && readable(parts[i], 0x2d0) && readable(parent, 0x2d0) &&
                           fp::spineBendQuat((const float*)(parts[i] + field::bodyPart::pose),
                                             (const float*)(parent + field::bodyPart::pose),
                                             (const float*)(root + field::character::gaze), bend * share, q);
        if (bent[i] && bent[i] != parts[i]) {
            if (readable(bent[i], 0x600) && *(uintptr_t*)(bent[i] + field::bodyPart::character) == root)
                spineBendPart(bent[i], identity);
            bent[i] = 0;
        } // the part changed: put back only if still ours
        if (apply) {
            spineBendPart(parts[i], q);
            bent[i] = parts[i];
        } else if (bent[i]) {
            spineBendPart(bent[i], identity);
            bent[i] = 0;
        }
    }
    if (on) ++spineBendTicks;
}
static void poseBuilder(uintptr_t root, float time, unsigned char mode) {
    healthHit(fp::HealthHook::poseBuilder);
    if (root == *(uintptr_t*)(gameAt(fp::sig::playerRoot))) {
        if (crouchHeld.load()) {
            *(float*)(root + field::character::duckAmount) = 1.f;
            ++crouchTicks;
        }
        __try {
            spineBendStep(root);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    auto bh = *(uintptr_t*)(root + field::character::behaviour);
    Aim a;
    if (!bh || !getAim(bh, a)) {
        originalPose(root, time, mode);
        return;
    }
    auto contextual = (uintptr_t*)(root + field::character::contextualTarget);
    uintptr_t saved = *contextual;
    *contextual = 0;
    float d[3];
    gazeToPoint(a, d);
    memcpy((void*)(root + field::character::gaze), d, 12);
    steadyHeadAround(root, originalPose, time,
                     mode); // only the controlled player in first person reaches here (getAim)
    if (!*contextual) *contextual = saved;
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
    30}; // [Camera] HeadBobSteady 0..100: the head's height eased toward its smoothed value
static std::atomic<unsigned> steadyTicks{0};
static fp::HeadSteadyState headSteadyState;
static fp::BodySteadyState bodySteadyState[2];
static uintptr_t steadiedHead = 0, steadiedRoot = 0, steadyNeck = 0, steadyChest = 0; // physics thread only
static bool bodyCalm(uintptr_t root, uintptr_t head);
static bool steadyAllowed(uintptr_t root, uintptr_t head) {
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        viewMode.load() != 0 || headSteady.load() <= 0)
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
    if (!getAim(bh, a)) {
        originalStep(bh);
        return;
    }
    if (*(unsigned char*)(a.root + field::character::combatStance))
        ++combatSteps;
    else
        ++unexpectedRelaxedSteps;
    auto mask = (unsigned*)(gameAt(fp::sig::keyWord));
    unsigned saved = *mask;
    auto state = (unsigned*)(bh + field::behaviour::hoverState);
    unsigned hovered = *state;
    if (hovered == 4) *state = 0;
    originalStep(bh);
    if (hovered == 4 && *state == 0) *state = hovered;
    *mask = saved;
}
static void travelTarget(float* out, const float* origin, const float* direction, float scale) {
    auto caller = (uintptr_t)_ReturnAddress();
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
    if (!*(unsigned char*)(root + field::character::combatStance) && bh && getAim(bh, a) && a.travelValid &&
        *(unsigned char*)(root + field::character::walking)) {
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
    auto mask = (unsigned*)(gameAt(fp::sig::keyWord));
    unsigned saved = *mask;
    float travel[3];
    bool moving = fp::travel(saved, a.yaw, travel);
    AcquireSRWLockExclusive(&aimLock);
    if (aim.root == a.root) {
        if (moving) {
            memcpy(aim.travel, travel, 12);
            aim.travelValid = true;
        } else if (!*(unsigned char*)(a.root + field::character::walking))
            aim.travelValid = false;
    }
    ReleaseSRWLockExclusive(&aimLock);
    float analog[3];
    memcpy(analog, (void*)(gameAt(fp::sig::analogStick)), 12);
    auto intent = (unsigned char*)(behavior + field::behaviour::moveIntent);
    unsigned char savedIntent = *intent;
    // Native analog exploration already owns continuous walking, run/crouch,
    // collision queries and braking. Feed it camera-relative digital intent.
    const bool stick = !moving && fp::analogActive(analog); // a controller stick keeps its native vector
    *mask &= ~15u;
    if (!stick) memcpy((void*)(gameAt(fp::sig::analogStick)), travel, 12);
    *intent = 0;
    uintptr_t previousRoot = exploringRoot;
    float previousDirection[3];
    memcpy(previousDirection, exploringDirection, 12);
    exploringRoot = moving ? a.root : 0;
    memcpy(exploringDirection, travel, 12);
    originalExplore(behavior);
    exploringRoot = previousRoot;
    memcpy(exploringDirection, previousDirection, 12);
    memcpy((void*)(gameAt(fp::sig::analogStick)), analog, 12);
    *mask = saved;
    *intent = savedIntent;
    if (moving) ++relaxedSteps;
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
    for (int h = 0; h < 2; h++) {
        const uintptr_t part = parts[h];
        const uintptr_t mesh = readable(part, 0x20) ? *(uintptr_t*)(part + field::bodyPart::meshNode) : 0;
        if (!mesh || !readable(mesh, 0x168) || *(uintptr_t*)(mesh + field::sceneNode::bodyPart) != part) {
            handNames[h][0] = 0;
            continue;
        }
        char name[17]{};
        memcpy(name, (void*)(mesh + field::sceneNode::name), 16);
        sides[h] = fp::handSideFromName(name);
        // Status JSON only: keep letters, digits and '_'.
        for (int i = 0; i < 16; i++) {
            const char c = i < 15 ? name[i] : 0;
            handNames[h][i] =
                !c || ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')
                    ? c
                    : '_';
        }
    }
    if (s.hand >= 0) s.handSide = sides[s.hand];
    handSlot = s.slot;
    handIndex = s.hand;
    handExpected = s.expectedHand;
    handSideSeen = s.handSide;
    return s;
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
    // Held side binding: affect only the native new-press classifier. The same
    // call's cursorAim rewrites +0x1B0 from real aim afterwards, so aiming and
    // native follow-ups/continuations are untouched.
    float* aimHeading = (float*)(self + field::behaviour::aimHeading);
    float savedHeading = *aimHeading, presented = savedHeading;
    bool flipped = false;
    Aim side;
    if (attackSideKey && (GetAsyncKeyState(attackSideKey) & 0x8000) && getAim(self, side) &&
        (*(unsigned*)(gameAt(fp::sig::buttonWord)) & 1) &&
        !*(unsigned char*)(self + field::behaviour::pressBlocked) &&
        !(*(unsigned*)(self + field::behaviour::pressFlags) & 1)) {
        const float body = *(float*)(side.root + field::character::bodyHeading);
        if (std::isfinite(savedHeading) && std::isfinite(body)) {
            presented = fp::flippedSideHeading(savedHeading, body,
                                               *(int*)(side.root + field::character::gripFlags) != 0);
            *aimHeading = presented;
            flipped = true;
            ++sideFlips;
        }
    }
    originalBehavior(self);
    if (flipped && *aimHeading == presented) *aimHeading = savedHeading; // cursorAim did not run.
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
// Only replace the local coordinates read by the native gameplay cursor draw.
// Native input polling and UI coordinate reads retain their original results.
// The UI manager's poll (0x2F7E0, returning to 0x2F847) stores the pointer at
// manager+0x18/+0x1C; world hover (0x11E6F0), click and object-drag picks
// (0x117AA0, 0x1183C0) unproject that through the current view. While first
// person owns gameplay input it reads the on-screen reticle, so the game picks
// what the crosshair shows. Native reach, eligibility and drag ownership stay
// native. Stale, off-screen or free-cursor states keep the native pointer.
static constexpr ULONGLONG reticleFreshMs = 250;
static bool gameplayOwnsPointer() {
    return InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
           InterlockedCompareExchange(&inputEnabled, 0, 0) && !cursorFreeNow() &&
           *(int*)(gameAt(fp::sig::uiOpenCount)) <= 0;
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
static volatile DWORD placingThread = 0;
static int dragPlace(uintptr_t object, uintptr_t camera, float h) {
    bool held = false;
    if (InterlockedCompareExchange(&active, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0)) {
        uintptr_t dragged = 0;
        int kind = 0;
        __try {
            kind = dragKind(&dragged);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            kind = 0;
        }
        held = kind == 1 && dragged == object &&
               viewMode.load() == 0; // the hold distance is a first-person feature
    }
    if (!held) {
        holdObject = 0;
        return originalDragPlace(object, camera, h);
    } // a later grab starts fresh
    placingThread = GetCurrentThreadId();
    const int result = originalDragPlace(object, camera, h);
    placingThread = 0;
    return result;
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
    if (levelBlocked(level, origin, dir, length) != 1) return length;
    float lo = 0, hi = length;
    for (int i = 0; i < 7; i++) {
        const float mid = (lo + hi) * .5f;
        if (levelBlocked(level, origin, dir, mid) == 1)
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
static unsigned clearanceFrame = 0;
static float lastViewClearance = -1;
static float viewClearance(const float* pose) {
    ++clearanceFrame;
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(pose[i])) return lastViewClearance = -1;
    const uintptr_t level = levelGrid();
    if (!level || !levelRay || !levelRay2) return lastViewClearance = -1;
    float aspect = 16.f / 9.f;
    RECT r;
    if (hookedWindow && GetClientRect(hookedWindow, &r) && r.bottom > 0 && r.right > 0)
        aspect = float(r.right) / float(r.bottom);
    const float tv = std::tan(renderedFov * 3.14159265f / 360.f), th = tv * aspect;
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
        if (hit < 0) return lastViewClearance = -1;
        if (hit == 0) continue;
        float lo = 0, hi = length;
        for (int i = 0; i < 6; i++) {
            const float mid = (lo + hi) * .5f;
            const int m = levelBlocked(level, pose + 9, d, mid);
            if (m < 0) return lastViewClearance = -1;
            if (m == 1)
                hi = mid;
            else
                lo = mid;
        }
        best = std::fmin(best, lo * float(1.0 / n)); // along the ray -> view depth
    }
    return lastViewClearance = best;
}
// Drop where you hold it. During an 0x2000 drag the input step 0x11F610 asks
// 0x117AA0 for the drop spot each frame; it unprojects the pointer (0x8F720, return 0x117B18) and
// accepts only a downward ray (dir.y below -eps). A valid spot is saved (drag+0x70, +0x88) and
// drag+0x14D cleared; otherwise the item reverts to its last valid transform and release
// (0x11B340 -> 0x119CB0 -> 0x119C30) puts it back. In first person you look level at a held item,
// so it was always invalid. While first person holds an item and owns the pointer, that pick
// reports a short downward ray onto the hold point (wall- and reach-clamped like the placement),
// so the drop spot is where the item is held. A nearer real surface keeps the native result.
static bool firstPersonHold() {
    if (viewMode.load() != 0) return false; // third person keeps the game's own drop spot
    if (!InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0))
        return false;
    if (!gameplayOwnsPointer() && !InterlockedCompareExchange(&rmbFree, 0, 0))
        return false; // right mouse frees the cursor: still dropped where held
    const uintptr_t drag = *(uintptr_t*)(gameAt(fp::sig::dragManager));
    if (!readable(drag, 0x20) || *(unsigned*)(drag + 8) != 0x2000) return false;
    const uintptr_t object = *(uintptr_t*)(drag + field::drag::object);
    return object && holdObject.load() == object && holdApplied.load() > 0;
}
static std::atomic<unsigned> dropSpots{0};
static char dropSpot(char hit, uintptr_t out) {
    float* dir = (float*)(out + field::pick::direction);
    float* origin = (float*)(out + field::pick::origin);
    const float length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!std::isfinite(length) || length < 0.5f || !std::isfinite(origin[0] + origin[1] + origin[2]))
        return hit;
    float t = holdReachClamp(origin, dir, holdApplied.load());
    if (hit && *(float*)(out + field::pick::distance) <= t)
        return hit; // a surface before the hold point: native
    const uintptr_t level = levelGrid();
    if (level) t = levelClear(level, origin, dir, t);
    if (!(t >= 1.f)) return hit;
    float spot[3];
    for (int i = 0; i < 3; i++) spot[i] = origin[i] + dir[i] * t;
    constexpr float drop = 4.f; // a short downward ray ending at the hold point
    // 0x117AA0 tests that segment against the level too; under a ceiling lip keep the native result.
    {
        const float top[3] = {spot[0], spot[1] + drop, spot[2]}, down[3] = {0, -1, 0};
        if (level && levelBlocked(level, top, down, drop) != 0) return hit;
    }
    origin[0] = spot[0];
    origin[1] = spot[1] + drop;
    origin[2] = spot[2];
    dir[0] = 0;
    dir[1] = -1;
    dir[2] = 0;
    for (int i = 0; i < 3; i++) ((float*)(out + 8))[i] = spot[i];
    *(float*)(out + field::pick::distance) = drop;
    ++dropSpots;
    return 1;
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
static void selfPickMask() {
    selfPickSet = 0;
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    const uintptr_t mesh = readable(root, 0x20) ? *(uintptr_t*)(root + field::character::sceneNode) : 0;
    if (!mesh) return;
    const unsigned count = walkScene(mesh, selfPickNodes, selfPickParents, 1024);
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t node = selfPickNodes[i];
        if (!readable(node, 0x10)) continue;
        unsigned& flags = *(unsigned*)(node + 8);
        if (!(flags & pickSkipBit)) {
            flags |= pickSkipBit;
            selfPickNodes[selfPickSet++] = node;
        }
    } // compacted in place: only the ones changed here
    if (selfPickSet) ++selfPicksMasked;
}
static void selfPickRestore() {
    for (unsigned i = 0; i < selfPickSet; ++i)
        if (readable(selfPickNodes[i], 0x10)) *(unsigned*)(selfPickNodes[i] + 8) &= ~pickSkipBit;
    selfPickSet = 0;
}
static char pickPastSelf(PickRay pick, uintptr_t camera, float x, float y, uintptr_t out) {
    if (!selfPickWanted() || selfPickSet)
        return pick(camera, x, y, out); // nested: the outer call already masked
    char hit = 0;
    __try {
        selfPickMask();
        hit = pick(camera, x, y, out);
    } __finally {
        selfPickRestore();
    }
    return hit;
}
