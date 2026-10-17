#pragma once
#include "game_layout.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace fp {

struct ContractOwner {
    const char* name;
    sig::Target target;
    const char* pattern;
    int32_t entryOffset;
};

struct FieldContract {
    const char* name;
    unsigned owner;
    uint32_t offset;
    const char* pattern;
    const char* optionalGroup;
    // Global-table references keep their instruction shape, but their rel32 is
    // relocation-sensitive. Check its destination against the resolved global.
    sig::Target relativeTo = sig::targetCount;
    uint32_t displacementOffset = 0, nextOffset = 0;
    int32_t relativeAdd = 0;
    uint32_t relativeSize = 1;
};

enum class ContractFailure : uint8_t {
    none, unresolvedLayout, badPattern, ownerMissing, ownerAmbiguous, ownerDisagrees, outside, changed,
    referenceDisagrees
};

inline const char* contractFailureText(ContractFailure why) {
    switch (why) {
    case ContractFailure::none: return "ok";
    case ContractFailure::unresolvedLayout: return "game targets are unresolved";
    case ContractFailure::badPattern: return "invalid contract pattern";
    case ContractFailure::ownerMissing: return "native owner not found";
    case ContractFailure::ownerAmbiguous: return "native owner is ambiguous";
    case ContractFailure::ownerDisagrees: return "native owner disagrees with resolved target";
    case ContractFailure::outside: return "native access is outside executable code";
    case ContractFailure::changed: return "native field access changed";
    case ContractFailure::referenceDisagrees: return "native global-table reference changed";
    }
    return "unknown";
}

struct ContractReport {
    unsigned checked = 0;
    unsigned failures = 0;
    unsigned optionalFailures = 0;
    std::vector<ContractFailure> why;
    const char* firstRequired = nullptr;
    const char* firstOptional = nullptr;
    ContractFailure firstRequiredReason = ContractFailure::none;
    ContractFailure firstOptionalReason = ContractFailure::none;
};

namespace detail {
inline bool validateContractTables(const ImageView& file, Layout& layout, ContractReport& report,
                                   const ContractOwner* owners, size_t ownerCount,
                                   const FieldContract* fields, size_t fieldCount) {
    report = ContractReport{};
    report.why.assign(fieldCount, ContractFailure::none);
    std::vector<ContractFailure> ownerWhy(ownerCount, ContractFailure::none);
    std::vector<uint32_t> addresses(ownerCount, 0);
    for (size_t i = 0; i < ownerCount; ++i) {
        if (!layout.resolved) {
            ownerWhy[i] = ContractFailure::unresolvedLayout;
            continue;
        }
        if (!file.data || file.textBegin >= file.textEnd || file.textEnd > file.size) {
            ownerWhy[i] = ContractFailure::outside;
            continue;
        }
        const ContractOwner& owner = owners[i];
        Pattern pattern;
        uint32_t at = 0;
        if (!owner.pattern || !parsePattern(owner.pattern, pattern)) {
            ownerWhy[i] = ContractFailure::badPattern;
            continue;
        }
        const unsigned matches = findPattern(file, pattern, file.textBegin, file.textEnd, &at);
        if (matches != 1) {
            ownerWhy[i] = matches ? ContractFailure::ownerAmbiguous : ContractFailure::ownerMissing;
            continue;
        }
        const int64_t entry = int64_t(at) + owner.entryOffset;
        if (entry < file.textBegin || entry >= file.textEnd) {
            ownerWhy[i] = ContractFailure::outside;
            continue;
        }
        if (owner.target < sig::targetCount && layout.rva[owner.target] &&
            layout.rva[owner.target] != uint32_t(entry)) {
            ownerWhy[i] = ContractFailure::ownerDisagrees;
            continue;
        }
        addresses[i] = uint32_t(entry);
    }
    Layout accepted = layout;
    for (size_t i = 0; i < fieldCount; ++i) {
        const FieldContract& field = fields[i];
        ContractFailure why = field.owner < ownerCount ? ownerWhy[field.owner] : ContractFailure::badPattern;
        if (why == ContractFailure::none) {
            Pattern pattern;
            const uint64_t at = uint64_t(addresses[field.owner]) + field.offset;
            if (!field.pattern || !parsePattern(field.pattern, pattern))
                why = ContractFailure::badPattern;
            else if (at < file.textBegin || at > file.textEnd || pattern.bytes.size() > file.textEnd - at)
                why = ContractFailure::outside;
            else {
                for (size_t byte = 0; byte < pattern.bytes.size(); ++byte)
                    if (pattern.mask[byte] && file.data[size_t(at) + byte] != pattern.bytes[byte]) {
                        why = ContractFailure::changed;
                        break;
                    }
                if (why == ContractFailure::none && field.relativeTo < sig::targetCount) {
                    if (field.displacementOffset > pattern.bytes.size() ||
                        pattern.bytes.size() - field.displacementOffset < sizeof(int32_t) ||
                        field.nextOffset > pattern.bytes.size())
                        why = ContractFailure::badPattern;
                    else {
                        int32_t relative = 0;
                        memcpy(&relative, file.data + size_t(at) + field.displacementOffset, sizeof relative);
                        const int64_t target = int64_t(at) + field.nextOffset + relative;
                        const int64_t expected = int64_t(layout.rva[field.relativeTo]) + field.relativeAdd;
                        if (!layout.rva[field.relativeTo] || !field.relativeSize || target < 0 ||
                            target > int64_t(file.size) || field.relativeSize > file.size - size_t(target) ||
                            expected != target)
                            why = ContractFailure::referenceDisagrees;
                    }
                }
            }
        }
        ++report.checked;
        report.why[i] = why;
        if (why == ContractFailure::none) continue;
        ++report.failures;
        if (field.optionalGroup && *field.optionalGroup) {
            bool declared = false;
            for (unsigned target = 0; target < sig::targetCount; ++target)
                if (sig::targetOptionalGroups[target] &&
                    !strcmp(sig::targetOptionalGroups[target], field.optionalGroup)) {
                    accepted.rva[target] = 0;
                    declared = true;
                }
            if (declared) {
                ++report.optionalFailures;
                if (!report.firstOptional) {
                    report.firstOptional = field.name;
                    report.firstOptionalReason = why;
                }
                continue;
            }
        }
        if (!report.firstRequired) {
            report.firstRequired = field.name;
            report.firstRequiredReason = why;
        }
    }
    if (!layout.resolved || report.failures != report.optionalFailures) return false;
    layout = accepted;
    return true;
}
} // namespace detail

} // namespace fp

#include "game_contracts.h"

namespace fp {
inline bool validateNativeContracts(const ImageView& file, Layout& layout, ContractReport& report) {
    return detail::validateContractTables(file, layout, report, contract::owners,
                                         sizeof contract::owners / sizeof *contract::owners,
                                         contract::fields, sizeof contract::fields / sizeof *contract::fields);
}
} // namespace fp
