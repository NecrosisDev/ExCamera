#pragma once
#include <cstdint>

namespace fp {

struct RawRegistration {
    bool known = false, present = false;
    uintptr_t target = 0;
    uint32_t flags = 0;
    bool matches(uintptr_t owner) const {
        return owner && known && present && target == owner && flags == 0;
    }
};

// `target` remembers a registration we made; each operation needs a fresh
// observation before treating it as ours. Windows offers no atomic compare/remove.
template <class Query, class Install, class Remove, class Publish>
bool reconcileRawRegistration(uintptr_t desired, uintptr_t& target, Query query, Install install,
                              Remove remove, Publish publish) {
    if (!desired && !target) {
        publish(false);
        return true;
    }
    RawRegistration current = query();
    if (!current.known) {
        publish(false);
        return false; // Retain the candidate so a later successful query can retire it.
    }
    if (target && !current.matches(target)) {
        target = 0;
        publish(false);
    }
    if (target && target != desired) {
        publish(false);
        if (!remove()) return false;
        target = 0;
        current = query(); // Another component may register during the handoff.
    }
    if (!desired) {
        publish(false);
        return true;
    }
    if (current.matches(target) && target == desired) {
        publish(true);
        return true;
    }
    if (!current.known || current.present || !install(desired)) {
        publish(false);
        return false;
    }
    target = desired;
    current = query(); // A successful call alone does not establish continued ownership.
    if (!current.matches(target)) {
        if (current.known) target = 0;
        publish(false);
        return false;
    }
    publish(true);
    return true;
}

} // namespace fp
