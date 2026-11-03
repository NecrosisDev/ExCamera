#include "hook_broker.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include <MinHook.h>

namespace exanima {
namespace {

bool read_memory(void* output, const void* input, size_t size) noexcept {
#ifdef _MSC_VER
    __try {
        std::memcpy(output, input, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(output, input, size);
    return true;
#endif
}

bool executable_memory(const void* address) noexcept {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information)) return false;
    if (information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (information.Protect & executable) != 0;
}

enum class JumpKind { none, valid, invalid };

struct Jump {
    JumpKind kind = JumpKind::none;
    void* target = nullptr;
};

Jump decode_jump(const void* address) noexcept {
    uint8_t bytes[16]{};
    if (!read_memory(bytes, address, sizeof(bytes))) return {JumpKind::invalid, nullptr};
    const auto current = reinterpret_cast<uintptr_t>(address);
    if (bytes[0] == 0xe9) {
        int32_t displacement;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        return {JumpKind::valid, reinterpret_cast<void*>(current + 5 + static_cast<intptr_t>(displacement))};
    }
    if (bytes[0] == 0xeb) {
        const auto displacement = static_cast<int8_t>(bytes[1]);
        return {JumpKind::valid, reinterpret_cast<void*>(current + 2 + displacement)};
    }
    if (bytes[0] == 0xff && bytes[1] == 0x25) {
        int32_t displacement;
        std::memcpy(&displacement, bytes + 2, sizeof(displacement));
        const auto slot = reinterpret_cast<const void*>(current + 6 + static_cast<intptr_t>(displacement));
        uintptr_t target;
        if (!read_memory(&target, slot, sizeof(target))) return {JumpKind::invalid, nullptr};
        return {JumpKind::valid, reinterpret_cast<void*>(target)};
    }
    if (bytes[0] == 0x48 && bytes[1] == 0xb8 && bytes[10] == 0xff && bytes[11] == 0xe0) {
        uintptr_t target;
        std::memcpy(&target, bytes + 2, sizeof(target));
        return {JumpKind::valid, reinterpret_cast<void*>(target)};
    }
    return {};
}

Result admission_result(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::original: return EXA_RESULT_OK;
        case EntryKind::modified: return EXA_RESULT_ENTRY_MODIFIED;
        case EntryKind::foreign_detour: return EXA_RESULT_FOREIGN_DETOUR;
        case EntryKind::invalid_detour: return EXA_RESULT_INVALID_DETOUR;
        case EntryKind::not_readable: return EXA_RESULT_ENTRY_NOT_READABLE;
        case EntryKind::not_executable: return EXA_RESULT_ENTRY_NOT_EXECUTABLE;
        default: return EXA_RESULT_BACKEND_FAILURE;
    }
}

} // namespace

EntryInspection inspect_entry(const void* target, std::span<const uint8_t> expected) noexcept {
    if (!executable_memory(target)) return {EntryKind::not_executable, nullptr};
    uint8_t actual[32]{};
    if (expected.empty() || expected.size() > sizeof(actual)) return {EntryKind::not_readable, nullptr};
    if (!read_memory(actual, target, expected.size())) return {EntryKind::not_readable, nullptr};
    if (std::memcmp(actual, expected.data(), expected.size()) == 0) {
        return {EntryKind::original, target};
    }

    const void* seen[8]{};
    const void* current = target;
    for (size_t count = 0; count < 8; ++count) {
        for (size_t index = 0; index < count; ++index) {
            if (seen[index] == current) return {EntryKind::invalid_detour, current};
        }
        seen[count] = current;
        if (!executable_memory(current)) return {EntryKind::invalid_detour, current};
        const Jump jump = decode_jump(current);
        if (jump.kind == JumpKind::invalid) return {EntryKind::invalid_detour, current};
        if (jump.kind == JumpKind::none) {
            return {count == 0 ? EntryKind::modified : EntryKind::foreign_detour, current};
        }
        current = jump.target;
    }
    return {EntryKind::invalid_detour, current};
}

HookBroker::~HookBroker() {
    (void)stop();
}

HookBroker::Record* HookBroker::find(std::string_view id) noexcept {
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
        return record.id == id;
    });
    return iterator == records_.end() ? nullptr : &*iterator;
}

const HookBroker::Record* HookBroker::find(std::string_view id) const noexcept {
    const auto iterator = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
        return record.id == id;
    });
    return iterator == records_.end() ? nullptr : &*iterator;
}

size_t HookBroker::active_count() const noexcept {
    return static_cast<size_t>(std::count_if(records_.begin(), records_.end(), [](const Record& record) {
        return record.active;
    }));
}

Result HookBroker::initialize() {
    if (backend_ready_) return EXA_RESULT_OK;
    if (!records_.empty()) return EXA_RESULT_INVALID_STATE;
    const MH_STATUS status = MH_Initialize();
    if (status == MH_ERROR_ALREADY_INITIALIZED) return EXA_RESULT_ALREADY_INITIALIZED;
    if (status != MH_OK) return EXA_RESULT_BACKEND_FAILURE;
    backend_ready_ = true;
    return EXA_RESULT_OK;
}

Result HookBroker::install(const HookDefinition& definition) noexcept {
    if (!backend_ready_) return EXA_RESULT_INVALID_STATE;
    if (definition.id.empty() || !definition.target || !definition.detour || !definition.original ||
        definition.expected_entry.empty() || definition.expected_entry.size() > 32) {
        return EXA_RESULT_INVALID_ARGUMENT;
    }
    if (find(definition.id)) return EXA_RESULT_DUPLICATE_ID;
    if (!executable_memory(definition.detour)) return EXA_RESULT_ENTRY_NOT_EXECUTABLE;
    const EntryInspection inspection = inspect_entry(definition.target, definition.expected_entry);
    if (inspection.kind != EntryKind::original) return admission_result(inspection.kind);

    // Installation is transactional: every failure preserves the caller's previous value.
    void* const previous = *definition.original;
    const MH_STATUS created = MH_CreateHook(definition.target, definition.detour, definition.original);
    if (created != MH_OK) {
        *definition.original = previous;
        return EXA_RESULT_BACKEND_FAILURE;
    }
    try {
        records_.push_back(Record{
            definition.id,
            definition.target,
            definition.detour,
            definition.original,
            std::vector<uint8_t>(definition.expected_entry.begin(), definition.expected_entry.end()),
            false,
        });
    } catch (...) {
        (void)MH_RemoveHook(definition.target);
        *definition.original = previous;
        return EXA_RESULT_BACKEND_FAILURE;
    }
    const MH_STATUS enabled = MH_EnableHook(definition.target);
    if (enabled != MH_OK) {
        (void)MH_RemoveHook(definition.target);
        records_.pop_back();
        *definition.original = previous;
        return EXA_RESULT_BACKEND_FAILURE;
    }
    records_.back().active = true;
    return EXA_RESULT_OK;
}

Result HookBroker::enable(std::string_view id) noexcept {
    if (!backend_ready_) return EXA_RESULT_INVALID_STATE;
    Record* record = find(id);
    if (!record) return EXA_RESULT_NOT_FOUND;
    if (record->active) return EXA_RESULT_OK;
    const EntryInspection inspection = inspect_entry(record->target, record->expected_entry);
    if (inspection.kind != EntryKind::original) return admission_result(inspection.kind);
    if (MH_EnableHook(record->target) != MH_OK) return EXA_RESULT_BACKEND_FAILURE;
    record->active = true;
    return EXA_RESULT_OK;
}

Result HookBroker::disable(std::string_view id) noexcept {
    if (!backend_ready_) return EXA_RESULT_INVALID_STATE;
    Record* record = find(id);
    if (!record) return EXA_RESULT_NOT_FOUND;
    if (!record->active) return EXA_RESULT_OK;
    if (MH_DisableHook(record->target) != MH_OK) return EXA_RESULT_BACKEND_FAILURE;
    record->active = false;
    return EXA_RESULT_OK;
}

Result HookBroker::stop() noexcept {
    Result first_error = EXA_RESULT_OK;
    for (size_t index = records_.size(); index-- > 0;) {
        Record& record = records_[index];
        if (!record.active) continue;
        if (MH_DisableHook(record.target) != MH_OK) {
            first_error = EXA_RESULT_BACKEND_FAILURE;
            continue;
        }
        record.active = false;
        if (record.original) *record.original = nullptr;
    }
    for (size_t index = records_.size(); index-- > 0;) {
        Record& record = records_[index];
        if (record.active) continue;
        if (MH_RemoveHook(record.target) != MH_OK) {
            first_error = EXA_RESULT_BACKEND_FAILURE;
            continue;
        }
        if (record.original) *record.original = nullptr;
        records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    if (records_.empty() && backend_ready_) {
        if (MH_Uninitialize() == MH_OK) backend_ready_ = false;
        else first_error = EXA_RESULT_BACKEND_FAILURE;
    }
    return first_error;
}

} // namespace exanima
