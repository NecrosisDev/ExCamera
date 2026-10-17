#ifdef NDEBUG
#error "The tests require assert()."
#endif
#include "raw_registration.h"
#include "input_policy.h"
#include <cassert>
#include <cstdio>

struct Fixture {
    fp::RawRegistration current{true, false, 0, 0};
    uintptr_t target = 0;
    bool active = false, installOk = true, removeOk = true, replaceOnInstall = false;
    unsigned installs = 0, removes = 0;
    fp::RawLook look;
    bool reconcile(uintptr_t desired) {
        return fp::reconcileRawRegistration(
            desired, target, [&] { return current; },
            [&](uintptr_t window) {
                ++installs;
                if (!installOk) return false;
                current = {true, true, replaceOnInstall ? 99 : window, 0};
                return true;
            },
            [&] {
                ++removes;
                if (!removeOk) return false;
                current = {true, false, 0, 0};
                return true;
            },
            [&](bool registered) {
                if (!registered || registered != active) look = fp::RawLook{};
                active = registered;
            });
    }
};
int main() {
    {
        Fixture f;
        assert(f.reconcile(1) && f.active && f.target == 1 && f.installs == 1);
        f.look.event(false, 7, -3, true);
        long x = 0, y = 0;
        assert(f.look.take(x, y) && x == 7 && y == -3);
        assert(f.reconcile(1) && f.installs == 1 && f.removes == 0);
        f.current = {true, true, 2, 0};
        assert(!f.reconcile(1) && !f.active && !f.target && f.removes == 0);
        assert(!f.look.take(x, y)); // Drained relative input must stop overriding cursor deltas.
        assert(f.reconcile(0) && f.current.target == 2 && f.removes == 0);
    }
    {
        Fixture f;
        assert(f.reconcile(1));
        f.current.flags = 0x100; // Same window, but a replacement with different policy.
        assert(!f.reconcile(1) && !f.active && f.current.flags == 0x100 && !f.removes);
        assert(f.reconcile(0) && !f.removes);
    }
    {
        Fixture f;
        f.current = {true, true, 0, 0}; // A host's foreground raw registration has no target HWND.
        assert(!f.reconcile(1) && !f.active && !f.installs && !f.removes);
        f.current = {};
        assert(!f.reconcile(1) && !f.installs && !f.removes);
    }
    {
        Fixture f;
        assert(f.reconcile(1));
        f.look.event(false, 90, 80, true);
        f.current.known = false;
        assert(!f.reconcile(1) && !f.active && f.target == 1 && !f.removes);
        assert(!f.look.relative && !f.look.dx && !f.look.dy);
        f.current.known = true;
        assert(f.reconcile(0) && !f.target && f.removes == 1);
    }
    {
        Fixture f;
        f.installOk = false;
        assert(!f.reconcile(1) && !f.active && !f.target && f.installs == 1);
        f.installOk = true;
        assert(f.reconcile(1) && f.active);
        f.removeOk = false;
        assert(!f.reconcile(2) && !f.active && f.target == 1 && f.installs == 2);
        f.removeOk = true;
        assert(f.reconcile(2) && f.active && f.target == 2 && f.removes == 2);
        assert(f.reconcile(0) && !f.active && !f.target && !f.current.present);
    }
    {
        Fixture f;
        f.replaceOnInstall = true;
        assert(!f.reconcile(1) && !f.active && !f.target && f.current.target == 99);
        assert(f.reconcile(0) && !f.removes && f.current.target == 99);
    }
    puts("RAW REGISTRATION: takeover, flags, foreground owner, query/install/remove failures, "
         "window handoff and relative-latch retirement passed.");
}
