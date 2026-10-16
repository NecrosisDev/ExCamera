#pragma once
// A set of non-null pointers for walks over the game's object graphs (open addressing, reused between
// calls, so a walk allocates nothing once it has run). Portable.
#include <cstdint>
#include <vector>

namespace fp {

class PointerSet {
public:
    // Empties the set, sized for up to `capacity` pointers.
    void reset(unsigned capacity) {
        size_t size = 16;
        while (size < size_t(capacity) * 2) size *= 2;
        slots.assign(size, 0);
        mask = size - 1;
    }
    // True if p was not in the set (it is now). p must not be 0, and the set must hold fewer than
    // `capacity` pointers.
    bool insert(uintptr_t p) {
        size_t i = size_t((uint64_t(p) >> 3) * 0x9E3779B97F4A7C15ull >> 20) & mask;
        while (slots[i]) {
            if (slots[i] == p) return false;
            i = (i + 1) & mask;
        }
        slots[i] = p;
        return true;
    }

private:
    std::vector<uintptr_t> slots;
    size_t mask = 0;
};

} // namespace fp
