#pragma once
// Finds every game address ExCamera uses by its code signature (game_signatures.h), instead of
// trusting fixed addresses. Resolving targets establishes their locations; native_contracts.h
// separately checks reviewed object-field instruction windows before activation.
// No Win32 here: the DLL resolves the game's own file, the launcher the same file, and the tests
// any buffer. A layout defaults to the source build's addresses (game_signatures.h knownRva), so
// code that reads it before resolution, and the unit tests' fake memory, see the known build.
#include "game_signatures.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace fp {

// A PE image laid out at its RVAs (as the loader maps it). imageBase: where its absolute pointers
// point (the preferred base for a file, the real base for a loaded module).
struct ImageView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t imageBase = 0;
    uint32_t textBegin = 0, textEnd = 0;
};

// Builds a view of a PE file read into memory: the headers and every section's raw data copied to
// its RVA in `mapped`. False if it is not a well-formed x64 PE.
inline bool mapPeFile(const uint8_t* file, size_t fileSize, std::vector<uint8_t>& mapped, ImageView& view) {
    auto u16 = [&](size_t at) {
        return uint16_t(file[at] | file[at + 1] << 8);
    };
    auto u32 = [&](size_t at) {
        return uint32_t(u16(at) | uint32_t(u16(at + 2)) << 16);
    };
    // Check the subtraction's minimum first: a truncated MZ file must not make
    // fileSize - 0x108 wrap and turn e_lfanew into an unchecked read.
    if (fileSize < 0x108 || u16(0) != 0x5a4d) return false;
    const uint32_t nt = u32(0x3c);
    if (nt > fileSize - 0x108 || u32(nt) != 0x4550 || u16(nt + 4) != 0x8664 || u16(nt + 0x18) != 0x20b)
        return false;
    const unsigned sections = u16(nt + 6);
    const size_t table = nt + 0x18 + u16(nt + 0x14);
    const uint32_t imageSize = u32(nt + 0x18 + 0x38), headerSize = u32(nt + 0x18 + 0x3c);
    if (table + sections * 40ull > fileSize || imageSize > 256u * 1024 * 1024 || headerSize > fileSize)
        return false;
    uint64_t imageBase = 0;
    memcpy(&imageBase, file + nt + 0x18 + 0x18, 8);
    mapped.assign(imageSize, 0);
    memcpy(mapped.data(), file, headerSize < imageSize ? headerSize : imageSize);
    view = ImageView{};
    for (unsigned i = 0; i < sections; ++i) {
        const size_t s = table + i * 40ull;
        const uint32_t virtualSize = u32(s + 8), rva = u32(s + 12), rawSize = u32(s + 16), raw = u32(s + 20);
        const uint32_t mappedSize = virtualSize ? virtualSize : rawSize;
        const uint32_t copy = rawSize < virtualSize || !virtualSize ? rawSize : virtualSize;
        if (rva > imageSize || mappedSize > imageSize - rva || copy > imageSize - rva ||
            raw > fileSize || copy > fileSize - raw)
            return false;
        memcpy(mapped.data() + rva, file + raw, copy);
        if (!memcmp(file + s, ".text\0\0\0", 8)) {
            view.textBegin = rva;
            view.textEnd = rva + mappedSize;
        }
    }
    if (!view.textEnd || view.textEnd > imageSize) return false;
    view.data = mapped.data();
    view.size = mapped.size();
    view.imageBase = imageBase;
    return true;
}

struct Pattern {
    std::vector<uint8_t> bytes, mask; // mask 1: must match
    size_t anchor = 0;                // first fixed byte, searched with memchr
};

// "48 8B ?? 05": two hex digits per byte, ?? for any byte. False on anything else.
inline bool parsePattern(const char* text, Pattern& out) {
    out = Pattern{};
    auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?' && p[1] == '?') {
            out.bytes.push_back(0);
            out.mask.push_back(0);
        } else if (hex(p[0]) >= 0 && hex(p[1]) >= 0) {
            out.bytes.push_back(uint8_t(hex(p[0]) * 16 + hex(p[1])));
            out.mask.push_back(1);
        } else
            return false;
        p += 2;
    }
    while (out.anchor < out.mask.size() && !out.mask[out.anchor]) ++out.anchor;
    return out.anchor < out.mask.size();
}

// Matches of the pattern in [begin, end) of the view, as RVAs; stops counting at `limit`.
inline unsigned findPattern(const ImageView& v, const Pattern& p, uint32_t begin, uint32_t end,
                            uint32_t* first, unsigned limit = 2) {
    if (begin > end || end > v.size || p.bytes.size() > end - begin) return 0;
    unsigned found = 0;
    const uint8_t* const lo = v.data + begin + p.anchor;
    const uint8_t* const hi = v.data + end - p.bytes.size() + p.anchor + 1;
    for (const uint8_t* at = lo; at < hi && found < limit;) {
        at = (const uint8_t*)memchr(at, p.bytes[p.anchor], size_t(hi - at));
        if (!at) break;
        const uint8_t* start = at - p.anchor;
        bool match = true;
        for (size_t i = 0; i < p.bytes.size() && match; ++i) match = !p.mask[i] || start[i] == p.bytes[i];
        if (match) {
            if (!found && first) *first = uint32_t(start - v.data);
            ++found;
        }
        ++at;
    }
    return found;
}

enum class Unresolved : uint8_t { none, badPattern, notFound, ambiguous, disagree, outside, slotMismatch };

inline const char* unresolvedText(Unresolved u) {
    switch (u) {
    case Unresolved::none:
        return "ok";
    case Unresolved::badPattern:
        return "bad pattern";
    case Unresolved::notFound:
        return "not found";
    case Unresolved::ambiguous:
        return "more than one match";
    case Unresolved::disagree:
        return "references disagree";
    case Unresolved::outside:
        return "outside the image";
    case Unresolved::slotMismatch:
        return "slot holds something else";
    }
    return "?";
}

struct Layout {
    uint32_t rva[sig::targetCount];
    uint32_t textBegin = 0, textEnd = 0;
    bool resolved = false; // false: the source build's addresses (knownRva), never checked
    bool moved = false;    // resolved, and at least one target is not at its source-build address
    Layout() {
        for (uint32_t& r : rva) r = 0;
        for (const sig::Signature& s : sig::signatures) rva[s.target] = s.knownRva;
        textBegin = 0x1000;
        textEnd = 0x2ab000;
    }
    // Source-build defaults are useful to offline fixtures, but never prove an
    // optional native capability. An unknown or incomplete group is unavailable.
    bool optionalGroupAvailable(const char* group) const {
        if (!resolved || !group || !*group) return false;
        bool declared = false;
        for (unsigned t = 0; t < sig::targetCount; ++t) {
            const char* member = sig::targetOptionalGroups[t];
            if (!member || strcmp(member, group)) continue;
            declared = true;
            if (!rva[t]) return false;
        }
        return declared;
    }
};

struct ResolveReport {
    Unresolved why[sig::targetCount]{};
    unsigned failures = 0;         // all genuinely failed targets, including optional members
    unsigned optionalFailures = 0; // a subset of failures; these do not reject the required core
    // A healthy member can be withheld because another target in its group
    // failed. Keep why[] truthful instead of inventing a failure for that member.
    bool optionalDisabled[sig::targetCount]{};
    // Targets that resolved although some of their signatures no longer match: still found, but one game
    // update closer to lost.
    bool degraded[sig::targetCount]{};
    unsigned degradedCount = 0;
};

namespace detail {
// Same resolver for the generated native tables and small adversarial fixtures.
// Required targets are all-or-nothing. Every address in a failed optional group
// is withheld together; individual diagnostics retain their original outcomes.
inline bool resolveLayoutTables(const ImageView& v, Layout& out, ResolveReport& report,
                                const sig::Signature* signatures, size_t signatureCount,
                                const sig::SlotCheck* slots, size_t slotCount) {
    report = ResolveReport{};
    uint32_t rva[sig::targetCount]{};
    unsigned matched[sig::targetCount]{};
    Unresolved missWhy[sig::targetCount]{};
    bool disagree[sig::targetCount]{};
    for (size_t i = 0; i < signatureCount; ++i) {
        const sig::Signature& s = signatures[i];
        Pattern p;
        Unresolved miss = Unresolved::none;
        uint32_t at = 0;
        if (!parsePattern(s.pattern, p))
            miss = Unresolved::badPattern;
        else if (const unsigned n = findPattern(v, p, v.textBegin, v.textEnd, &at); n != 1)
            miss = n ? Unresolved::ambiguous : Unresolved::notFound;
        uint64_t value = 0;
        if (miss == Unresolved::none) {
            if (s.kind == sig::Kind::Code)
                value = uint64_t(at) + s.offset;
            else if (size_t(at) + s.offset > v.size || v.size - (size_t(at) + s.offset) < 4) {
                miss = Unresolved::outside;
            } else {
                int32_t disp;
                memcpy(&disp, v.data + at + s.offset, 4);
                value = uint64_t(int64_t(at) + s.insnEnd + disp + s.addend);
            }
            // Zero is the withheld-address sentinel for optional capabilities.
            if (value >= v.size || (!value && sig::targetOptionalGroups[s.target]))
                miss = Unresolved::outside;
        }
        if (miss != Unresolved::none) {
            // An ambiguous or malformed reference must not be relabeled merely
            // "not found" because a different signature happened to miss first.
            if (missWhy[s.target] == Unresolved::none || missWhy[s.target] == Unresolved::notFound)
                missWhy[s.target] = miss;
            report.degraded[s.target] = true;
            continue;
        }
        if (matched[s.target] && rva[s.target] != uint32_t(value)) disagree[s.target] = true;
        rva[s.target] = uint32_t(value);
        ++matched[s.target];
    }
    auto fail = [&](unsigned t, Unresolved why) {
        if (report.why[t] == Unresolved::none) {
            report.why[t] = why;
            ++report.failures;
        }
    };
    for (unsigned t = 0; t < sig::targetCount; ++t) {
        if (disagree[t])
            fail(t, Unresolved::disagree);
        else if (!matched[t])
            fail(t, missWhy[t] == Unresolved::none ? Unresolved::notFound : missWhy[t]);
    }
    // A function reached only through a vtable (no caller to anchor on) whose own signatures all missed: the
    // slot, found independently, holds its address. Found that way it cannot also be checked against the
    // slot, so it counts as degraded.
    for (size_t i = 0; i < slotCount; ++i) {
        const sig::SlotCheck& c = slots[i];
        if (report.why[c.slot] != Unresolved::none || matched[c.holds] ||
            report.why[c.holds] != Unresolved::notFound || v.size < 8 || rva[c.slot] > v.size - 8)
            continue;
        uint64_t held = 0;
        memcpy(&held, v.data + rva[c.slot], 8);
        const uint64_t at = held - v.imageBase;
        if (held < v.imageBase || at < v.textBegin || at >= v.textEnd) continue;
        rva[c.holds] = uint32_t(at);
        report.why[c.holds] = Unresolved::none;
        --report.failures;
        report.degraded[c.holds] = true;
    }
    for (size_t i = 0; i < slotCount; ++i) {
        const sig::SlotCheck& c = slots[i];
        if (report.why[c.slot] != Unresolved::none || report.why[c.holds] != Unresolved::none) continue;
        uint64_t held = 0;
        if (v.size < 8 || rva[c.slot] > v.size - 8) {
            fail(c.slot, Unresolved::outside);
            continue;
        }
        memcpy(&held, v.data + rva[c.slot], 8);
        if (held != v.imageBase + rva[c.holds]) fail(c.slot, Unresolved::slotMismatch);
    }
    for (unsigned t = 0; t < sig::targetCount; ++t) {
        if (report.why[t] != Unresolved::none) report.degraded[t] = false;
        report.degradedCount += report.degraded[t];
        const char* group = sig::targetOptionalGroups[t];
        if (!group || report.why[t] == Unresolved::none) continue;
        ++report.optionalFailures;
        for (unsigned member = 0; member < sig::targetCount; ++member)
            if (sig::targetOptionalGroups[member] && !strcmp(sig::targetOptionalGroups[member], group)) {
                rva[member] = 0;
                report.optionalDisabled[member] = true;
            }
    }
    if (report.failures != report.optionalFailures) return false;
    bool moved = false;
    for (size_t i = 0; i < signatureCount; ++i) {
        const sig::Signature& s = signatures[i];
        moved = moved || (rva[s.target] && rva[s.target] != s.knownRva);
    }
    memcpy(out.rva, rva, sizeof rva);
    out.textBegin = v.textBegin;
    out.textEnd = v.textEnd;
    out.resolved = true;
    out.moved = moved;
    return true;
}
} // namespace detail

// Resolves every target from whichever of its signatures match exactly once;
// the ones that match must agree. Required failure leaves out unchanged.
inline bool resolveLayout(const ImageView& v, Layout& out, ResolveReport& report) {
    return detail::resolveLayoutTables(v, out, report, sig::signatures,
                                       sizeof sig::signatures / sizeof *sig::signatures, sig::slotChecks,
                                       sizeof sig::slotChecks / sizeof *sig::slotChecks);
}

// The function entries the mod hooks or calls, compared in the running game with the file: a detour
// someone else put there first (another mod, an overlay) makes them differ. 16 bytes covers the longest
// prologue MinHook rewrites. Every target with an entry signature is compared, once.
// read(rva, out16) fetches the running game's bytes: directly in the DLL, ReadProcessMemory in the
// launcher. Returns how many differ (or cannot be read); the first `max` go to `out`.
template <class Read>
unsigned patchedEntries(const ImageView& file, const Layout& layout, Read read, sig::Target* out,
                        unsigned max) {
    unsigned n = 0;
    bool seen[sig::targetCount]{}; // several signatures can name one entry: compare it once
    for (const sig::Signature& s : sig::signatures) {
        if (!s.entry || seen[s.target]) continue;
        seen[s.target] = true;
        const uint32_t rva = layout.rva[s.target];
        if (!rva) continue; // a withheld optional group has no live entry to inspect
        uint8_t live[16];
        if (rva + 16 <= file.size && read(rva, live) && !memcmp(live, file.data + rva, 16)) continue;
        if (n < max) out[n] = s.target;
        ++n;
    }
    return n;
}

} // namespace fp
