#pragma once
#include "game_layout.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vendor/minhook/src/hde/hde64.h"

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

struct FieldOffsetBinding {
    uint32_t witnessIndex;
    uint32_t insnOffset;
    int32_t expectedDisp;
    bool negated;
    size_t defaultValue;
    size_t* liveTarget;
};

enum class ClosureKind : uint8_t { intra, target, owner, callee };

struct ClosureCallee {
    const char* name;
    const char* pattern;
};

struct ClosureBranch {
    const char* group;
    uint32_t witnessIndex;
    uint16_t dispOffset;
    uint16_t insnEnd;
    ClosureKind kind;
    int32_t indexOrOffset;
};

struct ClosureVmtSlot {
    const char* group;
    sig::Target vmtTarget;
    uint32_t slotOffset;
    ClosureKind kind;
    uint32_t targetOrCallee;
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
    unsigned adaptedFields = 0;
    std::vector<ContractFailure> why;
    const char* firstRequired = nullptr;
    const char* firstOptional = nullptr;
    ContractFailure firstRequiredReason = ContractFailure::none;
    ContractFailure firstOptionalReason = ContractFailure::none;
};

namespace detail {

struct ObservedOffset {
    size_t* liveTarget = nullptr;
    size_t witnessIndex = 0;
    size_t value = 0;
    size_t defaultValue = 0;
};

inline bool matchPatternAt(const ImageView& file, uint32_t at, const Pattern& pat) noexcept {
    if (!file.data || at < file.textBegin || at > file.textEnd || pat.bytes.size() > file.textEnd - at)
        return false;
    for (size_t i = 0; i < pat.bytes.size(); ++i)
        if (pat.mask[i] && file.data[size_t(at) + i] != pat.bytes[i]) return false;
    return true;
}

inline bool tryAdaptWitnessDisplacements(const ImageView& file, uint32_t at, const Pattern& pattern,
                                         size_t witnessIdx, const FieldOffsetBinding* bindings,
                                         size_t bindingCount, std::vector<ObservedOffset>& outObserved) {
    if (!bindings || bindingCount == 0) return false;
    bool hasAnyBinding = false;
    for (size_t b = 0; b < bindingCount; ++b)
        if (bindings[b].witnessIndex == witnessIdx) {
            hasAnyBinding = true;
            break;
        }
    if (!hasAnyBinding) return false;

    const size_t len = pattern.bytes.size();
    std::vector<ObservedOffset> localObs;
    size_t off = 0;
    while (off < len) {
        uint8_t expBuf[16]{};
        uint8_t actBuf[16]{};
        const size_t rem = len - off < 16 ? len - off : 16;
        std::memcpy(expBuf, pattern.bytes.data() + off, rem);
        const size_t fileRem = file.size - (size_t(at) + off) < 16 ? file.size - (size_t(at) + off) : 16;
        std::memcpy(actBuf, file.data + size_t(at) + off, fileRem);

        hde64s expHs{}, actHs{};
        hde64_disasm(expBuf, &expHs);
        hde64_disasm(actBuf, &actHs);
        if ((expHs.flags & F_ERROR) != 0 || (actHs.flags & F_ERROR) != 0 || expHs.len == 0 ||
            expHs.len != actHs.len || off + expHs.len > len)
            return false;

        bool byteDiffers = false;
        for (size_t k = 0; k < expHs.len; ++k) {
            if (pattern.mask[off + k] && file.data[size_t(at) + off + k] != pattern.bytes[off + k]) {
                byteDiffers = true;
                break;
            }
        }

        if (!byteDiffers) {
            for (size_t b = 0; b < bindingCount; ++b) {
                if (bindings[b].witnessIndex == witnessIdx && bindings[b].insnOffset == off)
                    localObs.push_back({bindings[b].liveTarget, witnessIdx, bindings[b].defaultValue,
                                        bindings[b].defaultValue});
            }
            off += expHs.len;
            continue;
        }

        const uint32_t dispFlags = expHs.flags & (F_DISP8 | F_DISP32);
        if ((expHs.flags & (F_RELATIVE | F_DISP16)) != 0 || (actHs.flags & (F_RELATIVE | F_DISP16)) != 0 ||
            dispFlags == 0 || dispFlags != (actHs.flags & (F_DISP8 | F_DISP32)))
            return false;

        const size_t dispSize = (dispFlags & F_DISP32) ? 4u : 1u;
        const size_t immSize = (expHs.flags & F_IMM64) ? 8u
                             : (expHs.flags & F_IMM32) ? 4u
                             : (expHs.flags & F_IMM16) ? 2u
                             : (expHs.flags & F_IMM8)  ? 1u
                                                       : 0u;
        if (dispSize + immSize >= expHs.len) return false;
        const size_t dispPos = size_t(expHs.len) - immSize - dispSize;

        for (size_t k = 0; k < expHs.len; ++k) {
            if (k >= dispPos && k < dispPos + dispSize) {
                if (!pattern.mask[off + k]) return false;
            } else if (pattern.mask[off + k] && file.data[size_t(at) + off + k] != pattern.bytes[off + k]) {
                return false;
            }
        }

        const int32_t expDisp = dispSize == 4 ? static_cast<int32_t>(expHs.disp.disp32)
                                              : static_cast<int32_t>(static_cast<int8_t>(expHs.disp.disp8));
        const int32_t actDisp = dispSize == 4 ? static_cast<int32_t>(actHs.disp.disp32)
                                              : static_cast<int32_t>(static_cast<int8_t>(actHs.disp.disp8));

        const FieldOffsetBinding* matched = nullptr;
        for (size_t b = 0; b < bindingCount; ++b) {
            if (bindings[b].witnessIndex == witnessIdx && bindings[b].insnOffset == off) {
                if (bindings[b].expectedDisp != expDisp) return false;
                matched = &bindings[b];
                break;
            }
        }
        if (!matched) {
            unsigned fallbackCount = 0;
            for (size_t b = 0; b < bindingCount; ++b) {
                if (bindings[b].witnessIndex == witnessIdx && bindings[b].expectedDisp == expDisp) {
                    matched = &bindings[b];
                    ++fallbackCount;
                }
            }
            if (fallbackCount != 1) return false;
        }

        size_t proposed = 0;
        if (matched->negated) {
            if (actDisp >= 0 || actDisp < -0x10000) return false;
            proposed = static_cast<size_t>(-static_cast<int64_t>(actDisp));
        } else {
            if (actDisp <= 0 || actDisp > 0x100000) return false;
            proposed = static_cast<size_t>(actDisp);
        }
        localObs.push_back({matched->liveTarget, witnessIdx, proposed, matched->defaultValue});
        off += expHs.len;
    }

    outObserved.insert(outObserved.end(), localObs.begin(), localObs.end());
    return true;
}

inline bool validateContractTables(const ImageView& file, Layout& layout, ContractReport& report,
                                   const ContractOwner* owners, size_t ownerCount,
                                   const FieldContract* fields, size_t fieldCount,
                                   const FieldOffsetBinding* bindings = nullptr, size_t bindingCount = 0,
                                   void (*onReset)() = nullptr, void (*onCommit)() = nullptr) {
    if (onReset) onReset();
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

    std::vector<ContractFailure> witnessWhy(fieldCount, ContractFailure::none);
    std::vector<ObservedOffset> observed;
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
                if (why == ContractFailure::none) {
                    for (size_t b = 0; b < bindingCount; ++b)
                        if (bindings[b].witnessIndex == i)
                            observed.push_back({bindings[b].liveTarget, i, bindings[b].defaultValue,
                                                bindings[b].defaultValue});
                } else if (why == ContractFailure::changed &&
                           tryAdaptWitnessDisplacements(file, uint32_t(at), pattern, i, bindings, bindingCount,
                                                        observed)) {
                    why = ContractFailure::none;
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
        witnessWhy[i] = why;
    }

    // Cross-witness / multi-access agreement: if two observations of the same live field disagree,
    // fail every witness touching that field rather than guessing between conflicting offsets.
    for (size_t a = 0; a < observed.size(); ++a) {
        if (!observed[a].liveTarget || witnessWhy[observed[a].witnessIndex] != ContractFailure::none) continue;
        bool conflict = false;
        for (size_t b = a + 1; b < observed.size(); ++b) {
            if (observed[b].liveTarget != observed[a].liveTarget ||
                witnessWhy[observed[b].witnessIndex] != ContractFailure::none)
                continue;
            if (observed[b].value != observed[a].value) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            for (const auto& obs : observed)
                if (obs.liveTarget == observed[a].liveTarget && witnessWhy[obs.witnessIndex] == ContractFailure::none)
                    witnessWhy[obs.witnessIndex] = ContractFailure::changed;
        }
    }

    Layout accepted = layout;
    for (size_t i = 0; i < fieldCount; ++i) {
        const FieldContract& field = fields[i];
        const ContractFailure why = witnessWhy[i];
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
    if (!layout.resolved || report.failures != report.optionalFailures) {
        if (onReset) onReset();
        return false;
    }
    for (const auto& obs : observed) {
        if (!obs.liveTarget || witnessWhy[obs.witnessIndex] != ContractFailure::none) continue;
        if (*obs.liveTarget != obs.value) {
            *obs.liveTarget = obs.value;
            ++report.adaptedFields;
        }
    }
    if (onCommit) onCommit();
    layout = accepted;
    return true;
}

inline bool validateClosureTables(const ImageView& file, const Layout& layout, const char* group,
                                  const ContractOwner* owners, size_t ownerCount,
                                  const FieldContract* fields, size_t fieldCount,
                                  const ClosureCallee* callees, size_t calleeCount,
                                  const ClosureBranch* branches, size_t branchCount,
                                  const ClosureVmtSlot* vmtSlots, size_t vmtSlotCount,
                                  uintptr_t liveBase = 0,
                                  bool (*liveReadable)(uintptr_t, size_t) = nullptr) {
    if (!file.data || file.textBegin >= file.textEnd || file.textEnd > file.size ||
        !layout.resolved || !group || !*group || !layout.optionalGroupAvailable(group))
        return false;

    auto checkLiveEqualsFile = [&](uint32_t rva, size_t bytes) -> bool {
        if (!liveBase) return true;
        if (bytes == 0 || uint64_t(rva) + bytes > file.size) return false;
        const uintptr_t addr = liveBase + static_cast<uintptr_t>(rva);
        if (liveReadable && !liveReadable(addr, bytes)) return false;
        return std::memcmp(reinterpret_cast<const void*>(addr), file.data + rva, bytes) == 0;
    };

    std::vector<uint32_t> addresses(ownerCount, 0);
    auto resolveOwner = [&](size_t idx) -> bool {
        if (idx >= ownerCount) return false;
        if (addresses[idx]) return true;
        const ContractOwner& owner = owners[idx];
        Pattern pat;
        uint32_t at = 0;
        if (!owner.pattern || !parsePattern(owner.pattern, pat) ||
            findPattern(file, pat, file.textBegin, file.textEnd, &at) != 1)
            return false;
        const int64_t entry = int64_t(at) + owner.entryOffset;
        if (entry < file.textBegin || entry >= file.textEnd) return false;
        if (owner.target < sig::targetCount && layout.rva[owner.target] != uint32_t(entry))
            return false;
        addresses[idx] = uint32_t(entry);
        return true;
    };

    unsigned checkedWitnesses = 0;
    for (size_t i = 0; i < fieldCount; ++i) {
        const FieldContract& f = fields[i];
        if (!f.optionalGroup || std::strcmp(f.optionalGroup, group) != 0) continue;
        if (!resolveOwner(f.owner)) return false;
        Pattern pat;
        const uint64_t at = uint64_t(addresses[f.owner]) + f.offset;
        if (!f.pattern || !parsePattern(f.pattern, pat) || at < file.textBegin || at > file.textEnd ||
            pat.bytes.size() > file.textEnd - at)
            return false;
        if (!checkLiveEqualsFile(uint32_t(at), pat.bytes.size())) return false;
        ++checkedWitnesses;
    }
    if (!checkedWitnesses) return false;

    auto checkCalleeAt = [&](size_t calleeIdx, uint32_t destRva) -> bool {
        if (calleeIdx >= calleeCount) return false;
        Pattern pat;
        if (!callees[calleeIdx].pattern || !parsePattern(callees[calleeIdx].pattern, pat))
            return false;
        if (!matchPatternAt(file, destRva, pat)) return false;
        // Import thunk `jmp qword ptr [rip + disp32]`: verify IAT cell is inside image outside .text.
        if (pat.bytes.size() == 6 && file.data[destRva] == 0xFF && file.data[destRva + 1] == 0x25) {
            int32_t iatDisp = 0;
            std::memcpy(&iatDisp, file.data + destRva + 2, sizeof iatDisp);
            const int64_t iatRva = int64_t(destRva) + 6 + iatDisp;
            if (iatRva < 0 || uint64_t(iatRva) + 8 > file.size ||
                (iatRva >= file.textBegin && iatRva < file.textEnd))
                return false;
        }
        return checkLiveEqualsFile(destRva, pat.bytes.size());
    };

    for (size_t i = 0; i < branchCount; ++i) {
        const ClosureBranch& b = branches[i];
        if (!b.group || std::strcmp(b.group, group) != 0) continue;
        if (b.witnessIndex >= fieldCount) return false;
        const FieldContract& w = fields[b.witnessIndex];
        if (!resolveOwner(w.owner)) return false;
        const uint64_t wAt = uint64_t(addresses[w.owner]) + w.offset;
        if (wAt + b.dispOffset + 4 > file.textEnd || b.dispOffset + 4 > b.insnEnd) return false;
        int32_t rel = 0;
        std::memcpy(&rel, file.data + size_t(wAt) + b.dispOffset, sizeof rel);
        const int64_t dest = int64_t(wAt) + b.insnEnd + rel;
        if (dest < file.textBegin || dest >= file.textEnd) return false;
        const uint32_t destRva = static_cast<uint32_t>(dest);

        switch (b.kind) {
        case ClosureKind::intra:
            if (dest != int64_t(addresses[w.owner]) + b.indexOrOffset) return false;
            break;
        case ClosureKind::target: {
            if (b.indexOrOffset < 0 || static_cast<unsigned>(b.indexOrOffset) >= sig::targetCount)
                return false;
            const uint32_t tRva = layout.rva[static_cast<unsigned>(b.indexOrOffset)];
            if (!tRva || destRva != tRva || !checkLiveEqualsFile(destRva, 16)) return false;
            break;
        }
        case ClosureKind::owner: {
            if (b.indexOrOffset < 0 || !resolveOwner(static_cast<size_t>(b.indexOrOffset))) return false;
            const uint32_t cEntry = addresses[static_cast<size_t>(b.indexOrOffset)];
            if (destRva != cEntry || !checkLiveEqualsFile(destRva, 16)) return false;
            break;
        }
        case ClosureKind::callee:
            if (b.indexOrOffset < 0 || !checkCalleeAt(static_cast<size_t>(b.indexOrOffset), destRva))
                return false;
            break;
        }
    }

    for (size_t i = 0; i < vmtSlotCount; ++i) {
        const ClosureVmtSlot& vs = vmtSlots[i];
        if (!vs.group || std::strcmp(vs.group, group) != 0) continue;
        if (vs.vmtTarget >= sig::targetCount) return false;
        const uint32_t vmtRva = layout.rva[vs.vmtTarget];
        const uint64_t cellRva = uint64_t(vmtRva) + vs.slotOffset;
        if (!vmtRva || cellRva + sizeof(uint64_t) > file.size) return false;
        uint64_t fileVa = 0;
        std::memcpy(&fileVa, file.data + size_t(cellRva), sizeof fileVa);
        if (fileVa < file.imageBase) return false;
        const uint64_t dest = fileVa - file.imageBase;
        if (dest < file.textBegin || dest >= file.textEnd) return false;
        const uint32_t destRva = static_cast<uint32_t>(dest);

        if (vs.kind == ClosureKind::target) {
            if (vs.targetOrCallee >= sig::targetCount || layout.rva[vs.targetOrCallee] != destRva ||
                !checkLiveEqualsFile(destRva, 16))
                return false;
        } else if (vs.kind == ClosureKind::callee) {
            if (!checkCalleeAt(vs.targetOrCallee, destRva)) return false;
        } else {
            return false;
        }

        if (liveBase) {
            const uintptr_t liveCell = liveBase + static_cast<uintptr_t>(cellRva);
            if (liveReadable && !liveReadable(liveCell, sizeof(uintptr_t))) return false;
            uintptr_t liveVa = 0;
            std::memcpy(&liveVa, reinterpret_cast<const void*>(liveCell), sizeof liveVa);
            if (liveVa != liveBase + static_cast<uintptr_t>(destRva)) return false;
        }
    }

    return true;
}

} // namespace detail

} // namespace fp

#include "game_contracts.h"

namespace fp {
inline bool validateNativeContracts(const ImageView& file, Layout& layout, ContractReport& report) {
    return detail::validateContractTables(
        file, layout, report, contract::owners, sizeof contract::owners / sizeof *contract::owners,
        contract::fields, sizeof contract::fields / sizeof *contract::fields,
        contract::fieldBindings.data(), contract::fieldBindings.size(),
        &game::live::resetLiveOffsets, &game::live::recomputeDerivedOffsets);
}

inline bool validateOptionalClosure(const ImageView& file, const Layout& layout, const char* group,
                                    uintptr_t liveBase = 0,
                                    bool (*liveReadable)(uintptr_t, size_t) = nullptr) {
    return detail::validateClosureTables(
        file, layout, group, contract::owners, sizeof contract::owners / sizeof *contract::owners,
        contract::fields, sizeof contract::fields / sizeof *contract::fields,
        contract::closureCallees.data(), contract::closureCallees.size(),
        contract::closureBranches.data(), contract::closureBranches.size(),
        contract::closureVmtSlots.data(), contract::closureVmtSlots.size(),
        liveBase, liveReadable);
}
} // namespace fp
