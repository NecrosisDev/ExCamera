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

static bool woundSkinMesh(uintptr_t node, WoundSkinMesh& m) {
    if (!readable(node, 0x300)) return false;
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
    m.tags = *(uintptr_t*)(node + 0x2f8);
    return readable(m.triangles, size_t(m.lastFace + 1) * 12) && readable(m.tags, size_t(m.lastFace + 1) * 4);
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
static void woundSkinMap(Wound& w) {
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
        float best = woundMapReachCm * woundMapReachCm, bestW[3]{}, bestP[3][3]{};
        unsigned bestTri[3]{};
        for (int f = 0; f <= m.lastFace; ++f) {
            if (*(const unsigned*)(m.tags + size_t(f) * 4) != 0) continue; // the body itself, not a worn piece
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
    return loadStoneGl(s.stone) && loadGl(s.copy, "glCopyImageSubData") && loadGl(s.mipmap, "glGenerateTextureMipmap") &&
           loadGl(s.level, "glGetTextureLevelParameteriv");
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
float markHash(float x) { return fract(sin(x * 12.9898) * 43758.5453); }
float burnNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = markHash(i.x + i.y * 57.0), b = markHash(i.x + 1.0 + i.y * 57.0);
    float c = markHash(i.x + (i.y + 1.0) * 57.0), d = markHash(i.x + 1.0 + (i.y + 1.0) * 57.0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// Healed-burn scar tissue: x how much of it, y its mottling, at this texel.
vec2 burnScarAt(vec2 t) {
    vec2 o = vec2(burnSeed * 13.1, burnSeed * 7.7);
    float f = 0.55 * burnNoise(t * 18.0 + o) + 0.3 * burnNoise(t * 41.0 + o) + 0.15 * burnNoise(t * 97.0 + o);
    return vec2(smoothstep(f - 0.15, f + 0.15, burnScar * 1.6), burnNoise(t * 60.0 + o.yx));
}
// x: reddened, y: blistered, z: charred, w: ash speckle, each 0..1 at this texel.
vec4 burnAt(vec2 t) {
    vec2 o = vec2(burnSeed * 13.1, burnSeed * 7.7);
    float f = 0.55 * burnNoise(t * 18.0 + o) + 0.3 * burnNoise(t * 41.0 + o) + 0.15 * burnNoise(t * 97.0 + o);
    float red = smoothstep(f - 0.15, f + 0.15, burnLevel * 1.6);
    float blister = smoothstep(0.3, 0.6, burnLevel) * smoothstep(0.75, 0.85, burnNoise(t * 170.0 + o)) * red;
    float charred = smoothstep(f - 0.08, f + 0.08, (burnLevel - 0.5) * 1.8);
    return vec4(red, blister * (1.0 - charred), charred, step(0.85, burnNoise(t * 260.0 + o)));
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
            float mottle = 0.78 + 0.22 * sin(q.x * 0.9 + seed * 31.0) * sin(q.y * 1.3 - seed * 17.0);
            float r = length(q) / (Q.z * mottle);
            float w = min((1.0 - smoothstep(0.3, 1.0, r)) * (0.5 + 0.4 * severity) * (1.0 + 0.35 * fracture), 1.0);
            // Over a broken bone the bruise spreads wide and goes deep purple-black at its centre.
            vec3 outer = mix(mix(vec3(0.85, 0.42, 0.4), vec3(0.62, 0.45, 0.62), aux), vec3(0.5, 0.33, 0.5), fracture);
            vec3 inner = mix(mix(vec3(0.6, 0.18, 0.16), vec3(0.42, 0.17, 0.2), aux), vec3(0.26, 0.08, 0.13), fracture);
            vec3 bruise = mix(outer, inner, 1.0 - smoothstep(0.0, 0.7, r));
            float graze = severity > 0.6 ? step(0.8, markHash(floor(q.x * 2.0) + floor(q.y * 2.0) * 57.0 + seed * 101.0)) *
                                               (1.0 - smoothstep(0.0, 0.45, r)) : 0.0;
            tint *= mix(vec3(1.0), mix(bruise, vec3(0.35, 0.05, 0.04), graze), w);
            continue;
        }
        float a = q.x / Q.z;
        float taper = sqrt(max(1.0 - a * a, 0.0));
        float wobble = 0.22 * Q.w * sin(a * 5.0 + seed * 40.0);
        float x = abs(q.y - wobble) / max(Q.w * (0.25 + 0.75 * taper), 0.05);
        float inside = taper > 0.0 ? 1.0 - smoothstep(0.6, 1.0, x) : 0.0;
        float lip = taper * smoothstep(0.7, 1.1, x) * (1.0 - smoothstep(1.3, 2.4, x));
        float smear = (1.0 - smoothstep(1.2, 4.0, x)) * (1.0 - smoothstep(0.8, 1.25, abs(a))) *
                      (0.6 + 0.4 * markHash(floor(q.x * 1.5 + seed * 13.0) + floor(q.y * 1.5) * 31.0));
        vec3 cut = mix(vec3(1.0), mix(vec3(0.55, 0.3, 0.24), vec3(0.7, 0.22, 0.2), aux), smear * (0.55 + 0.35 * severity));
        cut = mix(cut, vec3(1.0, 0.5, 0.45), lip * 0.8);
        cut = mix(cut, mix(vec3(0.5, 0.035, 0.03), vec3(0.07, 0.003, 0.003), 1.0 - smoothstep(0.0, 0.8, x)), inside);
        // Healing into a scar: the gash closes to a pale, thin line with a faint pink halo.
        float line = taper * (1.0 - smoothstep(0.25, 0.6, x));
        float halo = taper * (1.0 - smoothstep(0.6, 2.0, x));
        cut = mix(cut, mix(mix(vec3(1.0), vec3(1.05, 0.9, 0.88), halo * 0.45), vec3(1.14, 1.02, 0.97), line * 0.85), scar);
        tint *= cut;
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
            float a = q.x / Q.z;
            float taper = sqrt(max(1.0 - a * a, 0.0));
            float wobble = 0.22 * Q.w * sin(a * 5.0 + seed * 40.0);
            float x = abs(q.y - wobble) / max(Q.w * (0.25 + 0.75 * taper), 0.05);
            float lip = taper * smoothstep(0.7, 1.1, x) * (1.0 - smoothstep(1.3, 2.4, x));
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
        float hb = 0.12 * b.y - 0.05 * b.z * step(0.6, burnNoise(texUv * 120.0));
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
        float a = q.x / Q.z;
        float taper = sqrt(max(1.0 - a * a, 0.0));
        float wobble = 0.22 * Q.w * sin(a * 5.0 + R.z * 40.0);
        float x = abs(q.y - wobble) / max(Q.w * (0.25 + 0.75 * taper), 0.05);
        float inside = taper > 0.0 ? 1.0 - smoothstep(0.6, 1.0, x) : 0.0;
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

// Called by the view-effects pass inside its state snapshot (it restores framebuffer, viewport,
// program, vertex array, buffer, texture units, samplers and capabilities). cueLock held.
static void bakeWoundSkins(gl::Api& g, fp::CueResources& r) {
    if (woundSkinBroken || woundMarks.load(std::memory_order_relaxed) <= 0.f) return;
    WoundSkinGl s{};
    if (!loadWoundSkinGl(s) || !g.uniform4fv || !g.genFramebuffers) { woundSkinBroken = true; return; }
    if (!r.woundBakeProgram) {
        r.woundBakeProgram = woundSkinProgram(g, woundSkinFragment);
        r.woundBakeNormalProgram = woundSkinProgram(g, woundSkinNormalFragment); // none: colour only
        r.woundBakeSpecProgram = woundSkinProgram(g, woundSkinSpecFragment);     // none: the skin's own sheen
        if (!r.woundBakeProgram) { woundSkinBroken = true; return; }
        g.genVertexArrays(1, &r.woundBakeVao);
        g.genBuffers(1, &r.woundBakeVbo);
        g.genFramebuffers(1, &r.woundBakeFbo);
        if (!r.woundBakeVao || !r.woundBakeVbo || !r.woundBakeFbo) { woundSkinBroken = true; return; }
    }
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
            // Marks change (a new one, a mend) or age (bruises darken, mends close): re-bake then, at most every 2 s.
            const bool changed = slot.bakedVersion != woundSkinVersion;
            const bool aging = slot.ready && now - slot.bakedAt >= 2000;
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
            // A burn covers the whole skin: one more quad over all of UV space.
            unsigned vertices = count * 6;
            if (burn > 0.f || burnScar > 0.f) {
                const float all[6][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {0.f, 1.f}, {1.f, 0.f}, {1.f, 1.f}};
                memcpy(corners[vertices], all, sizeof all);
                vertices += 6;
            }
            // Each map: the original into the copy, then the marks over it in UV space, then mipmaps.
            auto bakeMap = [&](WoundSkinMap& map, unsigned& copy, GLuint program, bool known) -> bool {
                if (!program || !map.source || map.failed || !woundSkinTexture(s, map, copy)) return false;
                s.copy(map.source, GL_TEXTURE_2D, 0, 0, 0, 0, copy, GL_TEXTURE_2D, 0, 0, 0, 0, map.width, map.height, 1);
                g.bindFramebuffer(glc::framebuffer, r.woundBakeFbo);
                g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, copy, 0);
                if (!known && g.checkFramebufferStatus(glc::framebuffer) != glc::framebufferComplete) { map.failed = true; return false; }
                setViewport(g, 0, 0, map.width, map.height);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_STENCIL_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                const bool srgb = map.format == 0x8C43 || map.format == 0x8C41;
                (srgb ? glEnable : glDisable)(glc::framebufferSrgb);
                g.useProgram(program);
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, map.source);
                if (g.bindSampler) g.bindSampler(0, 0);
                g.uniform1i(g.getUniformLocation(program, "baseTex"), 0);
                g.uniform1f(g.getUniformLocation(program, "markCount"), float(count));
                g.uniform2f(g.getUniformLocation(program, "texSize"), float(map.width), float(map.height));
                g.uniform1f(g.getUniformLocation(program, "burnLevel"), burn);
                g.uniform1f(g.getUniformLocation(program, "burnScar"), burnScar);
                g.uniform1f(g.getUniformLocation(program, "burnSeed"), float(slot.actor % 997) / 997.f);
                g.uniform4fv(g.getUniformLocation(program, "markP"), GLsizei(count), &P[0][0]);
                g.uniform4fv(g.getUniformLocation(program, "markQ"), GLsizei(count), &Q[0][0]);
                g.uniform4fv(g.getUniformLocation(program, "markR"), GLsizei(count), &R[0][0]);
                g.bindVertexArray(r.woundBakeVao);
                g.bindBuffer(glc::arrayBuffer, r.woundBakeVbo);
                g.bufferData(glc::arrayBuffer, ptrdiff_t(vertices * 8), corners, glc::dynamicDraw);
                g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
                g.enableVertexAttribArray(0);
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices));
                s.mipmap(copy);
                return true;
            };
            if (!bakeMap(slot.diffuse, r.woundSkinTex[k], r.woundBakeProgram, slot.ready)) continue;
            slot.normalReady = bakeMap(slot.normal, r.woundSkinNrm[k], r.woundBakeNormalProgram, slot.normalReady);
            slot.specReady = bakeMap(slot.spec, r.woundSkinSpc[k], r.woundBakeSpecProgram, slot.specReady);
            slot.bakedVersion = woundSkinVersion;
            slot.bakedAt = ages ? now : now + 0x7fffffffull; // nothing ages: no re-bake until the marks change
            slot.ready = true;
            ++baked;
            woundSkinBakes.fetch_add(1, std::memory_order_relaxed);
        }
    } __finally {
        ReleaseSRWLockExclusive(&woundLock);
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
    const int index = *(const int*)(part + woundPartIndex);
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
    const auto* weights = (const fp::wear::Weights*)m.weights.data;
    auto share = [&](unsigned p) {
        float s = 0.f;
        for (int k = 0; k < 4; ++k)
            if (weights[p].bone[k] >= 1 && weights[p].bone[k] <= m.bones && severedBone[weights[p].bone[k] - 1] &&
                weights[p].weight[k] > 0.f)
                s += weights[p].weight[k];
        return s;
    };
    out.faces.assign(size_t(m.lastFace) + 1, 0);
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (!(bits & (1u << bit)) || out.ringCount == 8) continue;
        auto& ring = out.rings[out.ringCount++];
        ring.bit = int(bit);
        ring.topBone = boneOfPart[woundCuts[bit].parts[0]];
        ring.parentBone = boneOfPart[woundCuts[bit].parent];
        for (int p : woundCuts[bit].parts)
            if (p >= 0 && boneOfPart[p] >= 0) ring.farBone = boneOfPart[p];
    }
    for (int f = 0; f <= m.lastFace; ++f) {
        const unsigned* tri = (const unsigned*)(m.triangles + size_t(f) * 12);
        int limb = 0;
        unsigned p[3];
        for (int c = 0; c < 3; ++c) {
            if (tri[c] > unsigned(m.lastVertex)) return false;
            p[c] = *(const unsigned*)(m.posIndices + size_t(tri[c]) * 4);
            if (p[c] >= m.positions.count) return false;
            limb += share(p[c]) >= 0.5f ? 1 : 0;
        }
        out.faces[size_t(f)] = limb == 3 ? 1 : limb == 0 ? 0 : 2;
        if (limb != 0 && limb != 3) // the band: its corners make the ring (to the nearest cut's sample list)
            for (unsigned r = 0; r < out.ringCount; ++r) {
                auto& ring = out.rings[r];
                for (int c = 0; c < 3 && ring.count < woundBandSamples; ++c)
                    memcpy(ring.bind[ring.count++], (const float*)(m.source + size_t(p[c]) * 12), 12);
                break; // one ring per mesh is enough for a single cut; several cuts share the first (stage 1)
            }
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
    if (slot.material && slot.material != material) return false; // another part of the mesh keeps its own maps
    if (slot.material == material && slot.diffuse.source == originals[0] && slot.normal.source == originals[2] &&
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
    const int found = woundSkinSlotOf(node);
    if (found < 0) return false;
    WoundSkinSlot& slot = woundSkinSlots[found];
    GLuint originals[3]{};
    if (!stoneSkinOriginals(material, originals)) return false;
    const int lastFace = *(int*)(node + 0x268), lastVertex = *(int*)(node + 0x288);
    if (lastFace < 0 || lastFace >= int(fp::wear::maxFaces) || lastVertex < 0 || lastVertex >= int(fp::wear::maxVertices)) return false;
    WearPlan plan{};
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan)) return false;
    const uintptr_t tags = *(uintptr_t*)(node + 0x2f8);
    if (!readable(tags, (size_t(plan.first) + plan.count) * 4)) return false;
    static thread_local std::vector<unsigned char> body;
    body.assign(plan.count, 0);
    unsigned faces = 0;
    for (unsigned f = 0; f < plan.count; ++f) {
        body[f] = *(unsigned*)(tags + (size_t(plan.first) + f) * 4) == 0 ? 1 : 0;
        faces += body[f];
    }
    if (!faces) return false;
    const bool ready = woundSkinCurrent(slot, material, originals);
    // A severed limb: its faces draw with the limb, the band that bridged the joint is left out.
    const WoundSeverMesh* cut = woundSeverOf(node, slot.actor);
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
static bool woundSkinDraw(uintptr_t node, uintptr_t material, bool bind) {
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
