#pragma once
// Live check that the hooks work: every hook that runs on each game tick must have run while the
// game's clock moved. Signatures say the code looks as expected; this says it behaves as expected.
// Timed by the game's clock, not the wall clock, so a paused game, a menu or a loading screen never
// counts against a hook. Pure logic: the DLL feeds it counters (first_person.cpp, hookHits).
#include <cstdint>

namespace fp {

inline constexpr unsigned healthHooks = 6;
// The hooks every game tick reaches while a character is in play, in or out of combat, moving or not.
enum class HealthHook : uint8_t {
    pollInput,
    navSampler,
    playerBehavior,
    poseBuilder,
    partSettle,
    cameraMouse
};
inline constexpr const char* healthHookNames[healthHooks] = {"pollInput",   "navSampler", "playerBehavior",
                                                             "poseBuilder", "partSettle", "cameraMouse"};
// Game-clock milliseconds of play the check watches before it decides.
inline constexpr uint32_t healthWindowMs = 5000;

enum class HealthVerdict : uint8_t { watching, passed, failed };

struct HookHealth {
    bool watching = false, decided = false;
    uint32_t clockStart = 0;
    uint32_t hitsAtStart[healthHooks]{};
    HealthVerdict verdict = HealthVerdict::watching;
    int silent = -1; // the first hook that never ran (failed)
};

// Called a few times a second. inPlay: a controlled character exists and the mod owns the view.
// clockMs: the game's millisecond clock (it stops while the game is paused). hits: running totals.
// Leaving play before the window is full starts it again; the verdict, once given, stands.
inline HealthVerdict healthStep(HookHealth& h, bool inPlay, uint32_t clockMs,
                                const uint32_t (&hits)[healthHooks]) {
    if (h.decided) return h.verdict;
    if (!inPlay) {
        h.watching = false;
        return HealthVerdict::watching;
    }
    const uint32_t played = clockMs - h.clockStart; // unsigned: a wrapped clock still counts forward
    // Not yet watching, or the clock jumped (a load, a new game): the window starts here.
    if (!h.watching || played > 10 * healthWindowMs) {
        h.watching = true;
        h.clockStart = clockMs;
        for (unsigned i = 0; i < healthHooks; ++i) h.hitsAtStart[i] = hits[i];
        return HealthVerdict::watching;
    }
    if (played < healthWindowMs) return HealthVerdict::watching;
    h.decided = true;
    h.verdict = HealthVerdict::passed;
    for (unsigned i = 0; i < healthHooks; ++i)
        if (hits[i] == h.hitsAtStart[i]) {
            h.verdict = HealthVerdict::failed;
            h.silent = int(i);
            break;
        }
    return h.verdict;
}

} // namespace fp

namespace fp {
// [Compat] OtherHooks: what to do when another program has already hooked a function this mod hooks or
// calls, or replaced one of its dispatch slots. allow: hook on top of it (chain). refuse: do not start.
// ask: the launcher asks the player; with no one to ask (a mod loader started the DLL), do not start.
enum class OtherHooks : uint8_t { ask, allow, refuse };
inline OtherHooks otherHooksFromText(const wchar_t* t) {
    auto is = [&](const wchar_t* w) {
        if (!t) return false;
        const wchar_t* p = t;
        while (*p == L' ') ++p;
        for (; *w; ++w, ++p)
            if ((*p | 0x20) != *w) return false;
        while (*p == L' ') ++p;
        return *p == 0;
    };
    return is(L"allow") ? OtherHooks::allow : is(L"refuse") ? OtherHooks::refuse : OtherHooks::ask;
}
// approvedNow: the player said yes for this game session (the launcher's prompt).
inline bool chainAllowed(OtherHooks policy, bool approvedNow) {
    return policy == OtherHooks::allow || (policy == OtherHooks::ask && approvedNow);
}
} // namespace fp
