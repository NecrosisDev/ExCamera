// Stone skin: characters a script-runtime addon marks (Stone Skin, mark "stoneskin.look") are
// drawn with rock plates. Included after headwear_render.inl, whose submit/draw hooks carry it.
// Only the body faces (tag 0) of a marked character's _SKIN mesh change, and only their
// diffuse/specular/normal units (0..2, from material +0x100: +0x10, +0x18, +0x20), which are
// put back to the material's own textures after the draw. The game's program, lights and
// shadows shade the plates. No native memory is written. The marks come from
// ScriptRuntime.dll's character_mark_nodes export (marks version 1); without it nothing changes.
#include "../stone_skin.h"

static constexpr const char* stoneSkinMark = "stoneskin.look";
static constexpr unsigned stoneSkinMaxMarked = 64;
using StoneMarkNodes = int (*)(const char*, uintptr_t*, uint32_t*, int);
using StoneMarksVersion = uint32_t (*)();
using StoneBindTextures = void (APIENTRY*)(GLuint, GLsizei, const GLuint*);
using StoneCreateTextures = void (APIENTRY*)(GLenum, GLsizei, GLuint*);
using StoneTextureStorage = void (APIENTRY*)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
using StoneTextureSubImage = void (APIENTRY*)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
using StoneTextureParameter = void (APIENTRY*)(GLuint, GLenum, GLint);

static std::atomic<StoneMarkNodes> stoneMarkNodes{nullptr};
static std::atomic<ULONGLONG> stoneMarkProbed{0};
static std::atomic<unsigned> stoneSkinDraws{0}, stoneSkinFallbacks{0}, stoneSkinFaults{0};

struct StoneScope { uintptr_t actors[stoneSkinMaxMarked]{}; unsigned count = 0; };
static thread_local StoneScope stoneScope;

// The runtime's export, looked up again at most every two seconds until found. The module is
// pinned once found, so the cached entry stays valid for the life of the process.
static StoneMarkNodes stoneMarkExport() {
    if (auto fn = stoneMarkNodes.load(std::memory_order_acquire)) return fn;
    const ULONGLONG now = GetTickCount64();
    if (now - stoneMarkProbed.load(std::memory_order_relaxed) < 2000) return nullptr;
    stoneMarkProbed.store(now, std::memory_order_relaxed);
    HMODULE runtime = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"ScriptRuntime.dll", &runtime) || !runtime) return nullptr;
    const auto version = reinterpret_cast<StoneMarksVersion>(GetProcAddress(runtime, "character_marks_version"));
    const auto fn = reinterpret_cast<StoneMarkNodes>(GetProcAddress(runtime, "character_mark_nodes"));
    if (!version || !fn || version() != 1) return nullptr;
    stoneMarkNodes.store(fn, std::memory_order_release);
    return fn;
}

// Once per colour pass: the marked characters' scene nodes (character +0x18).
static void stoneSkinPass() {
    stoneScope.count = 0;
    const StoneMarkNodes fn = stoneMarkExport();
    if (!fn) return;
    uint32_t values[stoneSkinMaxMarked]{};
    int count = 0;
    __try { count = fn(stoneSkinMark, stoneScope.actors, values, int(stoneSkinMaxMarked)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; stoneSkinFaults.fetch_add(1, std::memory_order_relaxed); }
    if (count <= 0) return;
    unsigned kept = 0;
    for (int k = 0; k < count && k < int(stoneSkinMaxMarked); ++k)
        if (values[k] && stoneScope.actors[k]) stoneScope.actors[kept++] = stoneScope.actors[k];
    stoneScope.count = kept;
}

static bool stoneSkinActor(uintptr_t actor) {
    for (unsigned k = 0; k < stoneScope.count; ++k)
        if (stoneScope.actors[k] == actor) return true;
    return false;
}

// The submission gate: a marked character's skinned mesh (its _SKIN child), outside the player's
// own first-person head scope (the headwear path keeps that and swaps the maps itself) unless
// ownBody: the headwear path left that draw alone.
static bool stoneSkinSubmission(uintptr_t node, bool ownBody) {
    if (!stoneScope.count || !readable(node, 0x2a0) || strncmp(nodeName(node), "_SKIN", 5)) return false;
    if (!ownBody && wearScope.colour && std::binary_search(wearScope.nodes, wearScope.nodes + wearScope.nodeCount, node)) return false;
    return stoneSkinActor(*(uintptr_t*)(node + field::sceneNode::parent));
}

struct StoneGl {
    StoneBindTextures bind = nullptr;
    StoneCreateTextures create = nullptr;
    StoneTextureStorage storage = nullptr;
    StoneTextureSubImage upload = nullptr;
    StoneTextureParameter parameter = nullptr;
};
static bool loadStoneGl(StoneGl& s) {
    return loadGl(s.bind, "glBindTextures") && loadGl(s.create, "glCreateTextures") &&
           loadGl(s.storage, "glTextureStorage2D") && loadGl(s.upload, "glTextureSubImage2D") &&
           loadGl(s.parameter, "glTextureParameteri");
}

// The maps (about 4 MB) are made once on a pool thread (about a quarter second), never on the
// render thread; stone characters draw natively until they are ready. Kept for the process.
static std::atomic<int> stoneMapsState{0}; // 0 not started, 1 making, 2 ready, 3 failed
static fp::stone::Maps* stoneMaps = nullptr;
static DWORD WINAPI stoneSkinMake(void*) {
    auto* maps = new(std::nothrow) fp::stone::Maps{};
    bool made = false;
    if (maps) {
        try { fp::stone::generate(*maps); made = true; }
        catch (const std::exception&) {}
    }
    if (made) stoneMaps = maps;
    else delete maps;
    stoneMapsState.store(made ? 2 : 3, std::memory_order_release);
    return 0;
}
static const fp::stone::Maps* stoneSkinMaps() {
    const int state = stoneMapsState.load(std::memory_order_acquire);
    if (state == 2) return stoneMaps;
    int idle = 0;
    if (state == 0 && stoneMapsState.compare_exchange_strong(idle, 1) &&
        !QueueUserWorkItem(stoneSkinMake, nullptr, WT_EXECUTELONGFUNCTION))
        stoneMapsState.store(3, std::memory_order_release);
    return nullptr;
}

// Direct-state uploads, so no unit's binding or the active unit changes; cueLock held.
static bool stoneSkinTextures(fp::CueResources& r, const StoneGl& s) {
    if (r.stoneTex[0] && r.stoneTex[1] && r.stoneTex[2]) return true;
    if (r.stoneTex[0] || r.stoneTex[1] || r.stoneTex[2]) return false; // a half-made set waits for retirement
    const fp::stone::Maps* maps = stoneSkinMaps();
    if (!maps) return false;
    GLint major = 0, minor = 0; // once per context: direct-state textures and multi-bind need 4.5
    glGetIntegerv(0x821B, &major);
    glGetIntegerv(0x821C, &minor);
    if (major < 4 || (major == 4 && minor < 5)) return false;
    s.create(0x0DE1, 3, r.stoneTex);
    const std::vector<uint8_t>* data[3] = {&maps->diffuse, &maps->specular, &maps->normal};
    for (unsigned t = 0; t < 3; ++t) {
        if (!r.stoneTex[t]) return false;
        s.storage(r.stoneTex[t], GLsizei(fp::stone::levels), 0x8058 /* RGBA8 */, GLsizei(fp::stone::size), GLsizei(fp::stone::size));
        for (unsigned l = 0, w = fp::stone::size; l < fp::stone::levels; ++l, w = w > 1 ? w / 2 : 1)
            s.upload(r.stoneTex[t], GLint(l), 0, 0, GLsizei(w), GLsizei(w), 0x1908 /* RGBA */, GL_UNSIGNED_BYTE,
                     data[t]->data() + maps->offset[l]);
        s.parameter(r.stoneTex[t], 0x2801 /* MIN_FILTER */, 0x2703 /* LINEAR_MIPMAP_LINEAR */);
        s.parameter(r.stoneTex[t], 0x2800 /* MAG_FILTER */, 0x2601 /* LINEAR */);
        s.parameter(r.stoneTex[t], 0x2802 /* WRAP_S */, 0x2901 /* REPEAT */);
        s.parameter(r.stoneTex[t], 0x2803 /* WRAP_T */, 0x2901);
    }
    return true;
}

// The material's own diffuse/specular/normal, as the submit bound them on units 0..2.
static bool stoneSkinOriginals(uintptr_t material, GLuint* names) {
    if (!readable(material, 0x108)) return false;
    const uintptr_t set = *(uintptr_t*)(material + 0x100);
    if (!readable(set, 0x28)) return false;
    for (unsigned t = 0; t < 3; ++t) {
        const uintptr_t texture = *(uintptr_t*)(set + 0x10 + t * 8);
        if (!readable(texture, 0x5c) || *(GLenum*)(texture + 0x40) != 0x0DE1) return false;
        names[t] = *(GLuint*)(texture + 0x58);
        if (!names[t]) return false;
    }
    return true;
}

// Draws the plan with `replacement` (from `prepare`, cueLock held) on units 0..units-1 for the faces
// marked in `faces` and the material's own textures elsewhere. Stone skin and wound skins
// (wounds_bake.inl) share it.
using SkinReplacement = const GLuint* (*)(fp::CueResources&, const StoneGl&, const void*);
static bool skinFacesEmit(const WearPlan& plan, bool bind, const unsigned char* stone, const GLuint* originals, GLsizei units,
                          SkinReplacement prepare, const void* context) { // SEH-only frame
    if (!ownedNativeSwap(cueCurrentDC())) return false;
    bool emitted = false;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (cueDeletesInFlight || cueRetiring.load() || cueBindingFailed.load() ||
            InterlockedCompareExchange(&stopping, 0, 0) || !ownedNativeSwap(cueCurrentDC())) return false;
        StoneGl s{};
        WearBindBuffer bindBuffer = nullptr;
        if (!loadStoneGl(s) || (bind && !loadGl(bindBuffer, "glBindBuffer"))) return false;
        auto* owner = prepareCue();
        const GLuint* replacement = owner ? prepare(owner->resources, s, context) : nullptr;
        if (!replacement) return false;
        if (bind) {
            wearBind(plan.material + 8);
            bindBuffer(0x8893, plan.buffer);
        }
        bool swapped = false;
        __try {
            for (unsigned f = 0; f < plan.count;) {
                unsigned end = f + 1;
                while (end < plan.count && stone[end] == stone[f]) ++end;
                if (stone[f] == 2) { // left out: a severed limb's torn band (wounds_bake.inl)
                    f = end;
                    continue;
                }
                if (bool(stone[f]) != swapped) {
                    s.bind(0, units, stone[f] ? replacement : originals);
                    swapped = stone[f] != 0;
                }
                wearDrawRange(plan, f, end - f);
                f = end;
            }
            emitted = true;
        } __finally {
            if (swapped) s.bind(0, units, originals);
        }
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return emitted;
}

static const GLuint* stoneSkinReplacement(fp::CueResources& r, const StoneGl& s, const void*) {
    return stoneSkinTextures(r, s) ? r.stoneTex : nullptr;
}

static bool stoneSkinDrawBody(uintptr_t node, uintptr_t material, bool bind, bool ownBody) {
    if (!stoneSkinSubmission(node, ownBody)) return false;
    GLuint originals[3]{};
    if (!stoneSkinOriginals(material, originals)) return false;
    const int lastFace = *(int*)(node + 0x268), lastVertex = *(int*)(node + 0x288);
    if (lastFace < 0 || lastFace >= int(fp::wear::maxFaces) || lastVertex < 0 || lastVertex >= int(fp::wear::maxVertices)) return false;
    WearPlan plan{};
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan)) return false;
    const uintptr_t tags = *(uintptr_t*)(node + 0x2f8);
    if (!readable(tags, (size_t(plan.first) + plan.count) * 4)) return false;
    static thread_local std::vector<unsigned char> stone;
    stone.assign(plan.count, 0);
    unsigned body = 0;
    for (unsigned f = 0; f < plan.count; ++f) {
        stone[f] = *(unsigned*)(tags + (size_t(plan.first) + f) * 4) == 0 ? 1 : 0; // tag 0: the body itself, not a worn piece
        body += stone[f];
    }
    if (!body) return false;
    return skinFacesEmit(plan, bind, stone.data(), originals, 3, stoneSkinReplacement, nullptr);
}

static bool stoneSkinDrawGuarded(uintptr_t node, uintptr_t material, bool bind, bool ownBody) {
    try { return stoneSkinDrawBody(node, material, bind, ownBody); }
    catch (const std::exception&) { stoneSkinFaults.fetch_add(1, std::memory_order_relaxed); return false; }
}
// true: the draw was made here (stone where the body shows, the rest native) and the native one is skipped.
static bool stoneSkinDraw(uintptr_t node, uintptr_t material, bool bind, bool ownBody) {
    if (!stoneScope.count || !wearScope.colourPass) return false;
    bool drawn = false;
    __try {
        drawn = stoneSkinDrawGuarded(node, material, bind, ownBody);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stoneSkinFaults.fetch_add(1, std::memory_order_relaxed);
        drawn = false;
    }
    if (drawn) stoneSkinDraws.fetch_add(1, std::memory_order_relaxed);
    else if (stoneSkinSubmission(node, ownBody)) stoneSkinFallbacks.fetch_add(1, std::memory_order_relaxed);
    return drawn;
}

// The headwear path's own draws of the player's skinned mesh: its native body runs take the
// stone maps while the player is stone. cueLock is held by the caller; nothing here throws.
static bool woundSkinWearBegin(uintptr_t node, uintptr_t material, StoneSwap& swap); // wounds_bake.inl
static bool stoneSkinWearBegin(uintptr_t node, uintptr_t material, StoneSwap& swap) {
    swap = {};
    bool stone = false;
    __try {
        stone = stoneScope.count && wearScope.colourPass && readable(node, 0x2a0) && !strncmp(nodeName(node), "_SKIN", 5) &&
                stoneSkinActor(*(uintptr_t*)(node + field::sceneNode::parent));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stone = false;
    }
    if (!stone) return woundSkinWearBegin(node, material, swap); // a wounded (not stone) player: the baked skin
    __try {
        if (!stoneSkinOriginals(material, swap.originals)) return false;
        StoneGl s{};
        if (!loadStoneGl(s)) return false;
        auto* owner = prepareCue();
        if (!owner || !stoneSkinTextures(owner->resources, s)) return false;
        for (unsigned t = 0; t < 3; ++t) swap.stone[t] = owner->resources.stoneTex[t];
        swap.bind = s.bind;
        swap.on = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stoneSkinFaults.fetch_add(1, std::memory_order_relaxed);
        swap = {};
    }
    if (swap.on) stoneSkinDraws.fetch_add(1, std::memory_order_relaxed);
    return swap.on;
}
static void stoneSkinWearRun(StoneSwap& swap, bool stone) {
    if (!swap.on || swap.swapped == stone) return;
    swap.bind(0, 3, stone ? swap.stone : swap.originals);
    swap.swapped = stone;
}
static void stoneSkinWearEnd(StoneSwap& swap) {
    if (swap.on && swap.swapped) swap.bind(0, 3, swap.originals);
    swap.swapped = false;
}
