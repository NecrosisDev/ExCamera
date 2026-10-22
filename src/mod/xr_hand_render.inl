// Mod-drawn articulated hand for tracked fingers. It replaces the native hand mesh at the hand
// part's own indexed draw, using the native program, model matrix and depth, so it lights and
// occludes like the body. Off unless the VR hands setting is on; nothing native is written.
#include "../xr/hand_mesh.h"
#include "../xr/hand_bend.h"
static_assert(sizeof(fp::xr::HandVertex) == sizeof(WearVertex) && offsetof(fp::xr::HandVertex, normal) == offsetof(WearVertex, normal) &&
              offsetof(fp::xr::HandVertex, tangent) == offsetof(WearVertex, tangent) && offsetof(fp::xr::HandVertex, velocity) == offsetof(WearVertex, velocity),
              "the hand mesh must use the native body vertex layout");
static std::atomic<unsigned> trackedHandDraws{0}, trackedHandFaults{0};

// Native slot 0 is the right hand; tracked side 0 is the left, as everywhere in the drive.
// The bare hand is the part's own mesh node. A glove or gauntlet is a rigid piece carrying the
// hand's part code in its name (17.. right, 18.. left) and hung on that part or its node; taking
// its draw too means the mod hand wears the glove's material. Weapons hang on the hand node as
// well but carry their own codes, so the name guard keeps them native.
static bool trackedHandNode(uintptr_t root, uintptr_t node, unsigned& side) {
    if (!readable(root, field::character::handParts + 16) || !readable(node, field::sceneNode::name + 16)) return false;
    for (side = 0; side < 2; ++side) {
        const uintptr_t part = trackedHandPart(root, side);
        if (!part || !readable(part, field::bodyPart::meshNode + 8)) continue;
        const uintptr_t own = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
        if (own == node) return true;
        const char* name = reinterpret_cast<const char*>(node + field::sceneNode::name);
        const bool coded = name[0] == '1' && name[1] == (side ? '7' : '8');
        if (coded && (*reinterpret_cast<uintptr_t*>(node + field::sceneNode::bodyPart) == part || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) == own)) return true;
    }
    return false;
}
// Joints into the part's achieved frame: the pose rows are its axes, 9..11 its origin.
static bool trackedHandLocal(uintptr_t part, const TrackedDriveFrame::HandJoints& world, std::array<XrVector3f, xd::handJointCount>& out) {
    if (!readable(part, field::bodyPart::pose + 48)) return false;
    const float* pose = reinterpret_cast<const float*>(part + field::bodyPart::pose);
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(pose[i])) return false;
    for (unsigned j = 0; j < xd::handJointCount; ++j) {
        const XrVector3f d = {world.position[j].x - pose[9], world.position[j].y - pose[10], world.position[j].z - pose[11]};
        out[j] = {d.x * pose[0] + d.y * pose[1] + d.z * pose[2], d.x * pose[3] + d.y * pose[4] + d.z * pose[5], d.x * pose[6] + d.y * pose[7] + d.z * pose[8]};
    }
    return true;
}
// SEH-only frame: the mesh and plan are built by the caller.
static bool trackedHandEmit(const xd::HandMesh& mesh, WearPlan& plan, bool bind) {
    bool drawn = false;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (cueDeletesInFlight || cueRetiring.load() || cueBindingFailed.load() || InterlockedCompareExchange(&stopping, 0, 0) ||
            !ownedNativeSwap(cueCurrentDC())) return false;
        WearGl api{};
        if (!wearGlLoader(api)) return false;
        if (bind) wearBind(plan.material + 8);
        auto* owner = prepareCue();
        if (!owner) return false;
        auto& g = owner->api;
        if (bind) g.bindBuffer(glc::elementArrayBuffer, plan.buffer);
        WearGlState state{};
        plan.stable = true; // the program and its uniforms are needed; the head mount is not
        if (!captureWearGl(plan, api, state, false)) return false;
        auto& res = owner->resources;
        if (!res.ready() && !res.prepare(g)) return false;
        if (!res.handVbo) g.genBuffers(1, &res.handVbo);
        if (!res.handEbo) g.genBuffers(1, &res.handEbo);
        if (!res.handVbo || !res.handEbo || glGetError() != GL_NO_ERROR) return false;
        __try {
            g.bindBuffer(glc::arrayBuffer, res.handVbo);
            g.bufferData(glc::arrayBuffer, static_cast<ptrdiff_t>(mesh.vertices.size() * sizeof(xd::HandVertex)), mesh.vertices.data(), glc::dynamicDraw);
            g.bindBuffer(glc::elementArrayBuffer, res.handEbo);
            g.bufferData(glc::elementArrayBuffer, static_cast<ptrdiff_t>(mesh.indices.size() * sizeof(uint32_t)), mesh.indices.data(), glc::dynamicDraw);
            if (glGetError() != GL_NO_ERROR) return false;
            mountWearAttributes(g, plan, res.handVbo);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indices.size()), GL_UNSIGNED_INT, nullptr);
            drawn = glGetError() == GL_NO_ERROR;
        } __finally {
            g.bindVertexArray(static_cast<GLuint>(state.vao));
            restoreWearAttributes(g, state);
            g.bindBuffer(glc::arrayBuffer, static_cast<GLuint>(state.arrayBuffer));
            g.bindBuffer(glc::elementArrayBuffer, static_cast<GLuint>(state.elementBuffer));
            if (state.cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
        }
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return drawn;
}
// The game's own hand (or glove) mesh for this draw, read from the node's streams and bent by the
// joints; empty when the streams are not what the body uses, so the caller falls back to our mesh.
static std::atomic<unsigned> trackedHandBends{0};
static std::atomic<unsigned> trackedHandSkinnedDrops{0}; // hand draws left to the skinned game hand the bones bend
static std::atomic<unsigned> trackedGloveBends{0};       // glove pieces drawn bent by the joints over the bent skin
static xd::HandMesh trackedNativeHandMesh(uintptr_t node, uintptr_t material, int lastVertex, const WearPlan& plan,
                                          const std::array<XrVector3f, xd::handJointCount>& local, const std::array<float, xd::handJointCount>& radius) {
    xd::HandMesh none{};
    WearAttribute positions{}, normals{};
    uintptr_t posIndices = 0, normIndices = 0;
    if (!wearAttribute(node, 1, positions) || (positions.format & 0xffffff) != 0xf30001 || !positions.count || positions.count > fp::wear::maxVertices ||
        !wearStream(node, positions, static_cast<unsigned>(lastVertex + 1), posIndices) || !readable(positions.data, size_t(positions.count) * 12)) return none;
    const bool hasNormals = wearAttribute(node, 2, normals) && (normals.format & 0xffffff) == 0xf30002 && normals.count && normals.count <= fp::wear::maxVertices &&
                            wearStream(node, normals, static_cast<unsigned>(lastVertex + 1), normIndices) && readable(normals.data, size_t(normals.count) * 12);
    const int firstVertex = *reinterpret_cast<int*>(material + 0xb8);
    if (firstVertex < 0 || plan.vertices == 0 || firstVertex + int(plan.vertices) - 1 > lastVertex) return none;
    const uintptr_t triangles = *reinterpret_cast<uintptr_t*>(node + 0x290);
    if (!readable(triangles, (size_t(plan.first) + plan.count) * 12)) return none;
    std::vector<float> pos(size_t(plan.vertices) * 3), nor(hasNormals ? size_t(plan.vertices) * 3 : 0);
    for (unsigned v = 0; v < plan.vertices; ++v) {
        const unsigned p = *reinterpret_cast<unsigned*>(posIndices + size_t(firstVertex + int(v)) * 4);
        if (p >= positions.count) return none;
        memcpy(&pos[size_t(v) * 3], reinterpret_cast<void*>(positions.data + size_t(p) * 12), 12);
        if (hasNormals) {
            const unsigned q = *reinterpret_cast<unsigned*>(normIndices + size_t(firstVertex + int(v)) * 4);
            if (q >= normals.count) return none;
            memcpy(&nor[size_t(v) * 3], reinterpret_cast<void*>(normals.data + size_t(q) * 12), 12);
        }
    }
    std::vector<uint32_t> tris(size_t(plan.count) * 3);
    for (unsigned f = 0; f < plan.count; ++f)
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned i = *reinterpret_cast<unsigned*>(triangles + (size_t(plan.first) + f) * 12 + c * 4);
            if (i < unsigned(firstVertex) || i >= unsigned(firstVertex) + plan.vertices) return none;
            tris[size_t(f) * 3 + c] = i - unsigned(firstVertex);
        }
    const xd::HandSource src{pos.data(), hasNormals ? nor.data() : nullptr, tris.data(), plan.vertices, plan.count};
    xd::HandMesh mesh = xd::bendHand(src, local, radius, xd::allHandJoints);
    if (!mesh.vertices.empty()) trackedHandBends.fetch_add(1, std::memory_order_relaxed);
    return mesh;
}
static bool trackedHandDrawBody(uintptr_t node, uintptr_t material, bool bind) {
    if (!trackedHandsWanted() || !wearScope.colourPass || viewMode.load() != 0 || !supersampleViewMatches()) return false;
    const uintptr_t root = savedRoot;
    unsigned side = 0;
    if (!root || !trackedHandNode(root, node, side)) return false;
    TrackedDriveFrame frame{};
    if (!trackedDriveRead(root, frame) || !frame.handJoints[side].valid) return false;
    { // the game's own skinned hand, bent by the finger bones, is the hand shown: the hand part's own draw
      // is dropped. A glove or gauntlet piece is a rigid mesh the bones cannot bend, so it is drawn here
      // from its own streams bent by the same joints (below), over the skin the bones bend beneath it.
      // When the bones fault their snapshot empties and the mod's hand is back.
        const PublishedBones bones = trackedBoneSnapshot();
        if (bones.count && bones.root == root) {
            const uintptr_t handPart = trackedHandPart(root, side);
            const uintptr_t own = readable(handPart, field::bodyPart::meshNode + 8) ? *reinterpret_cast<uintptr_t*>(handPart + field::bodyPart::meshNode) : 0;
            if (node == own || !vrHandMesh.load(std::memory_order_relaxed)) {
                trackedHandSkinnedDrops.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            trackedGloveBends.fetch_add(1, std::memory_order_relaxed); // a glove piece: on to the bent native mesh
        }
    }
    const uintptr_t part = trackedHandPart(root, side);
    // One hand per presented frame: further pieces of a gauntlet are hidden, not drawn again.
    static thread_local std::array<LONG, 2> drawnFrame{{-1, -1}};
    const LONG thisFrame = InterlockedCompareExchange(&swapCalls, 0, 0);
    if (drawnFrame[side] == thisFrame) return true;
    std::array<XrVector3f, xd::handJointCount> local{};
    if (!trackedHandLocal(part, frame.handJoints[side], local)) return false;
    const uintptr_t own = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (node != own && *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) == own) { // a glove hangs under the hand node with its own local frame
        const float* t = reinterpret_cast<const float*>(node + field::sceneNode::localTransform);
        for (unsigned i = 0; i < 12; ++i)
            if (!std::isfinite(t[i])) return false;
        for (auto& p : local) {
            const XrVector3f d = {p.x - t[9], p.y - t[10], p.z - t[11]};
            p = {d.x * t[0] + d.y * t[1] + d.z * t[2], d.x * t[3] + d.y * t[4] + d.z * t[5], d.x * t[6] + d.y * t[7] + d.z * t[8]};
        }
    }
    if (!readable(node, 0x2a0) || !readable(material, 0x108)) return false;
    const int lastFace = *reinterpret_cast<int*>(node + 0x268), lastVertex = *reinterpret_cast<int*>(node + 0x288);
    if (lastFace < 0 || lastFace >= static_cast<int>(fp::wear::maxFaces) || lastVertex < 0 || lastVertex >= static_cast<int>(fp::wear::maxVertices)) return false;
    WearPlan plan{};
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan) || !ownedNativeSwap(cueCurrentDC())) return false;
    // The game's hand mesh bent by the joints when the row asks for it; our own mesh otherwise or as the fallback.
    xd::HandMesh mesh;
    if (vrHandMesh.load(std::memory_order_relaxed)) mesh = trackedNativeHandMesh(node, material, lastVertex, plan, local, frame.handJoints[side].radius);
    if (mesh.vertices.empty()) mesh = xd::handMesh(local, frame.handJoints[side].radius, xd::allHandJoints);
    if (mesh.vertices.empty()) return false;
    const bool drawn = trackedHandEmit(mesh, plan, bind);
    if (drawn) drawnFrame[side] = thisFrame;
    if (drawn) trackedHandDraws.fetch_add(1, std::memory_order_relaxed);
    return drawn; // true: the native hand draw is skipped, so the mesh is hidden while ours shows
}
static bool trackedHandDrawGuarded(uintptr_t node, uintptr_t material, bool bind) {
    try { return trackedHandDrawBody(node, material, bind); }
    catch (const std::exception&) { trackedHandFaults.fetch_add(1, std::memory_order_relaxed); return false; }
}
static bool trackedHandDraw(uintptr_t node, uintptr_t material, bool bind) {
    __try { return trackedHandDrawGuarded(node, material, bind); }
    __except (EXCEPTION_EXECUTE_HANDLER) { trackedHandFaults.fetch_add(1, std::memory_order_relaxed); return false; }
}
// The submission gate's question: is this one of the player's hand draws while VR hands are on?
static bool trackedHandSubmission(uintptr_t node) {
    unsigned side = 0;
    return trackedHandsWanted() && savedRoot && trackedHandNode(savedRoot, node, side);
}
// Hands only: the player's own body below the head (playerBodyNode, view.inl) is not drawn while
// the headset drives it; held items and the hand draws themselves stay native.
static std::atomic<unsigned> trackedBodyHides{0};
static bool trackedBodySubmission(uintptr_t node) {
    return vrBodyHandsOnly.load(std::memory_order_relaxed) && savedRoot && playerBodyNode(savedRoot, node);
}
// Hands only with the finger bones on the skinned body: the body draw is cut down to the faces whose
// three vertices are carried by the hands (the hand nodes' bones and the finger bones), so the game's
// own skinned hands show, bent by the bones, and the rest of the body stays dropped. The face kinds
// are built from the skin's weight stream, as the headwear hide builds its own.
static std::atomic<unsigned> trackedBodyHandDraws{0};
static bool trackedBodyHandFacesEmit(const WearPlan& plan, bool bind, const unsigned char* keep) { // SEH-only frame
    if (!ownedNativeSwap(cueCurrentDC()) || !supersampleViewMatches() || viewMode.load() != 0 ||
        !InterlockedCompareExchange(&active, 0, 0) || !InterlockedCompareExchange(&cameraReady, 0, 0)) return false;
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
        if (!ownedNativeSwap(cueCurrentDC()) || GLuint(elementBuffer) != plan.buffer || glGetError() != GL_NO_ERROR) return false;
        for (unsigned f = 0; f < plan.count;) {
            unsigned end = f + 1;
            while (end < plan.count && keep[end] == keep[f]) ++end;
            if (keep[f]) wearDrawRange(plan, f, end - f);
            f = end;
        }
        emitted = true;
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return emitted;
}
static bool trackedBodyHandFaces(uintptr_t node, uintptr_t material, bool bind) {
    const PublishedBones bones = trackedBoneSnapshot();
    if (!bones.count || bones.root != savedRoot || bones.skinNode != node || !readable(node, 0x2a0) || !readable(material, 0x108)) return false;
    const int lastFace = *reinterpret_cast<int*>(node + 0x268), lastVertex = *reinterpret_cast<int*>(node + 0x288);
    if (lastFace < 0 || lastFace >= int(fp::wear::maxFaces) || lastVertex < 0 || lastVertex >= int(fp::wear::maxVertices)) return false;
    WearPlan plan{};
    if (!wearMeshFormat(node, material, lastFace, lastVertex, plan)) return false;
    WearAttribute positions{}, weights{};
    uintptr_t posIndices = 0;
    if (!wearAttribute(node, 1, positions) || (positions.format & 0xffffff) != 0xf30001 || !positions.count || positions.count > fp::wear::maxVertices ||
        !wearStream(node, positions, static_cast<unsigned>(lastVertex + 1), posIndices) ||
        !wearAttribute(node, 0x1000, weights) || (weights.format & 0xffffff) != 0xa41000 || weights.count != positions.count ||
        !readable(weights.data, size_t(weights.count) * sizeof(fp::wear::Weights))) return false;
    const uintptr_t triangles = *reinterpret_cast<uintptr_t*>(node + 0x290);
    if (!readable(triangles, (size_t(plan.first) + plan.count) * 12)) return false;
    static thread_local std::vector<unsigned char> keep;
    keep.assign(plan.count, 0);
    unsigned kept = 0;
    for (unsigned f = 0; f < plan.count; ++f) {
        const unsigned* tri = reinterpret_cast<const unsigned*>(triangles + (size_t(plan.first) + f) * 12);
        bool onHands = true;
        for (unsigned c = 0; c < 3 && onHands; ++c) {
            if (tri[c] > unsigned(lastVertex)) return false;
            const unsigned p = *reinterpret_cast<unsigned*>(posIndices + size_t(tri[c]) * 4);
            if (p >= weights.count) return false;
            const auto& w = *reinterpret_cast<const fp::wear::Weights*>(weights.data + size_t(p) * sizeof(fp::wear::Weights));
            float share = 0;
            for (unsigned k = 0; k < 4; ++k) {
                if (!std::isfinite(w.weight[k]) || w.weight[k] <= 0) continue;
                for (unsigned i = 0; i < bones.count; ++i)
                    if (bones.ids[i] == w.bone[k]) { share += w.weight[k]; break; }
            }
            onHands = share >= .999f;
        }
        keep[f] = onHands ? 1 : 0;
        kept += onHands;
    }
    if (!kept) return false;
    if (!trackedBodyHandFacesEmit(plan, bind, keep.data())) return false;
    trackedBodyHandDraws.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static bool trackedBodyHandFacesGuarded(uintptr_t node, uintptr_t material, bool bind) {
    try { return trackedBodyHandFaces(node, material, bind); }
    catch (const std::exception&) { trackedHandFaults.fetch_add(1, std::memory_order_relaxed); return false; }
}
static bool trackedBodyHidden(uintptr_t node, uintptr_t material, bool bind) {
    if (!vrBodyHandsOnly.load(std::memory_order_relaxed) || !wearScope.colourPass || viewMode.load() != 0 || !supersampleViewMatches()) return false;
    const uintptr_t root = savedRoot;
    unsigned side = 0;
    if (!root || trackedHandNode(root, node, side) || !playerBodyNode(root, node)) return false;
    TrackedDriveFrame frame{};
    if (!trackedDriveRead(root, frame)) return false; // the desktop player keeps their body
    bool drawn = false;
    __try { drawn = trackedBodyHandFacesGuarded(node, material, bind); }
    __except (EXCEPTION_EXECUTE_HANDLER) { trackedHandFaults.fetch_add(1, std::memory_order_relaxed); }
    if (!drawn) trackedBodyHides.fetch_add(1, std::memory_order_relaxed);
    return true; // either way the native body draw is taken: dropped, or replaced by its hand faces
}
