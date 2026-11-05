// OFF by default (unprovenRenderReads). Proven and witnessed (headwearRender group): node +218 is a
// TSkinController2 (+8 node, +10 bone count, +18 bone nodes, +20 48-byte inverse binds; skinController::boneLoop),
// +290 the faces (three dword vertex indices; meshNode::faces), +2F8 one dword per face (meshNode::faceValues).
// UVs PROVEN: the witnessed mesh build headwearNative_5e340 copies the attribute record (node +280, 0x18 bytes,
// format at +4, stream at +8 into node +270) whose format & 0xFFFF == 0x10 into the vertex layout's 0x10 slot
// (0x5E4E0-0x5E56E), and the program binder 0x1FA181 binds layout bit 0x10 to "vTex" at location 1 (names table
// .data 0x2E9F30: vPos 0, vTex 1, vNrm 2, vCol 3, vTx2 4, vTng 6, vDlt 7, at 0x1FA164-0x1FA232); the game's
// shading_v.glsl reads vTex as its texcoord. Texture units: the shared stoneSkinOriginals check (map flags at
// renderer +8, binder 0x2010C0) is what the bake's draw uses. Still not shown by game code: what selects body
// skin among a face's +2F8 bits (a packed word; 0x577CA tests 0x1F0000; the 0xFF/0x70000/0x180000/0xE00000 repack
// at 0x1C7BF8 belongs to a grid object, +2E8 its dimensions, not a mesh node; no game code found buckets mesh faces
// by any of these fields, the faces of a material being its own vertex range; all unproven flags, and the bake
// takes the whole word == 0), and,
// formerly, which texture unit the skin program samples for each map at draw time (the binder 0x20112B-0x2012AB hands
// out units in the order DifTex, OpcTex, EmiTex, SpcTex, NrmTex, GlsTex, DspTex, EnvMap to the maps present;
// names at .rdata 0x335218/0x335238/0x335258, table .data 0x2E9F70), and the Injuries addon's severed bits.
// The dev build's skin witness (woundSkinDiagnose, render-audit log) records the three open facts on a real
// draw. Severed limbs: the game has no severing of its own; which parts a cut removes comes only from the
// Injuries addon's "injuries.severed" bits, so they cannot be proven from game code and stay off.
// Wound skins: cuts and bruises baked into a copy of the struck character's own body texture, the way
// the game bakes an undead character's decay into its skin (textures.flb chardecomp_*, set up by
// 0x208140). Baked marks sit in the skin, follow every fold and are covered by clothes and armour.
// Included after stone_skin.inl, whose face-split draw (skinFacesEmit) it shares. Stage 1: the
// diffuse map only; wet shine, blood runs and anything on cloth or armour stay with the screen pass
// (view_effects.inl woundMarks), which leaves out the skin part of a baked mark. Stage 2: the gash's depth,
// lips and a bruise's swelling go into a copy of the normal map too (unit 2), and the player's own
// body in first person takes both copies through the headwear path's swap (stone_skin.inl). Stage 3: wet
// gloss goes into a copy of the spec map (unit 1: the skin shading reads R as specular strength, G as
// gloss, B as metal, shading.flb shadesss_f), fresh blood glossy and dried blood dull; a fracture (R.w 1)
// draws as a wider, darker bruise and swells more. Stage 4: burns, from Injuries' "injuries.burn" mark
// (a level per character), over the whole skin: reddened first, then blistered, then charred black in
// patches; a burnt character without cuts gets a slot too. Stage 5: scars. A healing cut (kind + 4, its
// fraction the scar's progress) closes into a pale, thin, slightly raised line with a faint pink halo and
// a little sheen; healed burns (Injuries' "injuries.burnscar") leave mottled, shiny, slightly lumpy tissue.
//
// 1. Mapping, once per wound: the hit, kept in its body part's frame, is carried into the frame of
//    the bone of the character's _SKIN mesh that is that part's scene node; the nearest body face
//    (tag 0) among the vertices that bone moves gives the spot and its UV, and the face's
//    position/UV gradient gives the blade's path and the cm-per-UV scale there.
// 2. Bake, in the view-effects pass when a character's marks change or age: the original diffuse
//    is copied into the character's slot texture, each mark's area is drawn in UV space multiplying
//    the copy, and the copy is mipmapped.
// 3. Draw: that character's body faces bind the slot texture on unit 0 instead of the original.
//
// Static facts this rests on (from headwear_render.inl and stone_skin.inl): _SKIN node +0x218 skin
// (+0x10 bone count, +0x18 bone transform pointers, +0x20 inverse binds, 48 bytes each, +0x38 bind
// positions); attributes 1 positions (0xF30001), 0x1000 weights (0xA41000), 0x10 UVs (0xF20010, the
// format exanimate/rfc.py reads); faces +0x290, tags +0x2F8 (0: the body). Whether the skinning uses
// the bone node's +0x40 matrix is inferred from the pointers the skin keeps.

#include "wound_glsl.h"

constexpr unsigned woundSkinLimit = fp::CueResources::woundSkinLimit, woundSkinMarks = 16;
constexpr float woundMapReachCm = 12.f;

struct WoundSkinMap { // one of the body material's maps and its copy's shape
    GLuint source = 0;
    GLint width = 0, height = 0, levels = 0;
    GLenum format = 0;
    bool failed = false;
};
struct WoundSkinSlot {
    uintptr_t character = 0, actor = 0; // the character and its scene node (character +0x18)
    uintptr_t material = 0;             // the body material whose maps were recorded at its draw
    WoundSkinMap diffuse, normal, spec; // units 0, 2 and 1
    unsigned bakedVersion = 0;
    ULONGLONG bakedAt = 0;
    bool ready = false, normalReady = false, specReady = false;
};
static WoundSkinSlot woundSkinSlots[woundSkinLimit]; // render thread only
static std::atomic<unsigned> woundSkinBakes{0}, woundSkinMapped{0}, woundSkinUnmapped{0}, woundSkinDraws{0}, woundSkinFaults{0};
static bool woundSkinBroken = false; // the bake program or a needed entry point is missing: screen marks only

static bool woundSkinBaked(uintptr_t character) { // declared in wounds.inl
    for (const WoundSkinSlot& s : woundSkinSlots)
        if (s.character == character && s.ready) return true;
    return false;
}

// ---- 1. mapping -------------------------------------------------------------------------------

struct WoundSkinMesh {
    uintptr_t node = 0, boneNodes = 0, inverseBind = 0, source = 0, posIndices = 0, uvIndices = 0, triangles = 0, tags = 0;
    int bones = 0, lastFace = 0, lastVertex = 0;
    WearAttribute positions{}, weights{}, uvs{};
};

// The mesh, skin and attribute offsets below are the ones headwear_render.inl reads; they are proved only
// by its admitted whole-function contracts, so without them there is no layout to map or draw with.
static bool woundSkinMesh(uintptr_t node, WoundSkinMesh& m) {
    if (!headwearRenderReady.load(std::memory_order_relaxed) || !readable(node, 0x300)) return false;
    m.node = node;
    m.lastFace = *(int*)(node + 0x268);
    m.lastVertex = *(int*)(node + 0x288);
    if (m.lastFace < 0 || m.lastFace >= int(fp::wear::maxFaces) || m.lastVertex < 0 || m.lastVertex >= int(fp::wear::maxVertices))
        return false;
    const uintptr_t skin = *(uintptr_t*)(node + 0x218);
    if (!readable(skin, 0x40) || *(uintptr_t*)(skin + 8) != node) return false;
    m.bones = *(int*)(skin + 0x10);
    m.boneNodes = *(uintptr_t*)(skin + 0x18);
    m.inverseBind = *(uintptr_t*)(skin + 0x20);
    m.source = *(uintptr_t*)(skin + 0x38);
    if (m.bones < 1 || m.bones > int(fp::wear::maxBones) || !readable(m.boneNodes, size_t(m.bones) * 8) ||
        !readable(m.inverseBind, size_t(m.bones) * 48))
        return false;
    const unsigned vertices = unsigned(m.lastVertex + 1);
    if (!wearAttribute(node, 1, m.positions) || (m.positions.format & 0xffffff) != 0xf30001 || !m.positions.count ||
        m.positions.count > fp::wear::maxVertices || !wearStream(node, m.positions, vertices, m.posIndices) ||
        !readable(m.source, size_t(m.positions.count) * 12))
        return false;
    if (!wearAttribute(node, 0x1000, m.weights) || (m.weights.format & 0xffffff) != 0xa41000 ||
        m.weights.count != m.positions.count || !readable(m.weights.data, size_t(m.weights.count) * sizeof(fp::wear::Weights)))
        return false;
    if (!wearAttribute(node, 0x10, m.uvs) || (m.uvs.format & 0xffffff) != 0xf20010 || !m.uvs.count ||
        !readable(m.uvs.data, size_t(m.uvs.count) * 8) || !wearStream(node, m.uvs, vertices, m.uvIndices))
        return false;
    m.triangles = *(uintptr_t*)(node + 0x290);
    return readable(m.triangles, size_t(m.lastFace + 1) * 12);
}

// The closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5), as barycentric weights.
static void woundClosest(const float* p, const float* a, const float* b, const float* c, float* w) {
    float ab[3], ac[3], ap[3];
    for (int i = 0; i < 3; ++i) { ab[i] = b[i] - a[i]; ac[i] = c[i] - a[i]; ap[i] = p[i] - a[i]; }
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) { w[0] = 1.f; w[1] = w[2] = 0.f; return; }
    float bp[3], cp[3];
    for (int i = 0; i < 3; ++i) { bp[i] = p[i] - b[i]; cp[i] = p[i] - c[i]; }
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) { w[1] = 1.f; w[0] = w[2] = 0.f; return; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) { const float v = d1 / (d1 - d3); w[0] = 1.f - v; w[1] = v; w[2] = 0.f; return; }
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) { w[2] = 1.f; w[0] = w[1] = 0.f; return; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) { const float t = d2 / (d2 - d6); w[0] = 1.f - t; w[2] = t; w[1] = 0.f; return; }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        w[1] = 1.f - t; w[2] = t; w[0] = 0.f; return;
    }
    const float denominator = 1.f / (va + vb + vc);
    w[1] = vb * denominator; w[2] = vc * denominator; w[0] = 1.f - w[1] - w[2];
}

// One wound onto its character's skin. woundLock held, render thread. Leaves w.mapped false when the
// struck part is no bone of a _SKIN mesh or no body face lies within reach (a hit on gear, say).
// The bake's proof gate: the skin controller and mesh layout witnesses (headwearRender group: boneLoop, faces),
// the observed skin assignment (skinShare hooked), the wounds group (UVs: the 0x10 attribute, as headwearNative_5e340
// and the binder 0x1FA181 show). The texture units (stoneSkinOriginals) and the draw range (wearMeshFormat, the
// game's own descriptor as headwearDrawBound 0x6B7F0 draws it) are checked at each draw.
static bool woundSkinProven() {
    return headwearRenderReady.load(std::memory_order_relaxed) && skinShareHooked.load(std::memory_order_acquire) &&
           layout.optionalGroupAvailable("wounds");
}

static void woundSkinMap(Wound& w) {
    if (!woundSkinProven()) return; // the marks stay with the screen pass
    w.mapTried = true;
    const uintptr_t actor = *(const uintptr_t*)(w.owner + field::character::sceneNode);
    const uintptr_t partNode = *(const uintptr_t*)(w.part + field::bodyPart::meshNode);
    if (!readable(actor, 0x2a0) || !partNode) return;
    float pose[12];
    memcpy(pose, (const void*)(w.part + field::bodyPart::pose), sizeof pose);
    float world[3], path[3];
    outOfPose(pose, w.local, true, world);
    outOfPose(pose, w.along, false, path);
    const int last = *(int*)(actor + field::sceneNode::lastChild);
    const uintptr_t list = *(uintptr_t*)(actor + field::sceneNode::children);
    if (last < 0 || last >= 6000 || !readable(list, (size_t(last) + 1) * 8)) return;
    for (int child = 0; child <= last; ++child) {
        const uintptr_t node = *(uintptr_t*)(list + size_t(child) * 8);
        if (!readable(node, 0x2a0) || strncmp((const char*)(node + field::sceneNode::name), "_SKIN", 5) ||
            *(uintptr_t*)(node + field::sceneNode::parent) != actor)
            continue;
        WoundSkinMesh m;
        if (!woundSkinMesh(node, m)) continue;
        int bone = -1;
        for (int k = 0; k < m.bones && bone < 0; ++k)
            if (*(uintptr_t*)(m.boneNodes + size_t(k) * 8) == partNode + field::sceneNode::transform) bone = k;
        if (bone < 0) continue;
        // The hit and the blade's path in that bone's frame, and the bone's bind into the same frame.
        float boneNow[16], toBone[16], bind[16];
        if (!fp::wear::affine((const float*)(partNode + field::sceneNode::transform), boneNow) ||
            !fp::wear::inverse(boneNow, toBone) ||
            !fp::wear::affine((const float*)(m.inverseBind + size_t(bone) * 48), bind))
            continue;
        float hit[3], dir[3];
        if (!fp::wear::point(toBone, world, hit) || !fp::wear::point(toBone, path, dir, true)) continue;
        const auto* weights = (const fp::wear::Weights*)m.weights.data;
        // Only faces of the node's skin materials (the game's own assignment, woundSkinMaterial), each over the
        // face range the game draws it with (material +B0..+B4).
        static thread_local std::vector<unsigned char> skinFace;
        skinFace.assign(size_t(m.lastFace) + 1, 0);
        const int lastMaterial = *(const int*)(node + 0x1c8);
        const uintptr_t materials = *(const uintptr_t*)(node + 0x1d0);
        if (lastMaterial < 0 || lastMaterial >= 256 || !readable(materials, size_t(lastMaterial + 1) * 8)) continue;
        for (int i = 0; i <= lastMaterial; ++i) {
            const uintptr_t material = *(const uintptr_t*)(materials + size_t(i) * 8);
            if (!woundSkinMaterial(material) || !readable(material, 0xb8)) continue;
            const int first = *(const int*)(material + 0xb0), lastOne = *(const int*)(material + 0xb4);
            for (int f = (std::max)(first, 0); f <= lastOne && f <= m.lastFace; ++f) skinFace[size_t(f)] = 1;
        }
        float best = woundMapReachCm * woundMapReachCm, bestW[3]{}, bestP[3][3]{};
        unsigned bestTri[3]{};
        for (int f = 0; f <= m.lastFace; ++f) {
            if (!skinFace[size_t(f)]) continue; // skin, as the game assigned it; worn pieces keep their own sets
            const unsigned* tri = (const unsigned*)(m.triangles + size_t(f) * 12);
            float corner[3][3];
            bool moved = false, valid = true;
            for (int c = 0; c < 3 && valid; ++c) {
                if (tri[c] > unsigned(m.lastVertex)) { valid = false; break; }
                const unsigned p = *(const unsigned*)(m.posIndices + size_t(tri[c]) * 4);
                if (p >= m.positions.count) { valid = false; break; }
                for (int k = 0; k < 4; ++k)
                    if (weights[p].bone[k] == bone + 1 && weights[p].weight[k] >= 0.25f) moved = true;
                valid = fp::wear::point(bind, (const float*)(m.source + size_t(p) * 12), corner[c]);
            }
            if (!valid || !moved) continue;
            float bary[3];
            woundClosest(hit, corner[0], corner[1], corner[2], bary);
            float q[3], distanceSq = 0.f;
            for (int i = 0; i < 3; ++i) {
                q[i] = bary[0] * corner[0][i] + bary[1] * corner[1][i] + bary[2] * corner[2][i];
                distanceSq += (q[i] - hit[i]) * (q[i] - hit[i]);
            }
            if (distanceSq < best) {
                best = distanceSq;
                memcpy(bestW, bary, sizeof bestW);
                memcpy(bestP, corner, sizeof bestP);
                memcpy(bestTri, tri, sizeof bestTri);
            }
        }
        if (best >= woundMapReachCm * woundMapReachCm) continue;
        float uv[3][2];
        for (int c = 0; c < 3; ++c) {
            const unsigned t = *(const unsigned*)(m.uvIndices + size_t(bestTri[c]) * 4);
            if (t >= m.uvs.count) return;
            memcpy(uv[c], (const float*)(m.uvs.data + size_t(t) * 8), 8);
        }
        // The face's gradient: a surface direction d = s e1 + t e2 maps to s duv1 + t duv2.
        float e1[3], e2[3], n[3];
        for (int i = 0; i < 3; ++i) { e1[i] = bestP[1][i] - bestP[0][i]; e2[i] = bestP[2][i] - bestP[0][i]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        const float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (!(nn > 1e-10f)) continue;
        const float nl = 1.f / std::sqrt(nn);
        for (float& v : n) v *= nl;
        auto toUv = [&](const float* d, float* out) {
            const float a11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], a12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
            const float a22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
            const float b1 = d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2], b2 = d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2];
            const float det = a11 * a22 - a12 * a12;
            if (!(std::fabs(det) > 1e-12f)) return false;
            const float s = (b1 * a22 - b2 * a12) / det, t = (a11 * b2 - a12 * b1) / det;
            out[0] = s * (uv[1][0] - uv[0][0]) + t * (uv[2][0] - uv[0][0]);
            out[1] = s * (uv[1][1] - uv[0][1]) + t * (uv[2][1] - uv[0][1]);
            return std::isfinite(out[0] + out[1]);
        };
        // The blade's path on the surface (or the face's own first edge), unit length in cm.
        float along[3];
        const float dn = dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2];
        for (int i = 0; i < 3; ++i) along[i] = dir[i] - dn * n[i];
        float al = along[0] * along[0] + along[1] * along[1] + along[2] * along[2];
        if (!(al > 1e-8f)) { memcpy(along, e1, sizeof along); al = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]; }
        al = 1.f / std::sqrt(al);
        for (float& v : along) v *= al;
        const float across[3] = {n[1] * along[2] - n[2] * along[1], n[2] * along[0] - n[0] * along[2], n[0] * along[1] - n[1] * along[0]};
        if (!toUv(along, w.uvAlong) || !toUv(across, w.uvAcross)) continue;
        for (int i = 0; i < 2; ++i) w.uv[i] = bestW[0] * uv[0][i] + bestW[1] * uv[1][i] + bestW[2] * uv[2][i];
        const float det = w.uvAlong[0] * w.uvAcross[1] - w.uvAlong[1] * w.uvAcross[0];
        if (!(std::fabs(det) > 1e-12f) || !std::isfinite(w.uv[0] + w.uv[1])) continue;
        w.skinNode = node;
        w.actor = actor;
        w.mapped = true;
        return;
    }
}

// ---- 2. bake ----------------------------------------------------------------------------------

using WoundCopyImage = void(APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint,
                                       GLsizei, GLsizei, GLsizei);
using WoundMipmap = void(APIENTRY*)(GLuint);
using WoundLevelParameter = void(APIENTRY*)(GLuint, GLint, GLenum, GLint*);
struct WoundSkinGl {
    StoneGl stone;
    WoundCopyImage copy = nullptr;
    WoundMipmap mipmap = nullptr;
    WoundLevelParameter level = nullptr;
};
static bool loadWoundSkinGl(WoundSkinGl& s) {
    return fp::glproc::loadForCurrentContext(s, [](WoundSkinGl& out) {
        return loadStoneGl(out.stone) && loadGl(out.copy, "glCopyImageSubData") && loadGl(out.mipmap, "glGenerateTextureMipmap") &&
               loadGl(out.level, "glGetTextureLevelParameteriv");
    });
}

// Drawn in the skin texture's own UV space. Each fragment multiplies the original texel by every mark
// of the character, so overlapping marks agree and no blending is needed. Albedo only: the game's
// lighting shades the result.
static const char* const woundSkinVertex = R"glsl(#version 130
in vec2 corner;
out vec2 texUv;
void main() {
    texUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
// Shared by the three fragment programs: the version, a hash and the burn pattern (value noise in UV
// space; seams in the UV layout show as seams in the pattern).
static const char* const woundSkinFragmentHead = R"glsl(#version 130
uniform float burnLevel; // 0..1: the character's burn
uniform float burnSeed;
uniform float burnScar;  // 0..1: scar tissue left by healed burns
)glsl" FP_WOUND_GLSL_HELPERS R"glsl(
// Healed-burn scar tissue: x how much of it, y its mottling, at this texel.
vec2 burnScarAt(vec2 t) {
    vec2 o = vec2(burnSeed * 13.1, burnSeed * 7.7);
    float f = 0.55 * woundValueNoise(t * 18.0 + o) + 0.3 * woundValueNoise(t * 41.0 + o) + 0.15 * woundValueNoise(t * 97.0 + o);
    return vec2(smoothstep(f - 0.15, f + 0.15, burnScar * 1.6), woundValueNoise(t * 60.0 + o.yx));
}
// x: reddened, y: blistered, z: charred, w: ash speckle, each 0..1 at this texel.
vec4 burnAt(vec2 t) {
    vec2 o = vec2(burnSeed * 13.1, burnSeed * 7.7);
    float f = 0.55 * woundValueNoise(t * 18.0 + o) + 0.3 * woundValueNoise(t * 41.0 + o) + 0.15 * woundValueNoise(t * 97.0 + o);
    float red = smoothstep(f - 0.15, f + 0.15, burnLevel * 1.6);
    float blister = smoothstep(0.3, 0.6, burnLevel) * smoothstep(0.75, 0.85, woundValueNoise(t * 170.0 + o)) * red;
    float charred = smoothstep(f - 0.08, f + 0.08, (burnLevel - 0.5) * 1.8);
    return vec4(red, blister * (1.0 - charred), charred, step(0.85, woundValueNoise(t * 260.0 + o)));
}
)glsl";

static const char* const woundSkinFragment = R"glsl(
uniform sampler2D baseTex;
uniform vec4 markP[16]; // UV centre; zw: first row of UV offset -> (cm along the blade, cm across)
uniform vec4 markQ[16]; // xy: second row; z: half length (a bruise's radius), w: half width, cm
uniform vec4 markR[16]; // x: kind + 0.98 x age; y: severity; z: seed; w: unused
uniform float markCount;
in vec2 texUv;
out vec4 fragColor;
void main() {
    vec4 base = textureLod(baseTex, texUv, 0.0);
    vec3 tint = vec3(1.0);
    if (burnScar > 0.0) {
        vec2 s = burnScarAt(texUv);
        tint *= mix(vec3(1.0), mix(vec3(1.1, 0.9, 0.88), vec3(1.2, 1.1, 1.05), s.y), s.x * 0.7);
    }
    if (burnLevel > 0.0) {
        vec4 b = burnAt(texUv);
        tint *= mix(vec3(1.0), vec3(1.0, 0.62, 0.52), b.x * 0.8);
        tint = mix(tint, vec3(1.15, 1.02, 0.82), b.y * 0.7);
        tint = mix(tint, mix(vec3(0.1, 0.07, 0.05), vec3(0.35, 0.33, 0.3), b.w), b.z);
    }
    for (int i = 0; i < 16; ++i) {
        if (float(i) >= markCount) break;
        vec4 P = markP[i], Q = markQ[i], R = markR[i];
        vec2 o = texUv - P.xy;
        vec2 q = vec2(dot(P.zw, o), dot(Q.xy, o));
        float kind = floor(R.x + 0.01), aux = clamp((R.x - kind) / 0.98, 0.0, 1.0), scar = 0.0;
        if (kind > 4.0) { kind -= 4.0; scar = aux; aux = 0.0; }
        float severity = R.y, seed = R.z, fracture = R.w;
        if (kind == 2.0) {
            float w;
            vec3 bruise = woundBruiseTint(q, Q.z, severity, aux, seed, fracture, 0.0, w);
            tint *= mix(vec3(1.0), bruise, w);
            continue;
        }
        float a, taper, wobble, x, inside, lip, smear;
        woundCutShape(q, Q.z, Q.w, seed, a, taper, wobble, x, inside, lip);
        vec3 cut = woundCutSkinTint(q, a, x, inside, lip, severity, aux, seed, 0.0, smear);
        // Healing into a scar: the gash closes to a pale, thin line with a faint pink halo.
        tint *= woundScarApply(cut, taper, x, scar);
    }
    fragColor = vec4(base.rgb * tint, base.a);
}
)glsl";

// The normal map's copy: the marks' height (cm) as a slope on the map's tangent-space x/y, the way the
// game's skin shading reads it (shading.flb shadesss_f: xy * 2 - 1, x along the vertex tangent, y along
// normal x tangent times its sign; taken here as +u and +v, inferred). Screen derivatives of the UV-space
// render are per texel; each slope is scaled by the cm per UV of the mark nearest the texel.
static const char* const woundSkinNormalFragment = R"glsl(
uniform sampler2D baseTex;
uniform vec4 markP[16];
uniform vec4 markQ[16];
uniform vec4 markR[16];
uniform float markCount;
uniform vec2 texSize;
in vec2 texUv;
out vec4 fragColor;
void main() {
    vec4 base = textureLod(baseTex, texUv, 0.0);
    float h = 0.0, best = 1e9;
    vec2 cmPerUv = vec2(1.0);
    for (int i = 0; i < 16; ++i) {
        if (float(i) >= markCount) break;
        vec4 P = markP[i], Q = markQ[i], R = markR[i];
        vec2 o = texUv - P.xy;
        vec2 q = vec2(dot(P.zw, o), dot(Q.xy, o));
        float kind = floor(R.x + 0.01), scar = 0.0;
        if (kind > 4.0) { scar = clamp((R.x - kind) / 0.98, 0.0, 1.0); kind -= 4.0; }
        float severity = R.y, seed = R.z, near;
        if (kind == 2.0) {
            float r = length(q) / Q.z;
            h += (0.15 * severity + 0.45 * R.w) * (1.0 - smoothstep(0.0, 1.0, r)); // the swelling, a fracture's more
            near = r;
        } else {
            float a, taper, wobble, x, inside, lip;
            woundCutShape(q, Q.z, Q.w, seed, a, taper, wobble, x, inside, lip);
            float line = taper * (1.0 - smoothstep(0.25, 0.6, x));
            h += mix(-0.4 * severity * taper * (1.0 - smoothstep(0.0, 1.0, x)) + 0.12 * severity * lip, 0.06 * line, scar); // the gash and lips, or the scar's ridge
            near = max(abs(a), x * 0.25);
        }
        if (near < best) {
            best = near;
            cmPerUv = vec2(length(vec2(P.z, Q.x)), length(vec2(P.w, Q.y)));
        }
    }
    vec2 slope = vec2(dFdx(h) * texSize.x, dFdy(h) * texSize.y) / max(cmPerUv, vec2(1e-4));
    if (burnLevel > 0.0) {
        // Blisters rise; charred skin cracks. About 150 cm of body to the texture's width.
        vec4 b = burnAt(texUv);
        float hb = 0.12 * b.y - 0.05 * b.z * step(0.6, woundValueNoise(texUv * 120.0));
        slope += vec2(dFdx(hb) * texSize.x, dFdy(hb) * texSize.y) / 150.0;
    }
    if (burnScar > 0.0) {
        vec2 s = burnScarAt(texUv);
        float hs = 0.04 * s.x * (s.y - 0.5); // lumpy scar tissue
        slope += vec2(dFdx(hs) * texSize.x, dFdy(hs) * texSize.y) / 150.0;
    }
    vec2 n = clamp(base.xy * 2.0 - 1.0 - slope, vec2(-0.95), vec2(0.95));
    fragColor = vec4(n * 0.5 + 0.5, base.z, base.w);
}
)glsl";

// The spec map's copy: fresh blood (the gash, the smear) glossy and strong, dried blood dull, never metal.
static const char* const woundSkinSpecFragment = R"glsl(
uniform sampler2D baseTex;
uniform vec4 markP[16];
uniform vec4 markQ[16];
uniform vec4 markR[16];
uniform float markCount;
in vec2 texUv;
out vec4 fragColor;
void main() {
    vec4 base = textureLod(baseTex, texUv, 0.0);
    float blood = 0.0, wet = 0.0, sheen = 0.0;
    for (int i = 0; i < 16; ++i) {
        if (float(i) >= markCount) break;
        vec4 P = markP[i], Q = markQ[i], R = markR[i];
        float kind = floor(R.x + 0.01), aux = clamp((R.x - kind) / 0.98, 0.0, 1.0), scar = 0.0;
        if (kind > 4.0) { kind -= 4.0; scar = aux; aux = 0.0; }
        if (kind == 2.0) continue; // bruises leave the skin's own sheen
        vec2 o = texUv - P.xy;
        vec2 q = vec2(dot(P.zw, o), dot(Q.xy, o));
        float a, taper, wobble, x, inside, lip;
        woundCutShape(q, Q.z, Q.w, R.z, a, taper, wobble, x, inside, lip);
        float smear = (1.0 - smoothstep(1.2, 4.0, x)) * (1.0 - smoothstep(0.8, 1.25, abs(a)));
        float cover = max(inside, 0.6 * smear * R.y) * (1.0 - scar);
        if (cover > blood) { blood = cover; wet = aux; }
        sheen = max(sheen, taper * (1.0 - smoothstep(0.25, 0.6, x)) * scar);
    }
    vec3 spec = vec3(mix(0.12, 0.75, wet), mix(0.25, 0.9, wet), 0.0); // strength, gloss, metal
    vec3 skin = mix(base.rgb, vec3(0.3, 0.6, 0.0), sheen * 0.6); // scars: a little sheen
    if (burnScar > 0.0) skin = mix(skin, vec3(0.35, 0.75, 0.0), burnScarAt(texUv).x * 0.8);
    if (burnLevel > 0.0) {
        vec4 b = burnAt(texUv);
        skin = mix(skin, vec3(0.5, 0.8, 0.0), b.y);   // blisters: wet and glossy
        skin = mix(skin, vec3(0.03, 0.08, 0.0), b.z); // char: dull
    }
    fragColor = vec4(mix(skin, spec, blood), base.a);
}
)glsl";

static GLuint woundSkinProgram(gl::Api& g, const char* fragmentText) {
    auto compile = [&](GLenum type, const char* text) -> GLuint {
        const GLuint shader = g.createShader(type);
        if (!shader) return 0;
        const char* parts[2] = {woundSkinFragmentHead, text};
        if (type == glc::vertexShader) g.shaderSource(shader, 1, &text, nullptr);
        else g.shaderSource(shader, 2, parts, nullptr);
        g.compileShader(shader);
        GLint ok = 0;
        g.getShaderiv(shader, glc::compileStatus, &ok);
        if (!ok) { g.deleteShader(shader); return 0; }
        return shader;
    };
    const GLuint vertex = compile(glc::vertexShader, woundSkinVertex), fragment = compile(glc::fragmentShader, fragmentText);
    GLuint program = vertex && fragment ? g.createProgram() : 0;
    if (program) {
        g.attachShader(program, vertex);
        g.attachShader(program, fragment);
        g.bindAttribLocation(program, 0, "corner");
        g.linkProgram(program);
        GLint ok = 0;
        g.getProgramiv(program, glc::linkStatus, &ok);
        g.detachShader(program, vertex);
        g.detachShader(program, fragment);
        if (!ok) { g.deleteProgram(program); program = 0; }
    }
    if (vertex) g.deleteShader(vertex);
    if (fragment) g.deleteShader(fragment);
    return program;
}

// The slot's texture: the original's size and format (uncompressed 8-bit colour only), full mip chain.
// A map's copy: the original's size and format (uncompressed 8-bit only; the normal map may be
// two-channel), full mip chain. `copy` is the context's texture name for it.
static bool woundSkinTexture(const WoundSkinGl& s, WoundSkinMap& map, unsigned& copy) {
    if (copy && map.width && map.format) return true;
    GLint width = 0, height = 0, format = 0;
    s.level(map.source, 0, 0x1000 /* WIDTH */, &width);
    s.level(map.source, 0, 0x1001 /* HEIGHT */, &height);
    s.level(map.source, 0, 0x1003 /* INTERNAL_FORMAT */, &format);
    const bool plain = format == 0x8058 /* RGBA8 */ || format == 0x8C43 /* SRGB8_ALPHA8 */ || format == 0x8051 /* RGB8 */ ||
                       format == 0x8C41 /* SRGB8 */ || format == 0x822B /* RG8 */;
    if (width < 16 || height < 16 || width > 8192 || height > 8192 || !plain) { map.failed = true; return false; }
    if (copy) { glDeleteTextures(1, &copy); copy = 0; }
    s.stone.create(GL_TEXTURE_2D, 1, &copy);
    if (!copy) { map.failed = true; return false; }
    GLint levels = 1;
    for (GLint size = (std::max)(width, height); size > 1; size /= 2) ++levels;
    s.stone.storage(copy, levels, GLenum(format), width, height);
    s.stone.parameter(copy, 0x2801, 0x2703); // LINEAR_MIPMAP_LINEAR
    s.stone.parameter(copy, 0x2800, 0x2601); // LINEAR
    map.width = width;
    map.height = height;
    map.levels = levels;
    map.format = GLenum(format);
    return true;
}

// The slots follow the characters with mapped marks nearest in the store's order; woundLock held.
// The slots follow the characters (by scene node) with mapped marks or a burn; woundLock held.
static void woundSkinAssign() {
    bool wanted[woundSkinLimit]{};
    auto claim = [&](uintptr_t actor, uintptr_t character) {
        if (!actor) return;
        unsigned k = 0;
        while (k < woundSkinLimit && woundSkinSlots[k].actor != actor) ++k;
        if (k == woundSkinLimit) {
            k = 0;
            while (k < woundSkinLimit && woundSkinSlots[k].actor) ++k;
            if (k == woundSkinLimit) return;
            woundSkinSlots[k] = {};
            woundSkinSlots[k].actor = actor;
        }
        if (character) woundSkinSlots[k].character = character;
        wanted[k] = true;
    };
    for (const Wound& w : woundStore)
        if (w.part && w.mapped) claim(w.actor, w.owner);
    for (int b = 0; b < woundBurnCount; ++b)
        if (woundBurns[b].level) claim(woundBurns[b].actor, 0);
    for (int b = 0; b < woundBurnScarCount; ++b)
        if (woundBurnScars[b].level) claim(woundBurnScars[b].actor, 0);
    for (int b = 0; b < woundSeveredCount; ++b)
        if (woundSevered[b].level) claim(woundSevered[b].actor, 0);
    for (unsigned k = 0; k < woundSkinLimit; ++k)
        if (!wanted[k] && woundSkinSlots[k].actor) woundSkinSlots[k] = {}; // its texture is kept for the next one
}

struct WoundBakeProgUniforms {
    GLuint program = 0;
    GLint baseTex = -1, markCount = -1, texSize = -1;
    GLint burnLevel = -1, burnScar = -1, burnSeed = -1;
    GLint markP = -1, markQ = -1, markR = -1;
};

static void cacheWoundBakeProgUniforms(gl::Api& g, GLuint program, WoundBakeProgUniforms& out) {
    out = {};
    if (!program) return;
    out.program = program;
    out.baseTex = g.getUniformLocation(program, "baseTex");
    out.markCount = g.getUniformLocation(program, "markCount");
    out.texSize = g.getUniformLocation(program, "texSize");
    out.burnLevel = g.getUniformLocation(program, "burnLevel");
    out.burnScar = g.getUniformLocation(program, "burnScar");
    out.burnSeed = g.getUniformLocation(program, "burnSeed");
    out.markP = g.getUniformLocation(program, "markP");
    out.markQ = g.getUniformLocation(program, "markQ");
    out.markR = g.getUniformLocation(program, "markR");
}

struct WoundBakeUniformSet {
    const fp::CueResources* owner = nullptr;
    WoundBakeProgUniforms diffuse{}, normal{}, spec{};
};

static const WoundBakeUniformSet& woundBakeUniformsFor(gl::Api& g, const fp::CueResources& r) {
    static WoundBakeUniformSet sets[8]{};
    static unsigned nextSet = 0;
    WoundBakeUniformSet* chosen = nullptr;
    for (WoundBakeUniformSet& entry : sets) {
        if (entry.owner == &r) {
            chosen = &entry;
            break;
        }
    }
    if (!chosen) {
        chosen = &sets[nextSet];
        nextSet = (nextSet + 1) % 8;
        *chosen = {};
        chosen->owner = &r;
    }
    if (chosen->diffuse.program != r.woundBakeProgram) cacheWoundBakeProgUniforms(g, r.woundBakeProgram, chosen->diffuse);
    if (chosen->normal.program != r.woundBakeNormalProgram) cacheWoundBakeProgUniforms(g, r.woundBakeNormalProgram, chosen->normal);
    if (chosen->spec.program != r.woundBakeSpecProgram) cacheWoundBakeProgUniforms(g, r.woundBakeSpecProgram, chosen->spec);
    return *chosen;
}

static bool initWoundBakeResources(gl::Api& g, fp::CueResources& r) {
    if (r.woundBakeProgram && r.woundBakeFbo && r.woundBakeVao && r.woundBakeVbo) {
        (void)woundBakeUniformsFor(g, r);
        return true;
    }
    if (woundSkinBroken || woundMarks.load(std::memory_order_relaxed) <= 0.f) return false;
    WoundSkinGl s{};
    if (!loadWoundSkinGl(s) || !g.uniform4fv || !g.genFramebuffers) { woundSkinBroken = true; return false; }
    if (!r.woundBakeProgram) {
        r.woundBakeProgram = woundSkinProgram(g, woundSkinFragment);
        r.woundBakeNormalProgram = woundSkinProgram(g, woundSkinNormalFragment); // none: colour only
        r.woundBakeSpecProgram = woundSkinProgram(g, woundSkinSpecFragment);     // none: the skin's own sheen
    }
    if (!r.woundBakeProgram) { woundSkinBroken = true; return false; }
    (void)woundBakeUniformsFor(g, r);
    if (!r.woundBakeVao) g.genVertexArrays(1, &r.woundBakeVao);
    if (!r.woundBakeVbo) g.genBuffers(1, &r.woundBakeVbo);
    if (!r.woundBakeFbo) g.genFramebuffers(1, &r.woundBakeFbo);
    if (!r.woundBakeVao || !r.woundBakeVbo || !r.woundBakeFbo) { woundSkinBroken = true; return false; }
    return true;
}

struct WoundBakeGlSnapshot {
    bool saved = false;
    GLint drawFbo = 0, readFbo = 0, vao = 0, vbo = 0, prog = 0;
    GLint activeTex = GLint(glc::texture0), tex0 = 0, sampler0 = 0;
    GLint viewport[4]{};
    GLboolean mask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean blend = GL_FALSE, depth = GL_FALSE, cull = GL_FALSE;
    GLboolean scissor = GL_FALSE, stencil = GL_FALSE, srgb = GL_FALSE;
};

static void saveWoundBakeGlSnapshot(gl::Api& g, WoundBakeGlSnapshot& st) {
    glGetIntegerv(glc::drawFramebufferBinding, &st.drawFbo);
    glGetIntegerv(glc::readFramebufferBinding, &st.readFbo);
    glGetIntegerv(glc::vertexArrayBinding, &st.vao);
    glGetIntegerv(glc::arrayBufferBinding, &st.vbo);
    glGetIntegerv(glc::currentProgram, &st.prog);
    glGetIntegerv(glc::activeTexture, &st.activeTex);
    if (g.activeTexture) g.activeTexture(glc::texture0);
    glGetIntegerv(glc::textureBinding2D, &st.tex0);
    if (g.bindSampler) glGetIntegerv(glc::samplerBinding, &st.sampler0);
    glGetIntegerv(GL_VIEWPORT, st.viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, st.mask);
    st.blend = glIsEnabled(GL_BLEND);
    st.depth = glIsEnabled(GL_DEPTH_TEST);
    st.cull = glIsEnabled(GL_CULL_FACE);
    st.scissor = glIsEnabled(GL_SCISSOR_TEST);
    st.stencil = glIsEnabled(GL_STENCIL_TEST);
    st.srgb = glIsEnabled(glc::framebufferSrgb);
    st.saved = true;
}

static void restoreWoundBakeGlSnapshot(gl::Api& g, fp::CueResources& r, const WoundBakeGlSnapshot& st) {
    if (!st.saved) return;
    if (r.woundBakeFbo && g.bindFramebuffer && g.framebufferTexture2D) {
        g.bindFramebuffer(glc::framebuffer, r.woundBakeFbo);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, 0, 0);
    }
    if (g.bindFramebuffer) {
        if (st.drawFbo == st.readFbo) {
            g.bindFramebuffer(glc::framebuffer, GLuint(st.drawFbo));
        } else {
            g.bindFramebuffer(glc::drawFramebuffer, GLuint(st.drawFbo));
            g.bindFramebuffer(glc::readFramebuffer, GLuint(st.readFbo));
        }
    }
    setViewport(g, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3]);
    if (g.bindVertexArray) g.bindVertexArray(GLuint(st.vao));
    if (g.bindBuffer) g.bindBuffer(glc::arrayBuffer, GLuint(st.vbo));
    if (g.useProgram) g.useProgram(GLuint(st.prog));
    if (g.activeTexture) {
        g.activeTexture(glc::texture0);
        if (g.bindTexture) g.bindTexture(GL_TEXTURE_2D, GLuint(st.tex0));
        if (g.bindSampler) g.bindSampler(0, GLuint(st.sampler0));
        g.activeTexture(GLenum(st.activeTex));
    }
    glColorMask(st.mask[0], st.mask[1], st.mask[2], st.mask[3]);
    if (st.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (st.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (st.srgb) glEnable(glc::framebufferSrgb); else glDisable(glc::framebufferSrgb);
}

// Called by the view-effects pass inside its state snapshot. Early-outs before touching any OpenGL
// state when no character skin slot needs baking this frame. cueLock held.
static void bakeWoundSkins(gl::Api& g, fp::CueResources& r) {
    if (!woundSkinProven()) return;
    if (woundSkinBroken || woundMarks.load(std::memory_order_relaxed) <= 0.f) return;
    WoundBakeGlSnapshot glSnapshot{};
    WoundSkinGl s{};
    const WoundBakeUniformSet* uniforms = nullptr;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&woundLock);
    __try {
        for (Wound& w : woundStore)
            if (w.part && !w.mapTried) {
                __try { woundSkinMap(w); }
                __except (EXCEPTION_EXECUTE_HANDLER) { w.mapped = false; woundSkinFaults.fetch_add(1, std::memory_order_relaxed); }
                (w.mapped ? woundSkinMapped : woundSkinUnmapped).fetch_add(1, std::memory_order_relaxed);
                if (w.mapped) ++woundSkinVersion;
            }
        woundSkinAssign();
        unsigned baked = 0;
        for (unsigned k = 0; k < woundSkinLimit && baked < 2; ++k) {
            WoundSkinSlot& slot = woundSkinSlots[k];
            if (!slot.actor || !slot.diffuse.source || slot.diffuse.failed) continue;
            const float burn = woundBurnLevel(slot.actor), burnScar = woundBurnScar(slot.actor);
            // Marks change (a new one, a mend) or age (bruises darken, mends close, blood dries): re-bake then,
            // ageing at most every 6 s. Each bake copies all three full-size maps; drying blood re-baked every 2 s
            // for a minute after every cut, which was most of a session's bakes and nothing an eye follows.
            const bool changed = slot.bakedVersion != woundSkinVersion;
            const bool aging = slot.ready && now - slot.bakedAt >= 6000;
            if (slot.ready && !changed && !aging) continue;
            float P[woundSkinMarks][4], Q[woundSkinMarks][4], R[woundSkinMarks][4], corners[woundSkinMarks * 6 + 6][2];
            unsigned count = 0;
            bool ages = false;
            for (const Wound& w : woundStore) {
                if (!w.part || !w.mapped || !slot.character || w.owner != slot.character || count == woundSkinMarks) continue;
                const float det = w.uvAlong[0] * w.uvAcross[1] - w.uvAlong[1] * w.uvAcross[0];
                const float age = float(now - w.born) / 1000.f;
                const bool scarring = w.kind != woundCrush && w.scar > 0.001f;
                const float aux = scarring ? (std::min)(w.scar, 1.f)
                                : w.kind == woundCrush ? (std::min)(age / woundBruiseSeconds, 1.f) : std::clamp(w.wet, 0.f, 1.f);
                ages = ages || (w.kind == woundCrush && age < woundBruiseSeconds) || w.fade < 1.f ||
                       (w.kind != woundCrush && w.wet > 0.f) || (scarring && w.scar < 1.f);
                const float length = w.halfLength * w.fade * (w.fracture ? woundFractureScale : 1.f), width = w.halfWidth * w.fade;
                const float p[4] = {w.uv[0], w.uv[1], w.uvAcross[1] / det, -w.uvAcross[0] / det};
                const float q[4] = {-w.uvAlong[1] / det, w.uvAlong[0] / det, length, width};
                const float m[4] = {float(w.kind + (scarring ? 4 : 0)) + 0.98f * aux, w.severity * w.fade, w.seed, w.fracture ? 1.f : 0.f};
                memcpy(P[count], p, sizeof p);
                memcpy(Q[count], q, sizeof q);
                memcpy(R[count], m, sizeof m);
                // The area the mark can touch: its length plus a margin, and its smear (or its radius) across.
                const float reachAlong = w.kind == woundCrush ? length * 1.3f : length * 1.3f + 0.5f;
                const float reachAcross = w.kind == woundCrush ? length * 1.3f : width * 4.5f + 0.5f;
                float c[4][2];
                for (int corner = 0; corner < 4; ++corner) {
                    const float sa = (corner & 1) ? reachAlong : -reachAlong, sb = (corner & 2) ? reachAcross : -reachAcross;
                    c[corner][0] = w.uv[0] + w.uvAlong[0] * sa + w.uvAcross[0] * sb;
                    c[corner][1] = w.uv[1] + w.uvAlong[1] * sa + w.uvAcross[1] * sb;
                }
                const int order[6] = {0, 1, 2, 2, 1, 3};
                for (int v = 0; v < 6; ++v) memcpy(corners[count * 6 + v], c[order[v]], 8);
                ++count;
            }
            if (!count && burn <= 0.f && burnScar <= 0.f) continue;
            if (!glSnapshot.saved) {
                saveWoundBakeGlSnapshot(g, glSnapshot);
                if (!initWoundBakeResources(g, r) || !r.woundBakeFbo || !loadWoundSkinGl(s)) {
                    woundSkinBroken = true;
                    break;
                }
                uniforms = &woundBakeUniformsFor(g, r);
            }
            // A burn covers the whole skin: one more quad over all of UV space.
            unsigned vertices = count * 6;
            if (burn > 0.f || burnScar > 0.f) {
                const float all[6][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {0.f, 1.f}, {1.f, 0.f}, {1.f, 1.f}};
                memcpy(corners[vertices], all, sizeof all);
                vertices += 6;
            }
            const float burnSeedVal = float(slot.actor % 997) / 997.f;
            // Upload the UV quad buffer once per character bake rather than 3 times across diffuse/normal/spec.
            g.bindVertexArray(r.woundBakeVao);
            g.bindBuffer(glc::arrayBuffer, r.woundBakeVbo);
            g.bufferData(glc::arrayBuffer, ptrdiff_t(vertices * 8), corners, glc::dynamicDraw);
            g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
            g.enableVertexAttribArray(0);
            // Each map: the original into the copy, then the marks over it in UV space, then mipmaps.
            auto bakeMap = [&](WoundSkinMap& map, unsigned& copy, const WoundBakeProgUniforms& u) -> bool {
                if (!u.program || !map.source || map.failed || !r.woundBakeFbo || !woundSkinTexture(s, map, copy)) return false;
                s.copy(map.source, GL_TEXTURE_2D, 0, 0, 0, 0, copy, GL_TEXTURE_2D, 0, 0, 0, 0, map.width, map.height, 1);
                g.bindFramebuffer(glc::framebuffer, r.woundBakeFbo);
                g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, copy, 0);
                if (g.checkFramebufferStatus(glc::framebuffer) != glc::framebufferComplete) { map.failed = true; return false; }
                setViewport(g, 0, 0, map.width, map.height);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_STENCIL_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                const bool srgb = map.format == 0x8C43 || map.format == 0x8C41;
                (srgb ? glEnable : glDisable)(glc::framebufferSrgb);
                g.useProgram(u.program);
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, map.source);
                if (g.bindSampler) g.bindSampler(0, 0);
                if (u.baseTex >= 0) g.uniform1i(u.baseTex, 0);
                if (u.markCount >= 0) g.uniform1f(u.markCount, float(count));
                if (u.texSize >= 0) g.uniform2f(u.texSize, float(map.width), float(map.height));
                if (u.burnLevel >= 0) g.uniform1f(u.burnLevel, burn);
                if (u.burnScar >= 0) g.uniform1f(u.burnScar, burnScar);
                if (u.burnSeed >= 0) g.uniform1f(u.burnSeed, burnSeedVal);
                if (count > 0) {
                    if (u.markP >= 0) g.uniform4fv(u.markP, GLsizei(count), &P[0][0]);
                    if (u.markQ >= 0) g.uniform4fv(u.markQ, GLsizei(count), &Q[0][0]);
                    if (u.markR >= 0) g.uniform4fv(u.markR, GLsizei(count), &R[0][0]);
                }
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices));
                s.mipmap(copy);
                return true;
            };
            if (!bakeMap(slot.diffuse, r.woundSkinTex[k], uniforms->diffuse)) continue;
            slot.normalReady = bakeMap(slot.normal, r.woundSkinNrm[k], uniforms->normal);
            slot.specReady = bakeMap(slot.spec, r.woundSkinSpc[k], uniforms->spec);
            slot.bakedVersion = woundSkinVersion;
            slot.bakedAt = ages ? now : now + 0x7fffffffull; // nothing ages: no re-bake until the marks change
            slot.ready = true;
            ++baked;
            woundSkinBakes.fetch_add(1, std::memory_order_relaxed);
        }
    } __finally {
        ReleaseSRWLockExclusive(&woundLock);
        restoreWoundBakeGlSnapshot(g, r, glSnapshot);
    }
}

// ---- severed limbs (DISMEMBER-PLAN.md, stage 1) ------------------------------------------------

// The game's part indices (+0x468) each "injuries.severed" bit cuts off, top first; the top part's parent.
struct WoundCut { int parts[3]; int parent; };
static constexpr WoundCut woundCuts[8] = {
    {{1, 2, 3}, 0}, {{2, 3, -1}, 1}, {{4, 5, 6}, 0}, {{5, 6, -1}, 4},          // thighs (at the hip), shins (at the knee)
    {{11, 12, 16}, 10}, {{12, 16, -1}, 11}, {{14, 15, 17}, 13}, {{15, 17, -1}, 14}, // upper arms (shoulder), forearms (elbow)
};
constexpr unsigned woundBandSamples = 96;

// One severed mesh: per face 0 body, 1 limb, 2 the band (left out); per cut, the band ring's bind
// positions and the bones each side follows. Rebuilt when the mesh or its bits change.
struct WoundSeverMesh {
    uintptr_t node = 0, actor = 0;
    uint32_t bits = 0;
    ULONGLONG usedAt = 0;             // the last draw that asked for it; the caps skip entries not drawn lately
    std::vector<unsigned char> faces; // by face index (+0x290)
    struct Ring {
        int bit = -1, parentBone = -1, topBone = -1, farBone = -1; // far: the hand or foot, to face the end away from
        unsigned count = 0;
        float bind[woundBandSamples][3];
    } rings[8];
    unsigned ringCount = 0;
};
static WoundSeverMesh woundSeverMeshes[4]; // render thread

static int woundBonePart(const WoundSkinMesh& m, int k) {
    const uintptr_t transform = *(const uintptr_t*)(m.boneNodes + size_t(k) * 8);
    if (transform < field::sceneNode::transform) return -1;
    const uintptr_t node = transform - field::sceneNode::transform;
    const uintptr_t part = *(const uintptr_t*)(node + field::sceneNode::bodyPart);
    if (!part) return -1;
    const int index = *(const int*)(part + field::bodyPart::index);
    return index >= 0 && index < 18 ? index : -1;
}

static bool woundSeverBuild(uintptr_t node, uintptr_t actor, uint32_t bits, WoundSeverMesh& out) {
    WoundSkinMesh m;
    if (!woundSkinMesh(node, m)) return false;
    out = {};
    out.node = node;
    out.actor = actor;
    out.bits = bits;
    bool severedBone[fp::wear::maxBones]{};
    int boneOfPart[18];
    for (int& v : boneOfPart) v = -1;
    for (int k = 0; k < m.bones; ++k) {
        const int part = woundBonePart(m, k);
        if (part < 0) continue;
        if (boneOfPart[part] < 0) boneOfPart[part] = k;
        for (unsigned bit = 0; bit < 8; ++bit)
            if (bits & (1u << bit))
                for (int p : woundCuts[bit].parts)
                    if (p == part) severedBone[k] = true;
    }
    out.faces.assign(size_t(m.lastFace) + 1, 0);
    bool ringBone[8][fp::wear::maxBones]{};
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (!(bits & (1u << bit)) || out.ringCount == 8) continue;
        const unsigned rIdx = out.ringCount++;
        auto& ring = out.rings[rIdx];
        ring.bit = int(bit);
        ring.topBone = boneOfPart[woundCuts[bit].parts[0]];
        ring.parentBone = boneOfPart[woundCuts[bit].parent];
        for (int p : woundCuts[bit].parts)
            if (p >= 0 && boneOfPart[p] >= 0) ring.farBone = boneOfPart[p];
        for (int k = 0; k < m.bones; ++k) {
            const int part = woundBonePart(m, k);
            if (part < 0) continue;
            for (int p : woundCuts[bit].parts)
                if (p == part) ringBone[rIdx][k] = true;
        }
    }
    const auto* weights = (const fp::wear::Weights*)m.weights.data;
    auto shareFor = [&](unsigned p, const bool* boneSet) {
        float s = 0.f;
        for (int k = 0; k < 4; ++k)
            if (weights[p].bone[k] >= 1 && weights[p].bone[k] <= m.bones && boneSet[weights[p].bone[k] - 1] &&
                weights[p].weight[k] > 0.f)
                s += weights[p].weight[k];
        return s;
    };
    for (int f = 0; f <= m.lastFace; ++f) {
        const unsigned* tri = (const unsigned*)(m.triangles + size_t(f) * 12);
        int limb = 0;
        unsigned p[3];
        for (int c = 0; c < 3; ++c) {
            if (tri[c] > unsigned(m.lastVertex)) return false;
            p[c] = *(const unsigned*)(m.posIndices + size_t(tri[c]) * 4);
            if (p[c] >= m.positions.count) return false;
            limb += shareFor(p[c], severedBone) >= 0.5f ? 1 : 0;
        }
        bool isBand = limb != 0 && limb != 3;
        // Match boundary triangles per cut so multiple severed limbs (or hip + knee on the same leg)
        // populate their own distinct ring samples rather than merging into rings[0].
        for (unsigned r = 0; r < out.ringCount; ++r) {
            int ringLimb = 0;
            for (int c = 0; c < 3; ++c) ringLimb += shareFor(p[c], ringBone[r]) >= 0.5f ? 1 : 0;
            if (ringLimb == 1 || ringLimb == 2) {
                isBand = true;
                auto& ring = out.rings[r];
                for (int c = 0; c < 3 && ring.count < woundBandSamples; ++c)
                    memcpy(ring.bind[ring.count++], (const float*)(m.source + size_t(p[c]) * 12), 12);
            }
        }
        out.faces[size_t(f)] = isBand ? 2 : (limb == 3 ? 1 : 0);
    }
    return true;
}

// The mesh's split for this draw, cached; null when the character has nothing severed.
// Out of line: the caps pass below uses __try, so it may not build a temporary with a destructor itself.
static void woundSeverDrop(WoundSeverMesh& s) { s = {}; }

// Keyed by mesh node, actor and bits together: a node or actor pointer freed with an arena round and
// reused for a new body never matches an old entry.
static const WoundSeverMesh* woundSeverOf(uintptr_t node, uintptr_t actor) {
    const uint32_t bits = woundSeveredBits(actor) & 0xFF;
    if (!bits) return nullptr;
    const ULONGLONG now = GetTickCount64();
    for (WoundSeverMesh& s : woundSeverMeshes)
        if (s.node == node && s.actor == actor && s.bits == bits) { s.usedAt = now; return &s; }
    static unsigned next = 0;
    WoundSeverMesh& slot = woundSeverMeshes[next];
    next = (next + 1) % 4;
    if (!woundSeverBuild(node, actor, bits, slot)) { slot = {}; return nullptr; }
    slot.usedAt = now;
    return &slot;
}

// The caps: each ring carried by the parent bone (the stump) and by the severed bone (the end), as a
// disc through the ring's centre facing away from the bone's own centre. woundLock held.
static void woundSeveredCaps(const float* view, WoundUniforms& out) {
    out.caps = 0;
    if constexpr (!unprovenRenderReads) return; // which parts a severed bit cuts off, and the skin layout, are unproven
    out.caps = 0;
    const ULONGLONG clock = GetTickCount64();
    for (WoundSeverMesh& s : woundSeverMeshes) {
        if (!s.node) continue;
        // Only an entry a draw used in the last half second, whose actor still carries those bits: a body
        // freed between arena rounds stops being drawn, and its entry is dropped before anything is read.
        if (clock - s.usedAt > 500 || !(woundSeveredBits(s.actor) & s.bits)) { woundSeverDrop(s); continue; }
        __try {
            if (!readable(s.node, field::sceneNode::parent + 8) ||
                *(const uintptr_t*)(s.node + field::sceneNode::parent) != s.actor) { s.node = 0; continue; }
            WoundSkinMesh m;
            if (!woundSkinMesh(s.node, m)) continue;
            float sectorOrigin[3]{};
            const uintptr_t sector = *(const uintptr_t*)(s.node + field::sceneNode::sector);
            if (sector && readable(sector, field::sector::origin + 12))
                memcpy(sectorOrigin, (const void*)(sector + field::sector::origin), sizeof sectorOrigin);
            for (unsigned r = 0; r < s.ringCount && out.caps + 2 <= 8; ++r) {
                const auto& ring = s.rings[r];
                if (ring.count < 3 || ring.parentBone < 0 || ring.topBone < 0) continue;
                for (int side = 0; side < 2; ++side) {
                    const int bone = side ? ring.topBone : ring.parentBone;
                    float now[16], bind[16];
                    if (!fp::wear::affine((const float*)(*(const uintptr_t*)(m.boneNodes + size_t(bone) * 8)), now) ||
                        !fp::wear::affine((const float*)(m.inverseBind + size_t(bone) * 48), bind))
                        break;
                    float skin[16];
                    fp::wear::product(bind, now, skin); // bind position -> where this bone carries it now
                    float centre[3]{}, q[3];
                    for (unsigned v = 0; v < ring.count; ++v) {
                        if (!fp::wear::point(skin, ring.bind[v], q)) break;
                        for (int i = 0; i < 3; ++i) centre[i] += q[i] / float(ring.count);
                    }
                    float radius = 0.f;
                    for (unsigned v = 0; v < ring.count; ++v) {
                        fp::wear::point(skin, ring.bind[v], q);
                        radius += std::sqrt((q[0] - centre[0]) * (q[0] - centre[0]) + (q[1] - centre[1]) * (q[1] - centre[1]) +
                                            (q[2] - centre[2]) * (q[2] - centre[2])) / float(ring.count);
                    }
                    // Facing: the ring's own plane (its centre, a first point and the point making the widest
                    // angle with it), turned away from that side's flesh: the parent bone's origin for the
                    // stump, the hand's or foot's for the severed end.
                    float first[3], wide[3]{}, n[3]{};
                    fp::wear::point(skin, ring.bind[0], q);
                    for (int i = 0; i < 3; ++i) first[i] = q[i] - centre[i];
                    float widest = 0.f;
                    for (unsigned v = 1; v < ring.count; ++v) {
                        fp::wear::point(skin, ring.bind[v], q);
                        const float d[3] = {q[0] - centre[0], q[1] - centre[1], q[2] - centre[2]};
                        const float x[3] = {first[1] * d[2] - first[2] * d[1], first[2] * d[0] - first[0] * d[2],
                                            first[0] * d[1] - first[1] * d[0]};
                        const float sq = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
                        if (sq > widest) { widest = sq; memcpy(wide, x, sizeof wide); }
                    }
                    if (!(widest > 1e-6f) || !(radius > 0.5f) || radius > 30.f) break;
                    const float wl = 1.f / std::sqrt(widest);
                    for (int i = 0; i < 3; ++i) n[i] = wide[i] * wl;
                    const int refBone = side ? ring.farBone : ring.parentBone;
                    float ref[16];
                    if (refBone < 0 || !fp::wear::affine((const float*)(*(const uintptr_t*)(m.boneNodes + size_t(refBone) * 8)), ref)) break;
                    if (n[0] * (centre[0] - ref[12]) + n[1] * (centre[1] - ref[13]) + n[2] * (centre[2] - ref[14]) < 0.f)
                        for (float& v : n) v = -v;
                    const float* facing = n;
                    // The nearer reading of the sector origin, as for the marks.
                    float best[3]{}, bestSq = woundReachCm * woundReachCm;
                    bool found = false;
                    for (int candidate = 0; candidate < 2; ++candidate) {
                        float c[3];
                        for (int k = 0; k < 3; ++k) q[k] = centre[k] + (candidate ? sectorOrigin[k] : 0.f);
                        for (int k = 0; k < 3; ++k) c[k] = q[0] * view[k] + q[1] * view[4 + k] + q[2] * view[8 + k] + view[12 + k];
                        const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
                        if (std::isfinite(sq) && sq < bestSq) { bestSq = sq; memcpy(best, c, sizeof c); found = true; }
                    }
                    if (!found) break;
                    float dir[3];
                    for (int k = 0; k < 3; ++k) dir[k] = facing[0] * view[k] + facing[1] * view[4 + k] + facing[2] * view[8 + k];
                    const float a[4] = {best[0], best[1], best[2], radius * 1.1f};
                    const float bb[4] = {dir[0], dir[1], dir[2], float(side)};
                    memcpy(out.capA[out.caps], a, sizeof a);
                    memcpy(out.capB[out.caps], bb, sizeof bb);
                    ++out.caps;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            woundSkinFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---- 3. draw ----------------------------------------------------------------------------------

static int woundSkinSlotOf(uintptr_t node) {
    if (!readable(node, 0x2a0) || strncmp(nodeName(node), "_SKIN", 5)) return -1;
    const uintptr_t parent = *(uintptr_t*)(node + field::sceneNode::parent);
    for (unsigned k = 0; k < woundSkinLimit; ++k)
        if (woundSkinSlots[k].actor && woundSkinSlots[k].actor == parent) return int(k);
    return -1;
}

// The submission gate: a wounded character's skinned mesh, outside the player's own first-person scope.
static bool woundSkinSubmission(uintptr_t node) {
    if (woundSkinBroken || !wearScope.colourPass) return false;
    if (wearScope.colour && std::binary_search(wearScope.nodes, wearScope.nodes + wearScope.nodeCount, node)) return false;
    return woundSkinSlotOf(node) >= 0;
}

struct WoundSkinSwap { unsigned slot; GLuint textures[3]; bool ready; };
static const GLuint* woundSkinReplacement(fp::CueResources& r, const StoneGl&, const void* context) {
    auto* swap = (WoundSkinSwap*)const_cast<void*>(context);
    const unsigned k = swap->slot;
    if (!swap->ready) return swap->textures; // a severed character before its bake: its own maps
    if (!r.woundSkinTex[k]) return nullptr;
    swap->textures[0] = r.woundSkinTex[k];
    if (woundSkinSlots[k].normalReady && r.woundSkinNrm[k]) swap->textures[2] = r.woundSkinNrm[k];
    if (woundSkinSlots[k].specReady && r.woundSkinSpc[k]) swap->textures[1] = r.woundSkinSpc[k];
    return swap->textures;
}

// Records the body material's maps on its first draw (or when the game re-made that material's
// textures); true when the slot's copies stand for exactly these originals.
static bool woundSkinCurrent(WoundSkinSlot& slot, uintptr_t material, const GLuint* originals) {
    // Every skin material of a character shares one set (0x207458), so the slot follows the maps, not the material.
    if (slot.material && slot.diffuse.source == originals[0] && slot.normal.source == originals[2] &&
        slot.spec.source == originals[1])
        return slot.ready;
    slot.material = material;
    slot.diffuse = {};
    slot.normal = {};
    slot.spec = {};
    slot.diffuse.source = originals[0];
    slot.normal.source = originals[2];
    slot.spec.source = originals[1];
    slot.ready = slot.normalReady = slot.specReady = false;
    slot.bakedVersion = 0;
    return false;
}

static bool woundSkinDrawBody(uintptr_t node, uintptr_t material, bool bind) {
    if (!woundSkinProven()) return false; // the game draws its own
    const int found = woundSkinSlotOf(node);
    if (found < 0) return false;
    // A skin material, as the game assigned it (woundSkinMaterial); any other keeps the game's draw.
    if (!woundSkinMaterial(material)) return false;
    WoundSkinSlot& slot = woundSkinSlots[found];
    GLuint originals[3]{};
    if (!stoneSkinOriginals(material, originals)) return false;
    const int lastFace = *(int*)(node + 0x268), lastVertex = *(int*)(node + 0x288);
    if (lastFace < 0 || lastFace >= int(fp::wear::maxFaces) || lastVertex < 0 || lastVertex >= int(fp::wear::maxVertices)) return false;
    WearPlan plan{};
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan)) return false;
    // The whole material, as the game draws it: the copies are the originals plus the marks.
    static thread_local std::vector<unsigned char> body;
    body.assign(plan.count, 1);
    const bool ready = woundSkinCurrent(slot, material, originals);
    // A severed limb (the Injuries addon's bits; unproven, off): its faces draw with the limb, the band left out.
    const WoundSeverMesh* cut = unprovenRenderReads ? woundSeverOf(node, slot.actor) : nullptr;
    if (!ready && !cut) return false;
    if (cut) {
        for (unsigned f = 0; f < plan.count; ++f) {
            const size_t face = size_t(plan.first) + f;
            if (face < cut->faces.size() && cut->faces[face] == 2) body[f] = 2;
        }
    }
    WoundSkinSwap swap{unsigned(found), {originals[0], originals[1], originals[2]}, ready};
    return skinFacesEmit(plan, bind, body.data(), originals, 3, woundSkinReplacement, &swap);
}

static bool woundSkinDrawGuarded(uintptr_t node, uintptr_t material, bool bind) {
    try { return woundSkinDrawBody(node, material, bind); }
    catch (const std::exception&) { woundSkinFaults.fetch_add(1, std::memory_order_relaxed); return false; }
}
// true: the draw was made here (the baked skin where the body shows, the rest native).
#if FP_DEV_TOOLS
// Dev builds with [Dev] RenderAudit=1: once, on the first body draw of a skinned (_SKIN) mesh whose material
// is already bound, the facts the wound skin bake still lacks proof of, read from the game's own GL state and
// mesh, into the render-audit log. Read only; nothing it finds turns anything on.
//  - The skin program's DifTex, SpcTex and NrmTex sampler units and the texture bound on each, against the
//    material set the bake copies (stoneSkinOriginals: set +10/+18/+20).
//  - The program's active attributes, and for each the array the game pointed it at (size, type, stride,
//    offset), against the mesh's attribute 0x10 record (format, stream) the bake takes as UVs.
//  - The distinct per-face values (+2F8) among the material's faces, with counts (the bake takes 0 as body).
//  - The +218 controller's VMT and class name (TSkinController2 is the proven one).
static void woundSkinDiagnose(uintptr_t node, uintptr_t material, bool bind) {
    static bool done = false;
    if (done || bind || !renderAuditOn() || !readable(node, 0x300) || strncmp(nodeName(node), "_SKIN", 5)) return;
    done = true;
    using GetUniformLocation = GLint(WINAPI*)(GLuint, const char*);
    using GetUniformiv = void(WINAPI*)(GLuint, GLint, GLint*);
    using GetProgramiv = void(WINAPI*)(GLuint, GLenum, GLint*);
    using GetActiveAttrib = void(WINAPI*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
    using GetAttribLocation = GLint(WINAPI*)(GLuint, const char*);
    using GetVertexAttribiv = void(WINAPI*)(GLuint, GLenum, GLint*);
    using GetVertexAttribPointerv = void(WINAPI*)(GLuint, GLenum, void**);
    using ActiveTexture = void(WINAPI*)(GLenum);
    const auto uniformLocation = (GetUniformLocation)wglGetProcAddress("glGetUniformLocation");
    const auto uniformiv = (GetUniformiv)wglGetProcAddress("glGetUniformiv");
    const auto programiv = (GetProgramiv)wglGetProcAddress("glGetProgramiv");
    const auto activeAttrib = (GetActiveAttrib)wglGetProcAddress("glGetActiveAttrib");
    const auto attribLocation = (GetAttribLocation)wglGetProcAddress("glGetAttribLocation");
    const auto vertexAttribiv = (GetVertexAttribiv)wglGetProcAddress("glGetVertexAttribiv");
    const auto vertexAttribPointerv = (GetVertexAttribPointerv)wglGetProcAddress("glGetVertexAttribPointerv");
    const auto activeTexture = (ActiveTexture)wglGetProcAddress("glActiveTexture");
    GLint program = 0, priorUnit = 0;
    glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &program);
    renderAuditLog("skin witness: node %p material %p program %d; skin hook %s, %u skin sets seen, bake gate %s",
                   (void*)node, (void*)material, program, skinShareHooked.load() ? "on" : "off", woundSkinSetsSeen.load(),
                   woundSkinProven() ? "open" : "closed");
    // The game's skin marking (0x207240): a material whose definition [mat+D0] has a 38-character name (byte +208
    // == 0x26) receives the character's shared skin set (mat+100) and a new definition (mat+D0, 0x207436). Whether
    // that rule can still be read at draw time is what these lines show: each material's definition, its name
    // length and name, and its set, so skin materials can be told by a value the game itself leaves behind.
    const int lastMaterial = *(int*)(node + 0x1c8);
    const uintptr_t table = *(uintptr_t*)(node + 0x1d0);
    for (int i = 0; i <= lastMaterial && i < 16 && readable(table, size_t(i + 1) * 8); ++i) {
        const uintptr_t m = *(uintptr_t*)(table + size_t(i) * 8);
        const uintptr_t def = m && readable(m, 0x108) ? *(uintptr_t*)(m + 0xd0) : 0;
        const uintptr_t set = m && readable(m, 0x108) ? *(uintptr_t*)(m + 0x100) : 0;
        unsigned length = 0;
        char name[40]{};
        if (def && readable(def, 0x209 + 39)) {
            length = *(unsigned char*)(def + 0x208);
            memcpy(name, (const void*)(def + 0x209), (std::min)(length, 39u));
        }
        renderAuditLog("skin witness: material[%d] %p%s%s: definition %p name length %u \"%s\", set %p, faces %d..%d", i,
                       (void*)m, m == material ? " (this draw)" : "", woundSkinMaterial(m) ? " SKIN (game-assigned set)" : "",
                       (void*)def, length, name, (void*)set,
                       m && readable(m, 0xb8) ? *(int*)(m + 0xb0) : -1, m && readable(m, 0xb8) ? *(int*)(m + 0xb4) : -1);
    }
    GLuint originals[3]{};
    const bool set = stoneSkinOriginals(material, originals);
    renderAuditLog("skin witness: material set +10/+18/+20 = %u %u %u%s", originals[0], originals[1], originals[2],
                   set ? "" : " (unreadable)");
    if (program > 0 && uniformLocation && uniformiv && activeTexture) {
        glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &priorUnit);
        const char* const names[3] = {"DifTex", "SpcTex", "NrmTex"};
        for (int k = 0; k < 3; ++k) {
            const GLint location = uniformLocation(GLuint(program), names[k]);
            GLint unit = -1, bound = 0;
            if (location >= 0) {
                uniformiv(GLuint(program), location, &unit);
                if (unit >= 0 && unit < 32) {
                    activeTexture(glc::texture0 + GLenum(unit));
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
                }
            }
            renderAuditLog("skin witness: %s location %d unit %d texture %d; set[%d] %u: %s", names[k], location, unit,
                           bound, k, originals[k], location < 0 ? "no such sampler"
                                                   : unit == k && GLuint(bound) == originals[k] ? "match"
                                                                                                : "MISMATCH");
        }
        activeTexture(GLenum(priorUnit));
    }
    if (program > 0 && programiv && activeAttrib && attribLocation && vertexAttribiv && vertexAttribPointerv) {
        GLint count = 0;
        programiv(GLuint(program), 0x8B89 /* ACTIVE_ATTRIBUTES */, &count);
        for (GLint i = 0; i < count && i < 16; ++i) {
            char name[64]{};
            GLint size = 0;
            GLenum type = 0;
            activeAttrib(GLuint(program), GLuint(i), sizeof name, nullptr, &size, &type, name);
            const GLint location = attribLocation(GLuint(program), name);
            GLint enabled = 0, components = 0, arrayType = 0, stride = 0, normalized = 0;
            void* offset = nullptr;
            if (location >= 0) {
                vertexAttribiv(GLuint(location), 0x8622 /* ENABLED */, &enabled);
                vertexAttribiv(GLuint(location), 0x8623 /* SIZE */, &components);
                vertexAttribiv(GLuint(location), 0x8625 /* TYPE */, &arrayType);
                vertexAttribiv(GLuint(location), 0x8624 /* STRIDE */, &stride);
                vertexAttribiv(GLuint(location), 0x886A /* NORMALIZED */, &normalized);
                vertexAttribPointerv(GLuint(location), 0x8645 /* POINTER */, &offset);
            }
            renderAuditLog("skin witness: attribute %s (GLSL type 0x%X) location %d: enabled %d, %d x 0x%X%s, stride %d, offset %p",
                           name, type, location, enabled, components, arrayType, normalized ? " normalized" : "", stride,
                           offset);
        }
    }
    WearAttribute uv{};
    if (wearAttribute(node, 0x10, uv))
        renderAuditLog("skin witness: mesh attribute 0x10: format 0x%X (the bake assumes 0xF20010), stream %u, count %u",
                       uv.format, uv.stream, uv.count);
    else
        renderAuditLog("skin witness: mesh attribute 0x10 not found");
    const int lastFace = *(int*)(node + 0x268), lastVertex = *(int*)(node + 0x288);
    WearPlan plan{};
    const uintptr_t values = *(uintptr_t*)(node + 0x2f8);
    if (lastFace >= 0 && lastFace < int(fp::wear::maxFaces) && lastVertex >= 0 && lastVertex < int(fp::wear::maxVertices) &&
        wearMeshFormat(node, material, lastFace, lastVertex, plan) && readable(values, (size_t(plan.first) + plan.count) * 4)) {
        unsigned seen[8]{}, counts[8]{}, distinct = 0, other = 0;
        for (unsigned f = 0; f < plan.count; ++f) {
            const unsigned v = *(unsigned*)(values + (size_t(plan.first) + f) * 4);
            unsigned k = 0;
            while (k < distinct && seen[k] != v) ++k;
            if (k == distinct && distinct < 8) seen[distinct++] = v;
            if (k < distinct) ++counts[k]; else ++other;
        }
        for (unsigned k = 0; k < distinct; ++k)
            renderAuditLog("skin witness: material faces %u..%u: +2F8 value %u on %u faces", plan.first,
                           plan.first + plan.count - 1, seen[k], counts[k]);
        if (other) renderAuditLog("skin witness: %u faces with further values", other);
    } else {
        renderAuditLog("skin witness: material face range or +2F8 unreadable");
    }
    const uintptr_t controller = *(uintptr_t*)(node + 0x218);
    if (controller && readable(controller, 0x18)) {
        const uintptr_t vmt = *(uintptr_t*)controller, game = uintptr_t(GetModuleHandleW(nullptr));
        const uintptr_t nameAt = readable(vmt, 0x20) ? *(uintptr_t*)(vmt + 0x18) : 0;
        char className[64]{};
        if (nameAt && readable(nameAt, 1)) {
            const unsigned length = (std::min)(unsigned(*(unsigned char*)nameAt), 63u);
            if (readable(nameAt, length + 1)) memcpy(className, (const void*)(nameAt + 1), length);
        }
        renderAuditLog("skin witness: +218 controller %p, VMT RVA 0x%llX (TSkinController2 is 0x30CF50), class %s, +8 is node: %s",
                       (void*)controller, (unsigned long long)(vmt - game), className,
                       *(uintptr_t*)(controller + 8) == node ? "yes" : "no");
    }
}
#endif
// The lights the game drew a node with, copied at its material draw (infrastructure, nothing reads it yet).
// The submit 0x200770 packs the node's lights into renderer state +170 (0x1FF430 at 0x2007AD; three vec4 a light,
// count at state +A8, uploaded at 0x200BBD) before the node's material draws, so at a draw the state holds that
// node's lights: A position (w shadow depth scale), B colour (w inverse range), C shadow spread and shaping
// (docs/render-assumptions.md, "Per-light uniform layout"). Render thread only.
struct MeshLights { uintptr_t node = 0; ULONGLONG tick = 0; unsigned count = 0; float light[18][4]{}; };
static MeshLights meshLights[16];
static unsigned meshLightsNext = 0;
static void captureMeshLights(uintptr_t node) {
    const uintptr_t renderer = *(const uintptr_t*)gameAt(fp::sig::headwearWorldRenderer);
    if (!readable(renderer, 0x170 + 18 * 16) || !node) return;
    const int count = *(const int*)(renderer + 0xa8);
    if (count < 0 || count > 6) return;
    MeshLights* slot = nullptr;
    for (MeshLights& m : meshLights) if (m.node == node) slot = &m;
    if (!slot) { slot = &meshLights[meshLightsNext]; meshLightsNext = (meshLightsNext + 1) % 16; }
    slot->node = node;
    slot->tick = GetTickCount64();
    slot->count = unsigned(count);
    memcpy(slot->light, (const void*)(renderer + 0x170), size_t(count) * 3 * 16);
#if FP_DEV_TOOLS
    static ULONGLONG logged = 0;
    if (renderAuditOn() && slot->tick - logged > 1000) {
        logged = slot->tick;
        renderAuditLog("mesh lights: node %p count %u%s; first: position %.1f %.1f %.1f (w %.4f), colour %.3f %.3f %.3f "
                       "(1/range %.5f), shadow spread %.3f, shaping %.3f %.3f %.3f",
                       (void*)node, slot->count, count ? "" : " (none)", slot->light[0][0], slot->light[0][1],
                       slot->light[0][2], slot->light[0][3], slot->light[1][0], slot->light[1][1], slot->light[1][2],
                       slot->light[1][3], slot->light[2][0], slot->light[2][1], slot->light[2][2], slot->light[2][3]);
    }
#endif
}

static bool woundSkinDraw(uintptr_t node, uintptr_t material, bool bind) {
    __try { captureMeshLights(node); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#if FP_DEV_TOOLS
    __try { woundSkinDiagnose(node, material, bind); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    if (woundSkinBroken || !wearScope.colourPass) return false;
    bool drawn = false;
    __try {
        drawn = woundSkinDrawGuarded(node, material, bind);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundSkinFaults.fetch_add(1, std::memory_order_relaxed);
        drawn = false;
    }
    if (drawn) woundSkinDraws.fetch_add(1, std::memory_order_relaxed);
    return drawn;
}

// The player's own skinned mesh in first person stays with the headwear path, which swaps maps for the
// body's runs through StoneSwap (stone_skin.inl falls back to this when the player is not stone).
static bool woundSkinWearBegin(uintptr_t node, uintptr_t material, StoneSwap& swap) {
    swap = {};
    if (!woundSkinProven() || !woundSkinMaterial(material)) return false;
    if (woundSkinBroken || !wearScope.colourPass) return false;
    __try {
        const int found = woundSkinSlotOf(node);
        if (found < 0 || !stoneSkinOriginals(material, swap.originals)) return false;
        WoundSkinSlot& slot = woundSkinSlots[found];
        if (!woundSkinCurrent(slot, material, swap.originals)) return false;
        StoneGl s{};
        if (!loadStoneGl(s)) return false;
        auto* owner = prepareCue();
        if (!owner || !owner->resources.woundSkinTex[found]) return false;
        swap.stone[0] = owner->resources.woundSkinTex[found];
        swap.stone[1] = slot.specReady && owner->resources.woundSkinSpc[found] ? owner->resources.woundSkinSpc[found]
                                                                                 : swap.originals[1];
        swap.stone[2] = slot.normalReady && owner->resources.woundSkinNrm[found] ? owner->resources.woundSkinNrm[found]
                                                                                   : swap.originals[2];
        swap.bind = s.bind;
        swap.on = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        woundSkinFaults.fetch_add(1, std::memory_order_relaxed);
        swap = {};
    }
    if (swap.on) woundSkinDraws.fetch_add(1, std::memory_order_relaxed);
    return swap.on;
}
