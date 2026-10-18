// Actual combat/input owners against allocated storage in this process only.
// The native callbacks below are contract spies, not a replay of game physics.
// No game, hook installation, desktop input, or real user INI is involved.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool fixtureKeys[256]{};
static unsigned cursorWrites = 0, cursorReads = 0, cursorClips = 0;
static HWND fixtureForeground() {
    return (HWND)1;
}
static DWORD fixtureWindowPid(HWND, DWORD* pid) {
    *pid = GetCurrentProcessId();
    return GetCurrentThreadId();
}
static SHORT WINAPI fixtureKeyState(int key) {
    return key > 0 && key < 256 && fixtureKeys[key] ? SHORT(-32768) : SHORT(0);
}
static BOOL WINAPI fixtureSetCursor(int, int) {
    ++cursorWrites;
    return TRUE;
}
static BOOL WINAPI fixtureGetCursor(LPPOINT point) {
    ++cursorReads;
    *point = {0, 0};
    return TRUE;
}
static BOOL WINAPI fixtureClipCursor(const RECT*) {
    ++cursorClips;
    return TRUE;
}
#define GetForegroundWindow fixtureForeground
#define GetWindowThreadProcessId fixtureWindowPid
#define GetAsyncKeyState fixtureKeyState
#define SetCursorPos fixtureSetCursor
#define GetCursorPos fixtureGetCursor
#define ClipCursor fixtureClipCursor
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#undef SetCursorPos
#undef GetCursorPos
#undef ClipCursor
#include "test_controlled_player.h"

static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
}
template <class T> static T& at(uintptr_t object, size_t offset) {
    return *(T*)(object + offset);
}
static bool closeFloat(float a, float b) {
    return std::isfinite(a) && std::fabs(a - b) < 1e-5f;
}
static bool vectorNear(const float* a, const float* b) {
    return closeFloat(a[0], b[0]) && closeFloat(a[1], b[1]) && closeFloat(a[2], b[2]);
}
// Fields used only by the native-call spies. Archived 163880 reads eligibility;
// 162E80/179EE0 withdraw active defense and reset an accepted motion's progress.
namespace nativeFixture {
constexpr size_t eligible = 0x12f4, capability = 0x12f5, attackParts = 0x970;
}
static uintptr_t actor, bh, head, otherActor, otherBh, otherHead, scene, otherScene, drag, gui;
static uintptr_t hands[2], fixtureParents[2];
static unsigned polledKeys = 0, polledButtons = 0, pollCalls = 0;
static float polledStick[3]{}, polledHeading = -.75f;
static unsigned char polledIntent = 1;
static unsigned reactiveCalls = 0, attackCalls = 0, selectorCalls = 0, cleanupCalls = 0;
static uintptr_t seenActor = 0, seenIncoming = 0;
static int attackIncoming = -99;
static unsigned char attackActive = 0xff;
static bool nativeUsesEligibility = false;
static char callOrder[32]{};
static unsigned orderSize = 0;
static void record(char value) {
    if (orderSize + 1 < sizeof callOrder) {
        callOrder[orderSize++] = value;
        callOrder[orderSize] = 0;
    }
}
static void nativePoll() {
    ++pollCalls;
    record('P');
    *(unsigned*)gameAt(fp::sig::keyWord) = polledKeys;
    *(unsigned*)gameAt(fp::sig::buttonWord) = polledButtons;
    std::memcpy((void*)gameAt(fp::sig::analogStick), polledStick, sizeof polledStick);
    const uintptr_t root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
    const uintptr_t behaviorObject = root ? at<uintptr_t>(root, field::character::behaviour) : 0;
    if (behaviorObject) {
        at<unsigned char>(behaviorObject, field::behaviour::moveIntent) = polledIntent;
        // Native polling is allowed to overwrite an older presentation. The
        // classifier test below must still see the current Aim after this call.
        at<float>(behaviorObject, field::behaviour::aimHeading) = polledHeading;
    }
}
static void nativeReactive(uintptr_t root) {
    ++reactiveCalls;
    seenActor = root;
    record('R');
    if (nativeUsesEligibility && at<unsigned char>(root, nativeFixture::eligible) &&
        at<unsigned char>(root, nativeFixture::capability))
        at<unsigned char>(root, field::character::defenseActive) = 1;
}
static void nativeAttack(uintptr_t root) {
    ++attackCalls;
    seenActor = root;
    attackIncoming = at<int>(root, field::character::defenseIncomingMotion);
    attackActive = at<unsigned char>(root, field::character::defenseActive);
    record('A');
}
static uint64_t nativeSelect(uintptr_t root, uintptr_t incoming) {
    ++selectorCalls;
    seenActor = root;
    seenIncoming = incoming;
    record('S');
    if (nativeUsesEligibility && (!at<unsigned char>(root, nativeFixture::eligible) ||
                                  !at<unsigned char>(root, nativeFixture::capability)))
        return 0xffffffffu;
    return 0x16u;
}
enum class CleanupEffect { nativePosture, replaceHead, selectOtherActor };
static CleanupEffect cleanupEffect = CleanupEffect::nativePosture;
static void nativeCleanup(uintptr_t root) {
    ++cleanupCalls;
    seenActor = root;
    record('C');
    const bool wasActive = at<unsigned char>(root, field::character::defenseActive) != 0;
    at<unsigned char>(root, field::character::defenseActive) = 0;
    at<int>(root, field::character::defenseIncomingMotion) = -1;
    for (unsigned i = 0; i < 2; ++i) {
        const uintptr_t hand = at<uintptr_t>(root, field::character::handParts + i * sizeof(uintptr_t));
        if (!hand) continue;
        at<unsigned>(hand, field::bodyPart::flags) &= ~8u;
        const uintptr_t parent = at<uintptr_t>(hand, field::bodyPart::parentPart);
        if (parent) at<unsigned>(parent, field::bodyPart::flags) &= ~8u;
    }
    if (cleanupEffect == CleanupEffect::replaceHead)
        at<uintptr_t>(root, field::character::headPart) = otherHead;
    else if (cleanupEffect == CleanupEffect::selectOtherActor)
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = otherActor;
    else if (wasActive && !(at<unsigned>(root, field::character::stateFlags) & 0x80u)) {
        at<int>(root, field::character::motion) = 3;
        at<float>(root, field::character::motionProgress) = 0.f;
        at<unsigned>(root, nativeFixture::attackParts) = 0;
    }
}
static unsigned headingCalls = 0, filterPushes = 0, filterUpdates = 0, fallbackCursorCalls = 0;
static uintptr_t seenFilter = 0;
static float filterDirection[3]{};
static float nativeHeading(float* direction) {
    ++headingCalls;
    record('H');
    return std::atan2(direction[0], direction[2]);
}
static void nativeFilterPush(uintptr_t filter, float* direction) {
    ++filterPushes;
    seenFilter = filter;
    std::memcpy(filterDirection, direction, sizeof filterDirection);
    record('F');
}
static void nativeFilterUpdate(uintptr_t filter) {
    ++filterUpdates;
    seenFilter = filter;
    record('U');
}
static void nativeCursor(uintptr_t) {
    ++fallbackCursorCalls;
}
enum class BehaviorCall { cursor, classifyOnly, replaceHeading, fault };
static BehaviorCall behaviorCall = BehaviorCall::cursor;
static unsigned classifiedButtons = 0;
static float classifiedHeading = 0.f;
static bool classifiedOpposite = false;
static void nativeBehavior(uintptr_t self) {
    input();
    const uintptr_t root = at<uintptr_t>(self, field::behaviour::character);
    classifiedButtons = *(unsigned*)gameAt(fp::sig::buttonWord);
    classifiedHeading = at<float>(self, field::behaviour::aimHeading);
    classifiedOpposite = fp::oppositeSide(classifiedHeading, at<float>(root, field::character::bodyHeading),
                                          at<int>(root, field::character::gripFlags) != 0);
    record('B');
    if (behaviorCall == BehaviorCall::cursor)
        cursorAim(self);
    else if (behaviorCall == BehaviorCall::replaceHeading)
        at<float>(self, field::behaviour::aimHeading) = 1.234f;
    else if (behaviorCall == BehaviorCall::fault)
        RaiseException(0xe0420020, 0, 0, nullptr);
}
static bool behaviorFaults(uintptr_t self) {
    __try {
        behavior(self);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
static void clearCalls() {
    reactiveCalls = attackCalls = selectorCalls = cleanupCalls = pollCalls = 0;
    headingCalls = filterPushes = filterUpdates = fallbackCursorCalls = 0;
    seenActor = seenIncoming = seenFilter = 0;
    attackIncoming = -99;
    attackActive = 0xff;
    orderSize = 0;
    callOrder[0] = 0;
    cursorWrites = cursorReads = cursorClips = 0;
}
static void identityPose(uintptr_t part) {
    float* pose = (float*)(part + field::bodyPart::pose);
    for (int i = 0; i < 12; ++i) pose[i] = i == 0 || i == 4 || i == 8 ? 1.f : 0.f;
}
static void physicalButtons(unsigned buttons) {
    fixtureKeys[VK_LBUTTON] = (buttons & 1u) != 0;
    fixtureKeys[VK_RBUTTON] = (buttons & 2u) != 0;
}
static void sample(unsigned buttons) {
    physicalButtons(buttons);
    polledButtons = buttons;
    input();
}
static void prime() {
    sample(0); // The actual owner sees release before the next fresh press.
    clearCalls();
}
static void resetFixture(int view = 0, bool fpManual = true, bool tpManual = true) {
    std::memset((void*)(base + 0x600000), 0, 0x180000);
    std::memset(fixtureKeys, 0, sizeof fixtureKeys);
    fixture::resetInput(view);
    *(uintptr_t*)gameAt(fp::sig::currentScene) = scene;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = drag;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = gui;
    *(int*)gameAt(fp::sig::uiOpenCount) = 0;
    *(unsigned*)gameAt(fp::sig::gameClock) = 100;
    for (unsigned i = 0; i < 2; ++i) {
        const uintptr_t root = i ? otherActor : actor, behaviorObject = i ? otherBh : bh;
        const fixture::ControlledPlayer player{root, behaviorObject, i ? otherHead : head,
                                                base + (i ? 0x700000 : 0x6f0000)};
        player.connect(i == 0);
        at<unsigned char>(root, field::character::combatStance) = 1;
        at<int>(root, field::character::defenseIncomingMotion) = -1;
        at<int>(root, field::character::motion) = 7;
        at<float>(root, field::character::motionProgress) = .37f;
        at<unsigned>(root, nativeFixture::attackParts) = 2;
        at<unsigned char>(root, nativeFixture::eligible) = 1;
        at<unsigned char>(root, nativeFixture::capability) = 1;
    }
    for (unsigned i = 0; i < 2; ++i) {
        at<uintptr_t>(actor, field::character::handParts + i * sizeof(uintptr_t)) = hands[i];
        at<uintptr_t>(hands[i], field::bodyPart::parentPart) = fixtureParents[i];
        at<unsigned>(hands[i], field::bodyPart::flags) = 0x80000008u;
        at<unsigned>(fixtureParents[i], field::bodyPart::flags) = 0x40000008u;
    }
    identityPose(head);
    identityPose(otherHead);
    fixture::bindings({'W', 'S', 'A', 'D', 'C', VK_SHIFT, 'V', 0, 0, 'E'});
    fpManualGuard = fpManual;
    tpManualGuard = tpManual;
    manualDefenseReady = true;
    windowsFreeze = true;
    attackCue = false;
    attackSideKey = 0;
    aim.root = actor;
    aim.head = head;
    aim.valid = true;
    aim.yaw = -.4f;
    aim.pitch = .1f;
    aim.rayValid = true;
    aim.rayYaw = .45f;
    aim.point[2] = 1000.f;
    polledKeys = polledButtons = 0;
    polledStick[0] = polledStick[1] = polledStick[2] = 0.f;
    polledHeading = -.75f;
    polledIntent = 1;
    inputClock = *(unsigned*)gameAt(fp::sig::gameClock);
    nativeUsesEligibility = false;
    cleanupEffect = CleanupEffect::nativePosture;
    behaviorCall = BehaviorCall::cursor;
    originalInput = nativePoll;
    originalBehavior = nativeBehavior;
    originalCursor = nativeCursor;
    originalDefenseReactive = nativeReactive;
    originalDefenseAttackUpdate = nativeAttack;
    originalDefenseMotionSelect = nativeSelect;
    defenseCleanup = nativeCleanup;
    check(controls(bh), "shared player fixture reaches the production input adapter");
    prime();
}
static uint64_t invokeDefense(uintptr_t root) {
    defenseReactive(root);
    defenseAttackUpdate(root);
    return defenseMotionSelect(root, otherActor);
}
enum class PollInterruption { none, gameplay, policy };
static PollInterruption pollInterruption = PollInterruption::none;
static SHORT WINAPI interruptedKeyState(int key) {
    // RMB is sampled by combatInput after beginSample captured its owner. This
    // makes the publication race deterministic without a game or another thread.
    if (key == VK_RBUTTON && pollInterruption != PollInterruption::none) {
        if (pollInterruption == PollInterruption::gameplay)
            inputOwner.releaseGameplay();
        else
            inputOwner.policyChanged();
        pollInterruption = PollInterruption::none;
    }
    return fixtureKeyState(key);
}
static void publicationTests() {
    for (const auto change : {PollInterruption::gameplay, PollInterruption::policy}) {
        resetFixture();
        const uint32_t generation = inputOwner.generation();
        const unsigned policy = inputOwner.policyRevision();
        pollInterruption = change;
        keyState = interruptedKeyState;
        sample(2);
        keyState = fixtureKeyState;
        check(pollInterruption == PollInterruption::none && guardRequest.stamp.generation == generation &&
                  guardRequest.stamp.policyRevision == policy &&
                  defensePermission(actor) == DefensePermission::denied,
              change == PollInterruption::gameplay
                  ? "a late gameplay handoff cannot retag an older guard sample with its new generation"
                  : "a late policy change cannot retag an older guard sample with its new revision");
        sample(2);
        check(defensePermission(actor) == DefensePermission::denied,
              "the next owner requires release after a change during guard publication");
        sample(0);
        sample(2);
        check(defensePermission(actor) == DefensePermission::held,
              "a fresh press acquires guard normally after the interrupted sample");
    }
}
static void profileTests() {
    for (int fpMode = 0; fpMode < 2; ++fpMode)
        for (int tpMode = 0; tpMode < 2; ++tpMode)
            for (int view = 0; view < 2; ++view) {
                resetFixture(view, fpMode != 0, tpMode != 0);
                const bool manual = (view ? tpMode : fpMode) != 0;
                const uint64_t chosen = invokeDefense(actor);
                char label[144];
                std::snprintf(label, sizeof label, "%s uses its own %s setting (FP=%d, TP=%d)",
                              view ? "TP" : "FP", manual ? "Manual" : "Automatic", fpMode, tpMode);
                check(reactiveCalls == (manual ? 0u : 1u) && attackCalls == 1 &&
                          selectorCalls == (manual ? 0u : 1u) && !cleanupCalls &&
                          chosen == (manual ? 0xffffffffu : 0x16u),
                      label);
                clearCalls();
                fixtureKeys['W'] = true;
                polledKeys = 0xb1; // forward, duck, run and alternate attack stay native semantic bits
                polledStick[0] = .2f;
                polledStick[2] = -.8f;
                sample(2);
                check(*(unsigned*)gameAt(fp::sig::buttonWord) == 0 &&
                          at<unsigned char>(bh, field::behaviour::moveIntent) == 0 &&
                          *(unsigned*)gameAt(fp::sig::keyWord) == 0xb1 &&
                          vectorNear((float*)gameAt(fp::sig::analogStick), polledStick),
                      "ordinary close-view RMB removes cursor walking without erasing other native requests");
                clearCalls();
                check(invokeDefense(actor) == 0x16u && reactiveCalls == 1 && attackCalls == 1 &&
                          selectorCalls == 1 && !cleanupCalls && seenIncoming == otherActor,
                      "a fresh held guard authorizes native owners; Automatic keeps forwarding");
            }
}
static void releaseTests() {
    for (int view : {0, 1}) {
        resetFixture(view);
        sample(2);
        check(defensePermission(actor) == DefensePermission::held,
              "fresh physical RMB publishes a held guard");
        fixtureKeys[VK_RBUTTON] = false;
        check(defensePermission(actor) == DefensePermission::denied,
              "physical guard release revokes permission even before another input poll");
        sample(0);
        inputOwner.buttonDown(2);
        input();
        check(defensePermission(actor) == DefensePermission::denied &&
                  !(*(unsigned*)gameAt(fp::sig::buttonWord) & 3u),
              "a released RMB down/up tap is never queued for later native defense");
        sample(3);
        check(defensePermission(actor) == DefensePermission::held &&
                  !(*(unsigned*)gameAt(fp::sig::buttonWord) & 3u),
              "held guard suppresses a simultaneous new ordinary attack");
        sample(1);
        sample(1);
        check(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1u) &&
                  defensePermission(actor) == DefensePermission::denied,
              "releasing RMB first cannot replay the consumed LMB press");
        sample(0);
        sample(1);
        check((*(unsigned*)gameAt(fp::sig::buttonWord) & 1u) != 0,
              "after physical LMB release a fresh ordinary attack remains available");
        at<unsigned>(bh, field::behaviour::pressFlags) = 1;
        at<unsigned>(bh, field::behaviour::swingState) = 0x31;
        at<unsigned>(actor, field::character::swingBits) = 0x26;
        sample(3);
        check(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 1u) &&
                  at<unsigned>(bh, field::behaviour::pressFlags) == 1 &&
                  at<unsigned>(bh, field::behaviour::swingState) == 0x31 &&
                  at<unsigned>(actor, field::character::swingBits) == 0x26 &&
                  at<int>(actor, field::character::motion) == 7 &&
                  closeFloat(at<float>(actor, field::character::motionProgress), .37f),
              "guard withdraws continued attack input without fabricating cancellation or recovery");

        sample(0);
        sample(2);
        inputOwner.releaseGameplay();
        check(defensePermission(actor) == DefensePermission::denied,
              "a released input generation cannot retain old guard permission");
        sample(2);
        check(defensePermission(actor) == DefensePermission::denied,
              "a held RMB crossing an input handoff waits for release");
        sample(0);
        sample(2);
        *(uintptr_t*)gameAt(fp::sig::currentScene) = otherScene;
        check(defensePermission(actor) == DefensePermission::denied,
              "guard publication is rejected by a new scene before its next input sample");
        sample(2);
        check(defensePermission(actor) == DefensePermission::denied,
              "scene handoff does not promote an already held RMB into a new guard");
        sample(0);
        sample(2);
        at<uintptr_t>(actor, field::character::headPart) = otherHead;
        check(defensePermission(actor) == DefensePermission::denied,
              "replacement head invalidates the previous guard publication");

        resetFixture(view);
        sample(2);
        viewMode = 1 - view;
        check(defensePermission(actor) == DefensePermission::denied,
              "switching FP and TP invalidates the old profile's guard before polling");
        sample(2);
        check(defensePermission(actor) == DefensePermission::denied,
              "the destination profile cannot adopt RMB until its original press releases");
        sample(0);
        sample(2);
        inputOwner.policyChanged();
        check(defensePermission(actor) == DefensePermission::denied,
              "a changed control policy rejects a previously published held guard");
        sample(2);
        check(defensePermission(actor) == DefensePermission::denied,
              "the new control policy requires a new guard press");
        sample(0);
        sample(2);
        cursorFree = 1;
        check(defensePermission(actor) == DefensePermission::denied,
              "free-pointer ownership revokes guard permission immediately");
        sample(2);
        check(!(*(unsigned*)gameAt(fp::sig::buttonWord) & 3u) &&
                  defensePermission(actor) == DefensePermission::denied,
              "input freeze cannot retain a gameplay guard from the previous owner");
    }
}
enum class Exemption {
    exploration,
    overhead,
    stopped,
    inactiveMod,
    cameraUnavailable,
    dead,
    inactiveActor,
    specialState,
    callback,
    interactionLatch,
    blockedPress,
    interactKey,
    heldObject,
    door,
    hinge,
    spring,
    bodyDrag,
    otherDrag,
    guiDrag
};
static void exemptionTests() {
    const Exemption cases[] = {Exemption::exploration,
                               Exemption::overhead,
                               Exemption::stopped,
                               Exemption::inactiveMod,
                               Exemption::cameraUnavailable,
                               Exemption::dead,
                               Exemption::inactiveActor,
                               Exemption::specialState,
                               Exemption::callback,
                               Exemption::interactionLatch,
                               Exemption::blockedPress,
                               Exemption::interactKey,
                               Exemption::heldObject,
                               Exemption::door,
                               Exemption::hinge,
                               Exemption::spring,
                               Exemption::bodyDrag,
                               Exemption::otherDrag,
                               Exemption::guiDrag};
    const char* labels[] = {"exploration",
                            "overhead blend",
                            "stopping",
                            "inactive mod",
                            "unready camera",
                            "dead player",
                            "inactive behavior",
                            "native special state 0x20",
                            "native action callback +1230",
                            "native interaction latch +96F",
                            "native press-blocked owner +96E",
                            "held native Interact binding",
                            "held-object placement",
                            "door drag",
                            "hinge drag",
                            "spring drag",
                            "body drag",
                            "other native drag",
                            "GUI cursor-carried item"};
    for (int view : {0, 1})
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            resetFixture(view);
            switch (cases[i]) {
            case Exemption::exploration:
                at<unsigned char>(actor, field::character::combatStance) = 0;
                break;
            case Exemption::overhead:
                viewMode = 2;
                break;
            case Exemption::stopped:
                stopping = 1;
                break;
            case Exemption::inactiveMod:
                active = 0;
                break;
            case Exemption::cameraUnavailable:
                cameraReady = 0;
                break;
            case Exemption::dead:
                at<unsigned>(actor, field::character::lifeFlags) = 0x100;
                break;
            case Exemption::inactiveActor:
                at<unsigned char>(bh, field::behaviour::inactive) = 1;
                break;
            case Exemption::specialState:
                at<unsigned>(actor, field::character::stateFlags) = 0x20;
                break;
            case Exemption::callback:
                at<uintptr_t>(actor, field::character::actionCallback) = otherActor;
                break;
            case Exemption::interactionLatch:
                at<unsigned char>(bh, field::behaviour::interactionLatch) = 1;
                break;
            case Exemption::blockedPress:
                at<unsigned char>(bh, field::behaviour::pressBlocked) = 1;
                break;
            case Exemption::interactKey:
                fixtureKeys['E'] = true;
                break;
            case Exemption::heldObject:
                at<unsigned>(drag, field::drag::flags) = 0x2000;
                break;
            case Exemption::door:
                at<unsigned>(drag, field::drag::flags) = 0x4000;
                break;
            case Exemption::hinge:
                at<unsigned>(drag, field::drag::flags) = 0x10000;
                break;
            case Exemption::spring:
                at<unsigned>(drag, field::drag::flags) = 0x8000;
                break;
            case Exemption::bodyDrag:
                at<unsigned>(drag, field::drag::flags) = 0x80000;
                break;
            case Exemption::otherDrag:
                at<unsigned>(drag, field::drag::flags) = 1;
                break;
            case Exemption::guiDrag:
                at<unsigned>(gui, field::gui::dragFormat) = 7;
                break;
            }
            prime(); // Begin the native interaction as a separate physical press.
            sample(3);
            clearCalls();
            const uint64_t selected = invokeDefense(actor);
            char label[180];
            std::snprintf(label, sizeof label,
                          "%s %s retains native buttons, movement intent and all defense owners",
                          view ? "TP" : "FP", labels[i]);
            check(*(unsigned*)gameAt(fp::sig::buttonWord) == 3 &&
                      at<unsigned char>(bh, field::behaviour::moveIntent) == 1 &&
                      defensePermission(actor) == DefensePermission::native && selected == 0x16u &&
                      reactiveCalls == 1 && attackCalls == 1 && selectorCalls == 1 && !cleanupCalls,
                  label);
        }
    resetFixture();
    check(invokeDefense(otherActor) == 0x16u && reactiveCalls == 1 && attackCalls == 1 &&
              selectorCalls == 1 && !cleanupCalls && seenActor == otherActor,
          "NPC callbacks remain native despite the controlled player's requested Manual policy");
    resetFixture();
    at<unsigned>(gui, field::gui::dragFormat + sizeof(unsigned)) = 0xfeedfaceu;
    prime();
    sample(2);
    check(defensePermission(actor) == DefensePermission::held &&
              at<unsigned>(gui, field::gui::dragFormat + sizeof(unsigned)) == 0xfeedfaceu,
          "32-bit GUI drag format does not read its nonzero neighboring dword as an interaction");
}
static void nativeStateTests() {
    resetFixture();
    sample(2);
    nativeUsesEligibility = true;
    for (unsigned eligible = 0; eligible < 2; ++eligible)
        for (unsigned capability = 0; capability < 2; ++capability) {
            at<unsigned char>(actor, nativeFixture::eligible) = (unsigned char)eligible;
            at<unsigned char>(actor, nativeFixture::capability) = (unsigned char)capability;
            at<unsigned char>(actor, field::character::defenseActive) = 0;
            clearCalls();
            defenseReactive(actor);
            const uint64_t selected = defenseMotionSelect(actor, otherActor);
            const bool nativeAccepts = eligible && capability;
            check(reactiveCalls == 1 && selectorCalls == 1 &&
                      selected == (nativeAccepts ? 0x16u : 0xffffffffu) &&
                      at<unsigned char>(actor, field::character::defenseActive) == (nativeAccepts ? 1 : 0) &&
                      at<unsigned char>(actor, nativeFixture::eligible) == eligible &&
                      at<unsigned char>(actor, nativeFixture::capability) == capability,
                  "held permission delegates native eligibility/capability without forcing either field");
        }
    for (bool manual : {false, true}) {
        resetFixture(0, manual, manual);
        if (manual) sample(2);
        at<unsigned>(actor, field::character::stateFlags) = 0x40284;
        at<unsigned>(actor, field::character::lifeFlags) = 0x20;
        at<unsigned>(actor, field::character::swingBits) = 0x26;
        at<unsigned char>(actor, field::character::defenseActive) = 1;
        at<int>(actor, field::character::defenseIncomingMotion) = 9;
        unsigned char before[0x1400];
        std::memcpy(before, (void*)actor, sizeof before);
        clearCalls();
        check(invokeDefense(actor) == 0x16u && !cleanupCalls &&
                  !std::memcmp(before, (void*)actor, sizeof before) &&
                  at<unsigned>(hands[0], field::bodyPart::flags) == 0x80000008u &&
                  at<unsigned>(hands[1], field::bodyPart::flags) == 0x80000008u &&
                  at<unsigned>(fixtureParents[0], field::bodyPart::flags) == 0x40000008u &&
                  at<unsigned>(fixtureParents[1], field::bodyPart::flags) == 0x40000008u,
              "Automatic/permitted hooks leave native contact, capability, accepted-motion and part flags "
              "untouched");
    }

    resetFixture();
    defenseAttackUpdate(actor);
    check(attackCalls == 1 && !cleanupCalls && attackIncoming == -1,
          "denied guard still runs ordinary offensive execution when no defense was retained");
    resetFixture();
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    defenseAttackUpdate(actor);
    check(cleanupCalls == 1 && attackCalls == 1 && !std::strcmp(callOrder, "CA") && attackIncoming == -1 &&
              attackActive == 0 && at<int>(actor, field::character::motion) == 7 &&
              closeFloat(at<float>(actor, field::character::motionProgress), .37f) &&
              at<unsigned>(actor, nativeFixture::attackParts) == 2 &&
              at<unsigned>(hands[0], field::bodyPart::flags) == 0x80000000u &&
              at<unsigned>(fixtureParents[1], field::bodyPart::flags) == 0x40000000u,
          "F5C-only withdrawal calls native cleanup before preserving the complete attack update");
    resetFixture();
    at<unsigned char>(actor, field::character::defenseActive) = 1;
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    defenseAttackUpdate(actor);
    check(cleanupCalls == 1 && !attackCalls && at<int>(actor, field::character::motion) == 3 &&
              at<float>(actor, field::character::motionProgress) == 0.f &&
              !at<unsigned>(actor, nativeFixture::attackParts),
          "native cleanup changing accepted motion prevents a stale attack-body call");
    resetFixture();
    at<unsigned char>(actor, field::character::defenseActive) = 1;
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    at<unsigned>(actor, field::character::stateFlags) = 0x80;
    defenseAttackUpdate(actor);
    check(cleanupCalls == 1 && attackCalls == 1 && attackIncoming == -1 && attackActive == 0 &&
              at<unsigned>(actor, field::character::stateFlags) == 0x80 &&
              at<int>(actor, field::character::motion) == 7,
          "native cleanup that preserves motion under state 0x80 keeps offensive execution");
    resetFixture();
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    cleanupEffect = CleanupEffect::replaceHead;
    defenseAttackUpdate(actor);
    check(cleanupCalls == 1 && !attackCalls && at<uintptr_t>(actor, field::character::headPart) == otherHead,
          "head replacement during native cleanup prevents execution against the previous body");
    resetFixture();
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    cleanupEffect = CleanupEffect::selectOtherActor;
    defenseAttackUpdate(actor);
    check(cleanupCalls == 1 && attackCalls == 1 && seenActor == actor,
          "changing player selection alone does not discard a still-valid old actor's native attack");
    resetFixture();
    at<unsigned char>(actor, field::character::defenseActive) = 1;
    at<int>(actor, field::character::defenseIncomingMotion) = 9;
    defenseReactive(actor);
    defenseReactive(actor);
    check(cleanupCalls == 1 && !reactiveCalls && !at<unsigned char>(actor, field::character::defenseActive) &&
              at<int>(actor, field::character::defenseIncomingMotion) == -1,
          "reactive withdrawal uses native cleanup once and does not repeatedly reset settled posture");
}
static void headingTests() {
    for (int view : {0, 1}) {
        resetFixture(view, false, false);
        aim.yaw = -.8f;
        aim.rayYaw = .4f;
        polledHeading = -.6f;
        physicalButtons(1);
        polledButtons = 1;
        behavior(bh);
        const float direction[3] = {std::sin(.4f), 0, std::cos(.4f)};
        check(classifiedButtons == 1 && closeFloat(classifiedHeading, .4f) && classifiedOpposite &&
                  !fp::oppositeSide(polledHeading, 0.f, false),
              "native classification reads the current ray heading after its input poll overwrites the old "
              "heading");
        check(pollCalls == 1 && headingCalls == 1 && filterPushes == 1 && filterUpdates == 1 &&
                  !fallbackCursorCalls && !std::strcmp(callOrder, "PBHFU") &&
                  seenFilter == bh + field::behaviour::aimFilter && vectorNear(filterDirection, direction),
              "actual behavior advances the native cursor filter once in its original call sequence");
        check(!cursorWrites && !cursorReads && !cursorClips,
              "the behavior fixture's shared clock avoids desktop cursor work");
        if (!view) {
            const float gaze[3] = {std::sin(aim.yaw) * std::cos(aim.pitch), std::sin(aim.pitch),
                                   std::cos(aim.yaw) * std::cos(aim.pitch)};
            check(vectorNear((float*)(actor + field::character::gaze), gaze),
                  "FP head gaze remains the physical look direction while weapon aim follows the cursor ray");
        }
        sample(0);
        clearCalls();
        attackSideKey = 'Q';
        fixtureKeys['Q'] = true;
        polledButtons = 1;
        physicalButtons(1);
        behavior(bh);
        check(classifiedButtons == 1 && !classifiedOpposite && closeFloat(classifiedHeading, 0.f) &&
                  closeFloat(at<float>(bh, field::behaviour::aimHeading), .4f) && headingCalls == 1 &&
                  filterPushes == 1 && filterUpdates == 1,
              "held side modifier flips only the new-press classifier and preserves one native aim-filter "
              "update");
        sample(0);
        at<unsigned>(bh, field::behaviour::pressFlags) = 1;
        clearCalls();
        polledButtons = 1;
        physicalButtons(1);
        behavior(bh);
        check(classifiedOpposite && closeFloat(classifiedHeading, .4f),
              "an accepted held press cannot be repeatedly reclassified by the side modifier");
    }
    for (BehaviorCall call :
         {BehaviorCall::classifyOnly, BehaviorCall::replaceHeading, BehaviorCall::fault}) {
        resetFixture(0, false, false);
        attackSideKey = 'Q';
        fixtureKeys['Q'] = true;
        physicalButtons(1);
        polledButtons = 1;
        behaviorCall = call;
        const AttackHeadingPresentation outer{otherBh, -.2f, -.1f, true};
        attackHeadingPresentation = outer;
        const bool faulted = call == BehaviorCall::fault ? behaviorFaults(bh) : (behavior(bh), false);
        const float expected = call == BehaviorCall::replaceHeading ? 1.234f : aim.rayYaw;
        check(faulted == (call == BehaviorCall::fault) &&
                  closeFloat(at<float>(bh, field::behaviour::aimHeading), expected) &&
                  attackHeadingPresentation.behavior == outer.behavior &&
                  attackHeadingPresentation.heading == outer.heading && attackHeadingPresentation.borrowed,
              "temporary attack heading restores its nested scope and preserves native writes, including SEH "
              "exit");
    }
    for (int wide : {0, 1}) {
        resetFixture(0, false, false);
        at<int>(actor, field::character::gripFlags) = wide;
        aim.rayYaw = .31f; // Between native 15-degree and 20-degree side thresholds.
        attackSideKey = 'Q';
        fixtureKeys['Q'] = true;
        physicalButtons(1);
        polledButtons = 1;
        behavior(bh);
        check(classifiedOpposite == (wide != 0) &&
                  closeFloat(at<float>(bh, field::behaviour::aimHeading), .31f) && filterUpdates == 1,
              "side inversion preserves the native grip-dependent 15/20-degree classification boundary");
    }
}
struct DefenseSettingsSurface {
    panel::PanelView view;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    explicit DefenseSettingsSurface(const wchar_t* directory) {
        // Missing art in our own directory deliberately selects the GDI path.
        // Do not discover or read installed resources or Exanima.ini.
        uiart::gameFolderOverride() = directory;
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 1000;
        info.bmiHeader.biHeight = -640;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        if (dc) bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap) previous = SelectObject(dc, bitmap);
        view.page.kind = panel::Page::item;
        view.page.settingsPage = true;
        view.page.title = "Settings";
        view.settingsGroup = 2; // consolidated Aim & combat page
        check(dc && bitmap, "owned offscreen settings surface is available");
    }
    ~DefenseSettingsSurface() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        uiart::gameFolderOverride().clear();
    }
    bool click(panel::SettingsWindow& settings, int wantedRow) {
        if (!dc || !bitmap) return false;
        view.settings = currentSettings();
        view.settingsScroll = 0;
        unsigned found = 0;
        panel::PanelView::SettingControl button{};
        for (;;) {
            view.paint(dc, 1000, 640);
            for (const auto& candidate : view.settingControls)
                if (candidate.row == wantedRow) {
                    button = candidate;
                    ++found;
                }
            if (found || view.settingsScroll == view.settingsScrollMax()) break;
            view.scrollSettings(1);
        }
        check(found == 1 && button.r.left >= view.layout.content.left && button.r.right <= 1000 &&
                  button.r.top >= 0 && button.r.bottom <= 640,
              "each defense choice has one visible button in the rendered settings page");
        if (found != 1) return false;
        int delta = -99;
        const POINT center{(button.r.left + button.r.right) / 2, (button.r.top + button.r.bottom) / 2};
        const int row = view.settingAt(center, delta);
        check(row == wantedRow && delta == 0,
              "rendered defense Switch hit-testing supplies its real zero-delta action");
        return row == wantedRow && settings.applyControl(row, delta);
    }
    void checkWidths() {
        if (!dc || !bitmap) return;
        const HGDIOBJ oldFont = SelectObject(dc, view.fonts.body);
        for (const wchar_t* previousLabel :
             {L"Manual (hold right mouse)", L"Automatic (Manual unavailable)"}) {
            SIZE extent{};
            if (GetTextExtentPoint32W(dc, previousLabel, int(std::wcslen(previousLabel)), &extent))
                std::printf("Previous defense label width: %ld/200 pixels (%ls)\n", extent.cx, previousLabel);
        }
        for (int row : {panel::setting::fpDefense, panel::setting::tpDefense})
            for (int mode : {0, 1})
                for (bool available : {false, true}) {
                    panel::ModSettings shown;
                    shown.fpDefense = shown.tpDefense = mode;
                    shown.manualDefenseAvailable = available;
                    const std::wstring value = shown.value(row);
                    SIZE extent{};
                    check(GetTextExtentPoint32W(dc, value.c_str(), int(value.size()), &extent) &&
                              extent.cx <= 200 && extent.cy <= 30,
                          "every defense value fits its actual 200-by-30-pixel fallback-font cell");
                }
        SelectObject(dc, oldFont);
    }
};
static void settingsTests() {
    resetFixture(0, false, false);
    wchar_t temporary[MAX_PATH]{}, directory[MAX_PATH]{};
    const bool madeName =
        GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"ecg", 0, directory);
    check(madeName, "owned temporary settings fixture can be created");
    if (!madeName) return;
    DeleteFileW(directory);
    if (!CreateDirectoryW(directory, nullptr)) {
        check(false, "owned temporary settings directory can be created");
        return;
    }
    const std::wstring moduleDir = std::wstring(directory) + L"\\module";
    const std::wstring userDir = std::wstring(directory) + L"\\user";
    const std::wstring source = moduleDir + L"\\first-person.ini";
    CreateDirectoryW(moduleDir.c_str(), nullptr);
    check(WritePrivateProfileStringW(L"Input", L"SensitivityPercent", L"130", source.c_str()) &&
              WritePrivateProfileStringW(L"ThirdPerson", L"HideBlockers", L"0", source.c_str()),
          "an old-format INI without defense rows is written only in the fixture directory");
    const std::wstring migrated = userSettingsFile(source, userDir);
    panel::ModSettings got = readSettings(migrated.c_str());
    check(
        migrated == userDir + L"\\first-person.ini" && got.fpDefense == 0 && got.tpDefense == 0 &&
            got.sensitivity == 130 && got.tpBlockers == fp::blockersPush &&
            GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES,
        "old settings migrate with Automatic in both views while preserving prior values and the source INI");
    WritePrivateProfileStringW(L"FirstPerson", L"Defense", L"MaNuAl", migrated.c_str());
    WritePrivateProfileStringW(L"ThirdPerson", L"Defense", L"automatic", migrated.c_str());
    got = readSettings(migrated.c_str());
    check(got.fpDefense == 1 && got.tpDefense == 0,
          "case-insensitive Manual parses independently for FP with TP Automatic");
    WritePrivateProfileStringW(L"FirstPerson", L"Defense", L"manual-extra", migrated.c_str());
    WritePrivateProfileStringW(L"ThirdPerson", L"Defense", L"2", migrated.c_str());
    got = readSettings(migrated.c_str());
    check(got.fpDefense == 0 && got.tpDefense == 0,
          "invalid and unsupported saved defense values fall back to Automatic");
    got.fpDefense = 2;
    got.tpDefense = -1;
    got.normalize();
    check(got.fpDefense == 0 && got.tpDefense == 0,
          "bulk settings normalization cannot introduce a third or negative defense mode");
    wcscpy_s(settingsPath, migrated.c_str());
    loadSettings(settingsPath);
    manualDefenseReady = false;
    panel::SettingsWindow settings;
    settings.getSettings = currentSettings;
    settings.changeSetting = applySetting;
    DefenseSettingsSurface surface(directory);
    surface.checkWidths();
    unsigned revision = inputOwner.policyRevision();
    check(surface.click(settings, panel::setting::fpDefense) && fpManualGuard.load() == 1 &&
              tpManualGuard.load() == 0 && inputOwner.policyRevision() == revision + 1,
          "the actual settings row owner changes only FP and invalidates old input policy");
    got = currentSettings();
    wchar_t saved[32]{};
    GetPrivateProfileStringW(L"FirstPerson", L"Defense", L"", saved, 32, settingsPath);
    check(!got.manualDefenseAvailable && got.fpDefense == 1 && !std::wcscmp(saved, L"manual") &&
              got.value(panel::setting::fpDefense) == L"Automatic fallback" &&
              got.value(panel::setting::tpDefense) == L"Automatic (native)",
          "unavailable Manual remains saved as a request while the UI reports effective Automatic");
    clearCalls();
    check(invokeDefense(actor) == 0x16u && reactiveCalls == 1 && attackCalls == 1 && selectorCalls == 1 &&
              !cleanupCalls,
          "unavailable optional capability keeps all three native defense owners effective");
    manualDefenseReady = true;
    got = currentSettings();
    check(got.manualDefenseAvailable && got.fpDefense == 1 && got.tpDefense == 0 &&
              got.value(panel::setting::fpDefense) == L"Manual (hold RMB)",
          "runtime admission exposes the retained Manual request without rewriting the other view");
    revision = inputOwner.policyRevision();
    check(surface.click(settings, panel::setting::tpDefense) && fpManualGuard.load() == 1 &&
              tpManualGuard.load() == 1 && inputOwner.policyRevision() == revision + 1 &&
              readSettings(settingsPath).tpDefense == 1,
          "the actual TP control persists its independent Manual request");
    check(surface.click(settings, panel::setting::fpDefense) && fpManualGuard.load() == 0 &&
              tpManualGuard.load() == 1 && readSettings(settingsPath).fpDefense == 0,
          "returning FP to Automatic preserves TP Manual and writes canonical text");
    GetPrivateProfileStringW(L"FirstPerson", L"Defense", L"", saved, 32, settingsPath);
    check(!std::wcscmp(saved, L"automatic"), "the rendered Automatic choice writes its canonical INI value");
    const std::wstring again = userSettingsFile(source, userDir);
    check(again == migrated && readSettings(again.c_str()).tpDefense == 1,
          "a later launch preserves the user's defense choices instead of recopying release defaults");
    settingsPath[0] = 0;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, migrated.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, source.c_str());
    check(DeleteFileW(migrated.c_str()) && DeleteFileW(source.c_str()) && RemoveDirectoryW(userDir.c_str()) &&
              RemoveDirectoryW(moduleDir.c_str()) && RemoveDirectoryW(directory),
          "the owned INI fixture is removed after verification");
}
static void writeThunk(fp::sig::Target target, const void* destination) {
    // x64 absolute jump to an owned C++ spy; no archived game instructions.
    const unsigned char jump[16] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0, 0x90, 0x90, 0x90, 0x90};
    unsigned char* code = (unsigned char*)gameAt(target);
    std::memcpy(code, jump, sizeof jump);
    std::memcpy(code + 2, &destination, sizeof destination);
}
// B7560 native contract spy: native relaxed/ready/active-defense selection.
// It deliberately does not animate arms; these checks prove owner requests only.
static uint32_t fixturePosture = 3;
static unsigned postureCalls = 0;
static uint32_t nativePosture(uintptr_t) {
    ++postureCalls;
    return fixturePosture;
}
static uint32_t invokePosture(uintptr_t behaviorObject) {
#if FP_REVISION >= 17
    originalCombatPosture = nativePosture;
    return combatPosture(behaviorObject);
#else
    return nativePosture(behaviorObject);
#endif
}
static void readySetup(int view = 0, bool manual = true) {
    resetFixture(view, manual, manual);
    fixturePosture = 3;
    postureCalls = 0;
    at<float>(actor, 0xD58) = 1.f; // reviewed native readiness witness
    at<float>(actor, field::character::balance) = 1.f;
    at<float>(actor, field::character::balanceMax) = 1.f;
    sample(2);
}
static void readyPostureTests() {
    for (int view : {0, 1}) {
        readySetup(view);
        const std::vector<unsigned char> before((unsigned char*)actor, (unsigned char*)actor + 0x1400);
        check(invokePosture(bh) == 4 && postureCalls == 1,
              "held Manual guard requests native ready posture even without an opponent");
        check(!std::memcmp(before.data(), (void*)actor, before.size()),
              "readiness request leaves accepted attack, recovery, head, arms and all actor bytes native");
        check(invokePosture(otherBh) == 3, "held guard cannot ready another actor");
        fixtureKeys[VK_RBUTTON] = false;
        check(invokePosture(bh) == 3, "physical release immediately returns posture selection to native");
        fixturePosture = 4;
        check(invokePosture(bh) == 4,
              "release preserves a native ready posture selected for a nearby threat");
        readySetup(view, false);
        check(invokePosture(bh) == 3, "Automatic does not gain an explicit RMB posture request");
        for (uint32_t state : {0u, 4u, 5u, 12u, 20u, 21u, 22u, 23u, 24u, 34u}) {
            readySetup(view);
            fixturePosture = state;
            check(invokePosture(bh) == state,
                  "native non-relaxed result and active parry choice are preserved");
        }
        readySetup(view);
        at<unsigned char>(actor, field::character::defenseActive) = 1;
        check(invokePosture(bh) == 3, "active defense keeps even its native relaxed result");
        for (float readiness : {0.f, .009f, NAN, INFINITY}) {
            readySetup(view);
            at<float>(actor, 0xD58) = readiness;
            check(invokePosture(bh) == 3, "native readiness gate and invalid scalar cannot force a pose");
        }
        for (float balance : {.949f, NAN, INFINITY}) {
            readySetup(view);
            at<float>(actor, field::character::balance) = balance;
            check(invokePosture(bh) == 3, "native balance limit and invalid balance cannot force a pose");
        }
        readySetup(view);
        at<float>(actor, field::character::balance) = .951f;
        check(invokePosture(bh) == 4, "native balance tolerance admits a ready request");
        readySetup(view);
        at<float>(actor, field::character::balanceMax) = NAN;
        check(invokePosture(bh) == 3, "invalid maximum balance leaves native posture intact");
        for (int exclusion = 0; exclusion < 17; ++exclusion) {
            readySetup(view);
            switch (exclusion) {
            case 0:
                viewMode = 2;
                break;
            case 1:
                *(uintptr_t*)gameAt(fp::sig::currentScene) = otherScene;
                break;
            case 2:
                inputOwner.releaseGameplay();
                break;
            case 3:
                inputOwner.policyChanged();
                break;
            case 4:
                at<uintptr_t>(actor, field::character::headPart) = otherHead;
                break;
            case 5:
                inputEnabled = 0;
                break;
            case 6:
                manualDefenseReady = false;
                break;
            case 7:
                at<unsigned char>(actor, field::character::combatStance) = 0;
                break;
            case 8:
                at<uintptr_t>(actor, field::character::behaviour) = otherBh;
                break;
            case 9:
                at<unsigned>(drag, field::drag::flags) = 1;
                break;
            case 10:
                at<unsigned>(gui, field::gui::dragFormat) = 1;
                break;
            case 11:
                at<unsigned>(actor, field::character::lifeFlags) = 0x100;
                break;
            case 12:
                at<unsigned>(actor, field::character::stateFlags) = 0x20;
                break;
            case 13:
                at<uintptr_t>(actor, field::character::actionCallback) = 1;
                break;
            case 14:
                fixtureKeys['E'] = true;
                break;
            case 15:
                active = 0;
                break;
            case 16:
                stopping = 1;
                break;
            }
            check(invokePosture(bh) == 3, "excluded context or stale owner leaves posture selection native");
        }
    }
    check(invokePosture(0) == 3, "failed optional identity leaves the native posture result intact");
}
// Native interception contract spies. Output coordinates/motions stand for
// native-owned results, not a reimplementation or simulation of weapon physics.
static unsigned nativeUpdates = 0, interceptionCalls = 0, interceptionSelections = 0;
static bool schedulerAttempts = false, incomingThreat = true, updateFault = false, interceptionFault = false;
static bool nativeSelectsPose = true;
static int updateChange = 0, nestedUpdate = 0, updateDepth = 0, threatPose = 20;
static uintptr_t interceptionActor = 0, interceptionIncoming = 0;
static float nativeWeaponTarget[3]{1.f, 2.f, 3.f};
static void invokeCombatUpdate(uintptr_t root);
static uint64_t nativeInterceptionSelection(uintptr_t root, uintptr_t incoming) {
    ++interceptionSelections;
    interceptionActor = root;
    interceptionIncoming = incoming;
    return uint64_t(threatPose);
}
static void nativeInterception(uintptr_t root) {
    ++interceptionCalls;
    record('I');
    if (interceptionFault) RaiseException(0xE0424242, 0, 0, nullptr);
    if (!incomingThreat || !at<unsigned char>(root, nativeFixture::eligible) ||
        !at<unsigned char>(root, nativeFixture::capability)) {
        nativeCleanup(root);
        return;
    }
    // Native 1636E0 may retain a pose while 162F80 updates its target. The
    // adapter must accept either native choice without adding a selection.
    if (nativeSelectsPose) {
        const uint64_t selected = defenseMotionSelect(root, otherActor);
        if (selected == 0xffffffffu) return;
        at<int>(root, field::character::motion) = int(selected);
    }
    at<unsigned char>(root, field::character::defenseActive) = 1;
    at<int>(root, field::character::defenseIncomingMotion) = 7;
    std::memcpy((void*)(root + 0xF7C), nativeWeaponTarget, sizeof nativeWeaponTarget);
}
static void nativeCombatUpdate(uintptr_t root) {
    ++nativeUpdates;
    record('U');
    if (updateFault) RaiseException(0xE0424242, 0, 0, nullptr);
    if (nestedUpdate && updateDepth == 0) {
        ++updateDepth;
        invokeCombatUpdate(nestedUpdate == 1 ? root : otherActor);
        --updateDepth;
    }
    // Model a native callback that changes ownership/physical state before the
    // adapter resumes. The native body itself remains authoritative.
    switch (updateChange) {
    case 1:
        fixtureKeys[VK_RBUTTON] = false;
        break;
    case 2:
        *(uintptr_t*)gameAt(fp::sig::currentScene) = otherScene;
        break;
    case 3:
        *(uintptr_t*)gameAt(fp::sig::playerRoot) = otherActor;
        break;
    case 4:
        at<uintptr_t>(root, field::character::headPart) = otherHead;
        break;
    case 5:
        inputOwner.policyChanged();
        break;
    case 6:
        viewMode = 2;
        break;
    case 7:
        inputEnabled = 0;
        break;
    case 8:
        stopping = 1;
        break;
    case 9:
        at<int>(root, field::character::motion) = 7;
        break;
    case 10:
        at<unsigned>(root, field::character::stateFlags) = 0x800;
        break;
    case 11:
        at<float>(root, field::character::balance) = .2f;
        break;
    }
    if (schedulerAttempts || at<unsigned char>(root, field::character::defenseActive)) defenseReactive(root);
}
static void invokeCombatUpdate(uintptr_t root) {
#if FP_REVISION >= 18
    originalCombatUpdate = nativeCombatUpdate;
    combatUpdate(root);
#else
    nativeCombatUpdate(root);
#endif
}
static bool catchCombatUpdate() {
    __try {
        invokeCombatUpdate(actor);
    } __except (GetExceptionCode() == 0xE0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
static bool updateScopeClear() {
#if FP_REVISION >= 18
    return defenseUpdate == nullptr;
#else
    return true;
#endif
}
static void interceptionSetup(int view = 0, bool manual = true) {
    readySetup(view, manual);
    at<int>(actor, field::character::motion) = 3;
    at<unsigned>(actor, field::character::stateFlags) = 0;
    at<unsigned char>(actor, field::character::physicsLive) = 1;
    originalDefenseReactive = nativeInterception;
    originalDefenseMotionSelect = nativeInterceptionSelection;
    nativeUpdates = interceptionCalls = interceptionSelections = 0;
    schedulerAttempts = updateFault = interceptionFault = false;
    incomingThreat = true;
    nativeSelectsPose = true;
    updateChange = nestedUpdate = updateDepth = 0;
    threatPose = 20;
    nativeWeaponTarget[0] = 1.f;
    nativeWeaponTarget[1] = 2.f;
    nativeWeaponTarget[2] = 3.f;
    clearCalls();
}
static void interceptionTests() {
    for (int view : {0, 1}) {
        interceptionSetup(view);
        float headBefore[12];
        std::memcpy(headBefore, (void*)(head + field::bodyPart::pose), sizeof headBefore);
        invokeCombatUpdate(actor);
        check(nativeUpdates == 1 && interceptionCalls == 1 && interceptionSelections == 1 &&
                  !std::strcmp(callOrder, "UI") && interceptionActor == actor &&
                  interceptionIncoming == otherActor,
              "held guard explicitly drives native interception after the physical update without an "
              "automatic trigger");
        check(at<int>(actor, field::character::motion) == 20 &&
                  vectorNear((float*)(actor + 0xF7C), nativeWeaponTarget) &&
                  at<unsigned char>(actor, field::character::defenseActive),
              "native attack-specific defensive motion and weapon target survive the adapter");
        check(!std::memcmp(headBefore, (void*)(head + field::bodyPart::pose), sizeof headBefore),
              "interception bridge does not alter the physical-head pose");
        threatPose = 24;
        nativeSelectsPose = false;
        nativeWeaponTarget[0] = -4.f;
        nativeWeaponTarget[1] = 6.f;
        invokeCombatUpdate(actor);
        check(nativeUpdates == 2 && interceptionCalls == 2 && interceptionSelections == 1 &&
                  at<int>(actor, field::character::motion) == 20 &&
                  vectorNear((float*)(actor + 0xF7C), nativeWeaponTarget),
              "native target tracking can retain its pose without the adapter forcing reselection");
        nativeSelectsPose = true;
        invokeCombatUpdate(actor);
        check(nativeUpdates == 3 && interceptionCalls == 3 && interceptionSelections == 2 &&
                  at<int>(actor, field::character::motion) == 24 &&
                  vectorNear((float*)(actor + 0xF7C), nativeWeaponTarget),
              "active native interception updates to a changed threat without an extra request");
        fixtureKeys[VK_RBUTTON] = false;
        invokeCombatUpdate(actor);
        check(interceptionCalls == 3 && !at<unsigned char>(actor, field::character::defenseActive) &&
                  at<int>(actor, field::character::defenseIncomingMotion) == -1,
              "release withdraws active interception through the existing native cleanup owner");
        interceptionSetup(view);
        schedulerAttempts = true;
        invokeCombatUpdate(actor);
        check(nativeUpdates == 1 && interceptionCalls == 1, "an existing native attempt is not duplicated");
        interceptionSetup(view);
        schedulerAttempts = true;
        incomingThreat = false;
        invokeCombatUpdate(actor);
        check(interceptionCalls == 1 && !at<unsigned char>(actor, field::character::defenseActive),
              "a rejected native attempt is not retried in the same update");
        for (int rejected = 0; rejected < 3; ++rejected) {
            interceptionSetup(view);
            if (rejected == 0) incomingThreat = false;
            if (rejected == 1) at<unsigned char>(actor, nativeFixture::eligible) = 0;
            if (rejected == 2) at<unsigned char>(actor, nativeFixture::capability) = 0;
            invokeCombatUpdate(actor);
            check(interceptionCalls == 1 && !interceptionSelections &&
                      !at<unsigned char>(actor, field::character::defenseActive),
                  "native no-threat or equipment/eligibility rejection remains authoritative");
        }
        for (int motion : {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20, 21, 22, 23, 24, 28, 34}) {
            interceptionSetup(view);
            at<int>(actor, field::character::motion) = motion;
            invokeCombatUpdate(actor);
            check(!interceptionCalls && at<int>(actor, field::character::motion) == motion &&
                      closeFloat(at<float>(actor, field::character::motionProgress), .37f),
                  "accepted attack, recovery and special motions are never interrupted by an extra request");
        }
        for (unsigned state : {1u, 2u, 4u, 0x20u, 0x40u, 0x200u, 0x800u, 0x10000u}) {
            interceptionSetup(view);
            at<unsigned>(actor, field::character::stateFlags) = state;
            invokeCombatUpdate(actor);
            check(!interceptionCalls,
                  "non-rest/ready native physical state cannot be replaced by manual interception");
        }
        interceptionSetup(view);
        at<int>(actor, field::character::motion) = 4;
        at<unsigned>(actor, field::character::stateFlags) = 8;
        invokeCombatUpdate(actor);
        check(interceptionCalls == 1, "accepted ready stance also admits explicit interception");
        for (int change = 1; change <= 11; ++change) {
            interceptionSetup(view);
            updateChange = change;
            invokeCombatUpdate(actor);
            check(!interceptionCalls,
                  "native update handoff or loss of physical readiness invalidates the held request");
        }
        for (int exclusion = 0; exclusion < 11; ++exclusion) {
            interceptionSetup(view);
            switch (exclusion) {
            case 0:
                viewMode = 2;
                break;
            case 1:
                manualDefenseReady = false;
                break;
            case 2:
                inputOwner.releaseGameplay();
                break;
            case 3:
                at<unsigned char>(actor, field::character::physicsLive) = 0;
                break;
            case 4:
                at<float>(actor, field::character::postureReadiness) = 0;
                break;
            case 5:
                at<float>(actor, field::character::balance) = NAN;
                break;
            case 6:
                at<float>(actor, field::character::balanceMax) = INFINITY;
                break;
            case 7:
                at<unsigned>(drag, field::drag::flags) = 1;
                break;
            case 8:
                at<unsigned>(gui, field::gui::dragFormat) = 1;
                break;
            case 9:
                fixtureKeys['E'] = true;
                break;
            case 10:
                at<uintptr_t>(actor, field::character::actionCallback) = 1;
                break;
            }
            invokeCombatUpdate(actor);
            check(nativeUpdates == 1 && !interceptionCalls,
                  "excluded context forwards physics without an extra interception");
        }
        interceptionSetup(view, false);
        invokeCombatUpdate(actor);
        check(nativeUpdates == 1 && !interceptionCalls, "Automatic retains its own initiation schedule");
        interceptionSetup(view);
        invokeCombatUpdate(otherActor);
        check(nativeUpdates == 1 && !interceptionCalls,
              "NPC physical update is not driven by the player's guard");
        for (int nested : {1, 2}) {
            interceptionSetup(view);
            nestedUpdate = nested;
            invokeCombatUpdate(actor);
            check(nativeUpdates == 2 && interceptionCalls == 1 && updateScopeClear(),
                  "nested same/other actor updates do not duplicate or leak the held interception request");
        }
        interceptionSetup(view);
        updateFault = true;
        check(catchCombatUpdate() && !interceptionCalls && updateScopeClear(),
              "native update faults propagate without adding a request and restore the scope");
        interceptionSetup(view);
        interceptionFault = true;
        check(catchCombatUpdate() && interceptionCalls == 1 && updateScopeClear(),
              "native interception faults propagate and restore the update scope");
    }
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr size_t bytes = 0x800000;
    fixture::NativeImage image(bytes);
    check(bool(image), "synthetic combat image allocated in the test process");
    if (!image) return 1;
    actor = base + 0x610000;
    bh = base + 0x620000;
    head = base + 0x630000;
    otherActor = base + 0x640000;
    otherBh = base + 0x650000;
    otherHead = base + 0x660000;
    scene = base + 0x670000;
    otherScene = base + 0x680000;
    drag = base + 0x690000;
    gui = base + 0x6a0000;
    hands[0] = base + 0x6b0000;
    hands[1] = base + 0x6c0000;
    fixtureParents[0] = base + 0x6d0000;
    fixtureParents[1] = base + 0x6e0000;
    writeThunk(fp::sig::headingFromDirection, (const void*)nativeHeading);
    writeThunk(fp::sig::aimFilterPush, (const void*)nativeFilterPush);
    writeThunk(fp::sig::aimFilterUpdate, (const void*)nativeFilterUpdate);
    bool executable = true;
    for (auto target : {fp::sig::headingFromDirection, fp::sig::aimFilterPush, fp::sig::aimFilterUpdate}) {
        DWORD old = 0;
        executable &= VirtualProtect((void*)gameAt(target), 16, PAGE_EXECUTE_READ, &old) != FALSE;
    }
    check(executable && FlushInstructionCache(GetCurrentProcess(), image.data(), bytes),
          "owned heading/filter spies can be called through the production resolved targets");
    if (executable) {
        publicationTests();
        profileTests();
        releaseTests();
        exemptionTests();
        nativeStateTests();
        headingTests();
        settingsTests();
        readyPostureTests();
        interceptionTests();
    }
    check(image.close(), "synthetic combat image released");
    std::printf("Combat control owner checks: %d; failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
