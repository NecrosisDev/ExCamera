#pragma once
// A callable export of the image actually loaded in the destination process.
// Reading an RVA from a newer file beside the launcher can select unrelated
// code in an older module that Windows still has mapped. No local file is used
// here. Read must copy exactly the requested bytes from a module-relative RVA.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fp {

template <class Read>
uint32_t loadedExportRva(uint32_t moduleSize, const char* name, Read read) {
    const size_t nameSize = name ? std::strlen(name) + 1 : 0;
    if (nameSize < 2 || nameSize > 64) return 0;
    auto range = [&](uint64_t at, uint64_t size) {
        return at <= moduleSize && size <= moduleSize - at;
    };
    auto bytes = [&](uint32_t at, void* out, size_t size) {
        return range(at, size) && read(at, out, size);
    };
    auto u16 = [](const uint8_t* p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); };
    auto u32 = [&](const uint8_t* p) { return uint32_t(u16(p) | uint32_t(u16(p + 2)) << 16); };
    uint8_t dos[64], nt[24 + 120];
    if (!bytes(0, dos, sizeof dos) || u16(dos) != 0x5a4d) return 0;
    const uint32_t pe = u32(dos + 60);
    if (!bytes(pe, nt, sizeof nt) || u32(nt) != 0x4550 || u16(nt + 4) != 0x8664 ||
        u16(nt + 24) != 0x20b || u16(nt + 20) < 120 || !u32(nt + 24 + 108))
        return 0;
    const uint32_t imageSize = u32(nt + 24 + 56);
    if (!imageSize || imageSize > moduleSize) return 0;
    moduleSize = imageSize;
    const unsigned sections = u16(nt + 6);
    const uint64_t sectionTable = uint64_t(pe) + 24 + u16(nt + 20);
    if (!sections || sections > 96 || !range(sectionTable, sections * 40ull)) return 0;
    const uint32_t exports = u32(nt + 24 + 112), exportSize = u32(nt + 24 + 116);
    uint8_t directory[40];
    if (!exports || exportSize < sizeof directory || !range(exports, exportSize) ||
        !bytes(exports, directory, sizeof directory))
        return 0;
    const uint32_t functions = u32(directory + 20), names = u32(directory + 24);
    const uint32_t addresses = u32(directory + 28), namePointers = u32(directory + 32),
                   ordinals = u32(directory + 36);
    // Named exports use 16-bit indices. These bounds also prevent unbounded
    // remote reads if a damaged header claims an enormous export table.
    if (!functions || functions > 65536 || !names || names > 65536 ||
        !range(addresses, functions * 4ull) || !range(namePointers, names * 4ull) ||
        !range(ordinals, names * 2ull))
        return 0;
    for (uint32_t i = 0; i < names; ++i) {
        uint8_t value[4];
        char exported[64];
        if (!bytes(namePointers + i * 4, value, 4) ||
            !bytes(u32(value), exported, nameSize))
            return 0;
        if (std::memcmp(exported, name, nameSize)) continue;
        if (!bytes(ordinals + i * 2, value, 2)) return 0;
        const uint16_t ordinal = u16(value);
        if (ordinal >= functions || !bytes(addresses + ordinal * 4, value, 4)) return 0;
        const uint32_t target = u32(value);
        // A forwarder points to text in the export directory, not a function.
        if (!target || !range(target, 1) || (target >= exports && target - exports < exportSize))
            return 0;
        for (unsigned s = 0; s < sections; ++s) {
            uint8_t section[40];
            if (!bytes(uint32_t(sectionTable + s * 40), section, sizeof section)) return 0;
            const uint32_t at = u32(section + 12), virtualSize = u32(section + 8),
                           span = virtualSize ? virtualSize : u32(section + 16);
            if (!range(at, span)) return 0;
            if (target >= at && target - at < span)
                return (u32(section + 36) & 0x20000000) ? target : 0; // executable section
        }
        return 0;
    }
    return 0;
}

} // namespace fp
