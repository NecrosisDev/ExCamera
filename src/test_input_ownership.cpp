// Portable sequence tests for the production owner. These establish press
// lifetime and transitions; native hooks are covered by the Windows fixtures.
#include "input_ownership.h"
#include <cstdio>

using Owner = fp::InputOwnership<10>;
using Context = fp::InputContext<10>;
static unsigned checks = 0, failures = 0;
static void check(bool value, const char* label) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
}
static Context gameplay() {
    Context context;
    context.root = 1;
    context.head = 2;
    context.scene = 3;
    context.combat = context.ordinary = true;
    context.bindings = {'W', 'A', 'S', 'D', 'C', 16, 'V', 0, 0, 'E'};
    return context;
}
static void windowLifetime() {
    Owner owner;
    owner.windowBegin('U');
    owner.windowClaim('U');
    owner.consumeBinding('U');
    owner.releaseGameplay();
    owner.policyChanged();
    check(owner.windowOwns('U') && owner.maskBinding('U', true),
          "gameplay and policy release retain both native and window obligations");
    check(!owner.maskBinding('U', false) && owner.windowOwns('U'),
          "physical release retires polling consumption without discarding the owed window up");
    check(owner.windowRelease('U') && !owner.windowRelease('U'),
          "exactly one window release belongs to the claimed physical press");
    owner.windowClaim('U');
    owner.windowClaim('I');
    owner.consumeBinding('U');
    owner.attackDown(true);
    const uint32_t generation = owner.generation();
    const unsigned policy = owner.policyRevision();
    owner.newWindowSession();
    check(!owner.windowRelease('U') && !owner.windowRelease('I') && owner.maskBinding('U', true) &&
              owner.attackSuppressed() && owner.generation() == generation && owner.policyRevision() == policy,
          "a new window session clears only its event recipients");
    owner.windowClaim('U');
    owner.windowBegin('U');
    check(!owner.windowRelease('U'), "a fresh unclaimed down retires a release lost outside the window");
    owner.windowClaim(0);
    owner.windowClaim(256);
    check(!owner.windowOwns(0) && !owner.windowOwns(256) && !owner.maskBinding(-1, true),
          "unassigned and invalid bindings cannot acquire ownership");
}
static void bufferedEvents() {
    Owner owner;
    Context context = gameplay();
    owner.beginSample(context, true, 0);
    owner.movementDown(1, true);
    owner.movementUp(1);
    check(owner.movementSample(0x20, 0, 15) == 0x21 && owner.movementSample(0x20, 0, 15) == 0x20,
          "a complete movement tap contributes once without erasing native run");
    owner.movementDown(2, true);
    owner.movementSample(2, 2, 15);
    check(owner.movementSample(2, 0, 15) == 0 && !owner.heldMovement(),
          "a lost movement release clears both buffered and stale native held bits");
    owner.attackDown(false);
    owner.buttonDown(1);
    auto sample = owner.beginSample(context, true, 0);
    check(owner.bufferedButtons(sample) == 1 && owner.bufferedButtons(sample) == 0,
          "a complete mouse click between native polls contributes once");
    owner.attackDown(false);
    owner.buttonDown(1);
    owner.suppressAttack();
    check(!owner.pendingButtons() && owner.attackSuppressed(),
          "guard overlap retires an attack tap in the same event owner");
    owner.buttonDown(2);
    owner.movementDown(1, true);
    owner.releaseGameplay();
    check(!owner.pendingButtons() && !owner.heldMovement() && !owner.pendingMovement(),
          "gameplay release discards all transient movement and mouse requests");
    context.policy = owner.policyRevision();
    sample = owner.beginSample(context, true, 0);
    const fp::InputStamp stamp = sample.stamp;
    owner.releaseGameplay();
    owner.policyChanged();
    check(stamp.generation != owner.generation() && stamp.policyRevision != owner.policyRevision(),
          "an already sampled request keeps its original generation and policy");
}
static void bindingLifetime() {
    Owner owner;
    Context context = gameplay();
    auto sample = owner.beginSample(context, true, 0);
    check(!owner.held(fp::HeldBinding::lean, 'U', true, true, sample),
          "an initial held binding waits for release");
    sample = owner.beginSample(context, true, 0);
    owner.held(fp::HeldBinding::lean, 'U', false, true, sample);
    sample = owner.beginSample(context, true, 0);
    check(owner.held(fp::HeldBinding::lean, 'U', true, true, sample),
          "a released binding accepts the next physical hold");
    owner.consumeBinding('U');
    check(!owner.held(fp::HeldBinding::lean, 'I', true, true, sample) && owner.maskBinding('U', true),
          "a rebind cannot inherit the new key's old hold or free the old native key");
    check(!owner.held(fp::HeldBinding::left, 'J', true, true, sample),
          "another held action has its own release barrier");
    owner.held(fp::HeldBinding::left, 'J', false, true, sample);
    check(owner.held(fp::HeldBinding::left, 'J', true, true, sample) &&
              !owner.held(fp::HeldBinding::lean, 'I', true, true, sample),
          "releasing one binding cannot arm a different held binding");
    owner.releaseGameplay();
    sample = owner.beginSample(context, true, 0);
    check(!owner.held(fp::HeldBinding::left, 'J', true, true, sample) && owner.maskBinding('U', true),
          "a gameplay generation change disarms actions while preserving native consumption");
}
static void contextChanges() {
    for (unsigned change = 0; change < 7; ++change) {
        Owner owner;
        Context context = gameplay();
        owner.beginSample(context, true, 0);
        owner.beginSample(context, true, 3);
        switch (change) {
        case 0: ++context.root; break;
        case 1: ++context.head; break;
        case 2: ++context.scene; break;
        case 3: ++context.mode; break;
        case 4: context.combat = false; break;
        case 5: owner.policyChanged(); context.policy = owner.policyRevision(); break;
        case 6: context.bindings[9] = 'F'; break;
        }
        const auto boundary = owner.beginSample(context, true, 3);
        const auto leftReleased = owner.beginSample(context, true, 2);
        const auto leftPressed = owner.beginSample(context, true, 3);
        char label[120];
        std::snprintf(label, sizeof label, "context change %u retains independent left/right release barriers", change);
        check(boundary.boundary && !boundary.continuing && boundary.buttons == 0 && leftReleased.buttons == 0 &&
                  leftPressed.buttons == 1 && owner.blockedButtons() == 2,
              label);
    }
    Owner owner;
    Context context = gameplay();
    owner.beginSample(context, true, 0);
    owner.beginSample(context, false, 2);
    check(owner.beginSample(context, true, 2).buttons == 0,
          "returning from UI ownership cannot promote a held mouse button");
}
static void nativeActionTransitions() {
    Owner owner;
    Context context = gameplay();
    owner.beginSample(context, true, 0);
    owner.attackDown(false);
    owner.buttonDown(1);
    auto sample = owner.beginSample(context, true, 1);
    check(owner.bufferedButtons(sample) == 1, "a fresh LMB begins the native action");
    // The native owner raises pressBlocked/interactionLatch during a physical
    // press. This changes defense eligibility, not ownership of the LMB hold.
    context.ordinary = false;
    sample = owner.beginSample(context, true, 1);
    check(!sample.boundary && sample.continuing && owner.bufferedButtons(sample) == 1,
          "native action eligibility cannot synthesize an LMB release halfway through its hold");
    context.ordinary = true;
    sample = owner.beginSample(context, true, 1);
    check(!sample.boundary && owner.bufferedButtons(sample) == 1,
          "native action completion preserves the same LMB press");
    owner.beginSample(context, true, 0);
    owner.attackDown(false);
    owner.buttonDown(1);
    context.ordinary = false;
    sample = owner.beginSample(context, true, 0);
    check(owner.bufferedButtons(sample) == 1,
          "a complete click between polls survives a transient native action change");

    context.ordinary = true;
    owner.beginSample(context, true, 0);
    sample = owner.beginSample(context, true, 2);
    check(owner.combatSample(sample.buttons, 2, true, true).guard,
          "a fresh RMB permits manual defense before an action transition");
    context.ordinary = false;
    sample = owner.beginSample(context, true, 3);
    check(sample.buttons == 1 && owner.blockedButtons() == 2,
          "leaving manual defense disarms only the held RMB, preserving LMB");
    context.ordinary = true;
    sample = owner.beginSample(context, true, 3);
    check(!owner.combatSample(sample.buttons, 3, true, true).guard,
          "held RMB cannot automatically rearm when the native action ends");
    owner.beginSample(context, true, 0);
    owner.combatSample(0, 0, true, true);
    sample = owner.beginSample(context, true, 2);
    check(owner.combatSample(sample.buttons, 2, true, true).guard,
          "release then repress rearms manual defense after the native action");
}
static void attackLifetime() {
    Owner owner;
    Context context = gameplay();
    owner.beginSample(context, true, 0);
    check(owner.combatSample(2, 2, true, true).guard, "fresh RMB authorizes manual defense");
    owner.attackDown(true);
    check(owner.combatSample(1, 1, true, true).buttons == 0,
          "a guard overlap keeps LMB consumed after guard releases first");
    check(owner.combatSample(1, 1, false, false).buttons == 0,
          "a consumed combat press stays consumed on native handoff");
    owner.combatSample(0, 0, true, true);
    check(owner.combatSample(1, 1, true, true).buttons == 1,
          "an observed release rearms a later press with no window event");

    owner.attackDown(true);
    check(owner.combatSample(1, 0, true, true).buttons == 0 && !owner.attackSuppressed(),
          "a fully released overlapping attack tap is flushed exactly once");
    owner.attackDown(false);
    check(owner.combatSample(1, 0, true, true).buttons == 1,
          "a fresh complete click in the same polling interval keeps its own attack request");

    owner.attackDown(true);
    owner.combatSample(1, 1, true, true);
    owner.attackDown(false);
    check(owner.combatSample(1, 1, true, true).buttons == 1,
          "a fresh press retires older consumption even if release was between polls");
    owner.suppressAttack();
    owner.releaseGameplay();
    owner.beginSample(context, true, 0);
    check(!owner.attackSuppressed() && !owner.combatSample(2, 0, true, true).guard,
          "a handoff discards old tap suppression and a released guard never queues");
}
static void quiescentReset() {
    Owner owner;
    Context context = gameplay();
    auto sample = owner.beginSample(context, true, 0);
    owner.held(fp::HeldBinding::lean, 'U', false, true, sample);
    owner.consumeBinding('U');
    owner.windowClaim('U');
    owner.attackDown(true);
    owner.combatSample(3, 3, true, true);
    owner.policyChanged();
    owner.releaseGameplay();
    owner.resetQuiescent();
    check(!owner.windowOwns('U') && !owner.bindingOwned('U') && !owner.attackSuppressed() &&
              owner.generation() == 0 && owner.policyRevision() == 0,
          "quiescent reset clears all ownership histories without calling native input");
    sample = owner.beginSample(context, true, 3);
    check(sample.boundary && !sample.continuing && sample.buttons == 0 &&
              !owner.held(fp::HeldBinding::lean, 'U', true, true, sample),
          "reset restores initial release barriers instead of manufacturing a fresh hold");
    owner.beginSample(context, true, 0);
    check(owner.combatSample(1, 1, false, false).buttons == 1,
          "reset cannot retain an earlier consumed combat press");
}
static void pointerGestures() {
    fp::PointerGesture gesture;
    fp::InputStamp stamp{1, 2, 3, 0, 4, 5};
    gesture.sync(stamp);
    gesture.press(gesture.current);
    for (int i = 0; i < 12; ++i) {
        gesture.move(1.f, -0.5f);
        gesture.poll(gesture.current);
    }
    check(gesture.current.x == 12. && gesture.current.y == -6. && gesture.polled.x == 12.,
          "small physical moves accumulate from the press across native polls");
    const auto seen = gesture.current;
    gesture.move(3.f, 2.f); // a new raw sample while the native press is being dispatched
    gesture.press(seen);
    check(gesture.current.x == 3. && gesture.current.y == 2. && gesture.polled.x == 0. && gesture.havePress,
          "a press consumes only its snapshot and preserves motion arriving during dispatch");
    gesture.sync(stamp);
    check(gesture.havePress && gesture.current.x == 3. && gesture.current.y == 2.,
          "rendered reticle changes cannot supply physical drag movement");
    for (unsigned change = 0; change < 6; ++change) {
        fp::InputStamp next = stamp;
        switch (change) {
        case 0: ++next.root; break;
        case 1: ++next.head; break;
        case 2: ++next.scene; break;
        case 3: ++next.mode; break;
        case 4: ++next.generation; break;
        case 5: ++next.policyRevision; break;
        }
        gesture.sync(stamp);
        gesture.move(30.f, 10.f);
        gesture.press(gesture.current);
        gesture.move(5.f, 6.f);
        gesture.sync(next);
        check(!gesture.havePress && gesture.current.x == 0. && gesture.polled.y == 0.,
              "each pointer owner change retires the previous click and accumulated movement");
    }
    gesture.move(INFINITY, 1.f);
    gesture.move(1.f, NAN);
    check(gesture.current.x == 0. && gesture.current.y == 0., "nonfinite mouse samples cannot poison a gesture");
    gesture.move(1e30f, -1e30f);
    check(gesture.current.x == 1048576. && gesture.current.y == -1048576.,
          "long uninterrupted turns saturate before conversion to client pixels");
}
int main() {
    windowLifetime();
    bufferedEvents();
    bindingLifetime();
    contextChanges();
    nativeActionTransitions();
    attackLifetime();
    quiescentReset();
    pointerGestures();
    std::printf("Input ownership: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
