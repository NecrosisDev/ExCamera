#include "hook_broker.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "../test_support.hpp"

namespace {

using Fn = int(*)();
Fn chained = nullptr;

int replacement() {
    return chained ? chained() + 100 : -1;
}

constexpr auto original_code = std::to_array<uint8_t>({0xb8, 17, 0, 0, 0, 0xc3});

struct Code {
    static std::array<uint8_t, 6> returning(uint8_t value) {
        return {0xb8, value, 0, 0, 0, 0xc3};
    }

    static std::array<uint8_t, 14> absolute_jump(const void* target) {
        std::array<uint8_t, 14> bytes{0xff, 0x25, 0, 0, 0, 0};
        const uintptr_t value = reinterpret_cast<uintptr_t>(target);
        std::memcpy(bytes.data() + 6, &value, sizeof(value));
        return bytes;
    }
};

class ExecutableMemory {
public:
    ExecutableMemory() : memory_(static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) {
        if (memory_) {
            std::memset(memory_, 0x90, 4096);
            write(original_code);
        }
    }
    ~ExecutableMemory() {
        if (memory_) VirtualFree(memory_, 0, MEM_RELEASE);
    }

    bool valid() const noexcept { return memory_ != nullptr; }
    void* address() const noexcept { return memory_; }
    int call() const noexcept { return reinterpret_cast<Fn>(memory_)(); }

    template <size_t Size>
    void write(const std::array<uint8_t, Size>& bytes) {
        std::memcpy(memory_, bytes.data(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), memory_, bytes.size());
    }

    template <size_t Size>
    void write(const uint8_t (&bytes)[Size]) {
        std::memcpy(memory_, bytes, Size);
        FlushInstructionCache(GetCurrentProcess(), memory_, Size);
    }

    bool protect(DWORD protection) {
        DWORD previous;
        return VirtualProtect(memory_, 4096, protection, &previous) != 0;
    }

private:
    uint8_t* memory_ = nullptr;
};

exanima::HookDefinition definition(const char* id, void* target, std::span<const uint8_t> expected = original_code) {
    return {id, target, reinterpret_cast<void*>(&replacement), reinterpret_cast<void**>(&chained), expected};
}

bool original_hook_disable_rearm_and_stop() {
    ExecutableMemory memory;
    CHECK(memory.valid());
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.test", memory.address())) == EXA_RESULT_OK);
    CHECK(memory.call() == 117);
    CHECK(broker.active_count() == 1);
    CHECK(broker.install(definition("camera.test", memory.address())) == EXA_RESULT_DUPLICATE_ID);
    CHECK(chained != nullptr);
    CHECK(memory.call() == 117);
    CHECK(broker.disable("camera.test") == EXA_RESULT_OK);
    CHECK(memory.call() == 17);
    CHECK(broker.enable("camera.test") == EXA_RESULT_OK);
    CHECK(memory.call() == 117);
    CHECK(broker.stop() == EXA_RESULT_OK);
    CHECK(memory.call() == 17);
    CHECK(chained == nullptr);
    CHECK(broker.installed_count() == 0);
    CHECK(!broker.ready());
    return true;
}

bool changed_original_entry_is_refused() {
    ExecutableMemory memory;
    CHECK(memory.valid());
    memory.write(Code::returning(18));
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.changed", memory.address())) == EXA_RESULT_ENTRY_MODIFIED);
    CHECK(memory.call() == 18);
    CHECK(broker.installed_count() == 0);
    CHECK(broker.stop() == EXA_RESULT_OK);
    return true;
}

bool foreign_detour_is_refused() {
    ExecutableMemory memory;
    ExecutableMemory foreign;
    CHECK(memory.valid() && foreign.valid());
    foreign.write(Code::returning(23));
    memory.write(Code::absolute_jump(foreign.address()));
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.foreign", memory.address())) == EXA_RESULT_FOREIGN_DETOUR);
    CHECK(memory.call() == 23);
    CHECK(broker.installed_count() == 0);
    CHECK(broker.stop() == EXA_RESULT_OK);
    return true;
}

bool cyclic_detour_is_refused() {
    ExecutableMemory first;
    ExecutableMemory second;
    CHECK(first.valid() && second.valid());
    first.write(Code::absolute_jump(second.address()));
    second.write(Code::absolute_jump(first.address()));
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.cycle", first.address())) == EXA_RESULT_INVALID_DETOUR);
    CHECK(broker.installed_count() == 0);
    CHECK(broker.stop() == EXA_RESULT_OK);
    return true;
}

bool invalid_detour_target_is_refused() {
    ExecutableMemory memory;
    CHECK(memory.valid());
    memory.write(Code::absolute_jump(nullptr));
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.invalid", memory.address())) == EXA_RESULT_INVALID_DETOUR);
    CHECK(broker.installed_count() == 0);
    CHECK(broker.stop() == EXA_RESULT_OK);
    return true;
}

bool non_executable_target_is_refused() {
    ExecutableMemory memory;
    CHECK(memory.valid());
    CHECK(memory.protect(PAGE_READWRITE));
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.nonexec", memory.address())) == EXA_RESULT_ENTRY_NOT_EXECUTABLE);
    CHECK(broker.installed_count() == 0);
    CHECK(memory.protect(PAGE_EXECUTE_READWRITE));
    CHECK(broker.stop() == EXA_RESULT_OK);
    return true;
}

bool changed_code_cannot_be_rearmed() {
    ExecutableMemory memory;
    CHECK(memory.valid());
    exanima::HookBroker broker;
    CHECK(broker.initialize() == EXA_RESULT_OK);
    CHECK(broker.install(definition("camera.rearm", memory.address())) == EXA_RESULT_OK);
    CHECK(broker.disable("camera.rearm") == EXA_RESULT_OK);
    memory.write(Code::returning(19));
    CHECK(broker.enable("camera.rearm") == EXA_RESULT_ENTRY_MODIFIED);
    CHECK(memory.call() == 19);
    CHECK(broker.stop() == EXA_RESULT_OK);
    CHECK(chained == nullptr);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"original hook disable rearm and stop", original_hook_disable_rearm_and_stop},
        {"changed original entry is refused", changed_original_entry_is_refused},
        {"foreign detour is refused", foreign_detour_is_refused},
        {"cyclic detour is refused", cyclic_detour_is_refused},
        {"invalid detour target is refused", invalid_detour_target_is_refused},
        {"non-executable target is refused", non_executable_target_is_refused},
        {"changed code cannot be rearmed", changed_code_cannot_be_rearmed},
    });
}
