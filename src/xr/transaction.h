#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace fp::xr {
// Byte-exact undo for native fields touched during an eye replay. Capture order is restore
// order reversed, so nested owners unwind correctly; destruction restores unless abandoned.
class Transaction {
    struct Block { void* address; size_t offset, size; };
    std::vector<Block> blocks_;
    std::vector<unsigned char> bytes_;
    bool (*readable_)(uintptr_t, size_t);
    static constexpr size_t limit = 4 * 1024 * 1024;

public:
    explicit Transaction(bool (*readable)(uintptr_t, size_t)) : readable_(readable) {}
    void capture(uintptr_t address, size_t size) {
        if (!size || size > limit - bytes_.size() || !readable_(address, size))
            throw std::runtime_error("stereo snapshot outside admitted memory");
        const size_t offset = bytes_.size();
        bytes_.resize(offset + size);
        std::memcpy(bytes_.data() + offset, reinterpret_cast<void*>(address), size);
        blocks_.push_back({reinterpret_cast<void*>(address), offset, size});
    }
    void restore() noexcept {
        for (auto i = blocks_.rbegin(); i != blocks_.rend(); ++i) std::memcpy(i->address, bytes_.data() + i->offset, i->size);
    }
    // For an owner that no longer exists: restoring would write into a successor's memory.
    void abandon() noexcept { blocks_.clear(); bytes_.clear(); }
    ~Transaction() { restore(); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};
}
