// Fire and smoke: volumetric flames with a smoke plume above them, drawn by the view effects' fire pass
// (view_effects.inl) on the finished world image, depth-tested against it, so they sit in the scene and
// fade where they meet a wall or the floor. Three sources, all read only:
// - fires: ScriptRuntime.dll's fire_nodes export (version 1; requested by the Fire mod): every fire
//   body in the player's sector (braziers, hearths, prop fires the Fire mod lit, flames it attached),
//   as its scene node and size;
// - burning people: characters marked "fire.burning" (character_mark_nodes, marks version 1), a flame
//   column up their body;
// - a flaming weapon: the player marked "fire.blade", flames along the right-hand weapon's blade
//   (blade.inl's bladeLine).
// No native memory is written and no game call is made; a missing export leaves its source unseen.
#include "../xr/physical_contact.h"

static std::atomic<float> fireLook{1.f};     // strength, 0 off; on by default, no row (auto quality scales it)
static std::atomic<float> fireQuality{1.f};  // 1 down to 0 as the frame slows (fireQualityNow)
static std::atomic<unsigned> fireShown{0}, fireFaults{0};

constexpr unsigned fireShownLimit = 12, fireNodeLimit = 64;
constexpr float fireReachCm = 4000.f;
// What the fire pass takes: view-space volumes, nearest first.
struct FireUniforms {
    unsigned count = 0;
    float a[fireShownLimit][4]; // base centre; w: radius at the base, cm
    float b[fireShownLimit][4]; // base to the flame's top (cm); w: kind (0 fire, 1 a burning body, 2 a blade)
    float c[fireShownLimit][4]; // intensity 0..1, seed, smoke 0..1, unused
    float right[3]{};           // world +x in view space: a frame fixed to the world for the noise
    float steps = 0.f;          // march steps per volume (auto quality)
    float smoke = 0.f;          // 1: smoke plumes drawn
    bool any() const { return count > 0; }
};

using FireNodes = int (*)(uintptr_t* nodes, float* sizes, int capacity);
using FireNodesVersion = uint32_t (*)();
static std::atomic<FireNodes> fireNodes{nullptr};
static std::atomic<ULONGLONG> fireNodesProbed{0};

// Looked up again at most every two seconds until found; the module is pinned once found.
static FireNodes fireNodeExport() {
    if (auto fn = fireNodes.load(std::memory_order_acquire)) return fn;
    const ULONGLONG now = GetTickCount64();
    if (now - fireNodesProbed.load(std::memory_order_relaxed) < 2000) return nullptr;
    fireNodesProbed.store(now, std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) return nullptr;
    const auto version = reinterpret_cast<FireNodesVersion>(GetProcAddress(runtime, "fire_nodes_version"));
    const auto fn = reinterpret_cast<FireNodes>(GetProcAddress(runtime, "fire_nodes"));
    if (!version || !fn || version() != 1) return nullptr;
    fireNodes.store(fn, std::memory_order_release);
    return fn;
}

static bool bladeLine(uintptr_t weapon, XrVector3f& hilt, XrVector3f& tip, XrVector3f& axis, float& length); // blade.inl
static bool sectorOriginOf(uintptr_t node, float* origin);                                                    // view_effects.inl

// Steps and smoke for this frame: like level detail, the scale falls from 1 to 0 as the eased frame
// period grows from 18 to 25 ms and climbs back 2% a frame. At 0 the flames still draw, coarser and
// without smoke; they never vanish on a slow frame.
static void fireQualityNow(float period, FireUniforms& out) {
    const float target = period > 0.f ? 1.f - std::clamp((period - 18.f) / 7.f, 0.f, 1.f) : 1.f;
    float scale = fireQuality.load(std::memory_order_relaxed);
    scale = target < scale ? target : (std::min)(target, scale + 0.02f);
    fireQuality.store(scale, std::memory_order_relaxed);
    out.steps = std::round(6.f + 12.f * scale);
    out.smoke = scale > 0.25f ? 1.f : 0.f;
}

namespace firepass {
struct Candidate { float base[3], axis[3], radius, intensity, seed, smoke, kind, distanceSq; };

// World point to view space (row-major, row vector times matrix), and a direction (rotation only).
inline void point(const float* m, const float* p, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = p[0] * m[i] + p[1] * m[4 + i] + p[2] * m[8 + i] + m[12 + i];
}
inline void direction(const float* m, const float* d, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = d[0] * m[i] + d[1] * m[4 + i] + d[2] * m[8 + i];
}
inline float seedOf(uintptr_t key) {
    uint64_t h = uint64_t(key) * 0x9E3779B97F4A7C15ull;
    return float((h >> 40) % 1000) * 0.137f;
}

// Whether the view matrix takes sector-local or global points is not established (view_effects.inl,
// held items), so both readings of a sector-local point are tried and the one nearer the camera, in
// front of it, wins.
inline bool place(const float* view, const float* local, const float* sectorOrigin, float* out, float& distanceSq) {
    bool found = false;
    distanceSq = fireReachCm * fireReachCm;
    for (int candidate = 0; candidate < 2; ++candidate) {
        float p[3], c[3];
        for (int k = 0; k < 3; ++k) p[k] = local[k] + (candidate ? sectorOrigin[k] : 0.f);
        point(view, p, c);
        const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        if (std::isfinite(sq) && sq < distanceSq && c[2] < 300.f) { // not well behind the camera (it looks down -z)
            distanceSq = sq;
            memcpy(out, c, sizeof c);
            found = true;
        }
        if (!(sectorOrigin[0] || sectorOrigin[1] || sectorOrigin[2])) break;
    }
    return found;
}

inline bool nodeOrigin(uintptr_t node, float* local, float* sectorOrigin) {
    if (!node || !readable(node, field::sceneNode::renderTransform + 64)) return false;
    const float* sampled = (const float*)(node + field::sceneNode::renderTransform);
    for (int i = 0; i < 3; ++i) local[i] = sampled[12 + i];
    if (!sectorOriginOf(node, sectorOrigin)) memset(sectorOrigin, 0, 12);
    return std::isfinite(local[0] + local[1] + local[2]);
}
} // namespace firepass

// Collects this frame's fire volumes in the space of `view` (the matrices the view was drawn with).
static void fireViewUniforms(const float* view, float period, FireUniforms& out) {
    out.count = 0;
    if (!(fireLook.load(std::memory_order_relaxed) > 0.f)) return;
    using firepass::Candidate;
    Candidate found[fireNodeLimit + 24];
    unsigned n = 0;
    const float worldUp[3] = {0.f, 1.f, 0.f}, worldRight[3] = {1.f, 0.f, 0.f};
    float up[3];
    firepass::direction(view, worldUp, up);
    firepass::direction(view, worldRight, out.right);
    __try {
        const WoundMarkNodes marks = woundMarkExport();
        // The blade's flames are found first: a fire node the Fire mod attached to the blade (attach_fire) is
        // the same flame, and is left out of the fires below.
        Candidate bladeFire{};
        bool hasBlade = false;
        float bladeFrom[3]{}, bladeTo[3]{};
        // A flaming weapon: the player marked "fire.blade"; flames along the primary weapon's blade.
        if (marks) {
            uintptr_t list[4]{};
            uint32_t values[4]{};
            const int count = std::clamp(marks("fire.blade", list, values, 4), 0, 4);
            const uintptr_t player = count > 0 && values[0] ? *(const uintptr_t*)gameAt(fp::sig::playerRoot) : 0;
            const uintptr_t weapon = player && readable(player, field::character::weapons + 8) &&
                                             *(const int*)(player + field::character::weaponCount) > 0
                                         ? *(const uintptr_t*)(player + field::character::weapons) : 0;
            XrVector3f hilt{}, tip{}, axis{};
            float length = 0.f;
            if (weapon && readable(weapon, field::bodyPart::meshNode + 8) && bladeLine(weapon, hilt, tip, axis, length)) {
                const uintptr_t mesh = *(const uintptr_t*)(weapon + field::bodyPart::meshNode);
                float origin[3]{};
                if (!mesh || !sectorOriginOf(mesh, origin)) memset(origin, 0, sizeof origin);
                const float from[3] = {hilt.x, hilt.y, hilt.z};
                Candidate c{};
                if (firepass::place(view, from, origin, c.base, c.distanceSq)) {
                    const float to[3] = {tip.x, tip.y, tip.z};
                    for (int k = 0; k < 3; ++k) {
                        bladeFrom[k] = from[k] + origin[k];
                        bladeTo[k] = to[k] + origin[k];
                    }
                    const float along[3] = {tip.x - hilt.x, tip.y - hilt.y, tip.z - hilt.z};
                    firepass::direction(view, along, c.axis);
                    c.radius = 6.f;
                    c.intensity = 0.9f;
                    c.seed = firepass::seedOf(weapon);
                    c.smoke = 0.25f;
                    c.kind = 2.f;
                    bladeFire = c;
                    hasBlade = true;
                }
            }
        }
        // Fires: the node's origin is where the flames stand; the native size scales them (1 is the
        // game's medium fire).
        if (const FireNodes fires = fireNodeExport()) {
            uintptr_t list[fireNodeLimit]{};
            float sizes[fireNodeLimit]{};
            const int count = std::clamp(fires(list, sizes, int(fireNodeLimit)), 0, int(fireNodeLimit));
            for (int i = 0; i < count && n < fireNodeLimit; ++i) {
                float local[3], origin[3];
                Candidate c{};
                if (!firepass::nodeOrigin(list[i], local, origin)) continue;
                if (hasBlade) { // within 15 cm of the blade line: the blade's own flame
                    float at[3], along[3], away[3];
                    for (int k = 0; k < 3; ++k) {
                        at[k] = local[k] + origin[k] - bladeFrom[k];
                        along[k] = bladeTo[k] - bladeFrom[k];
                    }
                    const float span = along[0] * along[0] + along[1] * along[1] + along[2] * along[2];
                    const float t = span > 1e-3f ? std::clamp((at[0] * along[0] + at[1] * along[1] + at[2] * along[2]) / span, 0.f, 1.f) : 0.f;
                    for (int k = 0; k < 3; ++k) away[k] = at[k] - along[k] * t;
                    if (away[0] * away[0] + away[1] * away[1] + away[2] * away[2] < 15.f * 15.f) continue;
                }
                if (!firepass::place(view, local, origin, c.base, c.distanceSq)) continue;
                const float size = std::clamp(std::isfinite(sizes[i]) ? sizes[i] : 1.f, 0.2f, 4.f);
                c.radius = 20.f * size;
                for (int k = 0; k < 3; ++k) c.axis[k] = up[k] * 70.f * size;
                c.intensity = 1.f;
                c.seed = firepass::seedOf(list[i]);
                c.smoke = 1.f;
                c.kind = 0.f;
                found[n++] = c;
            }
        }
        // Burning people: a column from about the knees to over the head around the body's centre.
        if (marks) {
            uintptr_t list[32]{};
            uint32_t values[32]{};
            const int count = std::clamp(marks("fire.burning", list, values, 32), 0, 32);
            for (int i = 0; i < count && n < fireNodeLimit + 16; ++i) {
                float local[3], origin[3];
                Candidate c{};
                if (!values[i] || !firepass::nodeOrigin(list[i], local, origin)) continue;
                for (int k = 0; k < 3; ++k) local[k] -= worldUp[k] * 70.f;
                if (!firepass::place(view, local, origin, c.base, c.distanceSq)) continue;
                c.radius = 30.f;
                for (int k = 0; k < 3; ++k) c.axis[k] = up[k] * 165.f;
                c.intensity = 0.75f;
                c.seed = firepass::seedOf(list[i]);
                c.smoke = 0.6f;
                c.kind = 1.f;
                found[n++] = c;
            }
        }
        if (hasBlade && n < fireNodeLimit + 24) found[n++] = bladeFire;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fireFaults.fetch_add(1, std::memory_order_relaxed);
    }
    const unsigned count = (std::min)(n, fireShownLimit);
    std::partial_sort(found, found + count, found + n,
                      [](const Candidate& x, const Candidate& y) { return x.distanceSq < y.distanceSq; });
    for (unsigned i = 0; i < count; ++i) {
        const Candidate& c = found[i];
        for (int k = 0; k < 3; ++k) {
            out.a[i][k] = c.base[k];
            out.b[i][k] = c.axis[k];
        }
        out.a[i][3] = c.radius;
        out.b[i][3] = c.kind;
        out.c[i][0] = c.intensity * fireLook.load(std::memory_order_relaxed);
        out.c[i][1] = c.seed;
        out.c[i][2] = c.smoke;
        out.c[i][3] = 0.f;
    }
    out.count = count;
    fireShown.store(count, std::memory_order_relaxed);
    if (count) fireQualityNow(period, out);
}
