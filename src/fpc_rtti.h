#pragma once
// Free Pascal x86_64 VMT / RTTI introspection over a mapped PE image or live module bytes.
// Exanima is compiled with Free Pascal x64: every class VMT in .rdata begins with a 96-byte (0x60)
// header whose first two qwords satisfy vInstanceSize > 0 && vInstanceSize2 == -vInstanceSize,
// followed by vParentRef (+0x10), a Pascal ShortString class-name pointer (+0x18), metadata pointers
// (+0x20..+0x58), and contiguous virtual method pointers into .text starting at +0x60 (vDestroy).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fp::rtti {

inline constexpr uint32_t vmtHeaderSize = 0x60;
inline constexpr uint32_t maxVmtMethods = 160;
inline constexpr int64_t minInstanceSize = 8;
inline constexpr int64_t maxInstanceSize = 0x100000;

struct VmtInfo {
    uint32_t rva = 0;
    uint32_t instanceSize = 0;
    uint32_t parentRefRva = 0;
    uint32_t nameRva = 0;
};

// Reads and validates the non-relocated (vInstanceSize, -vInstanceSize) pair at +0x00/+0x08 of a VMT.
inline bool readVmtInstanceSize(const uint8_t* vmtBytes, size_t availableBytes, uint32_t& sizeOut) noexcept {
    if (!vmtBytes || availableBytes < 16) return false;
    int64_t size1 = 0, size2 = 0;
    std::memcpy(&size1, vmtBytes, sizeof size1);
    std::memcpy(&size2, vmtBytes + sizeof size1, sizeof size2);
    if (size1 < minInstanceSize || size1 > maxInstanceSize || size2 != -size1) return false;
    sizeOut = static_cast<uint32_t>(size1);
    return true;
}

// Finds a unique Free Pascal x64 VMT by exact ShortString class name outside .text.
// Fails closed if zero or more than one VMT matches className.
template <class View>
bool findClassVmt(const View& v, const char* className, VmtInfo& out) noexcept {
    out = VmtInfo{};
    if (!v.data || v.size < vmtHeaderSize + 8 || v.textBegin >= v.textEnd || v.textEnd > v.size ||
        !className || !*className)
        return false;
    const size_t nameLen = std::strlen(className);
    if (nameLen == 0 || nameLen > 127) return false;

    VmtInfo candidate{};
    unsigned matches = 0;
    const size_t limit = v.size - (vmtHeaderSize + 8);
    size_t at = 0;
    while (at <= limit) {
        if (at >= v.textBegin && at < v.textEnd) {
            at = (static_cast<size_t>(v.textEnd) + 7u) & ~size_t(7u);
            continue;
        }
        uint32_t instSize = 0;
        if (!readVmtInstanceSize(v.data + at, 16, instSize)) {
            at += 8;
            continue;
        }
        uint64_t parentRef = 0, nameVa = 0, destroyVa = 0;
        std::memcpy(&parentRef, v.data + at + 0x10, sizeof parentRef);
        std::memcpy(&nameVa, v.data + at + 0x18, sizeof nameVa);
        std::memcpy(&destroyVa, v.data + at + vmtHeaderSize, sizeof destroyVa);

        if (destroyVa < v.imageBase) {
            at += 8;
            continue;
        }
        const uint64_t destroyRva = destroyVa - v.imageBase;
        if (destroyRva < v.textBegin || destroyRva >= v.textEnd) {
            at += 8;
            continue;
        }

        uint32_t parentRva = 0;
        if (parentRef != 0) {
            if (parentRef < v.imageBase || parentRef - v.imageBase > v.size - 8) {
                at += 8;
                continue;
            }
            parentRva = static_cast<uint32_t>(parentRef - v.imageBase);
            if (parentRva >= v.textBegin && parentRva < v.textEnd) {
                at += 8;
                continue;
            }
        }

        if (nameVa < v.imageBase || nameVa - v.imageBase >= v.size) {
            at += 8;
            continue;
        }
        const size_t nrva = static_cast<size_t>(nameVa - v.imageBase);
        if ((nrva >= v.textBegin && nrva < v.textEnd) || v.size - nrva < 1 + nameLen ||
            v.data[nrva] != static_cast<uint8_t>(nameLen) ||
            std::memcmp(v.data + nrva + 1, className, nameLen) != 0) {
            at += 8;
            continue;
        }

        candidate.rva = static_cast<uint32_t>(at);
        candidate.instanceSize = instSize;
        candidate.parentRefRva = parentRva;
        candidate.nameRva = static_cast<uint32_t>(nrva);
        ++matches;
        if (matches > 1) return false;
        at += 8;
    }
    if (matches != 1) return false;
    out = candidate;
    return true;
}

// Locates the virtual method slot inside a validated VMT holding methodRva.
// Checks preferredOffset first; if a base-class method insertion shifted the table, scans contiguous
// .text pointers starting at +0x60 and succeeds only when methodRva appears in a single slot.
template <class View>
bool findMethodSlot(const View& v, uint32_t vmtRva, uint32_t methodRva, uint32_t& slotRvaOut,
                    uint32_t preferredOffset = 0) noexcept {
    slotRvaOut = 0;
    if (!v.data || methodRva < v.textBegin || methodRva >= v.textEnd ||
        vmtRva > v.size || v.size - vmtRva < vmtHeaderSize + 8)
        return false;
    const uint64_t targetVa = v.imageBase + methodRva;
    if (preferredOffset >= vmtHeaderSize && preferredOffset <= v.size - vmtRva - 8) {
        uint64_t held = 0;
        std::memcpy(&held, v.data + vmtRva + preferredOffset, sizeof held);
        if (held == targetVa) {
            slotRvaOut = vmtRva + preferredOffset;
            return true;
        }
    }
    uint32_t matchRva = 0;
    unsigned matches = 0;
    for (uint32_t m = 0; m < maxVmtMethods; ++m) {
        const uint32_t off = vmtHeaderSize + m * 8u;
        if (off > v.size - vmtRva - 8) break;
        uint64_t entry = 0;
        std::memcpy(&entry, v.data + vmtRva + off, sizeof entry);
        if (entry < v.imageBase) break;
        const uint64_t entryRva = entry - v.imageBase;
        if (entryRva < v.textBegin || entryRva >= v.textEnd) break;
        if (entry == targetVa) {
            matchRva = vmtRva + off;
            ++matches;
            if (matches > 1) return false;
        }
    }
    if (matches != 1) return false;
    slotRvaOut = matchRva;
    return true;
}

} // namespace fp::rtti
