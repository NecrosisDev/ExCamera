// Wounds: where a weapon got through, the struck character keeps a mark that moves with the limb: a
// gash along the blade's path for a cut, a short deep hole for a thrust, a bruise for a crush, and
// blood running down from fresh cuts. The hits come from ScriptRuntime.dll's character_hits export
// (version 1), which records them inside the game's damage applier; ExCamera only reads them. Each
// mark is kept in its body part's own frame and the finishing pass (view_effects.inl) draws it on
// the surface at that spot, so a blow the armour stopped leaves nothing (it dealt no damage) and a
// cut through cloth shows on the cloth. No native memory is written.

static std::atomic<float> woundMarks{1.f}; // the Wounds row: 0 off

// The runtime's record, shared with ScriptRuntime's character_hits: version 2 (0x78 bytes); a version 1
// runtime writes the first 0x70 bytes only (no weapon, kind 0 or 3).
struct CharacterHitRecord {
    uint32_t size, serial;
    uint64_t victim, part; // the struck character and its struck body part (a rigid body)
    float point[3];        // contact point, in the space of the part's pose
    float direction[3];    // the weapon's motion at contact, same space; zero when unknown
    float pose[12];        // the part's achieved pose at the hit: three axes then the origin
    float health, balance; // what the damage applier received
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
// The character's bleeding rate (0..20): 0x168410 raises it from the wound inflow (+0x10D4) and, above
// 1, drains health and balance with it; it falls by 0.1 a tick once the inflow stops.
constexpr size_t woundCharacterBleed = 0x10DC;
// Injuries' "injuries.broken" mark: one bit per limb bone, the part indices (+0x468) each covers. A fracture
// bruise spreads this much wider.
constexpr size_t woundPartIndex = 0x468;
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
// A struck body part's character, where the game's weapon impact (0x159A60) reads its victim; that impact
// hands the same pointer to the damage applier, so it equals the record's victim.
constexpr size_t woundPartCharacter = 0x420;

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
    uint8_t kind = 0;
};

// What the finishing pass takes: view-space marks, nearest first.
constexpr unsigned woundDripLimit = 96, woundDripShown = 24;
constexpr float woundDripSeconds = 120.f, woundDripDrySeconds = 30.f;
// On the living: an open cut closes into a scar over this long (the Regenerate mark's own time while it
// runs); a bruise fades away over this long.
constexpr float woundScarSeconds = 300.f, woundBruiseHealSeconds = 900.f;
struct WoundUniforms {
    unsigned count = 0;
    unsigned drips = 0;      // blood drops on the floor, below
    bool burning = false;    // a character in view is burnt (the skin bake has work)
    float d[woundDripShown][4]; // a drop: view-space point it fell from; w: radius (cm) + 10 x dryness step (0..9)
    unsigned caps = 0;          // flesh caps on severed limbs: stump and end
    float capA[8][4], capB[8][4]; // centre (view space), radius (cm); facing (view space), 0 stump / 1 the severed end
    bool any() const { return count || drips || burning || caps; }
    float a[woundShownLimit][4]; // centre; w: half length, cm (a bruise's radius)
    float b[woundShownLimit][4]; // the blade's path, unit; w: kind
    float c[woundShownLimit][4]; // severity 0..1, seed, half width (cm), blood run length (cm)
};

static Wound woundStore[woundLimit];
static unsigned woundNext = 0;
static uint32_t woundSerial = 0; // the newest record taken
static SRWLOCK woundLock = SRWLOCK_INIT;
static std::atomic<CharacterHits> characterHits{nullptr};
static std::atomic<ULONGLONG> characterHitsProbed{0};
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
struct WoundDrip { float from[3], sectorOrigin[3], radius; ULONGLONG born; };
static WoundDrip woundDrips[96];
static unsigned woundDripNext = 0;
static bool woundSkinBaked(uintptr_t character);            // wounds_bake.inl
static void bakeWoundSkins(gl::Api& g, fp::CueResources& r); // wounds_bake.inl, from the view-effects pass

// Regeneration (the Injuries addon's "injuries.mend" mark, value = seconds to close a wound
// fully): ScriptRuntime.dll's character_mark_nodes export (marks version 1).
using WoundMarkNodes = int (*)(const char*, uintptr_t*, uint32_t*, int);
using WoundMarksVersion = uint32_t (*)();
constexpr unsigned woundMendLimit = 64;
static std::atomic<WoundMarkNodes> woundMarkNodes{nullptr};
static std::atomic<ULONGLONG> woundMarksProbed{0};
static ULONGLONG woundFadeTick = 0; // woundLock

static WoundMarkNodes woundMarkExport() {
    if (auto fn = woundMarkNodes.load(std::memory_order_acquire)) return fn;
    const ULONGLONG now = GetTickCount64();
    if (now - woundMarksProbed.load(std::memory_order_relaxed) < 2000) return nullptr;
    woundMarksProbed.store(now, std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) return nullptr;
    const auto version = reinterpret_cast<WoundMarksVersion>(GetProcAddress(runtime, "character_marks_version"));
    const auto fn = reinterpret_cast<WoundMarkNodes>(GetProcAddress(runtime, "character_mark_nodes"));
    if (!version || !fn || version() != 1) return nullptr;
    woundMarkNodes.store(fn, std::memory_order_release);
    return fn;
}

// The runtime's export, looked up again at most every two seconds until found; the module is pinned
// once found, so the cached entry stays valid for the life of the process.
static CharacterHits characterHitsExport() {
    if (auto fn = characterHits.load(std::memory_order_acquire)) return fn;
    const ULONGLONG now = GetTickCount64();
    if (now - characterHitsProbed.load(std::memory_order_relaxed) < 2000) return nullptr;
    characterHitsProbed.store(now, std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) return nullptr;
    const auto version = reinterpret_cast<CharacterHitsVersion>(GetProcAddress(runtime, "character_hits_version"));
    const auto fn = reinterpret_cast<CharacterHits>(GetProcAddress(runtime, "character_hits"));
    const uint32_t found = version ? version() : 0;
    if (!fn || (found != 1 && found != 2)) return nullptr;
    characterHitsVersion.store(found, std::memory_order_relaxed);
    characterHits.store(fn, std::memory_order_release);
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
    const float health = std::isfinite(h.health) ? h.health : 0.f;
    const float balance = std::isfinite(h.balance) ? h.balance : 0.f;
    // PROVISIONAL(2026-10-22, until the runtime reports the weapon's cut/crush split a cut is told
    // from a blow by moving more health per balance, ratify-by: 2026-11-05)
    const uint8_t kind = h.kind >= woundCut && h.kind <= woundThrust ? uint8_t(h.kind)
                         : health > 0.4f * balance                  ? woundCut
                                                                    : woundCrush;
    if (kind == woundCrush ? balance < 0.05f && health < 1e-3f : health < 1e-3f) return; // nothing got through
    Wound w;
    w.part = uintptr_t(h.part);
    float healthMax = 1.f;
    __try {
        if (!w.part || !readable(w.part, woundPartCharacter + 8)) return;
        w.owner = *(const uintptr_t*)(w.part + woundPartCharacter);
        if (h.victim && readable(uintptr_t(h.victim), field::character::healthMax + 4))
            healthMax = *(const float*)(uintptr_t(h.victim) + field::character::healthMax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!w.owner || w.owner != uintptr_t(h.victim) || !intoPose(h.pose, h.point, true, w.local)) return;
    if (!intoPose(h.pose, h.direction, false, w.along)) memset(w.along, 0, sizeof w.along);
    if (!std::isfinite(healthMax) || healthMax < 0.05f) healthMax = 1.f;
    // PROVISIONAL(2026-10-22, size from damage: a third of full health fills the scale; tune after
    // Hunter's first look, ratify-by: 2026-11-05)
    w.severity = kind == woundCrush ? std::clamp(0.35f + 0.8f * balance, 0.35f, 1.f)
                                    : std::clamp(0.35f + 2.f * health / healthMax, 0.35f, 1.f);
    w.kind = kind;
    __try {
        const int index = *(const int*)(w.part + woundPartIndex);
        w.partIndex = int8_t(index >= 0 && index < 18 ? index : -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w.partIndex = -1;
    }
    w.born = GetTickCount64();
    w.seed = float((h.serial * 2654435761u) >> 8) / float(1u << 24);
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
            if (!meshPose(node, pose) || !intoPose(pose, h.point, true, local)) continue;
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
    const float health = std::isfinite(h.health) ? h.health : 0.f;
    if (h.kind == woundCrush || health < 1e-3f) return; // only what drew blood
    uintptr_t node = 0;
    float pose[12], local[3];
    __try {
        node = strikingWeaponNode(h);
        if (!node || !meshPose(node, pose) || !intoPose(pose, h.point, true, local)) return;
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
    entry->amount[slot] = (std::min)(entry->amount[slot] + 0.4f + 0.6f * (std::min)(health * 4.f, 1.f), 1.f);
    entry->born[slot] = now;
    weaponBloodTaken.fetch_add(1, std::memory_order_relaxed);
}

// A held mesh's blood for the held-item pass: item-space centre, and in w the amount (the fraction)
// plus how wet it still is, drying over two minutes, as tenths in the whole part (0 for none).
static void weaponBloodFor(uintptr_t node, float (*out)[4]) {
    for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) out[k][0] = out[k][1] = out[k][2] = out[k][3] = 0.f;
    if (!TryAcquireSRWLockShared(&woundLock)) return;
    const ULONGLONG now = GetTickCount64();
    for (const WeaponBlood& w : weaponBlood) {
        if (w.node != node) continue;
        for (unsigned k = 0; k < bloodSpotsPerWeapon; ++k) {
            if (!(w.amount[k] > 0.f)) continue;
            const float wet = 1.f - (std::min)(float(now - w.born[k]) / 120000.f, 1.f);
            out[k][0] = w.local[k][0];
            out[k][1] = w.local[k][1];
            out[k][2] = w.local[k][2];
            out[k][3] = std::clamp(w.amount[k], 0.01f, 0.99f) + std::round(wet * 10.f);
        }
    }
    ReleaseSRWLockShared(&woundLock);
}

static void takeHits() {
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
    if (const WoundMarkNodes marks = woundMarkExport()) {
        __try { mendCount = marks("injuries.mend", mendNodes, mendSeconds, int(woundMendLimit)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { mendCount = 0; woundFaults.fetch_add(1, std::memory_order_relaxed); }
        mendCount = std::clamp(mendCount, 0, int(woundMendLimit));
    }
    uintptr_t brokenNodes[woundMendLimit]{};
    uint32_t brokenBits[woundMendLimit]{};
    int brokenCount = 0;
    if (const WoundMarkNodes marks = woundMarkExport()) {
        __try { brokenCount = marks("injuries.broken", brokenNodes, brokenBits, int(woundMendLimit)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { brokenCount = 0; woundFaults.fetch_add(1, std::memory_order_relaxed); }
        brokenCount = std::clamp(brokenCount, 0, int(woundMendLimit));
    }
    if (const WoundMarkNodes marks = woundMarkExport()) {
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
            if (readable(w.part, woundPartCharacter + 8)) {
                owner = *(const uintptr_t*)(w.part + woundPartCharacter);
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
            alive = (*(const unsigned*)(owner + field::character::lifeFlags) & 0x100) == 0;
            if (mendCount || brokenCount) ownerNode = *(const uintptr_t*)(owner + field::character::sceneNode);
            (void)*(const volatile float*)(owner + woundCharacterBleed);
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
            const int bit = woundBoneBit(w.partIndex);
            for (int k = 0; ownerNode && bit >= 0 && k < brokenCount; ++k)
                if (brokenNodes[k] == ownerNode) fracture = ((brokenBits[k] >> bit) & 1) != 0;
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
        if (w.kind != woundCrush && fadeDt > 0.f) {
            // Blood runs while the game says this character bleeds (and for the first seconds after any
            // cut), then dries.
            float bleed = 0.f;
            __try { bleed = *(const float*)(owner + woundCharacterBleed); }
            __except (EXCEPTION_EXECUTE_HANDLER) { bleed = 0.f; }
            if (!std::isfinite(bleed)) bleed = 0.f;
            const float age = float(now - w.born) / 1000.f;
            const float flow = w.scar > 0.3f ? 0.f // a closing cut has stopped bleeding
                               : (std::max)(std::clamp(bleed / woundBleedFull, 0.f, 1.f), age < woundFreshSeconds ? 0.6f : 0.f);
            if (flow > 0.02f) w.run = (std::min)(w.run + fadeDt * (0.4f + 1.6f * flow), 4.f + 14.f * w.severity);
            if (flow > 0.15f) w.drip += fadeDt * flow * 2.f; // up to two drops a second while it pours
            w.wet = flow >= w.wet ? flow : (std::max)(w.wet - fadeDt / woundDrySeconds, flow);
        }
        float world[3], path[3];
        outOfPose(pose, w.local, true, world);
        outOfPose(pose, w.along, false, path);
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
    }
    out.count = count;
    // The drops, nearest first, as their point in view space (the nearer reading of the sector origin, as
    // for the wounds); gone after two minutes, dry after half of one.
    struct ShownDrip { float distanceSq, centre[3], w; };
    ShownDrip drops[woundDripLimit];
    unsigned dropCount = 0;
    for (WoundDrip& d : woundDrips) {
        if (!d.born) continue;
        const float age = float(now - d.born) / 1000.f;
        if (age > woundDripSeconds) { d = {}; continue; }
        ShownDrip s{woundReachCm * woundReachCm, {}, 0.f};
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
        s.w = d.radius + 10.f * std::floor(dry * 9.f);
        drops[dropCount++] = s;
    }
    const unsigned dripCount = (std::min)(dropCount, woundDripShown);
    std::partial_sort(drops, drops + dripCount, drops + dropCount,
                      [](const ShownDrip& l, const ShownDrip& r) { return l.distanceSq < r.distanceSq; });
    for (unsigned k = 0; k < dripCount; ++k) {
        const float v[4] = {drops[k].centre[0], drops[k].centre[1], drops[k].centre[2], drops[k].w};
        memcpy(out.d[k], v, sizeof v);
    }
    out.drips = dripCount;
    woundSeveredCaps(view, out);
    ReleaseSRWLockExclusive(&woundLock);
}
