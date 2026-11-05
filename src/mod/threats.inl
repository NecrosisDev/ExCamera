// Threat markers (docs/THREAT-MARKERS.md), the game side: after the game's own update of each character
// (combatUpdate, combat.inl) the fields that decide a threat are copied into a small table; the render thread
// draws from that copy (threat_draw.inl). Read-only, every read a field the audit proves:
//   character::aimOpponent (+E38): for others, the AI's combat state 0xB8440 (run from TMotileAI's tick 0xB4570,
//     VMT 0x309960 slot +F0, through behaviour +8E8 at 0xB4BFE) sets it in 0xB8210 (0xB8288) and clears it there
//     with no target; for the player, combatAttention 0xBEAC0 (0xBED90) from TPlayerBehaviour's tick.
//   stateFlags (+DF4), motion (+9B8), motionProgress (+A28), combatStance (+12FE), sceneNode (+18).
// Part of first_person.cpp's single translation unit; included before combat.inl.
#include "../threat_markers.h"

static std::atomic<int> threatMode{fp::threat::combatOnly}, threatOpacity{35};
static bool sceneNodeWorldOrigin(uintptr_t node, float* point); // frame.inl

struct ThreatSeen {
    uintptr_t root = 0;
    ULONGLONG at = 0;
    fp::threat::Sample sample{};
    float position[3]{};
    bool placed = false;
};
constexpr unsigned threatSlots = 32;
static SRWLOCK threatLock = SRWLOCK_INIT;
static ThreatSeen threatTable[threatSlots];
static std::atomic<unsigned> threatReads{0}, threatReadFaults{0};

// Game thread, after the native update for `root` (not nested).
static void threatObserve(uintptr_t root) {
    if (threatMode.load(std::memory_order_relaxed) == fp::threat::off) return;
    ThreatSeen seen{};
    // The character being updated is the game's own live object, read on its thread right after its update (as
    // the balance shadow reads it), so a bad read is caught here instead of a VirtualQuery per character per step.
    __try {
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot));
        fp::threat::Sample& s = seen.sample;
        s.self = root == player;
        s.state = *reinterpret_cast<const unsigned*>(root + field::character::stateFlags);
        s.motion = *reinterpret_cast<const int*>(root + field::character::motion);
        s.progress = *reinterpret_cast<const float*>(root + field::character::motionProgress);
        s.stance = *reinterpret_cast<const uint8_t*>(root + field::character::combatStance) != 0;
        s.targetsPlayer = player && *reinterpret_cast<const uintptr_t*>(root + field::character::aimOpponent) == player;
        const uintptr_t node = *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode);
        seen.placed = node && sceneNodeWorldOrigin(node, seen.position);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threatReadFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    seen.root = root;
    seen.at = GetTickCount64();
    threatReads.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&threatLock);
    ThreatSeen* slot = nullptr;
    for (ThreatSeen& t : threatTable)
        if (t.root == root) slot = &t;
    if (!slot) // a free slot, else the one seen longest ago
        for (ThreatSeen& t : threatTable)
            if (!slot || t.at < slot->at) slot = &t;
    *slot = seen;
    ReleaseSRWLockExclusive(&threatLock);
}
