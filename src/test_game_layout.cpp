// Signature resolution (game_layout.h): synthetic images, and a real Exanima.exe when EXANIMA_EXE names one.
// Portable: no Windows headers, so it also runs where the game does not.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#define _CRT_SECURE_NO_WARNINGS // getenv is standard C; MSVC's getenv_s is not portable
#include "game_layout.h"
#include "hook_health.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>

using namespace fp;

static void testParse() {
    Pattern p;
    assert(parsePattern("48 8B ?? 05", p) && p.bytes.size() == 4 && p.mask[2] == 0 && p.anchor == 0);
    assert(parsePattern("?? ?? 0F", p) && p.anchor == 2);
    assert(!parsePattern("?? ??", p)); // nothing to anchor on
    assert(!parsePattern("4G", p));    // not hex
    assert(!parsePattern("48 8b", p)); // generated patterns are upper case
    puts("PASS: patterns parse; wildcards, anchors and bad text");
}

static void testFind() {
    std::vector<uint8_t> image(0x100, 0x90);
    const uint8_t code[] = {0x48, 0x8b, 0x05, 0x11, 0x22, 0x33, 0x44};
    memcpy(&image[0x40], code, sizeof code);
    ImageView v{image.data(), image.size(), 0x140000000ull, 0, 0x100};
    Pattern p;
    assert(parsePattern("48 8B 05 ?? ?? ?? ??", p));
    uint32_t at = 0;
    assert(findPattern(v, p, 0, 0x100, &at) == 1 && at == 0x40);
    memcpy(&image[0x80], code, sizeof code);
    assert(findPattern(v, p, 0, 0x100, &at) == 2 && at == 0x40); // ambiguous: counted, first kept
    assert(findPattern(v, p, 0x41, 0x100, &at) == 1 && at == 0x80);
    assert(findPattern(v, p, 0xfa, 0x100, &at) == 0); // too close to the end to fit
    assert(findPattern(v, p, 0x80, 0x40, &at) == 0); // reversed bounds never underflow the size check
    puts("PASS: search counts matches, respects the range and the end of the image");
}

static void testDefaults() {
    Layout l;
    assert(!l.resolved && !l.moved);
    for (const sig::Signature& s : sig::signatures) assert(l.rva[s.target] == s.knownRva);
    assert(!l.optionalGroupAvailable("manualDefense") && !l.optionalGroupAvailable(nullptr) &&
           !l.optionalGroupAvailable(""));
    puts("PASS: an unresolved layout is the source build's (what the offline suites' fake memory uses)");
}

static void testRejectsGarbage() {
    std::vector<uint8_t> junk(4096, 0xcc);
    std::vector<uint8_t> mapped;
    ImageView v;
    assert(!mapPeFile(junk.data(), junk.size(), mapped, v));
    // Even a DOS signature and an arbitrary e_lfanew cannot make a short file
    // pass the NT-header bounds check (fileSize - 0x108 used to underflow).
    for (size_t size = 0x40; size < 0x108; ++size) {
        std::vector<uint8_t> shortPe(size, 0);
        shortPe[0] = 0x4d;
        shortPe[1] = 0x5a;
        const uint32_t nt = 0xf0;
        memcpy(shortPe.data() + 0x3c, &nt, 4);
        assert(!mapPeFile(shortPe.data(), shortPe.size(), mapped, v));
    }
    // A view that is only int3 matches nothing: every target fails, the layout is left untouched.
    ImageView flat{junk.data(), junk.size(), 0x100000000ull, 0, uint32_t(junk.size())};
    Layout l;
    ResolveReport r;
    assert(!resolveLayout(flat, l, r) && r.failures == sig::targetCount && !l.resolved);
    for (unsigned t = 0; t < sig::targetCount; ++t) assert(r.why[t] == Unresolved::notFound);
    puts("PASS: a non-PE file is refused; an image without the code resolves nothing and changes nothing");
}

static void testSlotFallbackConflict() {
    // Two independent, unique function signatures disagree. The exact one sees
    // one entry, the register-blind one another. A separately resolved slot is
    // not permission to discard that contradiction and guess its held address.
    std::vector<uint8_t> image(0xa000, 0xcc);
    constexpr uint64_t base = 0x140000000ull;
    constexpr uint32_t exact = 0x1000, blind = 0x2000, reference = 0x3000, slot = 0x9000;
    unsigned functions = 0;
    const sig::Signature* exactSignature = nullptr;
    const sig::Signature* blindSignature = nullptr;
    const sig::Signature* slotSignature = nullptr;
    for (const sig::Signature& s : sig::signatures) {
        if (s.target == sig::navSampler) {
            if (!functions++)
                exactSignature = &s;
            else if (!blindSignature)
                blindSignature = &s;
        } else if (s.target == sig::navSlot && !slotSignature)
            slotSignature = &s;
    }
    assert(functions == 2 && exactSignature && blindSignature && slotSignature);
    auto stamp = [&](const sig::Signature& s, uint32_t at) {
        Pattern p;
        assert(parsePattern(s.pattern, p));
        memcpy(image.data() + at, p.bytes.data(), p.bytes.size());
        if (s.kind == sig::Kind::Relative) {
            const int32_t displacement = int32_t(int64_t(slot) - at - s.insnEnd - s.addend);
            memcpy(image.data() + at + s.offset, &displacement, 4);
        }
    };
    stamp(*exactSignature, exact);
    stamp(*blindSignature, blind);
    stamp(*slotSignature, reference);
    const uint64_t held = base + exact;
    memcpy(image.data() + slot, &held, 8);
    ImageView view{image.data(), image.size(), base, 0x1000, 0x8000};
    auto uniqueAt = [&](const sig::Signature& s, uint32_t expected) {
        Pattern p;
        uint32_t at = 0;
        assert(parsePattern(s.pattern, p));
        assert(findPattern(view, p, view.textBegin, view.textEnd, &at) == 1 && at == expected);
    };
    uniqueAt(*exactSignature, exact);
    uniqueAt(*blindSignature, blind);
    Layout layout;
    ResolveReport report;
    assert(!resolveLayout(view, layout, report)); // other targets are absent from this small fixture
    assert(report.why[sig::navSlot] == Unresolved::none);
    assert(report.why[sig::navSampler] == Unresolved::disagree && !report.degraded[sig::navSampler]);
    assert(!layout.resolved);

    // Fallback is still valid when neither function signature resolves.
    memset(image.data() + exact, 0xcc, 0x100);
    memset(image.data() + blind, 0xcc, 0x100);
    assert(!resolveLayout(view, layout, report));
    assert(report.why[sig::navSampler] == Unresolved::none && report.degraded[sig::navSampler]);

    // Multiple matching entries are uncertain even when none of the other
    // signatures resolves. The slot must not pick one and conceal ambiguity.
    stamp(*exactSignature, exact);
    stamp(*exactSignature, blind);
    assert(!resolveLayout(view, layout, report));
    assert(report.why[sig::navSampler] == Unresolved::ambiguous && !report.degraded[sig::navSampler]);
    puts("PASS: a vtable fallback survives missing function signatures but never erases their disagreement");
}

static void testOptionalGroups() {
    // Small unique patterns exercise the production resolver's complete-core
    // admission without redistributing the executable. Group membership comes
    // from the same generated metadata as the real native signatures.
    struct Fixture {
        std::vector<uint8_t> image = std::vector<uint8_t>(0x9000, 0xcc);
        std::vector<std::array<char, 24>> patterns = std::vector<std::array<char, 24>>(sig::targetCount);
        std::vector<sig::Signature> signatures;
        std::vector<sig::SlotCheck> slots;
        ImageView view{image.data(), image.size(), 0x140000000ull, 0x100, 0x7000};
        static uint32_t address(unsigned t) { return 0x100 + t * 16; }
        Fixture() {
            signatures.reserve(sig::targetCount + 1);
            for (unsigned t = 0; t < sig::targetCount; ++t) {
                snprintf(patterns[t].data(), patterns[t].size(), "A5 %02X %02X 5A", t & 255, t >> 8);
                Pattern pattern;
                assert(parsePattern(patterns[t].data(), pattern));
                memcpy(image.data() + address(t), pattern.bytes.data(), pattern.bytes.size());
                signatures.push_back(
                    {sig::Target(t), sig::Kind::Code, true, true, 0, 0, 0, address(t), patterns[t].data()});
            }
        }
        bool resolve(Layout& layout, ResolveReport& report) {
            return detail::resolveLayoutTables(view, layout, report, signatures.data(), signatures.size(),
                                               slots.data(), slots.size());
        }
        void remove(unsigned t) { memset(image.data() + address(t), 0xcc, 4); }
    };
    const char* group = nullptr;
    std::vector<unsigned> members;
    unsigned required = sig::targetCount;
    for (unsigned t = 0; t < sig::targetCount; ++t) {
        if (!sig::targetOptionalGroups[t]) {
            if (required == sig::targetCount) required = t;
            continue;
        }
        if (!group) group = sig::targetOptionalGroups[t];
        if (!strcmp(group, sig::targetOptionalGroups[t])) members.push_back(t);
    }
    assert(group && members.size() >= 2 && required != sig::targetCount);
    const unsigned failed = members[0], sibling = members[1];
    auto withheld = [&](Fixture& fixture, Layout& layout, ResolveReport& report, Unresolved why) {
        assert(fixture.resolve(layout, report) && layout.resolved && !layout.moved);
        assert(report.failures == 1 && report.optionalFailures == 1 && report.why[failed] == why);
        assert(report.why[sibling] == Unresolved::none);
        assert(layout.rva[required] == Fixture::address(required) && !layout.optionalGroupAvailable(group));
        for (unsigned member : members) assert(layout.rva[member] == 0 && report.optionalDisabled[member]);
        bool readZero = false;
        sig::Target target = sig::targetCount;
        assert(patchedEntries(
                   fixture.view, layout,
                   [&](uint32_t rva, uint8_t* out) {
                       readZero = readZero || !rva;
                       memcpy(out, fixture.image.data() + rva, 16);
                       return true;
                   },
                   &target, 1) == 0);
        assert(!readZero && target == sig::targetCount);
    };
    {
        Fixture fixture;
        Layout layout;
        ResolveReport report;
        assert(fixture.resolve(layout, report) && layout.resolved && !layout.moved);
        assert(!report.failures && !report.optionalFailures && layout.optionalGroupAvailable(group));
        assert(!layout.optionalGroupAvailable("unknownOptionalGroup"));
        for (unsigned t = 0; t < sig::targetCount; ++t)
            assert(layout.rva[t] == Fixture::address(t) && !report.optionalDisabled[t]);
    }
    {
        Fixture fixture;
        fixture.remove(failed);
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::notFound);
    }
    {
        Fixture fixture;
        memcpy(fixture.image.data() + 0x6000, fixture.image.data() + Fixture::address(failed), 4);
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::ambiguous);
    }
    {
        Fixture fixture;
        const uint8_t second[] = {0xf1, 0x3d, 0x7c, 0xe2};
        memcpy(fixture.image.data() + 0x6100, second, sizeof second);
        fixture.signatures.push_back({sig::Target(failed), sig::Kind::Code, true, false, 0, 0, 0,
                                      Fixture::address(failed), "F1 3D 7C E2"});
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::disagree);
    }
    {
        Fixture fixture;
        fixture.signatures[failed].offset = 0xf000;
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::outside);
    }
    {
        Fixture fixture;
        fixture.signatures[failed].pattern = "GG";
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::badPattern);
    }
    for (bool optionalAlsoMissing : {false, true}) {
        Fixture fixture;
        fixture.remove(required);
        if (optionalAlsoMissing) fixture.remove(failed);
        Layout layout, before = layout;
        ResolveReport report;
        assert(!fixture.resolve(layout, report) && !layout.resolved && !layout.moved);
        assert(report.failures == (optionalAlsoMissing ? 2u : 1u) &&
               report.optionalFailures == (optionalAlsoMissing ? 1u : 0u));
        assert(report.why[required] == Unresolved::notFound &&
               !memcmp(layout.rva, before.rva, sizeof layout.rva));
    }
    {
        Fixture fixture;
        // Even a valid required slot holding one candidate cannot rescue an
        // ambiguous optional function. Healthy group siblings stay withheld.
        const unsigned slot = sig::navSlot;
        assert(!sig::targetOptionalGroups[slot]);
        fixture.signatures[slot].offset = uint16_t(0x8000 - Fixture::address(slot));
        fixture.signatures[slot].knownRva = 0x8000;
        const uint64_t held = fixture.view.imageBase + Fixture::address(failed);
        memcpy(fixture.image.data() + 0x8000, &held, 8);
        fixture.slots.push_back({sig::Target(slot), sig::Target(failed)});
        memcpy(fixture.image.data() + 0x6000, fixture.image.data() + Fixture::address(failed), 4);
        Layout layout;
        ResolveReport report;
        withheld(fixture, layout, report, Unresolved::ambiguous);
    }
    puts("PASS: optional groups are complete or withheld; missing, ambiguous, conflicting and malformed "
         "members "
         "keep truthful diagnostics; required failures reject the layout; zero entries are never inspected");
}

static void testVirtualSectionBounds() {
    // A small valid PE makes the section test independent of the optional real
    // executable. Its raw bytes fit, but a wrapped VirtualSize must still fail.
    std::vector<uint8_t> file(0x400), mapped;
    auto put16 = [&](size_t at, uint16_t value) { memcpy(file.data() + at, &value, 2); };
    auto put32 = [&](size_t at, uint32_t value) { memcpy(file.data() + at, &value, 4); };
    put16(0, 0x5a4d);
    put32(0x3c, 0x80);
    put32(0x80, 0x4550);
    put16(0x84, 0x8664);
    put16(0x86, 1);
    put16(0x94, 0xf0);
    put16(0x98, 0x20b);
    put32(0xd0, 0x2000); // SizeOfImage
    put32(0xd4, 0x200);  // SizeOfHeaders
    memcpy(file.data() + 0x188, ".text", 5);
    put32(0x190, 0x20);   // VirtualSize
    put32(0x194, 0x1000); // VirtualAddress
    put32(0x198, 0x20);   // SizeOfRawData
    put32(0x19c, 0x200);  // PointerToRawData
    ImageView view;
    assert(mapPeFile(file.data(), file.size(), mapped, view));
    assert(view.textBegin == 0x1000 && view.textEnd == 0x1020);
    put32(0x190, 0xfffff800); // rva + VirtualSize used to wrap to 0x800, below textBegin
    assert(!mapPeFile(file.data(), file.size(), mapped, view));
    put32(0x190, 0x2000); // oversized without wrapping is invalid too
    assert(!mapPeFile(file.data(), file.size(), mapped, view));
    puts("PASS: complete virtual section bounds are checked before calculating the search range");
}

// Every code signature is unique in the real exe, every data reference agrees, every slot holds its
// function, and the result is the source build's addresses exactly.
static void testRealExe(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    if (file.empty()) {
        printf("FAIL: EXANIMA_EXE=%s could not be read (unset it to skip the real-game checks)\n", path);
        exit(1);
    }
    std::vector<uint8_t> mapped;
    ImageView v;
    assert(mapPeFile(file.data(), file.size(), mapped, v));
    Layout l;
    ResolveReport r;
    const bool ok = resolveLayout(v, l, r);
    for (unsigned t = 0; t < sig::targetCount; ++t)
        if (r.why[t] != Unresolved::none) printf("  %s: %s\n", sig::targetNames[t], unresolvedText(r.why[t]));
    assert(ok && l.resolved);
    unsigned moved = 0;
    for (const sig::Signature& s : sig::signatures) moved += l.rva[s.target] != s.knownRva;
    printf("PASS: %s resolves all %u targets (%u signatures)%s\n", path, unsigned(sig::targetCount),
           unsigned(sizeof sig::signatures / sizeof *sig::signatures),
           moved ? ", some moved" : ", every one at its source-build address");
    // The loaded image is the file mapped; unpatched, no entry differs from the file.
    std::vector<uint8_t> patched = mapped;
    auto readPatched = [&](uint32_t rva, uint8_t* out) {
        return memcpy(out, patched.data() + rva, 16), true;
    };
    sig::Target first = sig::targetCount;
    assert(patchedEntries(v, l, readPatched, &first, 1) == 0 && first == sig::targetCount);
    patched[l.rva[sig::pollInput]] = 0xe9; // a detour's jmp
    assert(patchedEntries(v, l, readPatched, &first, 1) == 1 && first == sig::pollInput);
    patched[l.rva[sig::cursorAim]] = 0xe9;
    sig::Target all[8];
    const unsigned n = patchedEntries(v, l, readPatched, all, 8);
    assert(n == 2 && ((all[0] == sig::pollInput && all[1] == sig::cursorAim) ||
                      (all[0] == sig::cursorAim && all[1] == sig::pollInput)));
    assert(patchedEntries(
               v, l, [](uint32_t, uint8_t*) { return false; }, all, 8) > 20); // unreadable counts as patched
    // A slot that no longer holds its function is refused, and so is the whole layout.
    std::vector<uint8_t> moddedSlot = mapped;
    memset(moddedSlot.data() + l.rva[sig::navSlot], 0, 8);
    ImageView mv = v;
    mv.data = moddedSlot.data();
    Layout l2;
    assert(!resolveLayout(mv, l2, r) && r.why[sig::navSlot] == Unresolved::slotMismatch && !l2.resolved);
    puts("PASS: a patched entry is reported by name; a slot holding something else refuses the layout");
    // A relinked build, in miniature: everything one page later. Relative code is unchanged, absolute
    // pointers (the vtable slots) keep their old values, i.e. they are right for a base one page lower.
    // Every target must be found exactly one page on, and the layout must say it moved.
    constexpr uint32_t shift = 0x1000;
    std::vector<uint8_t> later(mapped.size() + shift, 0);
    memcpy(later.data() + shift, mapped.data(), mapped.size());
    ImageView lv{later.data(), later.size(), v.imageBase - shift, v.textBegin + shift, v.textEnd + shift};
    Layout l3;
    assert(resolveLayout(lv, l3, r) && l3.resolved && l3.moved);
    for (unsigned t = 0; t < sig::targetCount; ++t) assert(l3.rva[t] == l.rva[t] + shift);
    assert(l3.textBegin == l.textBegin + shift);
    puts("PASS: a build with everything moved resolves every target at its new address, flagged as moved");

    // Damage, the way a game update would: each target must survive losing any one way of finding it.
    // resolveOne mirrors resolveLayout for one target (any unique match; the matches must agree).
    std::function<uint32_t(const ImageView&, unsigned)> resolveOne = [&](const ImageView& view,
                                                                         unsigned t) -> uint32_t {
        uint32_t got = 0;
        for (const sig::Signature& sg : sig::signatures) {
            if (sg.target != t) continue;
            Pattern p;
            uint32_t at = 0;
            if (!parsePattern(sg.pattern, p) || findPattern(view, p, view.textBegin, view.textEnd, &at) != 1)
                continue;
            int32_t disp = 0;
            if (sg.kind != sig::Kind::Code) memcpy(&disp, view.data + at + sg.offset, 4);
            const uint32_t rva = sg.kind == sig::Kind::Code
                                     ? at + sg.offset
                                     : uint32_t(int64_t(at) + sg.insnEnd + disp + sg.addend);
            if (got && got != rva) return 0;
            got = rva;
        }
        if (!got) // as resolveLayout: a vtable-only function from its slot
            for (const sig::SlotCheck& c : sig::slotChecks)
                if (c.holds == t)
                    if (const uint32_t slot = resolveOne(view, c.slot)) {
                        uint64_t held = 0;
                        memcpy(&held, view.data + slot, 8);
                        got = uint32_t(held - view.imageBase);
                    }
        return got;
    };
    std::vector<uint8_t> work = mapped;
    ImageView wv = v;
    wv.data = work.data();
    unsigned primaryLost = 0, entriesRewritten = 0, registersSwapped = 0;
    for (unsigned t = 0; t < sig::targetCount; ++t) {
        // 1. its first signature no longer matches anywhere: one byte of what it matched is changed
        const sig::Signature* primary = nullptr;
        for (const sig::Signature& sg : sig::signatures)
            if (sg.target == t) {
                primary = &sg;
                break;
            }
        Pattern p;
        uint32_t at = 0;
        assert(primary && parsePattern(primary->pattern, p) &&
               findPattern(wv, p, wv.textBegin, wv.textEnd, &at) == 1);
        const uint8_t keep = work[at + p.anchor];
        work[at + p.anchor] ^= 0x5A;
        const uint32_t found = resolveOne(wv, t);
        work[at + p.anchor] = keep;
        if (found != l.rva[t]) printf("  lost with its first signature: %s\n", sig::targetNames[t]);
        primaryLost += found == l.rva[t];
    }
    for (const sig::Signature& sg : sig::signatures) {
        if (!sg.entry) continue;
        const unsigned t = sg.target;
        bool firstOfTarget = true;
        for (const sig::Signature& o : sig::signatures) {
            if (&o == &sg) break;
            if (o.target == t && o.entry) firstOfTarget = false;
        }
        if (!firstOfTarget) continue;
        const uint32_t e = l.rva[t];
        // 2. the function's entry rewritten (an edit at its top): only its callers can find it now
        uint8_t keep[16];
        memcpy(keep, &work[e], 16);
        for (int k = 0; k < 16; ++k) work[e + k] = uint8_t(0x90 + k);
        const uint32_t found = resolveOne(wv, t);
        memcpy(&work[e], keep, 16);
        if (found != e) printf("  lost with its entry rewritten: %s\n", sig::targetNames[t]);
        entriesRewritten += found == e;
        // 3. a register choice changed (a recompile): the first instruction's ModRM byte, reg field swapped
        size_t m = 0;
        while (m < 4 && work[e + m] >= 0x40 && work[e + m] <= 0x4F) ++m; // REX
        if (work[e + m] == 0x0F) ++m;                                    // two-byte opcode
        const size_t modrm = e + m + 1;
        const uint8_t keepM = work[modrm];
        work[modrm] = uint8_t((keepM & 0xC7) | (((keepM >> 3) + 1) & 7) << 3);
        const uint32_t r2 = resolveOne(wv, t);
        work[modrm] = keepM;
        registersSwapped += r2 == e;
    }
    unsigned functions = 0;
    {
        bool seen[sig::targetCount]{};
        for (const sig::Signature& sg : sig::signatures)
            if (sg.entry && !seen[sg.target]) {
                seen[sg.target] = true;
                ++functions;
            }
    }
    printf("damage: %u/%u targets survive losing their first signature; %u/%u functions survive their entry "
           "rewritten, %u/%u a register change\n",
           primaryLost, unsigned(sig::targetCount), entriesRewritten, functions, registersSwapped, functions);
    fflush(stdout);
    assert(primaryLost == sig::targetCount && entriesRewritten == functions && registersSwapped == functions);
    puts("PASS: every target survives losing any one way of finding it; every function an edit at its top or "
         "a register change");
    // 4. every function's entry rewritten at once (a big patch): callers, address references and slots still find them all
    {
        bool seen[sig::targetCount]{};
        for (const sig::Signature& sg : sig::signatures)
            if (sg.entry && !seen[sg.target]) {
                seen[sg.target] = true;
                for (int k = 0; k < 16; ++k) work[l.rva[sg.target] + k] = uint8_t(0x90 + k);
            }
        Layout damaged; // the resolver the DLL runs, on the whole damaged image
        ResolveReport report;
        const bool resolved = resolveLayout(wv, damaged, report);
        unsigned found = 0;
        for (unsigned t = 0; t < sig::targetCount; ++t) {
            const bool same = resolved && damaged.rva[t] == l.rva[t];
            if (!same) printf("  lost with every entry rewritten: %s\n", sig::targetNames[t]);
            found += same;
        }
        work = mapped;
        printf("damage: all %u function entries rewritten at once: %u/%u targets still resolve\n", functions,
               found, unsigned(sig::targetCount));
        assert(found == sig::targetCount);
    }
    puts("PASS: with every function entry rewritten at once, every target still resolves");
}

static void testHookHealth() {
    uint32_t hits[healthHooks]{};
    auto tick = [&](HookHealth& h, bool play, uint32_t clock, bool run) {
        if (run)
            for (uint32_t& n : hits) ++n;
        return healthStep(h, play, clock, hits);
    };
    {
        HookHealth h; // every hook runs while the clock moves: passed after the window, and it stays so
        assert(tick(h, true, 1000, true) == HealthVerdict::watching);
        assert(tick(h, true, 1000 + healthWindowMs - 1, true) == HealthVerdict::watching);
        assert(tick(h, true, 1000 + healthWindowMs, true) == HealthVerdict::passed);
        assert(tick(h, false, 0, false) == HealthVerdict::passed);
    }
    {
        HookHealth h; // a paused game: the clock stands, nothing runs, nothing is decided
        tick(h, true, 500, false);
        for (int i = 0; i < 100; ++i) assert(tick(h, true, 500, false) == HealthVerdict::watching);
    }
    {
        HookHealth h; // one hook silent through a full window of play: failed, and named
        tick(h, true, 0, false);
        for (uint32_t c = 100; c <= healthWindowMs; c += 100) {
            for (unsigned i = 0; i < healthHooks; ++i)
                if (i != unsigned(HealthHook::poseBuilder)) ++hits[i];
            healthStep(h, true, c, hits);
        }
        assert(h.verdict == HealthVerdict::failed && h.silent == int(HealthHook::poseBuilder));
        assert(!strcmp(healthHookNames[h.silent], "poseBuilder"));
    }
    {
        HookHealth h; // leaving play (title, death, load) restarts the window instead of failing
        tick(h, true, 0, false);
        tick(h, true, healthWindowMs - 10, false);
        assert(tick(h, false, healthWindowMs + 10, false) == HealthVerdict::watching);
        assert(tick(h, true, 90000, true) == HealthVerdict::watching);
        assert(tick(h, true, 90000 + healthWindowMs, true) == HealthVerdict::passed);
    }
    {
        HookHealth h; // a clock jump (new game, loaded save) is a new window, not a silent hook
        tick(h, true, 1000, false);
        assert(tick(h, true, 1000 + 20 * healthWindowMs, false) == HealthVerdict::watching);
        assert(tick(h, true, 1000 + 21 * healthWindowMs, true) == HealthVerdict::passed);
    }
    {
        HookHealth h; // the game clock wrapping past 2^32 ms still counts forward
        tick(h, true, 0xffffff00u, false);
        assert(tick(h, true, 0xffffff00u + healthWindowMs, true) == HealthVerdict::passed);
    }
    puts("PASS: hook health decides on game-clock play only; a silent hook fails by name; pauses and loads "
         "never do");
}
static void testOtherHooks() {
    assert(otherHooksFromText(L"allow") == OtherHooks::allow &&
           otherHooksFromText(L" ALLOW ") == OtherHooks::allow);
    assert(otherHooksFromText(L"refuse") == OtherHooks::refuse);
    assert(otherHooksFromText(L"ask") == OtherHooks::ask && otherHooksFromText(L"") == OtherHooks::ask &&
           otherHooksFromText(nullptr) == OtherHooks::ask &&
           otherHooksFromText(L"allowed") == OtherHooks::ask);
    assert(chainAllowed(OtherHooks::allow, false) && chainAllowed(OtherHooks::ask, true));
    assert(!chainAllowed(OtherHooks::ask, false) && !chainAllowed(OtherHooks::refuse, true));
    puts("PASS: OtherHooks: allow chains, refuse never does, ask only with the player's yes; unknown text is "
         "ask");
}

int main() {
    testOtherHooks();
    testHookHealth();
    testParse();
    testFind();
    testDefaults();
    testRejectsGarbage();
    testSlotFallbackConflict();
    testOptionalGroups();
    testVirtualSectionBounds();
    if (const char* exe = getenv("EXANIMA_EXE"))
        testRealExe(exe);
    else
        puts("SKIP: set EXANIMA_EXE to an Exanima.exe to resolve a real game build");
    puts("ALL GAME LAYOUT TESTS PASSED");
}
