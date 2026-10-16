// Shared state: the resolved game layout, settings, the status file, notices, faults, input release, drag queries.
// Part of first_person.cpp (one translation unit): included there, in order.
// Threads. Each hook runs on whichever game thread calls the function it replaces. By role (several roles
// may share one OS thread; the status file's nav_thread, swap_thread and sector_thread say which):
// - render thread: swap (once a frame), cursorPosition, the picks, sectorUpdate, the view features;
// - camera: nav (the replaced sampler; navThread), the game's frame logic;
// - physics: poseBuilder, partSettle;
// - input: input (polling), behavior and its cursorAim/explore/combatStep (the player's behaviour tick);
// - window thread: windowProc (keys, mouse, focus);
// - this mod's worker: runSession (activation, F7/F6, notices, the status file, the hook health check);
// - the F8 window's own thread (settings_window.h).
// Values one thread writes and others read are atomics or Interlocked, or sit behind a lock below; the
// rest is used by one thread (most of it says which in its comment).
// Locks (SRW, never recursive). Nested only in this order (every function that takes one was read for it):
// cameraLock -> aimLock -> rawLock
// cameraLock -> reticleLock, hideLock, tpHideLock
// cameraLock (CameraGuard) guards the camera publication: what nav writes into the game's camera, the saved
// stock values and their restoration. Nothing waits while holding it: awaitRestore polls with
// TryAcquire and sleeps without it, so a stop can never deadlock against a frame in progress.
static uintptr_t base;
// Where each game target is in this game build (game_layout.h). Until activation resolves it, the
// source build's addresses, which the offline suites' fake memory is laid out by.
static fp::Layout layout;
static uintptr_t gameAt(fp::sig::Target t) {
    return base + layout.rva[t];
}
// Calls into the hooks every game tick reaches (fp::HookHealth): the live check that they work.
static std::atomic<uint32_t> healthHits[fp::healthHooks]{};
static void healthHit(fp::HealthHook h) {
    healthHits[unsigned(h)].fetch_add(1, std::memory_order_relaxed);
}
static HMODULE module;
using Nav = uintptr_t (*)(void*, float);
static Nav original;
static uintptr_t nav(void*, float);
static volatile LONG stopping = 0, restored = 0;
static volatile LONG pendingRestore = 0;
static volatile LONG active = 0;
static volatile LONG protectionFailure = 0;
static uintptr_t savedCamera = 0, savedView = 0, savedController = 0;
// Gameplay identity when the saved frame was committed: controlled root and scene.
static uintptr_t savedRoot = 0, savedScene = 0;
// Parked: first person released the camera because gameplay was
// left (title screen, death-screen EXIT). Hooks stay armed; nav re-attaches once
// parkStep allows. Guarded by cameraLock.
// live parks too (no-menu swaps, bad head frames); repeated live parks
// in one gameplay back off (fp::recordLivePark) instead of stopping.
struct Park {
    bool active = false, unverified = false, left = false, rearmPending = false;
    uintptr_t root = 0, scene = 0;
    unsigned stable = 0;
    fp::LiveParks recent{};
    ULONGLONG backoffUntil = 0;
};
static Park park;
// A released camera that may still be alive: restored if it is ever the current
// camera again with our values still in it (fov we wrote, near 1). cameraLock.
// root/scene: the gameplay it was released in; left latches once that gameplay
// is gone (a stop while it is still current cannot claim restoration).
// several records, so a second release
// in the same gameplay cannot silently drop the first; when all are taken by
// unresolved records of the current gameplay, restoration becomes unknown.
struct Retired {
    uintptr_t camera = 0, root = 0, scene = 0;
    bool left = false;
    float pose[12]{}, fov = 0, nearPlane = 0, writtenFov = 0, writtenNear = 1;
};
static constexpr unsigned retiredSlots = 4;
static Retired retired[retiredSlots];
static std::atomic<unsigned> parks{0}, rearms{0}, reclaims{0}, parkBackoffs{0};
// Bad head frames: nav thread, cameraLock. Mirrored for status.
static fp::HeadHold headHold;
static std::atomic<unsigned> heldFrames{0}, holds{0}, maxHold{0}, holdParks{0}, holdingNow{0};
static void publishHold() {
    heldFrames = headHold.heldFrames;
    holds = headHold.holds;
    maxHold = headHold.maxHold;
    holdParks = headHold.parks;
    holdingNow = headHold.run;
}
// F7 hold-to-stop: tick of the first key-down / key-up seen by the
// window procedure; the worker decides (fp::stopKeyStep). F6 re-arms after a clean stop.
static std::atomic<ULONGLONG> stopPress{0}, stopRelease{0};
static std::atomic<unsigned> stopTaps{0}, stopRearms{0};
static std::atomic<ULONGLONG> rearmedAt{0}; // tick of the last F6 re-arm (fp::rearmF6Accepted)
static std::atomic<int> parkKind{0};        // fp::OwnerChange of the last park, 0 none.
static volatile LONG parkedNow = 0;
// Restoration uncertainty is sticky: pointer equality later is not lifetime proof.
static volatile LONG restorationUnknown = 0, cameraFault = 0, cameraReady = 0;
enum CameraFault : LONG { NoCameraFault = 0, SourceLost = 1, OwnerChanged = 2 };
static void releaseInput();
static float stockNear = 100;
static std::atomic<unsigned> samples{0};
static float lastHead[12]{}, lastCamera[12]{};
// Head origin in the camera's (global) frame, paired with lastCamera: the
// start of the native line of sight (0x246610 uses the head part).
static float lastHeadGlobal[3]{};
static SRWLOCK aimLock = SRWLOCK_INIT;
struct Aim {
    uintptr_t root = 0, head = 0;
    float point[3]{}, origin[3]{}, anchor[3]{}, travel[3]{}, yaw = 0, pitch = 0, targetYaw = 0,
                                                             targetPitch = 0;
    unsigned revision = 0;
    bool valid = false, travelValid = false;
};
static Aim aim;
static volatile LONG mouseCaptured = 0;
static volatile LONG cursorFree = 0;
// Right mouse frees the cursor in first and third person, but only out of combat: in combat, holding it
// walks toward the aim, and the overhead view uses it for walking. Gameplay treats it like F8's free
// cursor without opening the panel. rmbMode: 0 off, 1 hold, 2 toggle.
static volatile LONG rmbFree = 0;
static std::atomic<int> rmbMode{1};
// The inventory, an item's description, a container or a corpse's loot, or the arena's end-of-match menu
// (NEXT FIGHT) on screen frees the cursor and holds look and movement, in first and third person, as F8
// does ([View] FreezeForWindows, F8 Controls). Set once a frame on the game's thread (windowFreeStep).
static volatile LONG windowFree = 0;
static std::atomic<bool> windowsFreeze{true};
// Crouch (poseBuilder): the game's duck key as polled (input), the mode ([Input] Crouch: 0 off, 1 hold, 2 toggle)
// and whether the duck is being held up this frame (crouchStep on the game's thread).
static std::atomic<int> crouchMode{2};
static std::atomic<bool> duckKey{false}, crouchHeld{false};
static std::atomic<unsigned> crouchTicks{0};
static constexpr unsigned duckKeyBit = 0x10;
static bool cursorFreeNow() {
    return InterlockedCompareExchange(&cursorFree, 0, 0) || InterlockedCompareExchange(&rmbFree, 0, 0) ||
           InterlockedCompareExchange(&windowFree, 0, 0);
}
static volatile LONG heldKeys = 0, tappedKeys = 0, inputEnabled = 0;
static volatile LONG tappedButtons = 0;
static constexpr unsigned movementBits = 15u; // forward, left, back, right in the game's key word (0x49CDC0)
static void movementKeys(unsigned* keys) {
    const unsigned fallback[4] = {'W', 'A', 'S', 'D'};
    for (unsigned i = 0; i < 4; ++i)
        keys[i] = base ? *(unsigned*)(gameAt(fp::sig::movementKeys) + i * 16) : fallback[i];
}
static unsigned keyMask(WPARAM key) {
    unsigned keys[4];
    movementKeys(keys);
    return fp::movementKey(unsigned(key), keys);
}
// The movement keys physically held now (the tests replace the keyboard).
static SHORT(WINAPI* keyState)(int) = GetAsyncKeyState;
static unsigned physicalMovement() {
    unsigned keys[4];
    movementKeys(keys);
    unsigned held = 0;
    for (unsigned k : keys)
        if (k > 0 && k < 256 && (keyState(int(k)) & 0x8000)) held |= fp::movementKey(k, keys);
    return held;
}
static fp::ButtonHandoff buttonHandoff; // Only native input thread accesses this.
static volatile LONG inputGeneration = 0;
static LONG sampledInputGeneration = -1;
static std::atomic<float> sensitivity{
    0.0025f}; // [Input] SensitivityPercent; the F8 settings page changes it live
// [View] SwingCrosshair=hide|show (the crosshair during an accepted swing). [Camera] HeadBob
// is gone.
static std::atomic<bool> hideSwingCrosshair{false};
// [View] IdleCrosshair=rmb|always: out of combat the crosshair shows only while right mouse frees the cursor.
static std::atomic<bool> idleCrosshairAlways{false};
// [Input] AimMarker=1 shows the crosshair at the aim while the head catches up; FlickCatchup 0..100 (fp::flickGain).
static std::atomic<bool> aimMarker{true};
static std::atomic<int> flickCatchup{50};
// which view F6 shows while the mod owns the camera: 0 first person, 1 third person (F6 cycles
// first -> third -> the game's overhead). [ThirdPerson] Distance / Shoulder / Height in first-person.ini.
static std::atomic<int> viewMode{0}, tpDistance{250}, tpShoulder{40}, tpHeight{15};
static std::atomic<bool> tpTight{true};
// combat framing (item 4: closer and centred while in combat mode), the third-person FOV (item 8: 0 = as first
// person) and the third -> overhead blend (item 7; viewMode 2 = blending out, ends in the game's view).
static std::atomic<bool> tpCombat{true};
static std::atomic<int> tpFov{0};
// camera lag 0..100, hiding what stands between the
// camera and the player instead of pulling in (walls still pull in), and the knockdown follow.
static std::atomic<int> tpLag{40};
static std::atomic<bool> tpHide{true}, tpDown{true};
static std::atomic<unsigned> tpHidden{0};
static std::atomic<ULONGLONG> tpBlendStart{0};
static float tpCombatK = 0.f; // swap shoulder / closer framing in tight spaces
static std::atomic<unsigned> terrainTilesApplied{0}, terrainWakes{0}, terrainLightMarks{0},
    terrainSlowPaint{0}; // terrain live (status)
// in third person the aim point is what the crosshair (the camera's centre ray) is over, so the
// character swings at it; written by the camera hook, read by the input step (aimLock).
static float tpAimLocal[3]{};
static bool tpAimSet = false;
static fp::ThirdPerson thirdPerson;          // cameraLock
static std::atomic<unsigned> tpPropPulls{0}; // frames a prop pulled the third-person camera in
static std::atomic<int> headSteadyRoll{80}, headSteadyPitch{70},
    headSteadyYaw{
        50}; // [Camera] HeadRoll/HeadPitch/HeadYaw 0..100: per-axis neck stiffening (fp::wobbleSlackScale) and the stabiliser's damping (fp::steadyHead)
static std::atomic<int> headSteady{
    70}; // [Camera] HeadStabilisation 0..100; default 70: all steadying is on the head
static std::atomic<bool> swinging{false}; // an ordinary attack the game accepted is under way (publishCue)
static std::atomic<unsigned> swingHides{0};
static std::atomic<unsigned> headRejects
    [8]{}; // why head frames were refused: no head, not the root's, mesh, non-finite, scale, scene, target, pose
static std::atomic<bool> rawWanted{
    false};                      // [Input] RawMouse; the worker registers / removes raw input to match
static HWND rawTarget = nullptr; // the window raw input is registered for (worker thread)
static bool invertY = false;
// [Input] RawMouse=1 opts into relative WM_INPUT counts (no pointer acceleration).
// Default off: the cursor-delta path is the accepted feel,
// and raw counts changed effective turn rate enough to upset head stabilization.
static bool rawMouse = false;
// [Input] AttackSideKey=<virtual-key code>. Optional held binding that switches
// the ordinary attack side for presses made while it is held. 0 = unassigned.
static int attackSideKey = 0;
// [Input] ProbeKey (default 0 = unbound). Diagnostic: writes a scene/head
// inventory JSON beside the DLL. It changes nothing in the game.
static int probeKey = 0;
static volatile LONG probeRequest = 0;
static std::atomic<unsigned> sideFlips{0};
// [View] AttackCue=1: the wind-up arc around the combat crosshair. CueSize/CueOpacity are percentages;
// CueThrust=1 draws thrusts below the crosshair.
static std::atomic<bool> attackCue{true}, cueThrust{true};
static std::atomic<int> cueSize{100}, cueOpacity{100};
// Published on the behaviour tick (publishCue), read by the frame thread only through these.
static std::atomic<int> cueArcSide{0}, cueArcPhase{0};
static std::atomic<float> cueArcFill{0};
static std::atomic<unsigned long long> cueArcFade{0}, cueArcPublished{0};
static std::atomic<int> cueState{0};  // fp::OriginCue, published by behavior.
static std::atomic<int> cueOrigin{0}; // fp::CueSide before roll, from the swinging weapon's hand.
// Last hand resolution (diagnostic): slot, hand, expected hand, hand side, grip class.
static std::atomic<int> handSlot{-1}, handIndex{-1}, handExpected{-1}, handSideSeen{0}, handGrip{-1},
    handCount{-1};
static char handNames[2][16]{}; // Hand part mesh names; written by the game thread, read for status only.
static std::atomic<unsigned> cueDraws{0}, uiReticleReads{0};
// Diagnostic: does the pick ray's camera (viewport+0x1C0, read by 0x8F460 for
// near/pose) equal the camera first person writes? -1 unknown, 0 no, 1 yes.
static std::atomic<int> pickCamera{-1};
static std::atomic<int> cueGl{0}; // 0 untried, 1 drawing, -1 unavailable on this context.
// Last on-screen reticle the native gameplay cursor was drawn at (client px).
// Leaf lock: never held while calling anything else.
static SRWLOCK reticleLock = SRWLOCK_INIT;
static LONG reticleX = 0, reticleY = 0, reticleFrame = -1;
static ULONGLONG reticleTick = 0;
// Pick clearance for that reticle: 1 clear, 0 level geometry between the head
// and the pick origin, -1 not determined (no level collision, no camera).
static int reticleClear = -1;
static std::atomic<unsigned> pickBlocked{0}, pickChecks{0}, clearanceQueries{0};
static std::atomic<int> lastClearance{-1};
// Native level ray queries (RVA 0x1430F0, 0x1431C0), as 0x2462F0 calls them:
// (level grid, global origin, unit direction, length) -> hit. Read-only.
using LevelRay = bool (*)(uintptr_t, const float*, const float*, float);
static LevelRay levelRay, levelRay2;
static volatile LONG rawRegistered = 0;
static fp::RawLook rawLook; // Guarded by rawLock.
static SRWLOCK rawLock = SRWLOCK_INIT;
static WNDPROC originalWindowProc;
static HWND hookedWindow;
static float stockPose[12]{}, stockFov = 30;
static std::atomic<int> selectedFov{75};
static std::atomic<int> nearClip{
    fp::nearClipDefaultTenths};  // [Camera] NearClip: the dynamic near plane's floor, tenths of a cm
static float renderedFov = 75;   // Protected by cameraLock, paired with lastCamera.
static float renderedNear = 1.f; // the near plane first person wrote (cameraLock); was always 1
// the view's heading as last drawn (render thread), so an aim that (re)starts begins where
// the view looks instead of snapping to the body's heading and level pitch.
static std::atomic<uintptr_t> viewRoot{0};
static std::atomic<float> viewYaw{0}, viewPitch{0};
static wchar_t settingsPath[MAX_PATH]{};
// Settings live in one per-user file that outlives release folders (Documents\ExaniMate\first-person.ini);
// the release's own first-person.ini holds the defaults and is never written. When the user file does not
// exist yet it starts as a copy of the most recently edited release ini (one written after its folder's DLL,
// so a fresh build's defaults never win over the player's), else of this release's. moduleIni: the ini
// beside this DLL; userDir: empty keeps the settings beside the DLL.
static std::wstring userSettingsFile(const std::wstring& moduleIni, const std::wstring& userDir) {
    if (userDir.empty()) return moduleIni;
    const std::wstring user = userDir + L"\\first-person.ini";
    if (GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return user;
    SHCreateDirectoryExW(nullptr, userDir.c_str(), nullptr); // with any missing parents
    std::wstring source = moduleIni;
    const size_t folderEnd = moduleIni.find_last_of(L'\\'),
                 releasesEnd = folderEnd == std::wstring::npos ? folderEnd
                                                               : moduleIni.find_last_of(L'\\', folderEnd - 1);
    if (releasesEnd != std::wstring::npos) {
        const std::wstring releases = moduleIni.substr(0, releasesEnd);
        ULONGLONG newest = 0;
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileW((releases + L"\\ExanimaFirstPerson-*").c_str(), &f);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;
                const std::wstring folder = releases + L"\\" + f.cFileName;
                WIN32_FILE_ATTRIBUTE_DATA ini{}, dll{};
                if (!GetFileAttributesExW((folder + L"\\first-person.ini").c_str(), GetFileExInfoStandard,
                                          &ini) ||
                    !GetFileAttributesExW((folder + L"\\ExanimaFirstPerson.dll").c_str(),
                                          GetFileExInfoStandard, &dll))
                    continue;
                const ULONGLONG edited =
                    (ULONGLONG(ini.ftLastWriteTime.dwHighDateTime) << 32) | ini.ftLastWriteTime.dwLowDateTime;
                const ULONGLONG built =
                    (ULONGLONG(dll.ftLastWriteTime.dwHighDateTime) << 32) | dll.ftLastWriteTime.dwLowDateTime;
                constexpr ULONGLONG packingSlack =
                    10ull * 10000000ull; // 10 s in file-time units: the build writes both
                if (edited > built + packingSlack && edited > newest) {
                    newest = edited;
                    source = folder + L"\\first-person.ini";
                }
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
    }
    return CopyFileW(source.c_str(), user.c_str(), TRUE) ||
                   GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES
               ? user
               : moduleIni;
}
// The per-user settings folder: Documents\ExCamera. Releases before the ExCamera name kept settings in
// Documents\ExaniMate; the file is copied over once (copied, not moved: the editor still uses that folder).
// The dev build keeps the editor's folder.
static std::wstring userSettingsDir(const std::wstring& profile) {
    const std::wstring dir = profile + L"\\Documents\\ExCamera", file = dir + L"\\first-person.ini",
                       old = profile + L"\\Documents\\ExaniMate\\first-person.ini";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        CopyFileW(old.c_str(), file.c_str(), TRUE);
    }
    return dir;
}
static int persistedFov = 75;
static void saveFov() {
    const int value = selectedFov.load();
    if (!settingsPath[0] || value == persistedFov) return;
    wchar_t text[16];
    swprintf_s(text, L"%d", value);
    if (WritePrivateProfileStringW(L"Camera", L"FOV", text, settingsPath)) persistedFov = value;
}
static unsigned inputClock = ~0u;
static wchar_t statusPath[MAX_PATH]{}, statusTempPath[MAX_PATH]{};
static const char* runtimeState = "starting"; // Worker thread owns diagnostic strings.
static FILE* openStatus() {
    FILE* f = nullptr;
    if (statusTempPath[0]) _wfopen_s(&f, statusTempPath, L"w");
    return f;
}
static void publishStatus(FILE* f) {
    if (!f) return;
    const bool ok = ferror(f) == 0;
    const bool closed = fclose(f) == 0;
    if (ok && closed)
        MoveFileExW(statusTempPath, statusPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
// Fault notice: the window title is invisible in fullscreen. A topmost,
// click-through, non-activating banner over the game window states the fault.
// Owned and pumped by the worker thread only; it never takes focus and hides
// while another application is in front. Code stays mapped until process exit.
static HWND noticeWindow;
static const wchar_t* noticeText = L"";
static ULONGLONG noticeShownMs = 0, noticeLastTick = 0, noticeStarted = 0;
static bool noticeVisible = false;
static constexpr ULONGLONG noticeLifetimeMs = 120000;
static ULONGLONG noticeVisibleMs = 10000; // Per notice: faults 10 s, F7 hints shorter.
static std::atomic<unsigned> notices{0};
static LRESULT CALLBACK noticeProc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HBRUSH fill = CreateSolidBrush(RGB(120, 18, 18));
        FillRect(dc, &r, fill);
        DeleteObject(fill);
        HBRUSH edge = CreateSolidBrush(RGB(235, 200, 120));
        FrameRect(dc, &r, edge);
        DeleteObject(edge);
        HFONT font = CreateFontW(-(r.bottom * 9 / 20), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 246, 230));
        DrawTextW(dc, noticeText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, message, wp, lp);
}
static BOOL CALLBACK findWindow(HWND w, LPARAM out);
static void showNotice(const wchar_t* text, ULONGLONG visibleMs = 10000) {
    if (noticeWindow) {
        DestroyWindow(noticeWindow);
        noticeWindow = nullptr;
        noticeVisible = false;
    }
    // An early activation fault can precede the first window enumeration.
    if (!hookedWindow || !IsWindow(hookedWindow)) {
        hookedWindow = nullptr;
        EnumWindows(findWindow, (LPARAM)&hookedWindow);
    }
    if (!hookedWindow) return;
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = noticeProc;
        wc.hInstance = module;
        wc.lpszClassName = L"ExCameraNotice";
        wc.hCursor = nullptr;
        cls = RegisterClassW(&wc);
        if (!cls) return;
    }
    RECT g;
    if (!GetWindowRect(hookedWindow, &g)) return;
    const int gw = g.right - g.left, gh = g.bottom - g.top;
    if (gw < 200 || gh < 100) return;
    const int width = gw * 4 / 5 < 1200 ? gw * 4 / 5 : 1200, height = gh / 14 < 40 ? 40 : gh / 14;
    noticeText = text;
    noticeVisibleMs = visibleMs;
    noticeWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                       WS_EX_TOOLWINDOW,
                                   L"ExCameraNotice", text, WS_POPUP, g.left + (gw - width) / 2,
                                   g.top + gh / 9, width, height, nullptr, nullptr, module, nullptr);
    if (!noticeWindow) return;
    SetLayeredWindowAttributes(noticeWindow, 0, 235, LWA_ALPHA);
    noticeVisible = false;
    noticeShownMs = 0;
    noticeStarted = noticeLastTick = GetTickCount64();
    ++notices;
}
// Returns false once the notice is gone. Visible time counts only while the
// game is frontmost, so a fault during alt-tab is still seen on return.
static bool pumpNotice() {
    if (!noticeWindow) return false;
    const ULONGLONG now = GetTickCount64();
    if (noticeVisible) noticeShownMs += now - noticeLastTick;
    noticeLastTick = now;
    const bool front = hookedWindow && GetForegroundWindow() == hookedWindow && !IsIconic(hookedWindow);
    if (noticeShownMs >= noticeVisibleMs || now - noticeStarted >= noticeLifetimeMs ||
        !IsWindow(hookedWindow)) {
        DestroyWindow(noticeWindow);
        noticeWindow = nullptr;
        noticeVisible = false;
    } else if (front != noticeVisible) {
        ShowWindow(noticeWindow, front ? SW_SHOWNOACTIVATE : SW_HIDE);
        noticeVisible = front;
        if (front)
            SetWindowPos(noticeWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return noticeWindow != nullptr;
}
// The fault's specifics (which target, which hook) for first-person-status.json; empty when there are none.
static char faultDetail[512]{};
// notice: shown instead of the generic fault text; must outlive the notice (a literal or a static buffer).
static void reportFault(const char* reason, const char* hooks = "unknown",
                        const char* restoration = "unknown", const wchar_t* notice = nullptr) {
    runtimeState = reason;
    viewMode = 0;
    InterlockedExchange(&cameraReady, 0);
    InterlockedExchange(&stopping, 1);
    InterlockedExchange(&active, 0);
    releaseInput();
    const bool verified = strcmp(restoration, "restored") == 0;
    const bool removed = strcmp(hooks, "removed") == 0 || strcmp(hooks, "not_installed") == 0;
    if (FILE* f = openStatus()) {
        fp::JsonOut j(f);
        j.unum("pid", GetCurrentProcessId()).num("revision", FP_REVISION).str("build", FP_BUILD);
        if (removed)
            j.boolean("installed", false);
        else if (strcmp(hooks, "retained") == 0)
            j.boolean("installed", true);
        else
            j.null("installed");
        j.boolean("active", false)
            .str("state", reason)
            .str("hooks", hooks)
            .str("restoration", restoration)
            .boolean("restored", verified)
            .boolean("restart_required", true)
            .str("detail", faultDetail);
        j.finish();
        publishStatus(f);
    }
    // Immutable text remains valid even if the receiving window times out.
    const wchar_t* message = fp::faultMessage(verified, removed, strcmp(restoration, "not_required") == 0);
    if (hookedWindow) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(hookedWindow, WM_SETTEXT, 0, (LPARAM)message, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200,
                            &ignored);
    }
    showNotice(notice ? notice : message + fp::faultTitlePrefix);
}
static std::atomic<unsigned> controlCalls{0}, pointCalls{0}, relaxedSteps{0};
static volatile LONG navThread = 0, swapThread = 0;
static HANDLE instanceMutex;
static SRWLOCK cameraLock = SRWLOCK_INIT;
static fp::NearPlane nearPlane; // dynamic near plane (cameraLock)
static uintptr_t filterRoot = 0, filterHead = 0;
static LARGE_INTEGER filterTime{}, frequency{};
static std::atomic<unsigned> walkTargets{0}, combatSteps{0}, unexpectedRelaxedSteps{0};
using TravelTarget = void (*)(float*, const float*, const float*, float);
static TravelTarget originalTravelTarget;
using WalkGait = void (*)(uintptr_t, const float*);
static WalkGait originalWalkGait;
static thread_local uintptr_t exploringRoot = 0;
static thread_local float exploringDirection[3]{};
struct CameraGuard {
    CameraGuard() { AcquireSRWLockExclusive(&cameraLock); }
    ~CameraGuard() { ReleaseSRWLockExclusive(&cameraLock); }
};
// Risk audit item 5: while look owns the mouse, the OS cursor is
// confined to the game's client area, so a fast flick in windowed mode or on two
// monitors cannot click another window. Released with input.
static volatile LONG cursorClipped = 0;
static void releaseInput() {
    if (InterlockedExchange(&cursorClipped, 0)) ClipCursor(nullptr);
    if (InterlockedExchange(&inputEnabled, 0)) InterlockedIncrement(&inputGeneration);
    AcquireSRWLockExclusive(&rawLock);
    rawLook.reset();
    ReleaseSRWLockExclusive(&rawLock);
    InterlockedExchange(&mouseCaptured, 0);
    InterlockedExchange(&heldKeys, 0);
    InterlockedExchange(&tappedKeys, 0);
    InterlockedExchange(&tappedButtons, 0);
}
static volatile LONG swapCalls = 0, nativeCursorUpdates = 0;
using Behavior = void (*)(uintptr_t);
using Point = void (*)(uintptr_t, float*);
using Swap = BOOL(WINAPI*)(HDC);
using Pose = void (*)(uintptr_t, float, unsigned char);
static Pose originalPose;
static Behavior originalStep;
static void (*originalInput)();
static Behavior originalBehavior, originalCursor, originalExplore;
static Point originalPoint;
static Swap originalSwap;
using Projection = void (*)(uintptr_t);
using Inverse = void (*)(uintptr_t, uintptr_t);
static Projection rebuildProjection;
static Inverse rebuildInverse;
static bool awaitRestore(DWORD timeout = 2000) {
    // Closing the publication gate lasts until a clean stop is re-armed (F6 after
    // F7, runSession). The same lock guards nav/swap publication and this
    // quiescence observation.
    InterlockedExchange(&stopping, 1);
    InterlockedExchange(&active, 0);
    releaseInput();
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (TryAcquireSRWLockExclusive(&cameraLock)) {
            // Stopping while a released camera of the current gameplay is unresolved
            // (never current again, so never reclaimed) cannot be called restored.
            for (const Retired& r : retired)
                if (r.camera && !r.left && *(uintptr_t*)(gameAt(fp::sig::playerRoot)) == r.root &&
                    *(uintptr_t*)(gameAt(fp::sig::currentScene)) == r.scene)
                    InterlockedExchange(&restorationUnknown, 1);
            const bool unknown = InterlockedCompareExchange(&restorationUnknown, 0, 0) != 0;
            const bool ready = !unknown && !savedCamera && !InterlockedCompareExchange(&pendingRestore, 0, 0);
            if (ready) InterlockedExchange(&cameraReady, 0);
            ReleaseSRWLockExclusive(&cameraLock);
            if (unknown) return false;
            if (ready) return true;
        }
        if (GetTickCount64() - started >= timeout) return false;
        Sleep(1); // Never hold a lock needed by the restoration callback while waiting.
    }
}
using CursorPosition = void (*)(LONG*, LONG*);
static CursorPosition originalCursorPosition;
// drags. The game's drag manager (0x4EFDD0) has flag 0x2000 at +8
// while something is dragged, the dragged object at +0x10; its mesh node at object +0x160.
// Doors and hinged things (0x4000/0x10000) are held in place; a locked door's rattle (0x80000) is the game's.
static std::atomic<float> doorDistance{
    0}; // a held door or lid: its point's distance along the pointer ray; 0 = none
static std::atomic<float> doorCameraOffset{
    0}; // how much farther the camera is than the eye along that ray (third person)
static uintptr_t heldDoorDrag();
static uintptr_t lastDoorObject = 0;
static void releaseHeldDoor();
static std::atomic<unsigned> doorHolds{0}, holdNotches{0}, holdPlacements{0}, warpsSkipped{0},
    grabsRefused{0};
static std::atomic<int> grabRange{
    fp::grabRangeDefault}; // [Input] GrabRange, cm from the eye (fp::clampGrabRange)
static std::atomic<float> holdTarget{0}, holdApplied{0};
static std::atomic<uintptr_t> holdObject{0};
static ULONGLONG holdTick = 0;
static char dragName[17]{}; // status: the drag manager's flags while a drag is on
static bool readable(uintptr_t address, size_t size);
static const char* nodeName(uintptr_t node);
static int dragKind(uintptr_t* object = nullptr);
static int dragKind(uintptr_t* object) {
    // 0x11F610 (the player's input step) reads the manager's flags: 0x2000 an object drag (placed by
    // 0x1183C0), 0x4000 and 0x10000 a hinged drag it releases beyond reach (the dragged object's
    // position at +0x128), 0x80000 a hand grab (cursor state 8). Live: a closed door
    // dragged by its face reads 0x4000, a hand grab beside an open door 0x80000.
    const uintptr_t drag = *(uintptr_t*)(gameAt(fp::sig::dragManager));
    if (!readable(drag, 0x20)) return 0;
    const unsigned flags = *(unsigned*)(drag + 8);
    const uintptr_t o = *(uintptr_t*)(drag + field::drag::object);
    if (object) *object = o;
    if (flags)
        snprintf(dragName, sizeof dragName, "0x%x", flags);
    else
        dragName[0] = 0;
    if (flags & 0x2000) return readable(o, 0x168) ? 1 : 0;
    if (flags & (0x4000 | 0x10000 | 0x80000)) return 2;
    return 0;
}
static void normalize(float* v);
static bool playerContext(uintptr_t behavior);
// Right mouse may free the cursor only while the player is in play under the mod's own view (not the
// overhead blend, no game screen such as the inventory or the K/P screens, not dead), F8 is closed,
// nothing is held or dragged, and the player is out of combat. root+0xDF4 == 0x20 is a state where the
// game itself acts on right mouse (0xC1CF0), so it is left alone there too.
static bool rmbEligible() {
    if (viewMode.load() >= 2 || InterlockedCompareExchange(&cursorFree, 0, 0)) return false;
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    if (!readable(root, 0x1300)) return false;
    const uintptr_t behavior = *(uintptr_t*)(root + field::character::behaviour);
    if (!readable(behavior, 0x8a0) || !playerContext(behavior)) return false;
    if (*(unsigned char*)(root + field::character::combatStance) ||
        *(unsigned*)(root + field::character::stateFlags) == 0x20)
        return false;
    uintptr_t object = 0;
    int kind = 0;
    __try {
        kind = dragKind(&object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (kind == 1) return true; // an item held in place
    if (kind == 2)
        return doorDistance.load() > 0 &&
               heldDoorDrag(); // a door or lid held in place, never a locked door's rattle
    return kind == 0;
}
static fp::RightMouseFree rmbState; // window thread only; rmbFree mirrors .on for the other threads
// Something grabbed with the cursor freed keeps its place on screen when right mouse is let go: the
// pointer stays as far from the crosshair as the cursor was, for as long as that same thing is held, and
// freeing the cursor again puts it back there.
static HWND gameWindow();
static std::atomic<uintptr_t> heldOffsetObject{0};
static std::atomic<LONG> heldOffsetX{0}, heldOffsetY{0};
static uintptr_t heldThing() {
    uintptr_t o = 0;
    __try {
        if (!dragKind(&o)) o = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o = 0;
    }
    return o;
}
static void keepHeldOffset(bool nowFree) {
    const uintptr_t held = heldThing();
    HWND w = gameWindow();
    if (!held || !w) {
        heldOffsetObject = 0;
        return;
    }
    AcquireSRWLockShared(&reticleLock);
    const LONG rx = reticleX, ry = reticleY;
    ReleaseSRWLockShared(&reticleLock);
    if (!nowFree) {
        POINT c{};
        if (GetCursorPos(&c) && ScreenToClient(w, &c)) {
            heldOffsetX = c.x - rx;
            heldOffsetY = c.y - ry;
            heldOffsetObject = held;
        }
        return;
    }
    if (heldOffsetObject.load() == held) {
        POINT c{rx + heldOffsetX.load(), ry + heldOffsetY.load()};
        if (ClientToScreen(w, &c)) SetCursorPos(c.x, c.y);
    }
    heldOffsetObject = 0;
}
static void rmbEvent(fp::RmbEvent e, bool eligible, bool* swallow = nullptr) {
    rmbState.on = InterlockedCompareExchange(&rmbFree, 0, 0) != 0; // the game thread may have ended it
    bool changed = false;
    const bool eaten = fp::rmbStep(rmbState, rmbMode.load(), e, eligible, changed);
    if (changed) {
        keepHeldOffset(rmbState.on);
        InterlockedExchange(&rmbFree, rmbState.on ? 1 : 0);
        releaseInput();
    } // releaseInput: no view jump on the way back
    if (swallow) *swallow = eaten;
}
