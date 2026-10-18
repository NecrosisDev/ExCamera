#pragma once
#include "input_policy.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fp {
// One hold follows its physical key through rebinds and ownership changes.
// The caller supplies the physical sample; this rule has no keyboard API.
struct BindingHold {
    int key = 0;
    uint32_t generation = 0;
    bool primed = false, ready = false;
    bool sample(int bound, bool down, bool eligible, bool boundary, uint32_t currentGeneration) {
        if (!primed || bound != key || generation != currentGeneration || boundary || !eligible)
            ready = !down;
        else if (!down)
            ready = true;
        key = bound;
        generation = currentGeneration;
        primed = true;
        return eligible && down && ready;
    }
};
enum class HeldBinding : unsigned { rotate, lean, left, right, count };
struct InputStamp {
    uintptr_t root = 0, head = 0, scene = 0;
    int mode = -1;
    uint32_t generation = 0;
    unsigned policyRevision = 0;
};

// GUI click/drag classification follows deliberate mouse movement, independently
// of the rendered crosshair. Head motion must neither cancel a double-click nor
// supply the movement needed to grab a world object.
struct PointerGesture {
    struct Point { double x = 0, y = 0; } current, polled;
    InputStamp owner;
    bool haveOwner = false, havePress = false;
    bool ownedBy(const InputStamp& next) const {
        return haveOwner && owner.root == next.root && owner.head == next.head && owner.scene == next.scene &&
               owner.mode == next.mode && owner.generation == next.generation &&
               owner.policyRevision == next.policyRevision;
    }
    void sync(const InputStamp& next) {
        if (!ownedBy(next)) {
            current = polled = {};
            havePress = false;
            owner = next;
            haveOwner = true;
        }
    }
    void move(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        // Saturate long uninterrupted turns before converting to client pixels.
        current.x = std::fmax(-1048576., std::fmin(1048576., current.x + x));
        current.y = std::fmax(-1048576., std::fmin(1048576., current.y + y));
    }
    void press(Point seen) {
        // A mouse sample arriving during native dispatch belongs to this press.
        current.x -= seen.x;
        current.y -= seen.y;
        polled = {};
        havePress = true;
    }
    void poll(Point seen) { polled = seen; }
};

template <size_t Bindings> struct InputContext {
    uintptr_t root = 0, head = 0, scene = 0;
    int mode = 0;
    bool combat = false, ordinary = false;
    unsigned policy = 0;
    std::array<unsigned, Bindings> bindings{};
    bool sameOwner(const InputContext& other) const {
        return root == other.root && head == other.head && scene == other.scene && mode == other.mode &&
               combat == other.combat && policy == other.policy && bindings == other.bindings;
    }
};

// Window messages and native polling have different release obligations.
// Only their mailboxes and invalidation counters cross threads. The polling
// history is private and is changed only by the native input thread.
template <size_t Bindings> class InputOwnership {
    std::array<std::atomic<bool>, 256> windowTaken{};
    // High bits identify a new LMB press; bit 0 suppresses that press, including
    // a complete down/up pair received between native polls.
    std::atomic<uint32_t> attackPress{0};
    std::atomic<uint32_t> generationValue{0};
    std::atomic<unsigned> policyValue{0};
    std::atomic<unsigned> heldKeys{0}, keyTaps{0}, buttonTaps{0};
    struct Poll {
        InputContext<Bindings> context{};
        bool haveContext = false, haveGeneration = false;
        uint32_t generation = 0, sampledAttack = 0;
        ButtonHandoff buttons;
        std::array<BindingHold, unsigned(HeldBinding::count)> bindings{};
        std::array<bool, 256> consumedKeys{};
        unsigned consumedButtons = 0;
    } poll;
    static bool validKey(int key) { return key > 0 && key < 256; }

  public:
    InputOwnership() { newWindowSession(); }
    struct Sample {
        bool boundary = false, continuing = false;
        unsigned buttons = 0;
        InputStamp stamp;
    };
    struct CombatSample {
        unsigned buttons = 0;
        bool guard = false;
    };
    uint32_t generation() const { return generationValue.load(); }
    unsigned policyRevision() const { return policyValue.load(); }
    void policyChanged() { policyValue.fetch_add(1); }

    // Called when gameplay relinquishes input. It invalidates publications and
    // the next poll; it must not discard a key-up owed by the hooked window.
    void releaseGameplay() {
        generationValue.fetch_add(1);
        clearTransient();
    }
    void clearTransient() {
        heldKeys.store(0);
        keyTaps.store(0);
        buttonTaps.store(0);
    }
    void movementDown(unsigned bits, bool fresh) {
        heldKeys.fetch_or(bits);
        if (fresh) keyTaps.fetch_or(bits);
    }
    void movementUp(unsigned bits) { heldKeys.fetch_and(~bits); }
    unsigned heldMovement() const { return heldKeys.load(); }
    unsigned pendingMovement() const { return keyTaps.load(); }
    void discardMovementTaps() { keyTaps.store(0); }
    unsigned movementSample(unsigned native, unsigned physical, unsigned mask) {
        heldKeys.fetch_and(physical);
        return (native & ~mask) | ((native | heldKeys.load()) & physical & mask) | keyTaps.exchange(0);
    }
    void buttonDown(unsigned bits) { buttonTaps.fetch_or(bits); }
    unsigned pendingButtons() const { return buttonTaps.load(); }
    void discardButtons() { buttonTaps.store(0); }

    // A different WndProc owned events while this hook session was absent.
    // Native polling history and gameplay publications have separate lifetimes.
    void newWindowSession() {
        for (auto& taken : windowTaken) taken.store(false);
    }
    bool windowOwns(int key) const { return validKey(key) && windowTaken[size_t(key)].load(); }
    bool windowRelease(int key) {
        return validKey(key) && windowTaken[size_t(key)].exchange(false);
    }
    void windowBegin(int key) {
        if (validKey(key)) windowTaken[size_t(key)].store(false);
    }
    void windowClaim(int key) {
        if (validKey(key)) windowTaken[size_t(key)].store(true);
    }
    void attackDown(bool overlap) {
        buttonTaps.fetch_and(~1u); // replace an older physical press's queued tap
        uint32_t before = attackPress.load();
        while (!attackPress.compare_exchange_weak(before, ((before & ~1u) + 2u) | unsigned(overlap))) {}
    }
    void suppressAttack() {
        attackPress.fetch_or(1u);
        buttonTaps.fetch_and(~1u);
    }
    bool attackSuppressed() const { return (attackPress.load() & 1u) != 0; }

    Sample beginSample(const InputContext<Bindings>& context, bool owned, unsigned sampledNativeButtons) {
        const uint32_t nextGeneration = generation();
        const bool changed = poll.haveContext && !context.sameOwner(poll.context);
        const bool defenseChanged = poll.haveContext && context.ordinary != poll.context.ordinary;
        const bool boundary = changed || !poll.haveGeneration || nextGeneration != poll.generation;
        poll.context = context;
        poll.haveContext = poll.haveGeneration = true;
        poll.generation = nextGeneration;
        if (boundary) {
            poll.buttons.reset();
            discardButtons();
        } else if (defenseChanged) {
            // Native attack/interaction latches change during a physical LMB
            // hold. Preserve that press and any queued click. RMB must still
            // be released before a newly eligible manual guard can take it.
            poll.buttons.blocked |= sampledNativeButtons & 2u;
        }
        const bool continuing = poll.buttons.owned;
        const unsigned acceptedButtons = poll.buttons.sample(owned, sampledNativeButtons);
        if (boundary || !owned) attackPress.fetch_and(~1u);
        return {boundary, continuing, acceptedButtons,
                {context.root, context.head, context.scene, context.mode, nextGeneration, context.policy}};
    }
    unsigned blockedButtons() const { return poll.buttons.blocked; }
    unsigned bufferedButtons(const Sample& sample) {
        const unsigned taps = buttonTaps.exchange(0);
        return (sample.buttons | (sample.continuing ? taps : 0)) & ~poll.buttons.blocked;
    }
    bool held(HeldBinding binding, int key, bool down, bool eligible, const Sample& sample) {
        return poll.bindings[unsigned(binding)].sample(key, validKey(key) && down, eligible,
                                                      sample.boundary, poll.generation);
    }
    void consumeBinding(int key) {
        if (validKey(key)) poll.consumedKeys[size_t(key)] = true;
    }
    bool bindingOwned(int key) const {
        return validKey(key) && (poll.consumedKeys[size_t(key)] || windowOwns(key));
    }
    bool maskBinding(int key, bool physicalDown) {
        if (!validKey(key)) return false;
        bool& consumed = poll.consumedKeys[size_t(key)];
        const bool taken = consumed || windowOwns(key);
        if (!physicalDown) consumed = false;
        return taken && physicalDown;
    }
    CombatSample combatSample(unsigned sampledNativeButtons, unsigned physical, bool ordinary, bool manual) {
        const uint32_t state = attackPress.load();
        if ((state & ~1u) != poll.sampledAttack) {
            poll.consumedButtons &= ~1u;
            poll.sampledAttack = state & ~1u;
        }
        // Retire only the sampled suppression. A newer window press wins this
        // comparison, including when its complete click falls between polls.
        if ((state & 1u) && !(physical & 1u)) {
            uint32_t expected = state;
            attackPress.compare_exchange_strong(expected, state & ~1u);
        }
        const unsigned messageMask = state & 1u;
        poll.consumedButtons &= physical;
        poll.consumedButtons |= messageMask & physical;
        const bool guard = ordinary && manual && (sampledNativeButtons & physical & 2u) &&
                           !(poll.buttons.blocked & 2u);
        if (ordinary) {
            sampledNativeButtons &= ~2u;
            poll.consumedButtons |= physical & 2u;
        }
        if (guard) {
            poll.consumedButtons |= physical & 1u;
            sampledNativeButtons &= ~1u;
        }
        return {sampledNativeButtons & ~(poll.consumedButtons | messageMask), guard};
    }

    // Only before callbacks exist, or after their owner has proved quiescence.
    // Fixtures use this directly; no synthetic input ticks are needed to erase
    // hidden function-local history. Normal handoffs use the operations above.
    void resetQuiescent() {
        newWindowSession();
        attackPress.store(0);
        generationValue.store(0);
        policyValue.store(0);
        clearTransient();
        poll = {};
    }
};
} // namespace fp
