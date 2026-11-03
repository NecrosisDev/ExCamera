#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <exanima/result.h>

namespace exanima {

enum class EntryKind {
    original,
    modified,
    foreign_detour,
    invalid_detour,
    not_readable,
    not_executable,
};

struct EntryInspection {
    EntryKind kind = EntryKind::not_readable;
    const void* final_target = nullptr;
};

EntryInspection inspect_entry(const void* target, std::span<const uint8_t> expected) noexcept;

struct HookDefinition {
    std::string id;
    void* target = nullptr;
    void* detour = nullptr;
    void** original = nullptr;
    std::span<const uint8_t> expected_entry;
};

class HookBroker {
public:
    HookBroker() = default;
    ~HookBroker();
    HookBroker(const HookBroker&) = delete;
    HookBroker& operator=(const HookBroker&) = delete;

    Result initialize();
    Result install(const HookDefinition& definition) noexcept;
    Result enable(std::string_view id) noexcept;
    Result disable(std::string_view id) noexcept;
    Result stop() noexcept;

    bool ready() const noexcept { return backend_ready_; }
    size_t installed_count() const noexcept { return records_.size(); }
    size_t active_count() const noexcept;

private:
    struct Record {
        std::string id;
        void* target = nullptr;
        void* detour = nullptr;
        void** original = nullptr;
        std::vector<uint8_t> expected_entry;
        bool active = false;
    };

    Record* find(std::string_view id) noexcept;
    const Record* find(std::string_view id) const noexcept;

    std::vector<Record> records_;
    bool backend_ready_ = false;
};

} // namespace exanima
