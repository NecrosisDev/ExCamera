// Signature resolution (game_layout.h): synthetic images, and a real Exanima.exe when EXANIMA_EXE names one.
// Portable: no Windows headers, so it also runs where the game does not.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "game_layout.h"
#include "hook_health.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    puts("PASS: search counts matches, respects the range and the end of the image");
}

static void testDefaults() {
    Layout l;
    assert(!l.resolved && !l.moved);
    for (const sig::Signature& s : sig::signatures) assert(l.rva[s.target] == s.knownRva);
    puts("PASS: an unresolved layout is the source build's (what the offline suites' fake memory uses)");
}

static void testRejectsGarbage() {
    std::vector<uint8_t> junk(4096, 0xcc);
    std::vector<uint8_t> mapped;
    ImageView v;
    assert(!mapPeFile(junk.data(), junk.size(), mapped, v));
    // A view that is only int3 matches nothing: every target fails, the layout is left untouched.
    ImageView flat{junk.data(), junk.size(), 0x100000000ull, 0, uint32_t(junk.size())};
    Layout l;
    ResolveReport r;
    assert(!resolveLayout(flat, l, r) && r.failures == sig::targetCount && !l.resolved);
    for (unsigned t = 0; t < sig::targetCount; ++t) assert(r.why[t] == Unresolved::notFound);
    puts("PASS: a non-PE file is refused; an image without the code resolves nothing and changes nothing");
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
    // The loaded image is the file mapped; unpatched, it matches itself.
    assert(codeMatchesFile(mapped.data(), v, l));
    std::vector<uint8_t> patched = mapped;
    patched[l.rva[sig::pollInput]] = 0xe9; // a detour's jmp
    sig::Target first = sig::targetCount;
    assert(!codeMatchesFile(patched.data(), v, l, &first) && first == sig::pollInput);
    patched[l.rva[sig::cursorAim]] = 0xe9;
    sig::Target all[8];
    const unsigned n = patchedEntries(
        v, l, [&](uint32_t rva, uint8_t* out) { return memcpy(out, patched.data() + rva, 16), true; }, all,
        8);
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
    if (const char* exe = getenv("EXANIMA_EXE"))
        testRealExe(exe);
    else
        puts("SKIP: set EXANIMA_EXE to an Exanima.exe to resolve a real game build");
    puts("ALL GAME LAYOUT TESTS PASSED");
}
