#pragma once
// Finds every game address ExCamera uses by its code signature (game_signatures.h), instead of
// trusting fixed addresses: a game build where every signature still matches exactly once is
// laid out as this mod expects, wherever the linker put things.
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
    if (fileSize < 0x40 || u16(0) != 0x5a4d) return false;
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
        const uint32_t copy = rawSize < virtualSize || !virtualSize ? rawSize : virtualSize;
        if (rva > imageSize || copy > imageSize - rva || raw > fileSize || copy > fileSize - raw)
            return false;
        memcpy(mapped.data() + rva, file + raw, copy);
        if (!memcmp(file + s, ".text\0\0\0", 8)) {
            view.textBegin = rva;
            view.textEnd = rva + (virtualSize ? virtualSize : rawSize);
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
    if (end > v.size || p.bytes.size() > end - begin) return 0;
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
};

struct ResolveReport {
    Unresolved why[sig::targetCount]{};
    unsigned failures = 0;
};

// Resolves every target. All or nothing: `out` is changed only if every target resolves, every
// target with several signatures resolves to one address, and every vtable slot holds its function.
inline bool resolveLayout(const ImageView& v, Layout& out, ResolveReport& report) {
    report = ResolveReport{};
    uint32_t rva[sig::targetCount]{};
    bool seen[sig::targetCount]{};
    auto fail = [&](sig::Target t, Unresolved why) {
        if (report.why[t] == Unresolved::none) {
            report.why[t] = why;
            ++report.failures;
        }
    };
    for (const sig::Signature& s : sig::signatures) {
        Pattern p;
        if (!parsePattern(s.pattern, p)) {
            fail(s.target, Unresolved::badPattern);
            continue;
        }
        uint32_t at = 0;
        const unsigned n = findPattern(v, p, v.textBegin, v.textEnd, &at);
        if (n != 1) {
            fail(s.target, n ? Unresolved::ambiguous : Unresolved::notFound);
            continue;
        }
        uint64_t value;
        if (s.kind == sig::Kind::Code)
            value = uint64_t(at) + s.offset;
        else {
            int32_t disp;
            memcpy(&disp, v.data + at + s.offset, 4);
            value = uint64_t(int64_t(at) + s.insnEnd + disp + s.addend);
        }
        if (value >= v.size) {
            fail(s.target, Unresolved::outside);
            continue;
        }
        if (seen[s.target] && rva[s.target] != uint32_t(value)) fail(s.target, Unresolved::disagree);
        seen[s.target] = true;
        rva[s.target] = uint32_t(value);
    }
    for (const sig::SlotCheck& c : sig::slotChecks) {
        if (report.why[c.slot] != Unresolved::none || report.why[c.holds] != Unresolved::none) continue;
        uint64_t held = 0;
        if (rva[c.slot] > v.size - 8) {
            fail(c.slot, Unresolved::outside);
            continue;
        }
        memcpy(&held, v.data + rva[c.slot], 8);
        if (held != v.imageBase + rva[c.holds]) fail(c.slot, Unresolved::slotMismatch);
    }
    if (report.failures) return false;
    bool moved = false;
    for (const sig::Signature& s : sig::signatures) moved = moved || rva[s.target] != s.knownRva;
    memcpy(out.rva, rva, sizeof rva);
    out.textBegin = v.textBegin;
    out.textEnd = v.textEnd;
    out.resolved = true;
    out.moved = moved;
    return true;
}

// The function entries the mod hooks or calls, compared in the running game with the file: a detour
// someone else put there first (another mod, an overlay) makes them differ. 16 bytes covers the longest
// prologue MinHook rewrites. Only Code targets at a pattern's start (entries, not return sites).
// read(rva, out16) fetches the running game's bytes: directly in the DLL, ReadProcessMemory in the
// launcher. Returns how many differ (or cannot be read); the first `max` go to `out`.
template <class Read>
unsigned patchedEntries(const ImageView& file, const Layout& layout, Read read, sig::Target* out,
                        unsigned max) {
    unsigned n = 0;
    for (const sig::Signature& s : sig::signatures) {
        if (s.kind != sig::Kind::Code || s.offset != 0) continue;
        const uint32_t rva = layout.rva[s.target];
        uint8_t live[16];
        if (rva + 16 <= file.size && read(rva, live) && !memcmp(live, file.data + rva, 16)) continue;
        if (n < max) out[n] = s.target;
        ++n;
    }
    return n;
}

inline bool codeMatchesFile(const uint8_t* loadedImage, const ImageView& file, const Layout& layout,
                            sig::Target* firstDifference = nullptr) {
    sig::Target first = sig::targetCount;
    const unsigned n = patchedEntries(
        file, layout, [&](uint32_t rva, uint8_t* out) { return memcpy(out, loadedImage + rva, 16), true; },
        &first, 1);
    if (n && firstDifference) *firstDifference = first;
    return n == 0;
}

} // namespace fp
