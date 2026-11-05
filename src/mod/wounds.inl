// Wounds: where a weapon got through, the struck character keeps a mark that moves with the limb: a
// gash along the blade's path for a cut, a short deep hole for a thrust, a bruise for a crush, and
// blood running down from fresh cuts. The hits come from ScriptRuntime.dll's character_hits export
// (version 1), which records them inside the game's damage applier; ExCamera only reads them. Each
// mark is kept in its body part's own frame and the finishing pass (view_effects.inl) draws it on
// the surface at that spot, so a blow the armour stopped leaves nothing (it dealt no damage) and a
// cut through cloth shows on the cloth. No native memory is written.

static std::atomic<float> woundMarks{1.f}; // the Wounds row: 0 off

// Game reads the render code makes on layouts no game code has yet been shown to read that way
// (docs/render-assumptions.md, "UNPROVEN"): the held items' light from the light-node fields, their wear
// and dirt from the item database record, the level's two nearest lamps, and a weapon's two transforms for a
// cut's direction. Off until each is proven from the game code; the effects then use their neutral
// fallbacks (default light from above, unread condition, no lamps, a direction picked on the surface).
constexpr bool unprovenRenderReads = false;

// The runtime's record, shared with ScriptRuntime's character_hits: version 2 (0x78 bytes); a version 1
// runtime writes the first 0x70 bytes only (no weapon, kind 0 or 3).
struct CharacterHitRecord {
    uint32_t size, serial;
    uint64_t victim, part; // the struck character and its struck body part (a rigid body)
    float point[3];        // contact point, in the space of the part's pose
    float direction[3];    // the weapon's motion at contact, same space; zero when unknown
    float pose[12];        // the part's achieved pose at the hit: three axes then the origin
    float permanent, recoverable; // the damage routine's third and second arguments (bodyDamageDetour)
    uint32_t kind;         // 0 unknown, 1 cut, 2 crush, 3 thrust
    uint32_t reserved;
    uint64_t weapon;       // version 2: the delivering weapon's rigid body; 0 from version 1
};
static_assert(sizeof(CharacterHitRecord) == 0x78, "character_hits record layout");
constexpr uint32_t characterHitsV1Size = 0x70;
static std::atomic<uint32_t> characterHitsVersion{0}; // 1 or 2 once the export is found
using CharacterHits = int (*)(uint32_t after, CharacterHitRecord* out, int capacity);
using CharacterHitsVersion = uint32_t (*)();

constexpr unsigned woundLimit = 96, woundsPerPart = 4;
constexpr unsigned woundShownLimit = 12; // the finishing program's arrays
constexpr float woundReachCm = 3000.f, woundMergeCm = 2.5f;
enum WoundKind : uint8_t { woundCut = 1, woundCrush = 2, woundThrust = 3 };
// The character's bleeding rate (character::bleed, 0..20), stepped by the bleed tick 0x168410: it rises with
// the wound inflow (+10D4), takes a share of itself off current health (+D58) and off the restorable ceiling
// (+D5C), falls once the inflow stops, and current health below 0 calls the death routine 0x156480.
// Injuries' "injuries.broken" mark: one bit per limb bone, the part indices (bodyPart::index) each covers.
// A fracture bruise spreads this much wider.
constexpr float woundFractureScale = 1.6f;
inline int woundBoneBit(int part) {
    switch (part) {
    case 1: return 0;           // right thigh
    case 2: case 3: return 1;   // right shin, foot
    case 4: return 2;           // left thigh
    case 5: case 6: return 3;   // left shin, foot
    case 10: case 11: return 4; // right shoulder, upper arm
    case 12: case 16: return 5; // right forearm, hand
    case 13: case 14: return 6; // left shoulder, upper arm
    case 15: case 17: return 7; // left forearm, hand
    default: return -1;
    }
}
constexpr float woundBleedFull = 6.f, woundFreshSeconds = 10.f, woundDrySeconds = 60.f, woundBruiseSeconds = 90.f;
// A struck body part's character is its body's root (bodyPart::rootPart): the game's weapon impact
// (0x159A60) reads its victim there and hands the same pointer to the damage applier, so it equals the
// record's victim.

constexpr uint32_t woundThrustMotion = 0xB; // see recordNativeHit
// Dead: character::stateFlags (+DF4) has 0x800 set, the test the game itself makes (the death routine 0x156480
// tests it before writing 0x800; 0x167ED0 also writes 0x800, 0xF12C4 and 0x156E48 write 0xA00; witness
// characterDie::deadState). +DF8's 0x100, used here before, is set by 0x17B100 and not by any death path.
constexpr unsigned woundDeadState = 0x800;
struct Wound {
    uintptr_t part = 0, owner = 0; // the part, and the character it named at the hit; a change retires the mark
    float local[3]{}, along[3]{};  // centre and the blade's path, in the part's own frame
    float halfLength = 0.f, halfWidth = 0.f, severity = 0.f, seed = 0.f;
    ULONGLONG born = 0;
    float fade = 1.f; // 1 as struck; closes towards 0 while the character is marked injuries.mend
    float run = 0.f;  // length of the blood runs below a cut (cm); grows while the character bleeds
    float wet = 1.f;  // 1 fresh blood, falls to 0 (dried) about a minute after the bleeding stops
    // Its spot on the character's skin texture (wounds_bake.inl): UV, and UV per cm along the blade and across.
    uintptr_t skinNode = 0;
    float uv[2]{}, uvAlong[2]{}, uvAcross[2]{};
    bool mapTried = false, mapped = false;
    bool fracture = false; // a bruise over a bone Injuries reports broken ("injuries.broken")
    int8_t partIndex = -1; // the struck part's index (+0x468), 0..17
    uintptr_t actor = 0;   // the character's scene node, once mapped (wounds_bake.inl)
    float drip = 0.f;      // counts up to the next drop of blood falling from a bleeding cut
    float scar = 0.f;      // a cut on the living closing into a scar: 0 open .. 1 a scar for good
    float pool = 0.f;      // blood that has run out of it onto the floor (grows its pool); poolTick: time to the next pool mark
    float poolTick = 0.f;
    float spurt = 0.f;     // an artery's beat timer; spurts: beats left (set at the hit on a deep cut of a limb or neck)
    uint8_t spurts = 0;
    uint8_t kind = 0;
    uint8_t material = 0; // the struck surface's material id from the game (1..25), 0 until that link is proven
};

// What the finishing pass takes: view-space marks, nearest first.
constexpr unsigned woundDripLimit = 128, woundDripShown = 48;
constexpr float woundDripSeconds = 120.f, woundDripDrySeconds = 30.f;
// On the living: an open cut closes into a scar over this long (the Regenerate mark's own time while it
// runs); a bruise fades away over this long.
constexpr float woundScarSeconds = 300.f, woundBruiseHealSeconds = 900.f;
// Marks for the floor blood layer (view_effects.inl bloodMap): laid down once where blood lands and kept.
// A drop (kind 0): a round splash. A flung or spurted drop (kind 1): a teardrop, its tail pointing the way
// it flew (dir). A pool mark (kind 2): a thin, wide layer under a bleeding wound, wider the more has run
// out, which the layer's additive thickness builds into a deepening, spreading pool.
constexpr unsigned woundSplatQueue = 96, woundSplatShown = 24;
struct WoundSplat { float at[3], sectorOrigin[3], dir[3]; float across, along, thickness, seed; uint8_t kind; };
static WoundSplat woundSplats[woundSplatQueue]; // woundLock: a FIFO, head..tail
static unsigned woundSplatHead = 0, woundSplatTail = 0;
static void queueSplat(const float* at, const float* sectorOrigin, const float* dir, float across, float along,
                       float thickness, float seed, uint8_t kind) {
    if (woundSplatTail - woundSplatHead >= woundSplatQueue) ++woundSplatHead; // the oldest unsent mark gives way
    WoundSplat& q = woundSplats[woundSplatTail++ % woundSplatQueue];
    memcpy(q.at, at, sizeof q.at);
    memcpy(q.sectorOrigin, sectorOrigin, sizeof q.sectorOrigin);
    if (dir) memcpy(q.dir, dir, sizeof q.dir); else memset(q.dir, 0, sizeof q.dir);
    q.across = across; q.along = along; q.thickness = thickness; q.seed = seed; q.kind = kind;
}
struct WoundUniforms {
    unsigned falls = 0;           // drops in the air this frame, view space
    float fA[16][4];              // position xyz; w: radius (cm)
    float fB[16][4];              // velocity xyz (cm/s), for its streak; w unused
    unsigned splats = 0;          // floor-layer marks this frame, view space
    float sA[woundSplatShown][4]; // centre xyz; w: half width across (cm)
    float sB[woundSplatShown][4]; // direction xyz (unit, or 0 for a round mark); w: half length along (cm)
    float sC[woundSplatShown][4]; // thickness 0..1, seed, kind, unused
    unsigned count = 0;
    unsigned drips = 0;      // blood drops on the floor, below
    bool burning = false;    // a character in view is burnt (the skin bake has work)
    float d[woundDripShown][4]; // a drop: view-space point it fell from; w: 256 x radius (tenths of a cm) + dryness (0..255)
    unsigned caps = 0;          // flesh caps on severed limbs: stump and end
    float capA[8][4], capB[8][4]; // centre (view space), radius (cm); facing (view space), 0 stump / 1 the severed end
    bool any() const { return count || drips || burning || caps || splats || falls; }
    float a[woundShownLimit][4]; // centre; w: half length, cm (a bruise's radius)
    float b[woundShownLimit][4]; // the blade's path, unit; w: kind
    float c[woundShownLimit][4]; // severity 0..1, seed, half width (cm), blood run length (cm)
    float m[woundShownLimit][4]; // x: the struck surface's material id (factory 0x1EAEE0, 1..25), 0 unknown
};

static Wound woundStore[woundLimit];
static unsigned woundNext = 0;
static uint32_t woundSerial = 0; // the newest record taken
static SRWLOCK woundLock = SRWLOCK_INIT;
static std::atomic<CharacterHits> characterHits{nullptr};
using BodyDamageFn = unsigned char (*)(uintptr_t, float, float, char);
static BodyDamageFn originalBodyDamage = nullptr;
using PhysicalImpactFn = void (*)(uintptr_t);
static PhysicalImpactFn originalPhysicalImpact = nullptr;
static thread_local uintptr_t currentImpactAttacker = 0;
static std::atomic<int> characterHitsState{0}; // 0 unprobed, 1 ready, -1 absent
static std::atomic<unsigned> woundsTaken{0}, woundsRetired{0}, woundFaults{0}, woundsMended{0};
static unsigned woundSkinVersion = 1; // woundLock: bumped when a baked (mapped) mark changes or goes
// Injuries' "injuries.burn" mark: a burn level (0..255) per character scene node, read each frame; woundLock.
struct WoundBurn { uintptr_t actor; uint32_t level; };
static WoundBurn woundBurns[64], woundBurnScars[64]; // "injuries.burn" and "injuries.burnscar"
static int woundBurnCount = 0, woundBurnScarCount = 0;
static float woundLevelOf(const WoundBurn* table, int count, uintptr_t actor) {
    for (int b = 0; b < count; ++b)
        if (table[b].actor == actor) return float(table[b].level) / 255.f;
    return 0.f;
}
static float woundBurnLevel(uintptr_t actor) { return woundLevelOf(woundBurns, woundBurnCount, actor); }
// Injuries' "injuries.severed": one bit per limb bone cut off (the "injuries.broken" bits); woundLock.
static WoundBurn woundSevered[64];
static int woundSeveredCount = 0;
static uint32_t woundSeveredBits(uintptr_t actor) {
    for (int b = 0; b < woundSeveredCount; ++b)
        if (woundSevered[b].actor == actor) return woundSevered[b].level;
    return 0;
}
static void woundSeveredCaps(const float* view, WoundUniforms& out); // wounds_bake.inl
static float woundBurnScar(uintptr_t actor) { return woundLevelOf(woundBurnScars, woundBurnScarCount, actor); }
// One mark's levels into its table; a change of 4/255 or more anywhere re-bakes the skins.
static void woundReadLevels(int (*marks)(const char*, uintptr_t*, uint32_t*, int), const char* name, WoundBurn* table,
                            int& count) {
    uintptr_t markNodes[64]{};
    uint32_t levels[64]{};
    int found = 0;
    __try { found = marks(name, markNodes, levels, 64); }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; woundFaults.fetch_add(1, std::memory_order_relaxed); }
    found = std::clamp(found, 0, 64);
    bool changed = found != count;
    for (int b = 0; b < found && !changed; ++b)
        changed = std::abs(woundLevelOf(table, count, markNodes[b]) * 255.f - float(levels[b])) >= 4.f;
    if (!changed) return;
    for (int b = 0; b < found; ++b) table[b] = {markNodes[b], (std::min)(levels[b], 255u)};
    count = found;
    ++woundSkinVersion;
}
// Drops of blood fallen from bleeding cuts: where they fell from (the cut, sector-local) and its sector's
// origin; the wound pass draws each on the first upward surface 20..200 cm below. woundLock.
struct WoundDrip { float from[3], sectorOrigin[3], radius; ULONGLONG born; float start[3]; float flight; };
// A drop in the air, for the first moments after it leaves: from start to from over flight seconds (0: straight
// down from start, which is then from), falling under gravity the while; drawn by the wound pass as a bead.
static WoundDrip woundDrips[woundDripLimit];
// The drops the wound pass draws stay the same ones until they dry and go: a drop given a place keeps it
// (its born time is kept here, so a slot reused by a newer drop starts unshown), and new drops only take
// free places. Choosing the nearest each frame instead swapped drops in and out as the camera moved.
static ULONGLONG woundDripShownBorn[woundDripLimit];
static unsigned woundDripNext = 0;
static bool woundSkinBaked(uintptr_t character);            // wounds_bake.inl
static void bakeWoundSkins(gl::Api& g, fp::CueResources& r); // wounds_bake.inl, from the view-effects pass
static bool initWoundBakeResources(gl::Api& g, fp::CueResources& r); // wounds_bake.inl, warm-up precompile

// Regeneration (the Injuries addon's "injuries.mend" mark, value = seconds to close a wound
// fully): ScriptRuntime.dll's character_mark_nodes export (marks version 1).
using WoundMarkNodes = int (*)(const char*, uintptr_t*, uint32_t*, int);
using WoundMarksVersion = uint32_t (*)();
constexpr unsigned woundMendLimit = 64;
static std::atomic<WoundMarkNodes> woundMarkNodes{nullptr};
static std::atomic<int> woundMarksState{0}; // 0 unprobed, 1 ready, -1 absent
static ULONGLONG woundFadeTick = 0; // woundLock

// Looked up once: ScriptRuntime.dll loads before game play if installed, and is pinned once found.
static WoundMarkNodes woundMarkExport() {
    const int state = woundMarksState.load(std::memory_order_acquire);
    if (state < 0) return nullptr;
    if (state > 0) return woundMarkNodes.load(std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) {
        woundMarksState.store(-1, std::memory_order_release);
        return nullptr;
    }
    const auto version = reinterpret_cast<WoundMarksVersion>(GetProcAddress(runtime, "character_marks_version"));
    const auto fn = reinterpret_cast<WoundMarkNodes>(GetProcAddress(runtime, "character_mark_nodes"));
    if (!version || !fn || version() != 1) {
        woundMarksState.store(-1, std::memory_order_release);
        return nullptr;
    }
    woundMarkNodes.store(fn, std::memory_order_relaxed);
    woundMarksState.store(1, std::memory_order_release);
    return fn;
}

// Looked up once; the module is pinned once found, so the cached entry stays valid for the life of the process.
static CharacterHits characterHitsExport() {
    const int state = characterHitsState.load(std::memory_order_acquire);
    if (state < 0) return nullptr;
    if (state > 0) return characterHits.load(std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) {
        characterHitsState.store(-1, std::memory_order_release);
        return nullptr;
    }
    const auto version = reinterpret_cast<CharacterHitsVersion>(GetProcAddress(runtime, "character_hits_version"));
    const auto fn = reinterpret_cast<CharacterHits>(GetProcAddress(runtime, "character_hits"));
    const uint32_t found = version ? version() : 0;
    if (!fn || (found != 1 && found != 2)) {
        characterHitsState.store(-1, std::memory_order_release);
        return nullptr;
    }
    characterHitsVersion.store(found, std::memory_order_relaxed);
    characterHits.store(fn, std::memory_order_relaxed);
    characterHitsState.store(1, std::memory_order_release);
    return fn;
}

// A world point (or, with origin false, a direction) into the pose's own frame; false for a degenerate pose.
static bool intoPose(const float* pose, const float* v, bool origin, float* out) {
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = v[i] - (origin ? pose[9 + i] : 0.f);
    for (int a = 0; a < 3; ++a) {
        const float* axis = pose + a * 3;
        const float lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
        if (!std::isfinite(lengthSq) || lengthSq < 1e-8f) return false;
        out[a] = (d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]) / lengthSq;
    }
    return std::isfinite(out[0] + out[1] + out[2]);
}

static void outOfPose(const float* pose, const float* local, bool origin, float* out) {
    for (int i = 0; i < 3; ++i)
        out[i] = (origin ? pose[9 + i] : 0.f) + local[0] * pose[i] + local[1] * pose[3 + i] + local[2] * pose[6 + i];
}

static void addWound(const CharacterHitRecord& h) {
    const float permanent = std::isfinite(h.permanent) ? h.permanent : 0.f;
    const float recoverable = std::isfinite(h.recoverable) ? h.recoverable : 0.f;
    // The kind recordNativeHit read from the game (woundKindOfHit): a gash, a bruise or a puncture.
    const uint8_t kind = h.kind >= woundCut && h.kind <= woundThrust ? uint8_t(h.kind) : woundCut;
    if (permanent < 1e-3f && recoverable < 1e-3f) return; // nothing got through: the routine took nothing
    Wound w;
    w.part = uintptr_t(h.part);
    float healthMax = 1.f;
    __try {
        if (!w.part || !readable(w.part, field::bodyPart::rootPart + 8)) return;
        w.owner = *(const uintptr_t*)(w.part + field::bodyPart::rootPart);
        if (h.victim && readable(uintptr_t(h.victim), field::character::healthMax + 4))
            healthMax = *(const float*)(uintptr_t(h.victim) + field::character::healthMax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!w.owner || w.owner != uintptr_t(h.victim) || !intoPose(h.pose, h.point, true, w.local)) return;
    if (!intoPose(h.pose, h.direction, false, w.along)) memset(w.along, 0, sizeof w.along);
    const uintptr_t player = gameAt(fp::sig::playerRoot) ? *(const uintptr_t*)gameAt(fp::sig::playerRoot) : 0;
    if (player && uintptr_t(h.victim) == player) {
        const float flash = std::clamp(0.4f + permanent * 3.0f, 0.4f, 1.0f);
        float curFlash = playerDamageFlash.load(std::memory_order_relaxed);
        while (flash > curFlash && !playerDamageFlash.compare_exchange_weak(curFlash, flash, std::memory_order_relaxed)) {}

        float dirWorld[3]{};
        outOfPose(h.pose, h.direction, false, dirWorld);
        float lenSq = dirWorld[0] * dirWorld[0] + dirWorld[1] * dirWorld[1] + dirWorld[2] * dirWorld[2];
        if (lenSq < 1e-4f) {
            dirWorld[0] = 0.f; dirWorld[1] = 0.4f; dirWorld[2] = 1.0f;
            lenSq = 1.16f;
        }
        const float invLen = 1.0f / std::sqrt(lenSq);
        dirWorld[0] *= invLen;
        dirWorld[1] *= invLen;
        dirWorld[2] *= invLen;

        const float impulse = std::clamp(permanent * 0.20f + recoverable * 0.10f, 0.02f, 0.08f);
        const float kickPitch = (-dirWorld[1] * 0.6f + dirWorld[2] * 0.4f) * impulse;
        const float kickYaw = dirWorld[0] * impulse * 0.8f;
        const float kickRoll = -dirWorld[0] * impulse * 0.35f;

        cameraImpactPitch.store(std::clamp(cameraImpactPitch.load(std::memory_order_relaxed) + kickPitch, -0.10f, 0.10f), std::memory_order_relaxed);
        cameraImpactYaw.store(std::clamp(cameraImpactYaw.load(std::memory_order_relaxed) + kickYaw, -0.10f, 0.10f), std::memory_order_relaxed);
        cameraImpactRoll.store(std::clamp(cameraImpactRoll.load(std::memory_order_relaxed) + kickRoll, -0.06f, 0.06f), std::memory_order_relaxed);
    }
    if (!std::isfinite(healthMax) || healthMax < 0.05f) healthMax = 1.f;
    // Size from what the blow took, as a share of maximum health (+103C): the damage routine (0x159700)
    // takes 0.1 times its second argument off current health (+D58; 0.05 times when +DF8 has 0x800000) and
    // 0.2 times its third off the restorable ceiling (+D5C), current health scaling down with it. So this is
    // 0.1 x recoverable + 0.2 x permanent over the maximum, 0 to 1: a blow that would empty full health fills
    // the scale. (It read the raw third argument before, five times too large.)
    const float taken = std::clamp((0.1f * recoverable + 0.2f * permanent) / healthMax, 0.f, 1.f);
    w.severity = 0.35f + 0.65f * taken;
    w.kind = kind;
    __try {
        const int index = *(const int*)(w.part + field::bodyPart::index);
        w.partIndex = int8_t(index >= 0 && index < 18 ? index : -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w.partIndex = -1;
    }
    w.born = GetTickCount64();
    w.seed = float((h.serial * 2654435761u) >> 8) / float(1u << 24);
    // A deep wound spurts with the heartbeat for the first several beats (the floor layer's teardrop jets).
    // Which skeletal part (bodyPart::index) is a thigh or an upper arm is not shown by any game code read so
    // far, so it is by depth alone: a blow taking about two thirds of full health or more.
    if (kind != woundCrush && w.severity > 0.78f) {
        w.spurts = 8;
        w.spurt = 0.25f;
    }
    switch (kind) {
    case woundCut: w.halfLength = 4.f + 7.f * w.severity; w.halfWidth = 0.35f + 0.5f * w.severity; break;
    case woundThrust: w.halfLength = 1.f + 0.8f * w.severity; w.halfWidth = 0.6f + 0.4f * w.severity; break;
    default: w.halfLength = 3.f + 4.f * w.severity; w.halfWidth = 0.f; break;
    }
    // A blow landing on a mark of the same kind deepens it; a part keeps its newest few marks.
    unsigned onPart = 0, oldest = woundLimit;
    for (unsigned i = 0; i < woundLimit; ++i) {
        Wound& e = woundStore[i];
        if (e.part != w.part || e.owner != w.owner) continue;
        const float dx = e.local[0] - w.local[0], dy = e.local[1] - w.local[1], dz = e.local[2] - w.local[2];
        if (e.kind == w.kind && dx * dx + dy * dy + dz * dz < woundMergeCm * woundMergeCm) {
            if (e.mapped) ++woundSkinVersion;
            e.severity = (std::min)(e.severity + 0.5f * w.severity, 1.f);
            e.halfLength = (std::max)(e.halfLength, w.halfLength);
            e.halfWidth = (std::max)(e.halfWidth, w.halfWidth);
            e.born = w.born;
            return;
        }
        ++onPart;
        if (oldest == woundLimit || e.born < woundStore[oldest].born) oldest = i;
    }
    if (onPart >= woundsPerPart) {
        woundStore[oldest] = w;
    } else {
        woundStore[woundNext] = w;
        woundNext = (woundNext + 1) % woundLimit;
    }
    woundsTaken.fetch_add(1, std::memory_order_relaxed);
}

// Blood on the weapon that dealt a cut or a thrust. A version 2 record names that weapon; for a
// version 1 record the striker's weapon is found read-only: first the game's own impact bookkeeping (a character whose
// +C18 impact kind is set and whose +C20 target is the struck part names its weapon at +C38), and
// failing that the carried weapon whose length passes nearest the contact point (within 20 cm). The
// spot is kept in that weapon mesh's own frame (its sampled pose, +A0), the frame the held-item
// surface paints in; the held-item pass looks it up by mesh node.
constexpr unsigned bloodWeaponLimit = 16, bloodSpotsPerWeapon = 4;
struct WeaponBlood {
    uintptr_t node = 0;
    float local[bloodSpotsPerWeapon][3]{};
    float amount[bloodSpotsPerWeapon]{};
    ULONGLONG born[bloodSpotsPerWeapon]{};
    float drip[bloodSpotsPerWeapon]{};
    float lastAt[3]{};        // the mesh origin at the last reading, for the swing's speed (cast-off)
    ULONGLONG lastTick = 0;
    float castCooldown = 0.f;
};
static WeaponBlood weaponBlood[bloodWeaponLimit];
static unsigned weaponBloodNext = 0;
static std::atomic<unsigned> weaponBloodTaken{0};

static bool meshPose(uintptr_t node, float* pose) {
    if (!readable(node, field::sceneNode::renderTransform + 64)) return false;
    const float* sampled = (const float*)(node + field::sceneNode::renderTransform);
    for (int row = 0; row < 4; ++row)
        for (int axis = 0; axis < 3; ++axis) pose[row * 3 + axis] = sampled[row * 4 + axis];
    return std::isfinite(pose[9] + pose[10] + pose[11]);
}

static bool sectorOriginOf(uintptr_t node, float* origin); // view_effects.inl
// A hit point in a weapon mesh's own frame. The mesh's drawn pose is sector-local and whether the hit
// point is too is not established here, so both readings are tried and the one that lands on the weapon
// (nearest its centre line, within its length) wins; false when neither reads.
static bool weaponLocal(uintptr_t node, const float* pose, const float* point, float* local) {
    float origin[3]{};
    const bool sector = sectorOriginOf(node, origin) && (origin[0] || origin[1] || origin[2]);
    float best = 0.f;
    bool found = false;
    for (int candidate = 0; candidate < (sector ? 2 : 1); ++candidate) {
        float p[3], l[3];
        for (int i = 0; i < 3; ++i) p[i] = point[i] - (candidate ? origin[i] : 0.f);
        if (!intoPose(pose, p, true, l)) continue;
        const float beyond = l[2] < -30.f ? -30.f - l[2] : l[2] > 160.f ? l[2] - 160.f : 0.f;
        const float score = l[0] * l[0] + l[1] * l[1] + beyond * beyond;
        if (!found || score < best) {
            best = score;
            memcpy(local, l, sizeof l);
            found = true;
        }
    }
    return found;
}

static uintptr_t strikingWeaponNode(const CharacterHitRecord& h) {
    // Version 2 records name the delivering weapon's rigid body; its mesh node is at +0x18.
    if (h.weapon && readable(uintptr_t(h.weapon), field::bodyPart::meshNode + 8)) {
        const uintptr_t node = *(const uintptr_t*)(uintptr_t(h.weapon) + field::bodyPart::meshNode);
        if (node && readable(node, field::sceneNode::renderTransform + 64)) return node;
    }
    const uintptr_t world = *(const uintptr_t*)gameAt(fp::sig::world);
    if (!readable(world, field::worldObject::characters + 8)) return 0;
    const uintptr_t roster = *(const uintptr_t*)(world + field::worldObject::characters);
    if (!readable(roster, field::list::items + 8)) return 0;
    const int characters = *(const int*)(roster + field::list::count);
    const uintptr_t entries = *(const uintptr_t*)(roster + field::list::items);
    if (characters <= 0 || characters > 4096 || !readable(entries, size_t(characters) * 8)) return 0;
    uintptr_t best = 0;
    float bestDistance = 20.f;
    for (int i = 0; i < characters; ++i) {
        const uintptr_t character = *(const uintptr_t*)(entries + size_t(i) * 8);
        if (!character || character == uintptr_t(h.victim) || !readable(character, field::character::impactWeapon + 8) ||
            !readable(character, field::character::weapons + 16))
            continue;
        if (*(const int*)(character + field::character::impactKind) &&
            *(const uintptr_t*)(character + field::character::impactTarget) == uintptr_t(h.part)) {
            const uintptr_t weapon = *(const uintptr_t*)(character + field::character::impactWeapon);
            if (readable(weapon, field::bodyPart::meshNode + 8)) return *(const uintptr_t*)(weapon + field::bodyPart::meshNode);
        }
        const int count = std::clamp(*(const int*)(character + field::character::weaponCount), 0, 2);
        for (int k = 0; k < count; ++k) {
            const uintptr_t weapon = *(const uintptr_t*)(character + field::character::weapons + size_t(k) * 8);
            if (!readable(weapon, field::bodyPart::meshNode + 8)) continue;
            const uintptr_t node = *(const uintptr_t*)(weapon + field::bodyPart::meshNode);
            float pose[12], local[3];
            if (!meshPose(node, pose) || !weaponLocal(node, pose, h.point, local)) continue;
            // Along its own length (z), from just behind the grip to past any blade's tip.
            if (local[2] < -30.f || local[2] > 160.f) continue;
            const float off = std::sqrt(local[0] * local[0] + local[1] * local[1]);
            if (off < bestDistance) {
                bestDistance = off;
                best = node;
            }
        }
    }
    return best;
}

static void addWeaponBlood(const CharacterHitRecord& h) {
    const float permanent = std::isfinite(h.permanent) ? h.permanent : 0.f;
    if (h.kind == woundCrush || permanent < 1e-3f) return; // only what drew blood
    uintptr_t node = 0;
    float pose[12], local[3];
    __try {
        node = strikingWeaponNode(h);
        if (!node || !meshPose(node, pose) || !weaponLocal(node, pose, h.point, local)) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    WeaponBlood* entry = nullptr;
    for (WeaponBlood& w : weaponBlood)
        if (w.node == node) entry = &w;
    if (!entry) {
        entry = &weaponBlood[weaponBloodNext];
        weaponBloodNext = (weaponBloodNext + 1) % bloodWeaponLimit;
        *entry = {};
        entry->node = node;
    }
    const ULONGLONG now = GetTickCount64();
    // A new spot near an old one adds to it; else it takes the weakest slot.
    unsigned slot = 0;
    for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) {
        const float dx = entry->local[k][0] - local[0], dy = entry->local[k][1] - local[1], dz = entry->local[k][2] - local[2];
        if (entry->amount[k] > 0.f && dx * dx + dy * dy + dz * dz < 9.f) {
            slot = k;
            break;
        }
        if (entry->amount[k] < entry->amount[slot]) slot = k;
    }
    memcpy(entry->local[slot], local, sizeof local);
    entry->amount[slot] = (std::min)(entry->amount[slot] + 0.4f + 0.6f * (std::min)(permanent * 4.f, 1.f), 1.f);
    entry->born[slot] = now;
    weaponBloodTaken.fetch_add(1, std::memory_order_relaxed);
}

// A held mesh's blood for the held-item pass: item-space centre, and in w the amount (the fraction)
// plus how wet it still is, drying over two minutes, as hundredths in the whole part (0 for none).
static void weaponBloodFor(uintptr_t node, float (*out)[4]) {
    for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) out[k][0] = out[k][1] = out[k][2] = out[k][3] = 0.f;
    AcquireSRWLockShared(&woundLock);
    const ULONGLONG now = GetTickCount64();
    for (const WeaponBlood& w : weaponBlood) {
        if (w.node != node) continue;
        for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) {
            if (!(w.amount[k] > 0.f)) continue;
            const float wet = 1.f - (std::min)(float(now - w.born[k]) / 120000.f, 1.f);
            out[k][0] = w.local[k][0];
            out[k][1] = w.local[k][1];
            out[k][2] = w.local[k][2];
            out[k][3] = std::clamp(w.amount[k], 0.01f, 0.99f) + std::round(wet * 100.f);
        }
    }
    ReleaseSRWLockShared(&woundLock);
}

static void takeHits() {
    if (originalBodyDamage) return;
    const CharacterHits fn = characterHitsExport();
    if (!fn) return;
    // Version 1 records are 0x70 bytes apart; they are read into raw space and widened.
    const bool v1 = characterHitsVersion.load(std::memory_order_relaxed) == 1;
    const uint32_t stride = v1 ? characterHitsV1Size : uint32_t(sizeof(CharacterHitRecord));
    alignas(8) unsigned char raw[16 * sizeof(CharacterHitRecord)];
    for (int round = 0; round < 4; ++round) { // at most 64 new hits a frame
        int count = 0;
        __try { count = fn(woundSerial, reinterpret_cast<CharacterHitRecord*>(raw), 16); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            woundFaults.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (int k = 0; k < (std::min)(count, 16); ++k) {
            CharacterHitRecord hit{};
            memcpy(&hit, raw + size_t(k) * stride, stride);
            if (hit.size != stride) return;
            woundSerial = hit.serial; // oldest first
            hit.kind = woundCut; // the runtime's own kind is not shown by game code: one open wound
            addWound(hit);
            addWeaponBlood(hit);
        }
        if (count < 16) return;
    }
}

// The marks for one view: each live mark's centre and path from its part's pose now, in the native view
// space (row-major view matrix), nearest first. Whether the view takes sector-local or global points
// is not established here, so both readings are tried as for the held items and the nearer wins.
static void woundViewUniforms(const float* view, WoundUniforms& out) {
    out.count = 0;
    if (woundMarks.load(std::memory_order_relaxed) <= 0.f) return;
    struct Shown { float distanceSq; float centre[3], path[3]; unsigned index; };
    Shown shown[woundLimit];
    unsigned visible = 0;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&woundLock);
    takeHits();
    // Marked characters' scene nodes (character +0x18), and the seconds since the last call.
    uintptr_t mendNodes[woundMendLimit]{};
    uint32_t mendSeconds[woundMendLimit]{};
    int mendCount = 0;
    uintptr_t brokenNodes[woundMendLimit]{};
    uint32_t brokenBits[woundMendLimit]{};
    int brokenCount = 0;
    if (const WoundMarkNodes marks = woundMarkExport()) {
        __try { mendCount = marks("injuries.mend", mendNodes, mendSeconds, int(woundMendLimit)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { mendCount = 0; woundFaults.fetch_add(1, std::memory_order_relaxed); }
        mendCount = std::clamp(mendCount, 0, int(woundMendLimit));
        __try { brokenCount = marks("injuries.broken", brokenNodes, brokenBits, int(woundMendLimit)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { brokenCount = 0; woundFaults.fetch_add(1, std::memory_order_relaxed); }
        brokenCount = std::clamp(brokenCount, 0, int(woundMendLimit));
        woundReadLevels(marks, "injuries.burn", woundBurns, woundBurnCount);
        woundReadLevels(marks, "injuries.burnscar", woundBurnScars, woundBurnScarCount);
        woundReadLevels(marks, "injuries.severed", woundSevered, woundSeveredCount);
    }
    out.burning = woundBurnCount > 0 || woundBurnScarCount > 0;
    const float fadeDt = woundFadeTick ? (std::min)(float(now - woundFadeTick) / 1000.f, 0.5f) : 0.f;
    woundFadeTick = now;
    for (unsigned i = 0; i < woundLimit; ++i) {
        Wound& w = woundStore[i];
        if (!w.part) continue;
        float pose[12], sectorOrigin[3]{};
        uintptr_t owner = 0;
        bool read = false;
        __try {
            if (readable(w.part, field::bodyPart::rootPart + 8)) {
                owner = *(const uintptr_t*)(w.part + field::bodyPart::rootPart);
                memcpy(pose, (const void*)(w.part + field::bodyPart::pose), sizeof pose);
                const uintptr_t node = *(const uintptr_t*)(w.part + field::bodyPart::meshNode);
                const uintptr_t sector = node && readable(node, field::sceneNode::sector + 8)
                                             ? *(const uintptr_t*)(node + field::sceneNode::sector) : 0;
                if (sector && readable(sector, field::sector::origin + 12))
                    memcpy(sectorOrigin, (const void*)(sector + field::sector::origin), sizeof sectorOrigin);
                read = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            woundFaults.fetch_add(1, std::memory_order_relaxed);
        }
        if (!read || owner != w.owner) { // the character is gone, or the part was freed or reused
            if (w.mapped) ++woundSkinVersion;
            w = {};
            woundsRetired.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        uintptr_t ownerNode = 0;
        bool alive = true, ownerGone = false;
        __try {
            alive = (*(const unsigned*)(owner + field::character::stateFlags) & woundDeadState) == 0;
            if (mendCount || brokenCount) ownerNode = *(const uintptr_t*)(owner + field::character::sceneNode);
            (void)*(const volatile float*)(owner + field::character::bleed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ownerGone = true;
        }
        if (ownerGone) { // the character was freed while its part's memory still reads: retire, once
            if (w.mapped) ++woundSkinVersion;
            w = {};
            woundsRetired.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // A bruise over a bone Injuries reports broken is a fracture (and back to a bruise once it heals).
        if (w.kind == woundCrush) {
            bool fracture = false;
            // Which part index is which bone in Injuries' bits is not shown by game code: no fractures until it is.
            if constexpr (unprovenRenderReads) {
                const int bit = woundBoneBit(w.partIndex);
                for (int k = 0; ownerNode && bit >= 0 && k < brokenCount; ++k)
                    if (brokenNodes[k] == ownerNode) fracture = ((brokenBits[k] >> bit) & 1) != 0;
            }
            if (fracture != w.fracture) {
                w.fracture = fracture;
                if (w.mapped) ++woundSkinVersion;
            }
        }
        // Healing on the living: a cut closes into a scar that stays, a bruise fades away (not while its
        // bone is broken); Regenerate's mark sets its own, faster pace. The dead keep their wounds.
        if (alive && fadeDt > 0.f) {
            float mend = 0.f;
            for (int k = 0; ownerNode && k < mendCount; ++k)
                if (mendNodes[k] == ownerNode && mendSeconds[k]) mend = float(mendSeconds[k]);
            if (w.kind == woundCrush) {
                if (!w.fracture || mend > 0.f) w.fade -= fadeDt / (mend > 0.f ? mend : woundBruiseHealSeconds);
            } else if (w.scar < 1.f) {
                const float before = w.scar;
                w.scar = (std::min)(w.scar + fadeDt / (mend > 0.f ? mend : woundScarSeconds), 1.f);
                if (w.mapped && std::floor(before * 10.f) != std::floor(w.scar * 10.f)) ++woundSkinVersion;
            }
            if (w.fade <= 0.02f) { // a bruise gone
                if (w.mapped) ++woundSkinVersion;
                w = {};
                woundsMended.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        float flow = 0.f;
        if (w.kind != woundCrush && fadeDt > 0.f) {
            // Blood runs while the game says this character bleeds (and for the first seconds after any
            // cut), then dries. The dead keep bleeding out of their cuts for a few minutes, slower and slower.
            float bleed = 0.f;
            __try { bleed = *(const float*)(owner + field::character::bleed); }
            __except (EXCEPTION_EXECUTE_HANDLER) { bleed = 0.f; }
            if (!std::isfinite(bleed)) bleed = 0.f;
            const float age = float(now - w.born) / 1000.f;
            flow = w.scar > 0.3f ? 0.f // a closing cut has stopped bleeding
                 : (std::max)(std::clamp(bleed / woundBleedFull, 0.f, 1.f), age < woundFreshSeconds ? 0.6f : 0.f);
            if (!alive && w.severity > 0.2f) flow = (std::max)(flow, (0.3f + 0.5f * w.severity) * (std::max)(1.f - age / 180.f, 0.f));
            if (flow > 0.02f) w.run = (std::min)(w.run + fadeDt * (0.4f + 1.6f * flow), 4.f + 14.f * w.severity);
            if (flow > 0.15f) w.drip += fadeDt * flow * 2.f; // up to two drops a second while it pours
            w.wet = flow >= w.wet ? flow : (std::max)(w.wet - fadeDt / woundDrySeconds, flow);
        }
        float world[3], path[3];
        outOfPose(pose, w.local, true, world);
        outOfPose(pose, w.along, false, path);
        if (flow > 0.1f && fadeDt > 0.f) {
            // What runs out gathers below: a pool mark every so often, wider the more has run out (a lying
            // body's pool spreads; a standing one's gathers at its feet), thin so the layer builds it up.
            w.pool += fadeDt * flow * (0.4f + 0.6f * w.severity);
            w.poolTick -= fadeDt;
            if (w.poolTick <= 0.f) {
                w.poolTick = 0.45f;
                const float radius = (std::min)(4.f + 11.f * std::sqrt(w.pool), 48.f);
                queueSplat(world, sectorOrigin, nullptr, radius, radius, 0.03f + 0.03f * flow,
                           w.seed + float(i) * 0.37f + w.pool, 2);
            }
        }
        if (w.spurts && alive && fadeDt > 0.f) {
            // An opened artery: with each heartbeat a jet of drops lands in a line a metre or so out, the
            // same way each time (the vessel's direction), each drop a teardrop pointing the way it flew.
            w.spurt -= fadeDt;
            if (w.spurt <= 0.f) {
                w.spurt = 0.8f + 0.1f * float(w.spurts % 3);
                --w.spurts;
                const float turn = w.seed * 6.2831853f;
                const float dir[3] = {std::cos(turn), 0.f, std::sin(turn)};
                const float strength = 0.4f + 0.6f * float(w.spurts) / 8.f;
                for (int k = 0; k < 6; ++k) {
                    const float hash = std::fmod(w.seed * 131.f + float(k) * 17.17f + float(w.spurts) * 3.3f, 1.f);
                    const float reach = (30.f + 130.f * strength) * (0.35f + 0.65f * float(k) / 5.f) * (0.85f + 0.3f * hash);
                    const float side = (hash - 0.5f) * 14.f;
                    const float at[3] = {world[0] + dir[0] * reach - dir[2] * side, world[1], world[2] + dir[2] * reach + dir[0] * side};
                    queueSplat(at, sectorOrigin, dir, 0.9f + 1.6f * hash, 3.f + 7.f * strength * (0.5f + float(k) / 5.f),
                               0.45f + 0.3f * strength, w.seed + hash, 1);
                    WoundDrip& d = woundDrips[woundDripNext];
                    woundDripNext = (woundDripNext + 1) % woundDripLimit;
                    memcpy(d.from, at, sizeof d.from);
                    memcpy(d.start, world, sizeof d.start);
                    memcpy(d.sectorOrigin, sectorOrigin, sizeof d.sectorOrigin);
                    d.radius = 0.9f + 1.6f * hash;
                    d.flight = 0.25f + 0.2f * reach / 160.f;
                    d.born = now;
                }
            }
        }
        if (w.drip >= 1.f) {
            w.drip -= 1.f;
            WoundDrip& d = woundDrips[woundDripNext];
            woundDripNext = (woundDripNext + 1) % woundDripLimit;
            const unsigned salt = unsigned(now) * 2654435761u + i * 40503u;
            d.from[0] = world[0] + float(int(salt % 9) - 4);
            d.from[1] = world[1];
            d.from[2] = world[2] + float(int((salt >> 8) % 9) - 4);
            memcpy(d.sectorOrigin, sectorOrigin, sizeof d.sectorOrigin);
            d.radius = 1.2f + float((salt >> 16) % 25) / 10.f;
            d.born = now;
            memcpy(d.start, d.from, sizeof d.start);
            d.flight = 0.f;
            queueSplat(d.from, sectorOrigin, nullptr, d.radius, d.radius, 0.55f, float(salt % 1000) / 1000.f, 0);
        }
        Shown s{};
        s.distanceSq = woundReachCm * woundReachCm;
        bool found = false;
        for (int candidate = 0; candidate < 2; ++candidate) {
            float p[3], c[3];
            for (int k = 0; k < 3; ++k) p[k] = world[k] + (candidate ? sectorOrigin[k] : 0.f);
            for (int k = 0; k < 3; ++k) c[k] = p[0] * view[k] + p[1] * view[4 + k] + p[2] * view[8 + k] + view[12 + k];
            const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
            if (std::isfinite(sq) && sq < s.distanceSq && c[2] < 20.f) { // not behind the camera (it looks down -z)
                s.distanceSq = sq;
                memcpy(s.centre, c, sizeof c);
                found = true;
            }
            if (!candidate && !(sectorOrigin[0] || sectorOrigin[1] || sectorOrigin[2])) break;
        }
        if (!found) continue;
        for (int k = 0; k < 3; ++k) s.path[k] = path[0] * view[k] + path[1] * view[4 + k] + path[2] * view[8 + k];
        const float pathSq = s.path[0] * s.path[0] + s.path[1] * s.path[1] + s.path[2] * s.path[2];
        if (std::isfinite(pathSq) && pathSq > 1e-8f) {
            const float inverse = 1.f / std::sqrt(pathSq);
            for (float& v : s.path) v *= inverse;
        } else {
            memset(s.path, 0, sizeof s.path); // the shader picks a direction on the surface
        }
        s.index = i;
        shown[visible++] = s;
    }
    const unsigned count = (std::min)(visible, woundShownLimit);
    std::partial_sort(shown, shown + count, shown + visible,
                      [](const Shown& l, const Shown& r) { return l.distanceSq < r.distanceSq; });
    for (unsigned k = 0; k < count; ++k) {
        const Shown& s = shown[k];
        const Wound& w = woundStore[s.index];
        // Kind in the whole part of b[3]; its fraction carries a cut's wetness or a bruise's age, 0..1.
        const float age = float(now - w.born) / 1000.f;
        // Kind + 4 for a scarring cut, its fraction then the scar's progress.
        const bool scarring = w.kind != woundCrush && w.scar > 0.001f;
        const float aux = scarring ? (std::min)(w.scar, 1.f)
                        : w.kind == woundCrush ? (std::min)(age / woundBruiseSeconds, 1.f) : std::clamp(w.wet, 0.f, 1.f);
        // A closing wound shrinks towards its middle and pales.
        const float a[4] = {s.centre[0], s.centre[1], s.centre[2], w.halfLength * w.fade * (w.fracture ? woundFractureScale : 1.f)};
        const float b[4] = {s.path[0], s.path[1], s.path[2], float(w.kind + (scarring ? 4 : 0)) + 0.98f * aux};
        // A mark baked into the skin passes its seed negated: the screen pass then leaves its skin part out.
        const bool baked = w.mapped && woundSkinBaked(w.owner);
        // A fracture passes its severity above 1.25 (the screen pass reads it as one).
        const float c[4] = {w.severity * w.fade + (w.fracture ? 1.5f : 0.f), baked ? -(w.seed + 0.001f) : w.seed,
                            w.halfWidth * w.fade, w.run * w.fade * (1.f - w.scar)};
        memcpy(out.a[k], a, sizeof a);
        memcpy(out.b[k], b, sizeof b);
        memcpy(out.c[k], c, sizeof c);
        const float m[4] = {float(w.material), 0.f, 0.f, 0.f};
        memcpy(out.m[k], m, sizeof m);
    }
    out.count = count;
    activeWoundsCount.store(visible, std::memory_order_relaxed);
    float maxWeaponSaturation = 0.f;
    for (WeaponBlood& wb : weaponBlood) {
        if (!wb.node) continue;
        float wpose[12];
        if (!meshPose(wb.node, wpose)) continue;
        float wSector[3]{};
        __try {
            if (readable(wb.node, field::sceneNode::sector + 8)) {
                const uintptr_t sector = *(const uintptr_t*)(wb.node + field::sceneNode::sector);
                if (sector && readable(sector, field::sector::origin + 12))
                    memcpy(wSector, (const void*)(sector + field::sector::origin), sizeof wSector);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) {
            if (wb.amount[k] > maxWeaponSaturation) maxWeaponSaturation = wb.amount[k];
            if (wb.amount[k] <= 0.35f) continue;
            const float age = float(now - wb.born[k]) / 1000.f;
            if (age >= 120.f) continue;
            const float wet = 1.f - age / 120.f;
            if (wet <= 0.15f) continue;
            if (fadeDt > 0.f) {
                wb.drip[k] += fadeDt * wb.amount[k] * wet * 0.8f;
                if (wb.drip[k] >= 1.f) {
                    wb.drip[k] -= 1.f;
                    float world[3];
                    outOfPose(wpose, wb.local[k], true, world);
                    WoundDrip& d = woundDrips[woundDripNext];
                    woundDripNext = (woundDripNext + 1) % woundDripLimit;
                    const unsigned salt = unsigned(now) * 2654435761u + k * 131071u + woundDripNext * 524287u;
                    d.from[0] = world[0] + float(int(salt % 7) - 3);
                    d.from[1] = world[1];
                    d.from[2] = world[2] + float(int((salt >> 8) % 7) - 3);
                    memcpy(d.sectorOrigin, wSector, sizeof d.sectorOrigin);
                    d.radius = 1.0f + float((salt >> 16) % 18) / 10.f;
                    d.born = now;
                    memcpy(d.start, d.from, sizeof d.start);
                    d.flight = 0.f;
                    queueSplat(d.from, wSector, nullptr, d.radius, d.radius, 0.5f, float(salt % 1000) / 1000.f, 0);
                }
            }
        }
        // Cast-off: a fast swing flings wet blood off the blade, in the swing's direction, as a line of
        // teardrops a metre or two out. The blade's speed is its mesh origin's between two readings.
        if (fadeDt > 0.f) {
            float v[3] = {wpose[9] - wb.lastAt[0], wpose[10] - wb.lastAt[1], wpose[11] - wb.lastAt[2]};
            const bool had = wb.lastTick != 0;
            const float dt = had ? float(now - wb.lastTick) / 1000.f : 0.f;
            memcpy(wb.lastAt, wpose + 9, sizeof wb.lastAt);
            wb.lastTick = now;
            wb.castCooldown = (std::max)(wb.castCooldown - fadeDt, 0.f);
            if (had && dt > 0.004f && dt < 0.2f) {
                for (float& c : v) c /= dt;
                const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const float flat = std::sqrt(v[0] * v[0] + v[2] * v[2]);
                if (std::isfinite(speed) && speed > 380.f && flat > 150.f && wb.castCooldown <= 0.f) {
                    for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) {
                        if (wb.amount[k] <= 0.45f) continue;
                        const float age = float(now - wb.born[k]) / 1000.f;
                        if (age >= 40.f) continue; // only still-liquid blood flies
                        float from[3];
                        outOfPose(wpose, wb.local[k], true, from);
                        const float dir[3] = {v[0] / flat, 0.f, v[2] / flat};
                        const float flight = (std::min)(speed, 1400.f) * 0.22f; // about a fifth of a second in the air
                        const unsigned salt = unsigned(now) * 2654435761u + k * 7919u;
                        const int drops = 1 + int(salt % 3);
                        for (int j = 0; j < drops; ++j) {
                            const float h = float((salt >> (4 * j)) % 100) / 100.f;
                            const float reach = flight * (0.5f + 0.7f * h), side = (h - 0.5f) * 30.f;
                            const float at[3] = {from[0] + dir[0] * reach - dir[2] * side, from[1],
                                                 from[2] + dir[2] * reach + dir[0] * side};
                            queueSplat(at, wSector, dir, 0.7f + 1.2f * h, 3.f + 9.f * (std::min)(speed / 1000.f, 1.f),
                                       0.4f, float(salt % 1000) / 1000.f + h, 1);
                            WoundDrip& d = woundDrips[woundDripNext];
                            woundDripNext = (woundDripNext + 1) % woundDripLimit;
                            memcpy(d.from, at, sizeof d.from);
                            memcpy(d.start, from, sizeof d.start);
                            memcpy(d.sectorOrigin, wSector, sizeof d.sectorOrigin);
                            d.radius = 0.7f + 1.2f * h;
                            d.flight = 0.2f + 0.15f * h;
                            d.born = now;
                        }
                        wb.amount[k] = (std::max)(wb.amount[k] - 0.12f, 0.f);
                        wb.castCooldown = 0.15f;
                    }
                }
            }
        }
    }
    activeWeaponBloodMax.store(maxWeaponSaturation, std::memory_order_relaxed);
    // The drops, as their point in view space (the nearer reading of the sector origin, as for the wounds);
    // gone after two minutes, dry after half of one. Those already shown first, then new ones nearest first
    // into what places are left (woundDripShownBorn).
    struct ShownDrip { float distanceSq, centre[3], w; unsigned index; bool kept; };
    ShownDrip drops[woundDripLimit];
    unsigned dropCount = 0;
    out.falls = 0;
    for (unsigned di = 0; di < woundDripLimit; ++di) {
        WoundDrip& d = woundDrips[di];
        if (!d.born) continue;
        // In the air: along its path from start to from (or straight down), falling under gravity, for its
        // first 1.2 s (a floor 2 m down is reached in 0.65 s; what falls further is out of sight).
        const float airAge = float(now - d.born) / 1000.f;
        if (airAge < 1.2f && out.falls < 16) {
            const float g981 = 981.f;
            const float span = d.flight > 0.f ? (std::min)(airAge / d.flight, 1.f) : 0.f;
            float at[3], vel[3];
            for (int k = 0; k < 3; ++k) {
                at[k] = d.start[k] + (d.from[k] - d.start[k]) * span;
                vel[k] = d.flight > 0.f && span < 1.f ? (d.from[k] - d.start[k]) / d.flight : 0.f;
            }
            at[1] -= 0.5f * g981 * airAge * airAge;
            vel[1] -= g981 * airAge;
            float best = woundReachCm * woundReachCm, c[3]{};
            bool found = false;
            for (int candidate = 0; candidate < 2; ++candidate) {
                float q[3], v[3];
                for (int k = 0; k < 3; ++k) q[k] = at[k] + (candidate ? d.sectorOrigin[k] : 0.f);
                for (int k = 0; k < 3; ++k) v[k] = q[0] * view[k] + q[1] * view[4 + k] + q[2] * view[8 + k] + view[12 + k];
                const float sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                if (std::isfinite(sq) && sq < best) { best = sq; memcpy(c, v, sizeof c); found = true; }
            }
            if (found) {
                const unsigned k = out.falls++;
                const float a[4] = {c[0], c[1], c[2], (std::max)(0.25f * d.radius + 0.12f, 0.28f)};
                const float b[4] = {vel[0] * view[0] + vel[1] * view[4] + vel[2] * view[8],
                                    vel[0] * view[1] + vel[1] * view[5] + vel[2] * view[9],
                                    vel[0] * view[2] + vel[1] * view[6] + vel[2] * view[10], 0.f};
                memcpy(out.fA[k], a, sizeof a);
                memcpy(out.fB[k], b, sizeof b);
            }
        }
        const float age = float(now - d.born) / 1000.f;
        if (age > woundDripSeconds) { d = {}; woundDripShownBorn[di] = 0; continue; }
        ShownDrip s{woundReachCm * woundReachCm, {}, 0.f, di, woundDripShownBorn[di] == d.born};
        bool found = false;
        for (int candidate = 0; candidate < 2; ++candidate) {
            float q[3], c[3];
            for (int k = 0; k < 3; ++k) q[k] = d.from[k] + (candidate ? d.sectorOrigin[k] : 0.f);
            for (int k = 0; k < 3; ++k) c[k] = q[0] * view[k] + q[1] * view[4 + k] + q[2] * view[8 + k] + view[12 + k];
            const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
            if (std::isfinite(sq) && sq < s.distanceSq) { s.distanceSq = sq; memcpy(s.centre, c, sizeof c); found = true; }
        }
        if (!found) continue;
        const float dry = (std::min)(age / woundDripDrySeconds, 1.f);
        s.w = 256.f * std::round(d.radius * 10.f) + std::floor(dry * 255.f); // radius in tenths; dryness in 256 steps
        drops[dropCount++] = s;
    }
    activeDripsCount.store(dropCount, std::memory_order_relaxed);
    // Kept drops never outnumber the places (a place is only given while one is free).
    unsigned kept = 0;
    for (unsigned k = 0; k < woundDripLimit; ++k)
        if (woundDripShownBorn[k] && woundDripShownBorn[k] != woundDrips[k].born) woundDripShownBorn[k] = 0; // reused
        else if (woundDripShownBorn[k]) ++kept;
    std::sort(drops, drops + dropCount, [](const ShownDrip& l, const ShownDrip& r) {
        return l.kept != r.kept ? l.kept : l.distanceSq < r.distanceSq;
    });
    unsigned dripCount = 0;
    for (unsigned k = 0; k < dropCount && dripCount < woundDripShown; ++k) {
        if (!drops[k].kept) {
            if (kept >= woundDripShown) break; // full: a new drop waits until a shown one dries away
            woundDripShownBorn[drops[k].index] = woundDrips[drops[k].index].born;
            ++kept;
        }
        drops[dripCount++] = drops[k];
    }
    for (unsigned k = 0; k < dripCount; ++k) {
        const float v[4] = {drops[k].centre[0], drops[k].centre[1], drops[k].centre[2], drops[k].w};
        memcpy(out.d[k], v, sizeof v);
    }
    out.drips = dripCount;
    // The floor layer's new marks, oldest first, in view space (the nearer reading of the sector origin, as
    // for the wounds); what does not fit this frame waits for the next.
    out.splats = 0;
    while (woundSplatHead != woundSplatTail && out.splats < woundSplatShown) {
        const WoundSplat& q = woundSplats[woundSplatHead++ % woundSplatQueue];
        float best = woundReachCm * woundReachCm, centre[3]{};
        bool found = false;
        for (int candidate = 0; candidate < 2; ++candidate) {
            float p[3], c[3];
            for (int k = 0; k < 3; ++k) p[k] = q.at[k] + (candidate ? q.sectorOrigin[k] : 0.f);
            for (int k = 0; k < 3; ++k) c[k] = p[0] * view[k] + p[1] * view[4 + k] + p[2] * view[8 + k] + view[12 + k];
            const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
            if (std::isfinite(sq) && sq < best) { best = sq; memcpy(centre, c, sizeof c); found = true; }
        }
        if (!found) continue;
        float dir[3] = {q.dir[0] * view[0] + q.dir[1] * view[4] + q.dir[2] * view[8],
                        q.dir[0] * view[1] + q.dir[1] * view[5] + q.dir[2] * view[9],
                        q.dir[0] * view[2] + q.dir[1] * view[6] + q.dir[2] * view[10]};
        if (!std::isfinite(dir[0] + dir[1] + dir[2])) dir[0] = dir[1] = dir[2] = 0.f;
        const unsigned k = out.splats++;
        const float a[4] = {centre[0], centre[1], centre[2], q.across};
        const float b[4] = {dir[0], dir[1], dir[2], q.along};
        const float c[4] = {q.thickness, q.seed, float(q.kind), 0.f};
        memcpy(out.sA[k], a, sizeof a);
        memcpy(out.sB[k], b, sizeof b);
        memcpy(out.sC[k], c, sizeof c);
    }
    woundSeveredCaps(view, out);
    ReleaseSRWLockExclusive(&woundLock);
}

static void emitImpactSpatter(const CharacterHitRecord& r, const float* sectorOrigin) {
    if (r.kind == woundCrush && r.permanent < 0.05f) return;
    if (r.permanent < 1e-3f) return;
    const ULONGLONG now = GetTickCount64();
    const float speed = std::sqrt(r.direction[0] * r.direction[0] +
                                  r.direction[1] * r.direction[1] +
                                  r.direction[2] * r.direction[2]);
    const float dirNorm[3] = {
        speed > 1e-3f ? r.direction[0] / speed : 0.f,
        speed > 1e-3f ? r.direction[1] / speed : -0.3f,
        speed > 1e-3f ? r.direction[2] / speed : 0.f
    };
    const int count = std::clamp(int(r.permanent * 16.f + speed * 0.15f) + 1, 2, 5);
    for (int k = 0; k < count; ++k) {
        WoundDrip& d = woundDrips[woundDripNext];
        woundDripNext = (woundDripNext + 1) % woundDripLimit;
        const unsigned salt = unsigned(now) * 2654435761u + k * 524287u + woundDripNext * 131071u;
        const float spreadDist = 3.f + float(salt % 14);
        const float alongDist = 6.f + float((salt >> 4) % 22) + (std::min)(speed * 0.35f, 30.f);
        const float lateralX = float(int((salt >> 8) % 17) - 8) / 8.f;
        const float lateralZ = float(int((salt >> 12) % 17) - 8) / 8.f;

        d.from[0] = r.point[0] + dirNorm[0] * alongDist + lateralX * spreadDist;
        d.from[1] = r.point[1] + dirNorm[1] * alongDist * 0.4f;
        d.from[2] = r.point[2] + dirNorm[2] * alongDist + lateralZ * spreadDist;
        std::memcpy(d.sectorOrigin, sectorOrigin, sizeof(d.sectorOrigin));
        d.radius = 0.9f + float((salt >> 16) % 22) / 10.f; // 0.9 to 3.1 cm
        d.born = now;
        std::memcpy(d.start, r.point, sizeof d.start);
        d.flight = 0.12f + 0.1f * float((salt >> 20) % 10) / 10.f;
        // On the floor layer a drop flung by the blow lands as a teardrop pointing the way it flew.
        const float flatLen = std::sqrt(dirNorm[0] * dirNorm[0] + dirNorm[2] * dirNorm[2]);
        const float flat[3] = {flatLen > 1e-3f ? dirNorm[0] / flatLen : 0.f, 0.f, flatLen > 1e-3f ? dirNorm[2] / flatLen : 0.f};
        queueSplat(d.from, sectorOrigin, flatLen > 1e-3f ? flat : nullptr, d.radius * 0.8f,
                   d.radius * (1.5f + (std::min)(speed * 0.004f, 3.f)), 0.5f, float(salt % 1000) / 1000.f, 1);
    }
}

// The kind of wound a hit leaves, from the game's own numbers. A thrust: the striker's motion is the one
// physicalImpact (0x159B28) measures along the weapon's own axis (witness physicalImpact::thrustMotion), so a
// puncture. Otherwise the striking weapon's largest damage type: its weapon data (part +6E0, found as 0x1531E0
// finds it, walking parent links +428; witness weaponDataOf::walk) holds slash +2C, crush +30 and pierce +34,
// which the builder 0x165AD0 makes from the item bytes the stats sheet labels Slash, Crush and Pierce for the
// current grip (witness weaponDataBuild::damageTypes). Slash makes a gash, crush a bruise, pierce a puncture.
// These are the weapon's values before armour; the split after armour exists only on physicalImpact's stack.
constexpr size_t woundWeaponData = 0x6E0, woundPartParent = 0x428;
constexpr size_t woundSlash = 0x2C, woundCrushValue = 0x30, woundPierce = 0x34;
static uint8_t woundKindOfHit(uintptr_t attacker, uintptr_t weapon) {
    if (readable(attacker + field::character::motion, sizeof(uint32_t)) &&
        *reinterpret_cast<const uint32_t*>(attacker + field::character::motion) == woundThrustMotion)
        return woundThrust;
    uintptr_t part = weapon, data = 0;
    for (int step = 0; part && step < 16; ++step) { // 0x1531E0's walk, bounded
        if (!readable(part, woundPartParent + 8) || !readable(part, woundWeaponData + 8)) return woundCut;
        data = *reinterpret_cast<const uintptr_t*>(part + woundWeaponData);
        if (data) break;
        const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(part + woundPartParent);
        if (parent == part) break;
        part = parent;
    }
    if (!data || !readable(data, woundPierce + 4)) return woundCut;
    const float slash = *reinterpret_cast<const float*>(data + woundSlash);
    const float crush = *reinterpret_cast<const float*>(data + woundCrushValue);
    const float pierce = *reinterpret_cast<const float*>(data + woundPierce);
    if (!std::isfinite(slash + crush + pierce)) return woundCut;
    if (crush > slash && crush >= pierce) return woundCrush;
    if (pierce > slash && pierce > crush) return woundThrust;
    return woundCut;
}

static void recordNativeHit(uintptr_t victim, uintptr_t attacker, float permanent, float recoverable) {
    if (!attacker || !victim) return;
    if (woundMarks.load(std::memory_order_relaxed) <= 0.f) return;

    CharacterHitRecord r{};
    r.size = sizeof(r);
    r.victim = victim;
    r.permanent = permanent;
    r.recoverable = recoverable;

    uintptr_t part = 0;
    int32_t index = -1;
    if (!readable(attacker + field::character::impactTarget, sizeof(uintptr_t))) return;
    part = *reinterpret_cast<const uintptr_t*>(attacker + field::character::impactTarget);
    if (!part || !readable(part + field::bodyPart::index, sizeof(int32_t))) return;
    index = *reinterpret_cast<const int32_t*>(part + field::bodyPart::index);
    if (index < 0 || index >= 18) return;
    r.part = part;

    if (!readable(attacker + field::character::impactPoint, 12) || !readable(part + field::bodyPart::pose, 48)) return;
    std::memcpy(r.point, reinterpret_cast<const void*>(attacker + field::character::impactPoint), 12);
    std::memcpy(r.pose, reinterpret_cast<const void*>(part + field::bodyPart::pose), 48);

    uintptr_t weapon = 0;
    float now[12]{}, before[12]{};
    if (readable(attacker + field::character::impactWeapon, sizeof(uintptr_t))) {
        weapon = *reinterpret_cast<const uintptr_t*>(attacker + field::character::impactWeapon);
        if (weapon && readable(weapon + field::bodyPart::points, 48) && readable(weapon + field::bodyPart::history, 48)) {
            std::memcpy(now, reinterpret_cast<const void*>(weapon + field::bodyPart::points), 48);
            std::memcpy(before, reinterpret_cast<const void*>(weapon + field::bodyPart::history), 48);
            for (int axis = 0; axis < 3; ++axis) {
                float a = 0.f, b = 0.f;
                for (int k = 0; k < 4; ++k) {
                    a += now[k * 3 + axis];
                    b += before[k * 3 + axis];
                }
                r.direction[axis] = (a - b) * 0.25f;
            }
        }
    }
    r.weapon = weapon;

    r.kind = woundKindOfHit(attacker, weapon);

    float sectorOrigin[3]{};
    if (part && readable(part + field::bodyPart::meshNode, sizeof(uintptr_t))) {
        const uintptr_t node = *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::meshNode);
        if (node && readable(node + field::sceneNode::sector, sizeof(uintptr_t))) {
            const uintptr_t sector = *reinterpret_cast<const uintptr_t*>(node + field::sceneNode::sector);
            if (sector && readable(sector + field::sector::origin, sizeof(sectorOrigin))) {
                std::memcpy(sectorOrigin, reinterpret_cast<const void*>(sector + field::sector::origin), sizeof(sectorOrigin));
            }
        }
    }

    AcquireSRWLockExclusive(&woundLock);
    r.serial = ++woundSerial;
    addWound(r);
    addWeaponBlood(r);
    emitImpactSpatter(r, sectorOrigin);
    ReleaseSRWLockExclusive(&woundLock);
}

// The damage routine 0x159700: it lowers +D58 by 0.1 times its second argument (0.05 times when +DF8 has
// 0x800000) and +D5C by 0.2 times its third (0x15972E-0x1597C5). The names below are the record's; what the
// two values mean beyond that arithmetic is not established here.
static unsigned char bodyDamageDetour(uintptr_t character, float recoverable, float permanent, char mayKill) {
    const auto result = originalBodyDamage(character, recoverable, permanent, mayKill);
    __try {
        recordNativeHit(character, currentImpactAttacker, permanent, recoverable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundFaults.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

static void physicalImpactDetour(uintptr_t attacker) {
    const auto outer = currentImpactAttacker;
    currentImpactAttacker = attacker;
    __try {
        originalPhysicalImpact(attacker);
    } __finally {
        currentImpactAttacker = outer;
    }
}

static void createWoundHooks() {
    if (!layout.optionalGroupAvailable("wounds")) return;
    const uintptr_t impactTarget = optionalGameAt(fp::sig::woundImpact);
    const uintptr_t damageTarget = optionalGameAt(fp::sig::bodyDamage);

    static const unsigned char applySig[] = {0x53, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xd8, 0x48, 0x89, 0xcb, 0x40, 0x30, 0xf6};
    static const unsigned char impactSig[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0xa4, 0x24, 0x90, 0xfe, 0xff, 0xff};

    if (damageTarget && readable(damageTarget, sizeof(applySig)) && std::memcmp(reinterpret_cast<const void*>(damageTarget), applySig, sizeof(applySig)) == 0) {
        MH_CreateHook(reinterpret_cast<void*>(damageTarget),
                      reinterpret_cast<void*>(bodyDamageDetour),
                      reinterpret_cast<void**>(&originalBodyDamage));
    }

    if (impactTarget && readable(impactTarget, sizeof(impactSig)) && std::memcmp(reinterpret_cast<const void*>(impactTarget), impactSig, sizeof(impactSig)) == 0) {
        MH_CreateHook(reinterpret_cast<void*>(impactTarget),
                      reinterpret_cast<void*>(physicalImpactDetour),
                      reinterpret_cast<void**>(&originalPhysicalImpact));
    }
}

