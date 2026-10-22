// First-person colour-only headwear. Included after overlay/supersampling.
// Read contracts: headwearRender optional group, exact reviewed source image.
// No native geometry, physics, transform, mask, callback or descriptor is written.
#include "../headwear_render.h"
#include <memory>

using WearPass = void(*)(uintptr_t, uintptr_t, unsigned);
using WearDraw = void(*)(uintptr_t);
static WearPass originalWearPass = nullptr;
using WearSubmit = void(*)(uintptr_t, int);
static WearSubmit originalWearSubmit = nullptr;
struct WearSubmission { uintptr_t node = 0, material = 0; int index = -1; };
static WearDraw originalWearDraw = nullptr, originalWearDrawBound = nullptr;

inline void callWearBind(uintptr_t stream) {
    ((WearDraw)gameAt(fp::sig::headwearBind))(stream);
}
static WearDraw wearBind = callWearBind;
static std::atomic<bool> headwearRenderReady{false};
static std::atomic<unsigned> headwearRenderDraws{0}, headwearRenderHidden{0},
    headwearRenderStable{0}, headwearRenderFallbacks{0}, headwearRenderFaults{0};

struct WearScope {
    bool colour = false;
    uintptr_t root = 0, head = 0, sector = 0;
    uintptr_t materials[512]{}, nodes[512]{};
    unsigned count = 0, nodeCount = 0;
    bool complete = false, colourPass = false; // colourPass: inside the main colour traversal at all
    WearSubmission submission{}, pending{};
};
static thread_local WearScope wearScope;
static thread_local bool wearDrawing = false;
// Stone skin (stone_skin.inl, included after this file) rides the same submit and draw hooks.
static void stoneSkinPass();
static bool stoneSkinSubmission(uintptr_t node, bool ownBody = false);
static bool stoneSkinDraw(uintptr_t node, uintptr_t material, bool bind, bool ownBody = false);
// Wound skins (wounds_bake.inl, included after stone_skin.inl) too.
static bool woundSkinSubmission(uintptr_t node);
static bool woundSkinDraw(uintptr_t node, uintptr_t material, bool bind);
// The player's own skinned mesh stays with the headwear path in first person; its native body
// runs take the stone maps there (cueLock held for all three).
struct StoneSwap {
    bool on = false, swapped = false;
    unsigned stone[3]{}, originals[3]{};
    void (APIENTRY* bind)(GLuint, GLsizei, const GLuint*) = nullptr;
};
static bool stoneSkinWearBegin(uintptr_t node, uintptr_t material, StoneSwap& swap);
static void stoneSkinWearRun(StoneSwap& swap, bool stone);
static void stoneSkinWearEnd(StoneSwap& swap);

struct WearVertex { float position[3], normal[3], tangent[4], velocity[3]; };
struct WearScratch {
    fp::wear::Face faces[fp::wear::maxFaces];
    unsigned tags[fp::wear::maxFaces];
    WearVertex vertices[fp::wear::maxVertices];
};

static SRWLOCK wearScratchLock = SRWLOCK_INIT;
static std::unique_ptr<WearScratch> wearScratchStorage;

inline WearScratch& wearScratch() { return *wearScratchStorage; }
inline bool ensureWearScratch() {
    if (!wearScratchStorage) wearScratchStorage.reset(new(std::nothrow) WearScratch{});
    return wearScratchStorage != nullptr;
}

struct WearPlan {
    uintptr_t node = 0, material = 0;
    unsigned first = 0, count = 0, vertices = 0, width = 0, buffer = 0, attributes = 0;
    uintptr_t indices = 0;
    GLenum type = 0;
    bool stable = false;
    float bind[16]{};
};

inline bool wearAttached(uintptr_t child, uintptr_t parent, uintptr_t sector) {
    if (!readable(child, 0x2a0) || !readable(parent, 0x2a0) ||
        *(uintptr_t*)(child + field::sceneNode::parent) != parent ||
        *(uintptr_t*)(child + field::sceneNode::sector) != sector) return false;
    const int last = *(int*)(parent + field::sceneNode::lastChild);
    const uintptr_t list = *(uintptr_t*)(parent + field::sceneNode::children);
    if (last < 0 || last >= 6000 || !readable(list, (size_t(last) + 1) * 8)) return false;
    for (int k = 0; k <= last; ++k)
        if (*(uintptr_t*)(list + k * 8) == child) return true;
    return false;
}

inline bool wearBranch(uintptr_t node, uintptr_t head, uintptr_t root, uintptr_t sector, float* bind = nullptr) {
    if (bind) fp::wear::identity(bind);
    uintptr_t seen[64];
    unsigned count = 0;
    while (node != head) {
        if (count == 64 || !headWearCandidate(node, root)) return false;
        for (unsigned k = 0; k < count; ++k)
            if (seen[k] == node) return false;
        seen[count++] = node;
        const uintptr_t parent = *(uintptr_t*)(node + field::sceneNode::parent);
        if (!wearAttached(node, parent, sector)) return false;
        if (bind) {
            float local[16];
            if (!fp::wear::affine((const float*)(node + field::sceneNode::localTransform), local)) return false;
            fp::wear::product(bind, local, bind);
        }
        node = parent;
    }
    return true;
}

// format: element type and semantic in the low 24 bits; the top byte holds the game's own stream flags
// (tested on their own, e.g. 0x1000000 at 0x66EC8), so a weights stream can read 0x2A41000.
struct WearAttribute { unsigned flags, format, stream, count; uintptr_t data; };
static_assert(sizeof(WearAttribute) == 0x18);

inline bool wearAttribute(uintptr_t node, unsigned semantic, WearAttribute& out) {
    const int last = *(int*)(node + 0x278);
    const uintptr_t list = *(uintptr_t*)(node + 0x280);
    if (last < 0 || last >= 64 || !readable(list, (size_t(last) + 1) * sizeof(WearAttribute))) return false;
    bool found = false;
    for (int k = 0; k <= last; ++k) {
        const auto& a = ((const WearAttribute*)list)[k];
        if ((a.format & 0xffff) == semantic) {
            if (found) return false;
            out = a;
            found = true;
        }
    }
    return found;
}

inline bool wearStream(uintptr_t node, const WearAttribute& a, unsigned vertices, uintptr_t& indices) {
    const int last = *(int*)(node + 0x26c);
    const uintptr_t streams = *(uintptr_t*)(node + 0x270);
    if (last < 0 || last >= 64 || a.stream > static_cast<unsigned>(last) || !readable(streams, (size_t(last) + 1) * 16)) return false;
    indices = *(uintptr_t*)(streams + a.stream * 16 + 8);
    return readable(indices, size_t(vertices) * 4);
}

inline void collectWearMaterials(uintptr_t node, bool& complete) {
    if (!readable(node, 0x2a0) || (*(unsigned*)(node + 8) & 0x5f) != 9) return;
    if (wearScope.nodeCount == 512) { complete = false; return; }
    wearScope.nodes[wearScope.nodeCount++] = node;
    const int last = *(int*)(node + 0x1c8);
    const uintptr_t list = *(uintptr_t*)(node + 0x1d0);
    if (last < 0) return;
    if (last >= 256 || !readable(list, (size_t(last) + 1) * 8)) { complete = false; return; }
    for (int k = 0; k <= last; ++k) {
        if (wearScope.count == 512) { complete = false; return; }
        wearScope.materials[wearScope.count++] = *(uintptr_t*)(list + k * 8);
    }
}

inline void collectWearHints(uintptr_t root, uintptr_t head, uintptr_t sector) {
    bool complete = true;
    uintptr_t children[256];
    unsigned lineage[256];
    const unsigned count = walkScene(head, children, lineage, 256);
    if (count == 256) complete = false;
    for (unsigned k = 1; k < count; ++k) collectWearMaterials(children[k], complete);
    const uintptr_t actor = *(uintptr_t*)(root + field::character::sceneNode);
    if (!readable(actor, 0x2a0)) return;
    const int last = *(int*)(actor + field::sceneNode::lastChild);
    const uintptr_t list = *(uintptr_t*)(actor + field::sceneNode::children);
    if (last < 0 || last >= 6000 || !readable(list, (size_t(last) + 1) * 8)) return;
    // wearAttached without its repeated actor/list queries: node was just read from actor's list.
    for (int k = 0; k <= last; ++k) {
        const uintptr_t node = *(uintptr_t*)(list + k * 8);
        if (readable(node, 0x2a0) && !strncmp((const char*)(node + field::sceneNode::name), "_SKIN", 5) &&
            *(uintptr_t*)(node + field::sceneNode::parent) == actor &&
            *(uintptr_t*)(node + field::sceneNode::sector) == sector)
            collectWearMaterials(node, complete);
    }
    std::sort(wearScope.materials, wearScope.materials + wearScope.count);
    std::sort(wearScope.nodes, wearScope.nodes + wearScope.nodeCount);
    wearScope.complete = complete;
}

// The native index stream and attribute set of one material of a node, as its indexed draw uses them.
inline bool wearMeshFormat(uintptr_t node, uintptr_t material, int lastFace, int lastVertex, WearPlan& plan) {
    const int first = *(int*)(material + 0xb0), last = *(int*)(material + 0xb4),
              firstVertex = *(int*)(material + 0xb8), endVertex = *(int*)(material + 0xbc);
    if (first < 0 || last < first || last > lastFace || firstVertex < 0 || endVertex < firstVertex || endVertex > lastVertex) return false;
    const uintptr_t descriptor = *(uintptr_t*)(material + 0x68);
    if (!readable(descriptor, 0x1c)) return false;
    const auto* d = (const unsigned*)descriptor;
    const unsigned width = d[1], count = static_cast<unsigned>(last - first + 1);
    if ((width != 1 && width != 2 && width != 4) || d[2] != 1 || d[3] != GL_TRIANGLES || d[4] != count * 3) return false;
    const uintptr_t indices = d[0] ? *(uintptr_t*)(descriptor + 0x14) : descriptor + 0x14;
    if (!d[0] && !readable(indices, size_t(count) * 3 * width)) return false;
    if (indices > UINTPTR_MAX - size_t(count) * 3 * width) return false;
    plan.node = node; plan.material = material; plan.first = static_cast<unsigned>(first); plan.count = count;
    plan.vertices = static_cast<unsigned>(endVertex - firstVertex + 1); plan.width = width; plan.buffer = d[0]; plan.indices = indices;
    plan.type = width == 1 ? GL_UNSIGNED_BYTE : width == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    plan.attributes = *(unsigned*)(material + 0x1c);
    return true;
}

inline bool prepareWearPlan(uintptr_t node, uintptr_t material, WearPlan& plan) {
    const bool hideAll = helmetMode.load(std::memory_order_relaxed) == fp::HelmetMode::hide;
    if (!ensureWearScratch()) return false;
    if (!wearScope.colour || !supersampleViewMatches() || viewMode.load() != 0 ||
       !InterlockedCompareExchange(&active, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0) ||
       InterlockedCompareExchange(&stopping, 0, 0) || !readable(material, 0x108)) return false;
    const uintptr_t root = wearScope.root, head = verifiedPlayerHead(root), sector = wearScope.sector;
    if (!head || head != wearScope.head || *(uintptr_t*)(head + field::sceneNode::sector) != sector) return false;
    if (!readable(node, 0x300) || *(uintptr_t*)(node + field::sceneNode::sector) != sector) return false;
    const uintptr_t actor = *(uintptr_t*)(root + field::character::sceneNode);
    const bool aggregate = !strncmp(nodeName(node), "_SKIN", 5) && wearAttached(node, actor, sector);
    const bool separate = !aggregate && node != head && wearBranch(node, head, root, sector, hideAll ? nullptr : plan.bind);
    if (!aggregate && !separate) return false;
    const int lastMaterial = *(int*)(node + 0x1c8), lastFace = *(int*)(node + 0x268),
              lastVertex = *(int*)(node + 0x288);
    const uintptr_t materials = *(uintptr_t*)(node + 0x1d0);
    if (lastMaterial < 0 || lastMaterial >= 256 || lastFace < 0 || lastFace >= int(fp::wear::maxFaces) ||
       lastVertex < 0 || lastVertex >= int(fp::wear::maxVertices) ||
       !readable(materials, (size_t(lastMaterial) + 1) * 8)) return false;
    bool listed = false;
    for (int k = 0; k <= lastMaterial; ++k)
        if (*(uintptr_t*)(materials + k * 8) == material) listed = true;
    if (!listed) return false;
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan)) return false;
    const int first = int(plan.first), firstVertex = *(int*)(material + 0xb8), endVertex = firstVertex + int(plan.vertices) - 1;
    const unsigned count = plan.count;
    const uintptr_t triangles = *(uintptr_t*)(node + 0x290);
    if (!readable(triangles, (size_t(lastFace) + 1) * 12)) return false;
    WearAttribute positions{}, weights{}, normals{};
    uintptr_t posIndices = 0, normIndices = 0, source = 0, skin = 0;
    if (aggregate || !hideAll) {
        if (!wearAttribute(node, 1, positions) || (positions.format & 0xffffff) != 0xf30001 ||
           !positions.count || positions.count > fp::wear::maxVertices ||
           !wearStream(node, positions, static_cast<unsigned>(lastVertex + 1), posIndices)) return false;
    }
    bool headBones[fp::wear::maxBones]{};
    unsigned boneCount = 0;
    if (aggregate) {
        skin = *(uintptr_t*)(node + 0x218);
        if (!readable(skin, 0x40) || *(uintptr_t*)(skin + 8) != node) return false;
        const int bones = *(int*)(skin + 0x10);
        const uintptr_t boneNodes = *(uintptr_t*)(skin + 0x18), inverseBind = *(uintptr_t*)(skin + 0x20);
        if (bones < 1 || bones > int(fp::wear::maxBones) || !readable(boneNodes, size_t(bones) * 8) ||
           (!hideAll && !readable(inverseBind, size_t(bones) * 48)) || !wearAttribute(node, 0x1000, weights) ||
           (weights.format & 0xffffff) != 0xa41000 || weights.count != positions.count ||
           !readable(weights.data, size_t(weights.count) * sizeof(fp::wear::Weights))) return false;
        boneCount = static_cast<unsigned>(bones);
        int headIndex = -1;
        for (int k = 0; k < bones; ++k) {
            const uintptr_t transform = *(uintptr_t*)(boneNodes + k * 8);
            if (transform < field::sceneNode::transform) continue;
            const uintptr_t bone = transform - field::sceneNode::transform;
            if (bone == head) {
                if (headIndex >= 0) return false;
                headIndex = k;
                headBones[k] = true;
            } else {
                headBones[k] = wearBranch(bone, head, root, sector);
            }
        }
        if (headIndex < 0) return false;
        if (!hideAll) {
            if (!fp::wear::affine((const float*)(inverseBind + headIndex * 48), plan.bind)) return false;
            source = *(uintptr_t*)(skin + 0x38);
        }
    } else if (!hideAll) {
        source = positions.data;
    }
    if (!hideAll && !readable(source, size_t(positions.count) * 12)) return false;
    const uintptr_t tags = aggregate ? *(uintptr_t*)(node + 0x2f8) : 0;
    if (aggregate && !readable(tags, (size_t(lastFace) + 1) * 4)) return false;
    const bool normal = (plan.attributes & 2) != 0, tangent = (plan.attributes & 8) != 0;
    if (!hideAll && normal && (!wearAttribute(node, 2, normals) || (normals.format & 0xffffff) != 0xf30002 ||
       !normals.count || normals.count > fp::wear::maxVertices ||
       !readable(normals.data, size_t(normals.count) * 12) ||
       !wearStream(node, normals, static_cast<unsigned>(lastVertex + 1), normIndices))) return false;
    const uintptr_t tangents = hideAll || !tangent ? 0 : *(uintptr_t*)(material + 0xe8);
    if (!hideAll && tangent && !readable(tangents, size_t(plan.vertices) * 16)) return false;
    bool any = false;
    for (unsigned f = 0; f < count; ++f) {
        const unsigned* tri = (const unsigned*)(triangles + (size_t(first) + f) * 12);
        bool onHead = true;
        for (unsigned c = 0; c < 3; ++c) {
            if (tri[c] < static_cast<unsigned>(firstVertex) || tri[c] > static_cast<unsigned>(endVertex)) return false;
            if (aggregate) {
                const unsigned p = *(unsigned*)(posIndices + size_t(tri[c]) * 4);
                if (p >= positions.count) return false;
                if (!fp::wear::headVertex(((const fp::wear::Weights*)weights.data)[p], headBones, boneCount)) onHead = false;
            }
        }
        auto kind = onHead ? (aggregate && *(unsigned*)(tags + (size_t(first) + f) * 4) == 0 ? fp::wear::Face::head : fp::wear::Face::gear) : fp::wear::Face::native;
        if (hideAll && kind == fp::wear::Face::gear) kind = fp::wear::Face::blocked;
        wearScratch().faces[f] = kind;
        wearScratch().tags[f] = aggregate ? *(unsigned*)(tags + (size_t(first) + f) * 4) : 1;
        if (kind == fp::wear::Face::native) continue;
        any = true;
        if (hideAll) continue;
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned v = tri[c] - static_cast<unsigned>(firstVertex), p = *(unsigned*)(posIndices + size_t(tri[c]) * 4);
            if (!fp::wear::point(plan.bind, (const float*)(source + size_t(p) * 12), wearScratch().vertices[v].position)) return false;
            if (normal) {
                const unsigned n = *(unsigned*)(normIndices + size_t(tri[c]) * 4);
                if (n >= normals.count || !fp::wear::point(plan.bind, (const float*)(normals.data + size_t(n) * 12), wearScratch().vertices[v].normal, true)) return false;
            }
            if (tangent) {
                if (!fp::wear::point(plan.bind, (const float*)(tangents + size_t(v) * 16), wearScratch().vertices[v].tangent, true)) return false;
                wearScratch().vertices[v].tangent[3] = *(float*)(tangents + size_t(v) * 16 + 12);
                if (!std::isfinite(wearScratch().vertices[v].tangent[3])) return false;
            }
            std::memset(wearScratch().vertices[v].velocity, 0, 12);
        }
    }
    if (hideAll && aggregate) {
        // A helmet's rim, aventail or chin strap is weighted partly to the neck, so its faces fail the
        // all-head test. A worn piece (tag) mostly on the head goes as a whole.
        unsigned groupTag[fp::wear::maxGroups], groupHead[fp::wear::maxGroups]{}, groupAll[fp::wear::maxGroups]{}, groups = 0;
        for (unsigned f = 0; f < count; ++f) {
            const unsigned tag = wearScratch().tags[f];
            if (!tag) continue;
            unsigned g = 0;
            while (g < groups && groupTag[g] != tag) ++g;
            if (g == groups) {
                if (groups == fp::wear::maxGroups) continue;
                groupTag[groups++] = tag;
            }
            ++groupAll[g];
            if (wearScratch().faces[f] == fp::wear::Face::blocked) ++groupHead[g];
        }
        for (unsigned f = 0; f < count; ++f) {
            if (wearScratch().faces[f] != fp::wear::Face::native || !wearScratch().tags[f]) continue;
            for (unsigned g = 0; g < groups; ++g)
                if (groupTag[g] == wearScratch().tags[f]) {
                    if (groupHead[g] * 2 > groupAll[g]) { wearScratch().faces[f] = fp::wear::Face::blocked; any = true; }
                    break;
                }
        }
    }
    if (!any) return false;
    for (unsigned f = 0; f < count; ++f)
        if (wearScratch().faces[f] == fp::wear::Face::gear || wearScratch().faces[f] == fp::wear::Face::blocked) {
            headwearSeen.store(GetTickCount64(), std::memory_order_relaxed);
            break;
        }
    if (hideAll) return supersampleViewMatches() && verifiedPlayerHead(root) == head &&
                       *(uintptr_t*)(node + field::sceneNode::sector) == sector;
    unsigned blocked[fp::wear::maxGroups]{}, nBlocked = 0;
    bool overflow = false;
    const float setback = static_cast<float>(eyeSetback.load());
    for (unsigned f = 0; f < count; ++f) {
        if (wearScratch().faces[f] == fp::wear::Face::gear) {
            const unsigned* tri = (const unsigned*)(triangles + (size_t(first) + f) * 12);
            if (fp::wear::blocks(wearScratch().vertices[tri[0] - firstVertex].position,
                                 wearScratch().vertices[tri[1] - firstVertex].position,
                                 wearScratch().vertices[tri[2] - firstVertex].position, setback)) {
                const unsigned tag = wearScratch().tags[f];
                bool found = false;
                for (unsigned k = 0; k < nBlocked; ++k) found = found || blocked[k] == tag;
                if (!found) {
                    if (nBlocked == fp::wear::maxGroups) overflow = true;
                    else blocked[nBlocked++] = tag;
                }
            }
        }
    }
    for (unsigned f = 0; f < count; ++f) {
        if (wearScratch().faces[f] == fp::wear::Face::gear) {
            bool hide = overflow;
            for (unsigned k = 0; k < nBlocked; ++k) hide = hide || blocked[k] == wearScratch().tags[f];
            if (hide) wearScratch().faces[f] = fp::wear::Face::blocked;
            else plan.stable = true;
        }
    }
    return supersampleViewMatches() && verifiedPlayerHead(root) == head &&
           *(uintptr_t*)(node + field::sceneNode::sector) == sector;
}

using WearGetAttrib = void (APIENTRY*)(GLuint, GLenum, GLint*);
using WearGetPointer = void (APIENTRY*)(GLuint, GLenum, void**);
using WearGetUniform = void (APIENTRY*)(GLuint, GLint, GLfloat*);
using WearGetUniformInt = void (APIENTRY*)(GLuint, GLint, GLint*);
using WearUniform = void (APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);

struct WearGl {
    WearGetAttrib attrib = nullptr;
    WearGetPointer pointer = nullptr;
    WearGetUniform uniform = nullptr;
    WearGetUniformInt uniformInt = nullptr;
    WearUniform setUniform = nullptr;
};

inline bool loadWearGl(WearGl& g) {
    return loadGl(g.attrib, "glGetVertexAttribiv") && loadGl(g.pointer, "glGetVertexAttribPointerv") &&
           loadGl(g.uniform, "glGetUniformfv") && loadGl(g.uniformInt, "glGetUniformiv") &&
           loadGl(g.setUniform, "glUniformMatrix4fv");
}
static bool (*wearGlLoader)(WearGl&) = loadWearGl;

struct WearAttribState {
    GLuint index = 0;
    GLint size = 0, type = 0, normalized = 0, stride = 0, buffer = 0;
    void* pointer = nullptr;
};

struct WearGlState {
    GLint arrayBuffer = 0, elementBuffer = 0, vao = 0, program = 0, location = -1;
    GLboolean cull = 0;
    unsigned count = 0;
    WearAttribState attributes[4];
    float matrices[32]{}, mounted[32]{};
};

inline bool captureWearGl(const WearPlan& plan, const WearGl& api, WearGlState& state, bool mount = true) {
    glGetIntegerv(0x8894, &state.arrayBuffer);
    glGetIntegerv(0x8895, &state.elementBuffer);
    glGetIntegerv(0x85b5, &state.vao);
    if (GLuint(state.elementBuffer) != plan.buffer) return false;
    state.cull = glIsEnabled(GL_CULL_FACE);
    if (!plan.stable) return glGetError() == GL_NO_ERROR;
    glGetIntegerv(0x8b8d, &state.program);
    const uintptr_t renderer = *(uintptr_t*)gameAt(fp::sig::headwearWorldRenderer);
    if (!readable(renderer, 0x88)) return false;
    const uintptr_t configuration = *(uintptr_t*)(renderer + 0x80);
    if (!readable(configuration, 0x18) || *(GLint*)(configuration + 8) != state.program || !state.program) return false;
    const uintptr_t uniforms = *(uintptr_t*)(configuration + 0xc);
    if (!readable(uniforms, 0x18)) return false;
    state.location = *(GLint*)uniforms;
    if (state.location < 0) return false;
    api.uniform(GLuint(state.program), state.location, state.matrices);
    api.uniform(GLuint(state.program), state.location + 1, state.matrices + 16);
    if (mount) {
        GLint sample = 0;
        if (*(int*)gameAt(fp::sig::sampleLast) > 0) {
            const GLint sampleLocation = *(GLint*)(uniforms + 0x14);
            if (sampleLocation < 0) return false;
            api.uniformInt(GLuint(state.program), sampleLocation, &sample);
        }
        if (sample < 0 || sample > 3 || sample > *(int*)gameAt(fp::sig::sampleLast)) return false;
        const float* view = (const float*)gameAt(fp::sig::sampleViews) + sample * 16;
        if (!readable((uintptr_t)view, 64)) return false;
        float inverseModel[16], viewProjection[16];
        if (!fp::wear::inverse(state.matrices, inverseModel) ||
           !fp::wear::mount(view, static_cast<float>(eyeSetback.load()), state.mounted)) return false;
        fp::wear::product(inverseModel, state.matrices + 16, viewProjection);
        fp::wear::product(state.mounted, viewProjection, state.mounted + 16);
        for (float v : state.mounted)
            if (!std::isfinite(v)) return false;
    }
    const GLuint indices[4] = {0, 2, 6, 7};
    const bool needed[4] = {true, (plan.attributes & 2) != 0, (plan.attributes & 8) != 0, (plan.attributes & 0x40) != 0};
    for (unsigned k = 0; k < 4; ++k) {
        if (needed[k]) {
            WearAttribState& a = state.attributes[state.count++];
            a.index = indices[k];
            GLint integer = 0, enabled = 0;
            api.attrib(a.index, 0x88fd, &integer);
            api.attrib(a.index, 0x8622, &enabled);
            api.attrib(a.index, 0x8623, &a.size);
            api.attrib(a.index, 0x8625, &a.type);
            api.attrib(a.index, 0x886a, &a.normalized);
            api.attrib(a.index, 0x8624, &a.stride);
            api.attrib(a.index, 0x889f, &a.buffer);
            api.pointer(a.index, 0x8645, &a.pointer);
            if (integer || !enabled || a.type != GL_FLOAT || a.size != (a.index == 6 ? 4 : 3) || (!a.buffer && state.vao))
                return false;
        }
    }
    return glGetError() == GL_NO_ERROR;
}

inline void restoreWearAttributes(const gl::Api& g, const WearGlState& state) {
    for (unsigned k = 0; k < state.count; ++k) {
        const auto& a = state.attributes[k];
        g.bindBuffer(0x8892, GLuint(a.buffer));
        g.vertexAttribPointer(a.index, a.size, GLenum(a.type), GLboolean(a.normalized), a.stride, a.pointer);
    }
    g.bindBuffer(0x8892, GLuint(state.arrayBuffer));
}

inline void mountWearAttributes(const gl::Api& g, const WearPlan& plan, GLuint buffer) {
    g.bindBuffer(0x8892, buffer);
    g.vertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(WearVertex), (const void*)offsetof(WearVertex, position));
    if (plan.attributes & 2) g.vertexAttribPointer(2, 3, GL_FLOAT, 0, sizeof(WearVertex), (const void*)offsetof(WearVertex, normal));
    if (plan.attributes & 8) g.vertexAttribPointer(6, 4, GL_FLOAT, 0, sizeof(WearVertex), (const void*)offsetof(WearVertex, tangent));
    if (plan.attributes & 0x40) g.vertexAttribPointer(7, 3, GL_FLOAT, 0, sizeof(WearVertex), (const void*)offsetof(WearVertex, velocity));
}

inline void wearDrawRange(const WearPlan& plan, unsigned first, unsigned count) {
    glDrawElements(GL_TRIANGLES, GLsizei(count * 3), plan.type, (const void*)(plan.indices + size_t(first) * 3 * plan.width));
}

using WearBindBuffer = void (APIENTRY*)(GLenum, GLuint);

inline bool emitWearNativeOnly(const WearPlan& plan, bool bind) {
    if (!ownedNativeSwap(cueCurrentDC()) || !wearScope.colour || !supersampleViewMatches() ||
       viewMode.load() != 0 || !InterlockedCompareExchange(&active, 0, 0) ||
       !InterlockedCompareExchange(&cameraReady, 0, 0) ||
       !readable(plan.node, 0x2a0) || *(uintptr_t*)(plan.node + field::sceneNode::sector) != wearScope.sector ||
       verifiedPlayerHead(wearScope.root) != wearScope.head) return false;
    bool emitted = false;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (cueDeletesInFlight || cueRetiring.load() || cueBindingFailed.load() ||
           InterlockedCompareExchange(&stopping, 0, 0) || !ownedNativeSwap(cueCurrentDC())) return false;
        WearBindBuffer bindBuffer = nullptr;
        if (bind && !loadGl(bindBuffer, "glBindBuffer")) return false;
        if (bind) {
            wearBind(plan.material + 8);
            bindBuffer(0x8893, plan.buffer);
        }
        GLint elementBuffer = 0;
        glGetIntegerv(0x8895, &elementBuffer);
        // No glGetError here: with the driver's threaded optimization each one waits for its thread to drain.
        if (!ownedNativeSwap(cueCurrentDC()) || GLuint(elementBuffer) != plan.buffer) return false;
        StoneSwap swap{};
        stoneSkinWearBegin(plan.node, plan.material, swap);
        __try {
            for (unsigned f = 0; f < plan.count;) {
                const auto kind = wearScratch().faces[f];
                const bool body = wearScratch().tags[f] == 0;
                unsigned end = f + 1;
                while (end < plan.count && wearScratch().faces[end] == kind && (!swap.on || (wearScratch().tags[end] == 0) == body)) ++end;
                if (kind == fp::wear::Face::native) {
                    stoneSkinWearRun(swap, body);
                    wearDrawRange(plan, f, end - f);
                } else {
                    headwearRenderHidden.fetch_add(end - f, std::memory_order_relaxed);
                }
                f = end;
            }
        } __finally {
            stoneSkinWearEnd(swap);
        }
        emitted = true;
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return emitted;
}

inline bool emitWearPlan(const WearPlan& plan, bool bind) {
    if (!plan.stable) return emitWearNativeOnly(plan, bind);
    if (!ownedNativeSwap(cueCurrentDC())) return false;
    bool emitted = false;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (cueDeletesInFlight || cueRetiring.load() || cueBindingFailed.load() ||
           InterlockedCompareExchange(&stopping, 0, 0) || !ownedNativeSwap(cueCurrentDC())) return false;
        WearGl api{};
        if (!wearGlLoader(api)) return false;
        if (bind) wearBind(plan.material + 8);
        auto* owner = prepareCue();
        if (!owner) return false;
        auto& g = owner->api;
        if (bind) g.bindBuffer(0x8893, plan.buffer);
        WearGlState state{};
        if (!captureWearGl(plan, api, state)) return false;
        bool stable = plan.stable;
        bool mounted = false;
        StoneSwap swap{};
        __try {
            stoneSkinWearBegin(plan.node, plan.material, swap);
            if (stable && !owner->resources.ready() && !owner->resources.prepare(g)) stable = false;
            if (stable && !owner->resources.headwearVbo) g.genBuffers(1, &owner->resources.headwearVbo);
            if (stable && (!owner->resources.headwearVbo || glGetError() != GL_NO_ERROR)) stable = false;
            if (stable) {
                g.bindBuffer(0x8892, owner->resources.headwearVbo);
                g.bufferData(0x8892, ptrdiff_t(size_t(plan.vertices) * sizeof(WearVertex)), wearScratch().vertices, 0x88e8);
                if (glGetError() != GL_NO_ERROR) stable = false;
            }
            for (unsigned f = 0; f < plan.count;) {
                const auto kind = wearScratch().faces[f];
                const bool body = wearScratch().tags[f] == 0;
                unsigned end = f + 1;
                while (end < plan.count && wearScratch().faces[end] == kind && (!swap.on || (wearScratch().tags[end] == 0) == body)) ++end;
                const bool gear = kind == fp::wear::Face::gear;
                if (kind == fp::wear::Face::native || (gear && stable)) {
                    stoneSkinWearRun(swap, body && kind == fp::wear::Face::native);
                    if (gear != mounted) {
                        if (gear) {
                            mountWearAttributes(g, plan, owner->resources.headwearVbo);
                            api.setUniform(state.location, 2, 0, state.mounted);
                            glDisable(GL_CULL_FACE);
                        } else {
                            restoreWearAttributes(g, state);
                            api.setUniform(state.location, 2, 0, state.matrices);
                            if (state.cull) glEnable(GL_CULL_FACE);
                            else glDisable(GL_CULL_FACE);
                        }
                        mounted = gear;
                    }
                    emitted = true;
                    wearDrawRange(plan, f, end - f);
                    if (gear) headwearRenderStable.fetch_add(end - f, std::memory_order_relaxed);
                } else {
                    headwearRenderHidden.fetch_add(end - f, std::memory_order_relaxed);
                }
                f = end;
            }
            emitted = true;
        } __finally {
            stoneSkinWearEnd(swap);
            g.bindVertexArray(GLuint(state.vao));
            if (plan.stable) {
                restoreWearAttributes(g, state);
                api.setUniform(state.location, 2, 0, state.matrices);
            }
            g.bindBuffer(0x8892, GLuint(state.arrayBuffer));
            g.bindBuffer(0x8893, GLuint(state.elementBuffer));
            if (state.cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
        }
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return emitted;
}

inline WearSubmission wearSubmission(uintptr_t node, int index) {
    if (wearDrawing || cueGlDepth || index < 0 || index >= 256) return {};
    // The player's hand draws are tickets for the VR hand whatever the headwear hints hold; a stone
    // character's skinned mesh is a ticket for the stone skin.
    const bool hand = wearScope.colourPass && (trackedHandSubmission(node) || trackedBodySubmission(node) || stoneSkinSubmission(node) ||
                                                woundSkinSubmission(node));
    if (!hand && (!headwearRenderReady.load(std::memory_order_relaxed) || !wearScope.colour ||
       (wearScope.complete && !std::binary_search(wearScope.nodes, wearScope.nodes + wearScope.nodeCount, node)))) return {};
    if (!readable(node, 0x1d8) || index > *(int*)(node + 0x1c8)) return {};
    const uintptr_t table = *(uintptr_t*)(node + 0x1d0);
    if (!readable(table, size_t(index + 1) * 8)) return {};
    return {node, *(uintptr_t*)(table + size_t(index) * 8), index};
}

inline void wearSubmitBody(uintptr_t node, int index, uintptr_t caller) {
    const WearSubmission previous = wearScope.submission;
    wearScope.submission = {};
    wearScope.pending = {};
    WearSubmission submitted{};
    bool completed = false;
    const bool single = caller == gameAt(fp::sig::headwearSubmitSingleReturn);
    const bool sampled = caller == gameAt(fp::sig::headwearSubmitSamplesReturn);
    __try {
        __try {
            if (single || sampled) submitted = wearSubmission(node, index);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            submitted = {};
        }
        if (sampled) wearScope.submission = submitted;
        if (single || sampled) noteSilhouetteSubmission(node, index, sampled);
        fp::profile::invoke<fp::profile::Hook::wearSubmitNative>(originalWearSubmit, node, index);
        completed = true;
    } __finally {
        wearScope.submission = previous;
        wearScope.pending = completed && single ? submitted : WearSubmission{};
    }
}

inline void wearSubmit(uintptr_t node, int index) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    fp::profile::invoke<fp::profile::Hook::wearSubmit>(wearSubmitBody, node, index, caller);
}

inline void wearDrawBody(uintptr_t material, bool bind, uintptr_t caller) {
    const WearSubmission submitted = bind ? wearScope.pending : wearScope.submission;
    wearScope.pending = {};
    const uintptr_t expected = gameAt(bind ? fp::sig::headwearDrawReturn : fp::sig::headwearDrawBoundReturn);
    bool handled = false;
    // A tracked VR hand takes the player's hand draws whole; the headwear path never sees them.
    if (caller == expected && submitted.node && submitted.material == material && !wearDrawing && !cueGlDepth &&
        trackedHandDraw(submitted.node, material, bind)) return;
    if (caller == expected && submitted.node && submitted.material == material && !wearDrawing && !cueGlDepth &&
        trackedBodyHidden(submitted.node, material, bind)) return; // hands only: the body draw is dropped, or cut down to the faces the finger bones carry
    if (caller == expected && submitted.node && submitted.material == material && !wearDrawing && !cueGlDepth &&
        stoneSkinDraw(submitted.node, material, bind)) return; // a stone character: body faces with the stone maps
    if (caller == expected && submitted.node && submitted.material == material && !wearDrawing && !cueGlDepth &&
        woundSkinDraw(submitted.node, material, bind)) return; // a wounded character: body faces with its baked skin
    if (caller == expected && submitted.node && submitted.material == material &&
       headwearRenderReady.load(std::memory_order_relaxed) && wearScope.colour && !wearDrawing && !cueGlDepth &&
       (!wearScope.complete || std::binary_search(wearScope.materials, wearScope.materials + wearScope.count, material)) &&
       TryAcquireSRWLockExclusive(&wearScratchLock)) {
        wearDrawing = true;
        __try {
            WearPlan plan{};
            bool prepared = false;
            __try {
                const uintptr_t table = readable(submitted.node, 0x1d8) ? *(uintptr_t*)(submitted.node + 0x1d0) : 0;
                const int last = table ? *(int*)(submitted.node + 0x1c8) : -1;
                if (submitted.index >= 0 && submitted.index <= last && last < 256 &&
                   readable(table, size_t(last + 1) * 8) && *(uintptr_t*)(table + size_t(submitted.index) * 8) == material)
                    prepared = prepareWearPlan(submitted.node, material, plan);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                headwearRenderFaults.fetch_add(1, std::memory_order_relaxed);
            }
            if (prepared) {
                handled = emitWearPlan(plan, bind);
                if (handled) headwearRenderDraws.fetch_add(1, std::memory_order_relaxed);
                else headwearRenderFallbacks.fetch_add(1, std::memory_order_relaxed);
            }
        } __finally {
            wearDrawing = false;
            ReleaseSRWLockExclusive(&wearScratchLock);
        }
    }
    // The player's own mesh with nothing for the headwear path to hide (no head faces in this
    // material) still takes the stone maps when the player is stone.
    if (!handled && caller == expected && submitted.node && submitted.material == material && !wearDrawing && !cueGlDepth &&
        stoneSkinDraw(submitted.node, material, bind, true)) return;
    if (!handled)
        fp::profile::invoke<fp::profile::Hook::wearDrawNative>(bind ? originalWearDraw : originalWearDrawBound, material);
}

inline void wearDraw(uintptr_t material) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    fp::profile::invoke<fp::profile::Hook::wearDraw>(wearDrawBody, material, true, caller);
}

inline void wearDrawBound(uintptr_t material) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    fp::profile::invoke<fp::profile::Hook::wearDraw>(wearDrawBody, material, false, caller);
}

inline void wearPassBody(uintptr_t list, uintptr_t sector, unsigned flags) {
    const WearScope previous = wearScope;
    wearScope = {};
    __try {
        __try {
            if (headwearRenderReady.load(std::memory_order_relaxed) && viewMode.load() == 0 && supersampleViewMatches()) {
                const uintptr_t root = supersampleRenderScope.root, head = verifiedPlayerHead(root);
                if (head && *(uintptr_t*)(head + field::sceneNode::sector) == sector) {
                    wearScope = {true, root, head, sector};
                    collectWearHints(root, head, sector);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wearScope = {};
        }
        wearScope.colourPass = true; // after the scope is settled, so no assignment above clears it
        stoneSkinPass(); // guards its own call into the runtime
        fp::profile::invoke<fp::profile::Hook::wearPassNative>(originalWearPass, list, sector, flags);
    } __finally {
        wearScope = previous;
    }
}

inline void wearPass(uintptr_t list, uintptr_t sector, unsigned flags) {
    fp::profile::invoke<fp::profile::Hook::wearPass>(wearPassBody, list, sector, flags);
}

static bool silhouetteSubmitHooked() { return headwearRenderReady.load(std::memory_order_relaxed); }

static bool headwearHooksCreated = false, headwearAdmitted = false;
inline bool admitHeadwearRenderEntries(const fp::ImageView& file, bool sourceVerified) {
    headwearRenderReady = false;
    headwearAdmitted = false;
    bool admitted = false;
    __try {
        admitted = sourceVerified && layout.optionalGroupAvailable("headwearRender") &&
                 loadedOptionalContractsImpl(file, "headwearRender");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        admitted = false;
    }
    if (!admitted) {
        if (headwearHooksCreated) {
            snprintf(faultDetail, sizeof faultDetail, "headwear render contracts changed after hook creation; restart required");
            return false;
        }
        for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
            const char* group = fp::sig::targetOptionalGroups[t];
            if (group && !strcmp(group, "headwearRender")) layout.rva[t] = 0;
        }
    }
    headwearAdmitted = admitted;
    return true;
}

inline bool createHeadwearRenderHooks() {
    if (!headwearAdmitted) return true;
    struct Entry { fp::sig::Target target; void* replacement; void** original; };
    const Entry entries[] = {
        {fp::sig::headwearColourPass, (void*)wearPass, (void**)&originalWearPass},
        {fp::sig::headwearDraw, (void*)wearDraw, (void**)&originalWearDraw},
        {fp::sig::headwearDrawBound, (void*)wearDrawBound, (void**)&originalWearDrawBound},
        {fp::sig::headwearSubmit, (void*)wearSubmit, (void**)&originalWearSubmit}};
    unsigned created = 0;
    for (const auto& e : entries) {
        if (MH_CreateHook((void*)gameAt(e.target), e.replacement, e.original) != MH_OK) {
            bool removed = true;
            while (created) {
                --created;
                removed = MH_RemoveHook((void*)gameAt(entries[created].target)) == MH_OK && removed;
            }
            headwearHooksCreated = !removed;
            headwearAdmitted = false;
            return removed;
        }
        ++created;
    }
    headwearHooksCreated = true;
    return true;
}
