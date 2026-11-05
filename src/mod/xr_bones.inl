// Finger bones: real bones on the game's own skeleton, so the skinned body mesh (and anything else
// weighted to the hand) bends its fingers, in VR to the tracked joints and otherwise to the pose of
// what the hand holds.
//
// The game skins the body on the CPU from a bone list on the skin object hung off the _SKIN aggregate
// node (+0x218): node transform pointers (+0x18), inverse binds (+0x20, 0x30 each) and a float per bone
// (+0x28), indexed by the 1-based ids in the 0xa41000 weight stream. A hand's vertices are all weighted
// to the hand node. We create one mesh node per finger segment with the game's own constructor, hang it
// on the hand node, register it as a bone (D39B0), give it the hand's inverse bind so it coincides with
// the hand at rest, and reweight the hand's vertices to the nearest rest segments. Each tick the bone's
// local transform carries its rest segment to the posed one and the game's own propagation updates the
// world transform the kernel reads. Nothing is ever freed: a node the skin may still point at stays
// attached, at rest, until the character goes away with it. All of it runs on the game thread from the
// input hook; every native call sits in its own SEH frame. Experimental, off by default.
//
// Two drivers share the bones: the VR drive (xr_bones_vr.inl, tracked or inferred joints in the hand's
// frame) and, in either build, the held-item poses the VR hands already use (hilt, hold with the item's
// radius, flat on a shield, open), laid out by the same joint model. The hand part's frame is the grip
// frame the joint model is built in, so its joints need no mapping beyond the game's units.
#include "../xr/hand_bend.h"
namespace xd = fp::xr;
static std::atomic<unsigned> trackedBoneCount{0}, trackedBoneRewrites{0}, trackedBoneFaults{0}, fingerPoseTicks{0};
static std::atomic<bool> trackedBoneFault{false};
static uintptr_t trackedHandPart(uintptr_t root, unsigned side) { return *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8); }
// Where the hand holds an item, from the item's own mesh: along its long axis the cross-section is
// measured in bins, and the narrowest bin within a hand's reach of the hand's origin is the grip (a
// hilt below a guard, a haft, a handle). Gives the grip point in the hand part's frame and the
// item's radius there. The cross-section profile is cached by immutable vertex-stream geometry.
struct HeldGripProfile {
    static constexpr unsigned bins = 24;
    uintptr_t data = 0;
    unsigned count = 0;
    unsigned char axis = 0;
    bool valid = false;
    XrVector3f mean{};
    float lo = 0, hi = 0;
    float width[bins]{};
    unsigned fill[bins]{};
};
static std::array<HeldGripProfile, 128> heldGripCache{};
static uintptr_t heldItemNode(uintptr_t root, unsigned side);
static bool heldItemGrip(uintptr_t root, unsigned side, XrVector3f& point, float& radiusMetres) {
    const uintptr_t node = heldItemNode(root, side);
    if (!node || !readable(node, 0x2a0)) return false;
    const float* e = reinterpret_cast<const float*>(node + 0x30);
    const float* local = reinterpret_cast<const float*>(node + field::sceneNode::localTransform);
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(e[i]) || e[i] < 0) return false;
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(local[i])) return false;
    WearAttribute positions{};
    if (!wearAttribute(node, 1, positions) || (positions.format & 0xffffff) != 0xf30001 || positions.count < 8 || positions.count > fp::wear::maxVertices ||
        !readable(positions.data, size_t(positions.count) * 12)) return false;
    const unsigned L = e[0] >= e[1] && e[0] >= e[2] ? 0 : e[1] >= e[2] ? 1 : 2;
    const size_t setBase = (((positions.data >> 6) ^ (size_t(positions.count) * 131u) ^ size_t(L)) & 31u) * 4u;
    HeldGripProfile* slot = nullptr;
    for (size_t way = 0; way < 4; ++way) {
        auto& c = heldGripCache[setBase + way];
        if (c.data == positions.data && c.count == positions.count && c.axis == L) {
            slot = &c;
            break;
        }
        if (!c.data && !slot) slot = &c;
    }
    if (!slot) slot = &heldGripCache[setBase + (((positions.data >> 4) ^ size_t(positions.count)) & 3u)];
    if (slot->data != positions.data || slot->count != positions.count || slot->axis != L) {
        *slot = {};
        slot->data = positions.data;
        slot->count = positions.count;
        slot->axis = static_cast<unsigned char>(L);
        const float* v = reinterpret_cast<const float*>(positions.data);
        XrVector3f mean{};
        float lo = 1e9f, hi = -1e9f;
        bool finiteAll = true;
        for (unsigned i = 0; i < positions.count; ++i) {
            const XrVector3f p = {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]};
            if (!xd::finite(p)) { finiteAll = false; break; }
            mean = xd::add(mean, p);
            const float a = (&p.x)[L];
            lo = (std::min)(lo, a);
            hi = (std::max)(hi, a);
        }
        if (finiteAll && (hi - lo > 2.f)) {
            mean = xd::scale(mean, 1.f / float(positions.count));
            for (unsigned i = 0; i < positions.count; ++i) {
                const XrVector3f p = {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]};
                const unsigned b = (std::min)(HeldGripProfile::bins - 1, unsigned(((&p.x)[L] - lo) / (hi - lo) * float(HeldGripProfile::bins)));
                XrVector3f d = xd::sub(p, mean);
                (&d.x)[L] = 0;
                slot->width[b] = (std::max)(slot->width[b], xd::length(d));
                ++slot->fill[b];
            }
            slot->mean = mean;
            slot->lo = lo;
            slot->hi = hi;
            slot->valid = true;
        }
    }
    if (!slot->valid) return false;
    // the hand's origin in the item's frame: p_hand = p_local . R + t, so p_local = (p_hand - t) . rows
    const XrVector3f t = {local[9], local[10], local[11]};
    XrVector3f handLocal{};
    for (unsigned i = 0; i < 3; ++i) (&handLocal.x)[i] = -(t.x * local[i * 3] + t.y * local[i * 3 + 1] + t.z * local[i * 3 + 2]);
    const float handAlong = (&handLocal.x)[L], reach = 12.f; // a hand's span along the item
    int best = -1;
    for (unsigned b = 0; b < HeldGripProfile::bins; ++b) {
        const float centre = slot->lo + (float(b) + .5f) / float(HeldGripProfile::bins) * (slot->hi - slot->lo);
        if (slot->fill[b] < 3 || std::abs(centre - handAlong) > reach) continue;
        if (best < 0 || slot->width[b] < slot->width[size_t(best)]) best = int(b);
    }
    if (best < 0) return false;
    XrVector3f grip = slot->mean;
    (&grip.x)[L] = slot->lo + (float(best) + .5f) / float(HeldGripProfile::bins) * (slot->hi - slot->lo);
    const XrVector3f handPoint = xd::add({grip.x * local[0] + grip.y * local[3] + grip.z * local[6], grip.x * local[1] + grip.y * local[4] + grip.z * local[7], grip.x * local[2] + grip.y * local[5] + grip.z * local[8]}, t);
    const float units = static_cast<float>(vrUnitsPerMetre.load(std::memory_order_relaxed));
    if (!xd::finite(handPoint) || !(units > 0)) return false;
    point = handPoint;
    radiusMetres = (std::min)(.08f, (std::max)(.008f, slot->width[size_t(best)] / units));
    return true;
}
static float heldRadiusFromExtents(uintptr_t root, unsigned side) {
    if (!root || !readable(root, 0x1300)) return 0;
    const uintptr_t hand = trackedHandPart(root, side);
    uintptr_t part = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8);
    const int weaponCount = *reinterpret_cast<int*>(root + field::character::weaponCount);
    if (!part && weaponCount > 0) {
        for (int w = 0; w < (std::min)(weaponCount, 2); ++w) {
            const uintptr_t weapon = *reinterpret_cast<uintptr_t*>(root + field::character::weapons + w * 8);
            if (readable(weapon, field::bodyPart::parentPart + 8) && *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::parentPart) == hand) { part = weapon; break; }
        }
    }
    if (!part && weaponCount > 0) part = *reinterpret_cast<uintptr_t*>(root + field::character::weapons);
    if (!part || !readable(part, field::bodyPart::meshNode + 8)) return 0;
    const uintptr_t node = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!readable(node, 0x40)) return 0;
    float e[3] = {*reinterpret_cast<float*>(node + 0x30), *reinterpret_cast<float*>(node + 0x34), *reinterpret_cast<float*>(node + 0x38)}; // half extents, native units
    for (float& v : e) { if (!std::isfinite(v) || v < 0) return 0; }
    std::sort(e, e + 3);
    const float radius = std::sqrt(e[0] * e[1]) / static_cast<float>(vrUnitsPerMetre.load(std::memory_order_relaxed));
    return (std::min)(.08f, (std::max)(.008f, radius));
}
// The radius (metres) of what the hand holds: at its grip when the mesh gives one, else from its half extents.
[[maybe_unused]] static float heldRadius(uintptr_t root, unsigned side) {
    XrVector3f grip{};
    float gripRadius = 0;
    if (heldItemGrip(root, side, grip, gripRadius)) return gripRadius;
    return heldRadiusFromExtents(root, side);
}
// The pose of what the hand holds: flat under a shield, the hilt of a wielded weapon, a hold on a carried item, else open.
static xd::HandPose heldHandPose(uintptr_t root, unsigned side) {
    if (!root || !readable(root, 0x1300)) return xd::HandPose::open;
    const uintptr_t hand = trackedHandPart(root, side);
    const uintptr_t shield = *reinterpret_cast<uintptr_t*>(root + field::character::shieldPart);
    if (shield && readable(shield, field::bodyPart::parentPart + 8) && (*reinterpret_cast<uintptr_t*>(shield + field::bodyPart::parentPart) == hand || side == 0)) return xd::HandPose::flat;
    const int weaponCount = *reinterpret_cast<int*>(root + field::character::weaponCount);
    for (int w = 0; w < (std::min)(weaponCount, 2); ++w) {
        const uintptr_t weapon = *reinterpret_cast<uintptr_t*>(root + field::character::weapons + w * 8);
        if (readable(weapon, field::bodyPart::parentPart + 8) && *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::parentPart) == hand) return xd::HandPose::hilt;
    }
    const uintptr_t held = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8);
    if (held) {
        for (int w = 0; w < (std::min)(weaponCount, 2); ++w) {
            if (held == *reinterpret_cast<uintptr_t*>(root + field::character::weapons + w * 8)) return xd::HandPose::hilt;
        }
        return xd::HandPose::hold;
    }
    // Two-handed secondary hand check: if weapon is wielded with 2 hands, the other hand grips the hilt too when not holding a shield
    if (weaponCount > 0 && !shield) {
        if (readable(root, field::character::gripFlags + 4)) {
            const unsigned grip = *reinterpret_cast<const unsigned*>(root + field::character::gripFlags);
            if (grip == 0x20 || grip == 0x30) return xd::HandPose::hilt;
        }
    }
    // Combat stance without weapons: clenched fist; outside combat stance: relaxed open
    if (readable(root, field::character::combatStance + 1) &&
        *reinterpret_cast<const unsigned char*>(root + field::character::combatStance) != 0) {
        return xd::HandPose::fist;
    }
    return xd::HandPose::open;
}
namespace fingerbones {
constexpr unsigned perHand = xd::chainSegments; // three segments a finger (fingerChain, hand_bend.h)
constexpr const char* nodeNamePrefix = "_VRBONE";
using Joints = std::array<XrVector3f, xd::handJointCount>;
struct Bone {
    uintptr_t node = 0;
    XrVector3f restA{}, restB{};
    unsigned finger = 0;
};
struct Hand {
    uintptr_t handNode = 0;  // the part's mesh node the bones hang on
    uintptr_t skinNode = 0;  // the _SKIN aggregate whose weights were last rewritten; 0 when none
    uintptr_t weightData = 0; // its weight stream, to restore into
    unsigned created = 0;     // bones made so far
    std::array<int, 4> pieceIds{}; // bone ids of rigid pieces (gloves) merged into the skin under this hand
    unsigned pieceCount = 0;
    bool fitted = false;      // the rest skeleton has been fitted to the mesh's fingers
    bool heldPoseValid = false;
    xd::HandPose lastPose = xd::HandPose::open;
    uintptr_t lastHeldNode = 0;
    float lastCurl[5]{};
    float lastRadius = -1.f, lastUnits = -1.f;
    bool lastHasAxis = false, lastHasGrip = false;
    XrVector3f lastAxis{}, lastGrip{};
    std::array<Bone, perHand> bones{};
    std::vector<std::pair<unsigned, fp::wear::Weights>> saved; // the vertices we rewrote, with their native weights
};
struct Skin {
    uintptr_t node = 0, skin = 0, boneNodes = 0, inverseBind = 0, extra = 0, source = 0;
    int bones = 0;
    WearAttribute weights{};
};
// p · [rows | origin] in the game's 12-float layout: three axis rows then the origin.
inline XrVector3f apply12(const float* m, XrVector3f p) {
    return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10], p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
}
// The joint model's hand in the hand part's frame and the game's units: the grip at the origin.
inline bool modelJoints(xd::HandPose pose, bool right, float itemRadius, float units, Joints& out, float& reach) {
    const xd::HandJoints joints = xd::inferJoints({{0, 0, 0, 1}, {}}, pose, right, itemRadius);
    if ((joints.validMask & xd::allHandJoints) != xd::allHandJoints || !std::isfinite(units) || units <= 0) return false;
    for (unsigned j = 0; j < xd::handJointCount; ++j) {
        out[j] = xd::scale(joints.pose[j].position, units);
        if (!xd::finite(out[j])) return false;
    }
    reach = (std::max)(joints.radius[xd::indexTipJoint] * units, .004f) * 2.5f;
    return true;
}
} // namespace fingerbones
// One character's bones: the player's or any character the world lists. A root that leaves the list
// is dropped without a touch (its tree is going with it); nothing of ours is freed.
namespace fingerbones {
struct Root {
    uintptr_t root = 0;
    std::array<Hand, 2> hands{};
};
constexpr unsigned maxRoots = 48; // characters driven per tick, in the world's own order
} // namespace fingerbones
static std::vector<fingerbones::Root> trackedBoneTable;
// What the render thread may know of the player's bones: the skin they were rewritten on and the ids
// (the hand nodes' own and the finger bones') the hands' vertices are weighted to. Written after each
// tick, read under the lock by the hands-only body draw.
struct PublishedBones {
    uintptr_t root = 0, skinNode = 0;
    unsigned count = 0;
    std::array<int, 2 + 2 * fingerbones::perHand + 8> ids{};
};
static SRWLOCK trackedBonePublishLock = SRWLOCK_INIT;
static PublishedBones trackedBonePublished;
static std::atomic<unsigned> trackedBoneRootsDriven{0};
static bool trackedBoneDrivenByVr = false; // set by the VR drive's tick, read and cleared by the held-pose tick after it
// The bones are plain scene nodes (T3DNode, 0x1C8 bytes): the mesh class's prepare (TMesh +D0, 5F860) logs
// "Mesh has no faces" for a mesh without geometry, the plain node's (+D0, 511B0) only sets a flag. The bone
// register D39B0 reads +300 of a node it has not registered yet, past a plain node's end; that word only picks
// whether it first rebuilds the node's world from its parent, harmless either way when the page past +1C8 is
// mapped. Any T3DNode allocated within 0x308 bytes of an unmapped page boundary is parked unused and replaced.
static constexpr size_t defaultNodeSize = 0x1C8;
static constexpr size_t minBoneRegisterSpan = 0x308;
static size_t runtimeNodeSize() noexcept {
    if (!layout.rva[fp::sig::nodeVmt]) return defaultNodeSize;
    const uintptr_t vmt = gameAt(fp::sig::nodeVmt);
    if (!readable(vmt, 16)) return defaultNodeSize;
    uint32_t instSize = 0;
    if (!fp::rtti::readVmtInstanceSize(reinterpret_cast<const uint8_t*>(vmt), 16, instSize)) return defaultNodeSize;
    if (instSize < 0x180 || instSize > 0x1000) return defaultNodeSize;
    return static_cast<size_t>(instSize);
}
static size_t runtimeBoneRegisterSpan() noexcept {
    return (std::max)(minBoneRegisterSpan, runtimeNodeSize());
}
static uintptr_t parkedEdgeNodes[16]{};
static unsigned parkedEdgeNodeCount = 0;
// Rigid hand pieces into the skin. The skin build (D9940, equipment refresh) merges into the _SKIN aggregate every
// mesh under the body tree whose +204 & 0xC00000 is 0x400000, each as its own bone; the refresh sets that bit from
// the model's own bit 0x40 (E7550), which rigid gloves and gauntlets lack. Marking those pieces just before the build
// lets the game merge them itself (D6080 reads only the general mesh streams: faces, attributes, triangles), and
// the finger-bone weight rewrite then carries their vertices with the fingers like the bare hand's. A piece is a mesh
// whose name carries a hand's part code (17.., 18..), hung on a hand node the build merges, with faces of its own.
static std::atomic<unsigned> skinMergedPieces{0};
static std::atomic<bool> skinMergeFault{false};
using SkinBuild = bool (*)(uintptr_t, uintptr_t, uintptr_t);
static SkinBuild originalSkinBuild = nullptr;
static constexpr unsigned skinnedBit = 0x400000, skinMask = 0xC00000;
// The pieces this build marked and their flags before it, so a piece the aggregate refuses is put back.
struct SkinMarked { uintptr_t node; unsigned flags; };
static thread_local std::array<SkinMarked, 16> skinMarked{};
static thread_local unsigned skinMarkedCount = 0;
static bool skinPiece(uintptr_t node) {
    if (!readable(node, 0x2a0) || (*reinterpret_cast<unsigned*>(node + 8) & 0xFFF) != 0x89 || *reinterpret_cast<int*>(node + 0x268) < 0) return false;
    const char* name = reinterpret_cast<const char*>(node + field::sceneNode::name);
    return name[0] == '1' && (name[1] == '7' || name[1] == '8');
}
static void skinMarkPieces(uintptr_t node, unsigned depth) {
    if (depth > 24 || !readable(node, 0x2a0)) return;
    const int last = *reinterpret_cast<int*>(node + field::sceneNode::lastChild);
    const uintptr_t children = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::children);
    if (last < 0 || last > 512 || !readable(children, (size_t(last) + 1) * 8)) return;
    const bool merged = (*reinterpret_cast<unsigned*>(node + 8) & 0xFFF) == 0x89 && (*reinterpret_cast<unsigned*>(node + 0x204) & skinMask) == skinnedBit;
    for (int k = 0; k <= last; ++k) {
        const uintptr_t child = *reinterpret_cast<uintptr_t*>(children + size_t(k) * 8);
        if (!child || !readable(child, 0x2a0)) continue;
        unsigned& flags = *reinterpret_cast<unsigned*>(child + 0x204);
        if (merged && skinPiece(child) && (flags & skinMask) != skinnedBit && skinMarkedCount < skinMarked.size()) {
            // a rigid hand piece the build would leave out
            skinMarked[skinMarkedCount++] = {child, flags};
            flags = (flags & ~skinMask) | skinnedBit;
        }
        skinMarkPieces(child, depth + 1);
    }
}
static bool skinMarkGuarded(uintptr_t tree) { // SEH-only frame
    __try { skinMarkPieces(tree, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { skinMergeFault = true; return false; }
}
// After the build: a marked piece that did not become a bone of the new skin (the aggregate refused it) gets
// its flags back, so it is not hidden as skinned and draws as the rigid piece it was.
static void skinSettleMarks(bool built, uintptr_t skinOut) {
    __try {
        const uintptr_t skin = built && readable(skinOut, 8) ? *reinterpret_cast<uintptr_t*>(skinOut) : 0;
        const int bones = skin && readable(skin, 0x20) ? *reinterpret_cast<int*>(skin + 0x10) : 0;
        const uintptr_t boneNodes = bones > 0 ? *reinterpret_cast<uintptr_t*>(skin + 0x18) : 0;
        const bool listed = bones > 0 && bones <= int(fp::wear::maxBones) && readable(boneNodes, size_t(bones) * 8);
        for (unsigned k = 0; k < skinMarkedCount; ++k) {
            bool merged = false;
            for (int b = 0; listed && b < bones && !merged; ++b)
                merged = *reinterpret_cast<uintptr_t*>(boneNodes + size_t(b) * 8) == skinMarked[k].node + field::sceneNode::transform;
            if (merged) skinMergedPieces.fetch_add(1, std::memory_order_relaxed);
            else if (readable(skinMarked[k].node, 0x208)) *reinterpret_cast<unsigned*>(skinMarked[k].node + 0x204) = skinMarked[k].flags;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { skinMergeFault = true; }
    skinMarkedCount = 0;
}
static bool skinBuildHook(uintptr_t tree, uintptr_t skinOut, uintptr_t data) {
    skinMarkedCount = 0;
    if (!skinMergeFault.load() && (fingerPoses.load(std::memory_order_relaxed) != 0 || vrFingerBones.load(std::memory_order_relaxed) != 0))
        skinMarkGuarded(tree);
    const bool built = originalSkinBuild(tree, skinOut, data);
    if (skinMarkedCount) skinSettleMarks(built, skinOut);
    return built;
}
static bool createSkinMergeHook() {
    if (!layout.optionalGroupAvailable("fingerBones")) return true;
    return MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::skinBuild)), reinterpret_cast<void*>(skinBuildHook), reinterpret_cast<void**>(&originalSkinBuild)) == MH_OK;
}
// A merged hand piece's bone id (its +17C) when it hangs on `handNode`, else 0.
static int skinPieceId(uintptr_t node, uintptr_t handNode) {
    if (!skinPiece(node) || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) != handNode) return 0;
    if ((*reinterpret_cast<unsigned*>(node + 0x204) & skinMask) != skinnedBit) return 0;
    return *reinterpret_cast<int*>(node + 0x17c);
}
// SEH-only frames around the game's own routines.
static uintptr_t trackedBoneCtorRawCall() {
    __try {
        return reinterpret_cast<uintptr_t (*)(uintptr_t, intptr_t)>(gameAt(fp::sig::nodeCtor))(gameAt(fp::sig::nodeVmt), 1); // a plain scene node: no mesh, so no "Mesh has no faces"
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedBoneFault = true; return 0; }
}
static uintptr_t trackedBoneCtorCall() {
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        const uintptr_t node = trackedBoneCtorRawCall();
        if (!node) return 0;
        if (readable(node, runtimeBoneRegisterSpan())) return node;
        if (parkedEdgeNodeCount < std::size(parkedEdgeNodes)) parkedEdgeNodes[parkedEdgeNodeCount++] = node;
    }
    return 0;
}
static bool trackedBoneAttachCall(uintptr_t parent, uintptr_t child) {
    __try {
        reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(gameAt(fp::sig::attachChild))(parent, child);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedBoneFault = true; return false; }
}
static int trackedBoneRegisterCall(uintptr_t skin, uintptr_t node) {
    if (!node || !readable(node, runtimeBoneRegisterSpan())) return 0;
    __try {
        return reinterpret_cast<int (*)(uintptr_t, uintptr_t)>(gameAt(fp::sig::boneRegister))(skin, node);
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedBoneFault = true; return 0; }
}
static bool trackedBonePropagateCall(uintptr_t node) {
    __try {
        reinterpret_cast<void (*)(uintptr_t)>(gameAt(fp::sig::propagateNodePose))(node);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { trackedBoneFault = true; return false; }
}
// A node we made, still ours: the game's mesh class, hung on the hand node, carrying our name.
static bool trackedBoneNodeValid(uintptr_t node, uintptr_t handNode) {
    return node && readable(node, runtimeNodeSize()) && *reinterpret_cast<uintptr_t*>(node) == gameAt(fp::sig::nodeVmt) &&
           *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) == handNode &&
           !strncmp(reinterpret_cast<const char*>(node + field::sceneNode::name), fingerbones::nodeNamePrefix, 7);
}
// The character's skinned body: the _SKIN aggregate under the actor and its skin object, with the streams the rewrite needs.
static bool trackedBoneSkin(uintptr_t root, fingerbones::Skin& out) {
    if (!readable(root, field::character::sceneNode + 8)) return false;
    const uintptr_t actor = *reinterpret_cast<uintptr_t*>(root + field::character::sceneNode);
    if (!actor || !readable(actor, field::sceneNode::name + 16)) return false;
    const int last = *reinterpret_cast<int*>(actor + field::sceneNode::lastChild);
    const uintptr_t children = *reinterpret_cast<uintptr_t*>(actor + field::sceneNode::children);
    if (last < 0 || last > 4096 || !readable(children, (size_t(last) + 1) * 8)) return false;
    for (int k = 0; k <= last; ++k) {
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(children + size_t(k) * 8);
        if (!node || !readable(node, 0x2a0) || strncmp(nodeName(node), "_SKIN", 5)) continue;
        const uintptr_t skin = *reinterpret_cast<uintptr_t*>(node + 0x218);
        if (!readable(skin, 0x40) || *reinterpret_cast<uintptr_t*>(skin + 8) != node) return false;
        out.node = node;
        out.skin = skin;
        out.bones = *reinterpret_cast<int*>(skin + 0x10);
        out.boneNodes = *reinterpret_cast<uintptr_t*>(skin + 0x18);
        out.inverseBind = *reinterpret_cast<uintptr_t*>(skin + 0x20);
        out.extra = *reinterpret_cast<uintptr_t*>(skin + 0x28);
        out.source = *reinterpret_cast<uintptr_t*>(skin + 0x38);
        WearAttribute positions{};
        if (out.bones < 1 || out.bones > int(fp::wear::maxBones) || !readable(out.boneNodes, size_t(out.bones) * 8) ||
            !readable(out.inverseBind, size_t(out.bones) * 48) || !readable(out.extra, size_t(out.bones) * 4) ||
            !wearAttribute(node, 1, positions) || (positions.format & 0xffffff) != 0xf30001 || !positions.count || positions.count > fp::wear::maxVertices ||
            !wearAttribute(node, 0x1000, out.weights) || (out.weights.format & 0xffffff) != 0xa41000 || out.weights.count != positions.count ||
            !readable(out.weights.data, size_t(out.weights.count) * sizeof(fp::wear::Weights)) || !readable(out.source, size_t(positions.count) * 12)) return false;
        return true;
    }
    return false;
}
// The hand's own bone id (1-based) in this skin, from the transform pointer list; 0 when it is not a bone.
static int trackedBoneHandId(const fingerbones::Skin& skin, uintptr_t handNode) {
    for (int k = 0; k < skin.bones; ++k)
        if (*reinterpret_cast<uintptr_t*>(skin.boneNodes + size_t(k) * 8) == handNode + field::sceneNode::transform)
            return readable(handNode, 0x180) && *reinterpret_cast<int*>(handNode + 0x17c) == k + 1 ? k + 1 : 0;
    return 0;
}
static void trackedBoneRestoreWeights(fingerbones::Hand& hand) {
    if (hand.skinNode && hand.weightData && readable(hand.skinNode, 0x2a0) && !strncmp(nodeName(hand.skinNode), "_SKIN", 5)) {
        WearAttribute weights{};
        if (wearAttribute(hand.skinNode, 0x1000, weights) && (weights.format & 0xffffff) == 0xa41000 && weights.data == hand.weightData &&
            readable(weights.data, size_t(weights.count) * sizeof(fp::wear::Weights)))
            for (const auto& [v, w] : hand.saved)
                if (v < weights.count) *reinterpret_cast<fp::wear::Weights*>(weights.data + size_t(v) * sizeof(fp::wear::Weights)) = w;
    }
    hand.saved.clear();
    hand.skinNode = hand.weightData = 0;
}
// Bones back to rest (the hand's own frame) so whatever still reads them sees the hand.
static void trackedBoneRest(fingerbones::Hand& hand) {
    hand.heldPoseValid = false;
    for (unsigned b = 0; b < hand.created; ++b) {
        const uintptr_t node = hand.bones[b].node;
        if (!trackedBoneNodeValid(node, hand.handNode)) continue;
        float* local = reinterpret_cast<float*>(node + field::sceneNode::localTransform);
        static constexpr float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        memcpy(local, identity, sizeof identity);
        trackedBonePropagateCall(node);
    }
}
static void trackedBoneForget(fingerbones::Hand& hand, bool restore) {
    if (restore) { trackedBoneRestoreWeights(hand); trackedBoneRest(hand); }
    hand = fingerbones::Hand{};
}
// The rest skeleton from one pose: each finger straightened along its metacarpal, keeping the segment
// lengths; the same reading hand_bend.h takes of the game's straight-fingered mesh.
static void trackedBoneRestSkeleton(const fingerbones::Joints& joint, fingerbones::Hand& hand) {
    unsigned b = 0;
    for (unsigned finger = 0; finger < 5; ++finger) {
        const xd::FingerChain c = xd::fingerChain(finger);
        const XrVector3f dir = xd::chainRestDirection(joint, c);
        XrVector3f rest = joint[c.first];
        for (unsigned k = 0; k < c.segments && b < fingerbones::perHand; ++k, ++b) {
            const float len = xd::length(xd::sub(joint[c.first + k + 1], joint[c.first + k]));
            hand.bones[b].restA = rest;
            hand.bones[b].restB = xd::add(rest, xd::scale(dir, len));
            hand.bones[b].finger = finger;
            rest = hand.bones[b].restB;
        }
    }
}
// One bone node: made by the game, named, at the hand's frame, hung on the hand node, registered.
static bool trackedBoneCreate(fingerbones::Hand& hand, unsigned index, const fingerbones::Skin& skin) {
    const uintptr_t node = trackedBoneCtorCall();
    if (!node || !readable(node, runtimeBoneRegisterSpan()) || *reinterpret_cast<uintptr_t*>(node) != gameAt(fp::sig::nodeVmt)) { trackedBoneFault = true; return false; }
    char name[16]{};
    snprintf(name, sizeof name, "%s%02u", fingerbones::nodeNamePrefix, index);
    memcpy(reinterpret_cast<void*>(node + field::sceneNode::name), name, sizeof name);
    static constexpr float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    memcpy(reinterpret_cast<void*>(node + field::sceneNode::localTransform), identity, sizeof identity);
    memcpy(reinterpret_cast<void*>(node + field::sceneNode::transform), reinterpret_cast<void*>(hand.handNode + field::sceneNode::transform), 48);
    if (!trackedBoneAttachCall(hand.handNode, node)) return false;
    if (*reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) != hand.handNode) { trackedBoneFault = true; return false; }
    const int id = trackedBoneRegisterCall(skin.skin, node);
    if (id < 1 || *reinterpret_cast<int*>(node + 0x17c) != id) { trackedBoneFault = true; return false; }
    hand.bones[index].node = node;
    return true;
}
// The merged pieces under a hand node, as bone ids registered in this skin.
static void trackedHandPieces(fingerbones::Hand& hand, const fingerbones::Skin& skin) {
    hand.pieceCount = 0;
    if (!readable(hand.handNode, 0x2a0)) return;
    const int last = *reinterpret_cast<int*>(hand.handNode + field::sceneNode::lastChild);
    const uintptr_t children = *reinterpret_cast<uintptr_t*>(hand.handNode + field::sceneNode::children);
    if (last < 0 || last > 256 || !readable(children, (size_t(last) + 1) * 8)) return;
    for (int k = 0; k <= last && hand.pieceCount < hand.pieceIds.size(); ++k) {
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(children + size_t(k) * 8);
        const int id = node ? skinPieceId(node, hand.handNode) : 0;
        if (id >= 1 && id <= skin.bones && *reinterpret_cast<uintptr_t*>(skin.boneNodes + size_t(id - 1) * 8) == node + field::sceneNode::transform)
            hand.pieceIds[hand.pieceCount++] = id;
    }
}
static bool trackedHandVertex(const fingerbones::Hand& hand, int handId, int bone) {
    if (bone == handId) return true;
    for (unsigned g = 0; g < hand.pieceCount; ++g)
        if (hand.pieceIds[g] == bone) return true;
    return false;
}
// The rest skeleton fitted to the mesh: for each finger, the vertices near its model segments give the
// finger's own direction (base to the far half's centroid) and length (the farthest projection, less a
// radius); the chain is rebuilt along that direction keeping the model's segment proportions. The hand's
// own fingers, not the model's guess, then carry the weights and the bends.
static void trackedBoneFitSkeleton(fingerbones::Hand& hand, const fingerbones::Skin& skin, int handId, const float* handBind, float reach) {
    for (unsigned finger = 0; finger < 5; ++finger) {
        unsigned first = 0;
        const unsigned count = xd::fingerChain(finger).segments;
        for (unsigned f = 0; f < finger; ++f) first += xd::fingerChain(f).segments;
        if (first + count > hand.created) return;
        const XrVector3f origin = hand.bones[first].restA;
        const XrVector3f dir0 = xd::unit(xd::sub(hand.bones[first + count - 1].restB, origin), {0, 0, -1});
        float farthest = 0;
        std::vector<XrVector3f> cluster;
        for (unsigned v = 0; v < skin.weights.count; ++v) {
            const auto& w = *reinterpret_cast<const fp::wear::Weights*>(skin.weights.data + size_t(v) * sizeof(fp::wear::Weights));
            if (!trackedHandVertex(hand, handId, w.bone[0]) || !(w.weight[0] >= .999f)) continue;
            const float* s = reinterpret_cast<const float*>(skin.source + size_t(v) * 12);
            if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2])) continue;
            const XrVector3f p = fingerbones::apply12(handBind, {s[0], s[1], s[2]});
            float best = 1e9f, t = 0;
            for (unsigned b = first; b < first + count; ++b) best = (std::min)(best, xd::detail::segmentDistance(p, hand.bones[b].restA, hand.bones[b].restB, t));
            if (best >= reach) continue;
            cluster.push_back(p);
            farthest = (std::max)(farthest, xd::dot(xd::sub(p, origin), dir0));
        }
        if (cluster.size() < 12 || farthest < 2.f) continue; // too little of a finger to trust: the model's stays
        XrVector3f farSum{};
        unsigned farCount = 0;
        for (const auto& p : cluster)
            if (xd::dot(xd::sub(p, origin), dir0) > farthest * .5f) { farSum = xd::add(farSum, p); ++farCount; }
        if (farCount < 4) continue;
        const XrVector3f dir = xd::unit(xd::sub(xd::scale(farSum, 1.f / float(farCount)), origin), dir0);
        if (xd::dot(dir, dir0) < .8f) continue; // a cluster that is not this finger
        float modelLength = 0;
        for (unsigned b = first; b < first + count; ++b) modelLength += xd::length(xd::sub(hand.bones[b].restB, hand.bones[b].restA));
        const float length = (std::max)(1.f, farthest - reach / 2.5f);
        if (modelLength <= 0) continue;
        XrVector3f rest = origin;
        for (unsigned b = first; b < first + count; ++b) {
            const float seg = xd::length(xd::sub(hand.bones[b].restB, hand.bones[b].restA)) / modelLength * length;
            hand.bones[b].restA = rest;
            hand.bones[b].restB = xd::add(rest, xd::scale(dir, seg));
            rest = hand.bones[b].restB;
        }
    }
}
// After a (re)build of the skin: every bone's slot carries the hand's inverse bind and extra, and the
// hand's single-bone vertices are weighted to their nearest rest segments.
static bool trackedBoneSync(fingerbones::Hand& hand, const fingerbones::Skin& skin, int handId, float reach) {
    for (unsigned b = 0; b < hand.created; ++b) {
        const uintptr_t node = hand.bones[b].node;
        if (!trackedBoneNodeValid(node, hand.handNode)) return false;
        int id = *reinterpret_cast<int*>(node + 0x17c);
        if (id == 0) id = trackedBoneRegisterCall(skin.skin, node);
        const int bones = *reinterpret_cast<int*>(skin.skin + 0x10); // the register may have grown the lists
        const uintptr_t boneNodes = *reinterpret_cast<uintptr_t*>(skin.skin + 0x18), inverseBind = *reinterpret_cast<uintptr_t*>(skin.skin + 0x20), extra = *reinterpret_cast<uintptr_t*>(skin.skin + 0x28);
        if (id < 1 || id > bones || handId > bones || !readable(boneNodes, size_t(bones) * 8) || !readable(inverseBind, size_t(bones) * 48) || !readable(extra, size_t(bones) * 4)) return false;
        if (*reinterpret_cast<uintptr_t*>(boneNodes + size_t(id - 1) * 8) != node + field::sceneNode::transform) return false;
        memcpy(reinterpret_cast<void*>(inverseBind + size_t(id - 1) * 48), reinterpret_cast<void*>(inverseBind + size_t(handId - 1) * 48), 48);
        *reinterpret_cast<float*>(extra + size_t(id - 1) * 4) = *reinterpret_cast<float*>(extra + size_t(handId - 1) * 4);
    }
    const float* handBind = reinterpret_cast<const float*>(skin.inverseBind + size_t(handId - 1) * 48);
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(handBind[i])) return false;
    trackedHandPieces(hand, skin); // gloves merged into this skin: their vertices bend with the fingers too
    if (!hand.fitted) { trackedBoneFitSkeleton(hand, skin, handId, handBind, reach); hand.fitted = true; }
    hand.saved.clear();
    for (unsigned v = 0; v < skin.weights.count; ++v) {
        auto& w = *reinterpret_cast<fp::wear::Weights*>(skin.weights.data + size_t(v) * sizeof(fp::wear::Weights));
        if (!trackedHandVertex(hand, handId, w.bone[0]) || !(w.weight[0] >= .999f)) continue; // only vertices the hand (or a glove on it) carries alone
        const float* s = reinterpret_cast<const float*>(skin.source + size_t(v) * 12);
        if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2])) continue;
        const XrVector3f p = fingerbones::apply12(handBind, {s[0], s[1], s[2]}); // the bind position in the hand's frame
        int best = -1, second = -1;
        float bestD = 1e9f, secondD = 1e9f, t = 0;
        for (int b = 0; b < int(hand.created); ++b) {
            const float d = xd::detail::segmentDistance(p, hand.bones[size_t(b)].restA, hand.bones[size_t(b)].restB, t);
            if (d < bestD) { second = best; secondD = bestD; best = b; bestD = d; }
            else if (d < secondD) { second = b; secondD = d; }
        }
        if (best < 0 || bestD >= reach) continue; // palm: stays with the hand
        const int bestId = *reinterpret_cast<int*>(hand.bones[size_t(best)].node + 0x17c);
        fp::wear::Weights out{};
        out.bone[0] = bestId;
        out.weight[0] = 1;
        if (second >= 0 && secondD < reach && hand.bones[size_t(second)].finger == hand.bones[size_t(best)].finger && secondD < bestD * 2.5f) {
            out.bone[1] = *reinterpret_cast<int*>(hand.bones[size_t(second)].node + 0x17c);
            out.weight[1] = bestD / (bestD + secondD) * .5f;
            out.weight[0] = 1 - out.weight[1];
        }
        hand.saved.emplace_back(v, w);
        w = out;
    }
    hand.skinNode = skin.node;
    hand.weightData = skin.weights.data;
    hand.heldPoseValid = false;
    trackedBoneRewrites.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Each tick: the local transform that carries the rest segment to the posed one, then the game's propagation.
static void trackedBonePose(fingerbones::Hand& hand, const fingerbones::Joints& joint) {
    unsigned b = 0;
    for (unsigned finger = 0; finger < 5; ++finger) {
        const xd::FingerChain c = xd::fingerChain(finger);
        for (unsigned k = 0; k < c.segments && b < hand.created; ++k, ++b) {
            const fingerbones::Bone& bone = hand.bones[b];
            if (!trackedBoneNodeValid(bone.node, hand.handNode)) continue;
            const XrVector3f posedA = joint[c.first + k], posedB = joint[c.first + k + 1];
            const XrQuaternionf q = xd::directionRotation(xd::unit(xd::sub(bone.restB, bone.restA), {0, 0, -1}), xd::unit(xd::sub(posedB, posedA), {0, 0, -1}));
            const XrVector3f r0 = xd::rotate(q, {1, 0, 0}), r1 = xd::rotate(q, {0, 1, 0}), r2 = xd::rotate(q, {0, 0, 1});
            const XrVector3f t = xd::sub(posedA, xd::rotate(q, bone.restA));
            const float local[12] = {r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z, t.x, t.y, t.z};
            for (float f : local)
                if (!std::isfinite(f)) return;
            memcpy(reinterpret_cast<void*>(bone.node + field::sceneNode::localTransform), local, sizeof local);
            if (!trackedBonePropagateCall(bone.node)) return;
        }
    }
}
// The common gate of both drivers: the row's say, no earlier fault, the targets resolved, and play:
// the mod's camera live in a level and no UI open. Never the title, a preview or a menu.
static bool trackedBonesWanted(bool rowOn) {
    return rowOn && nodeMoveHooked.load() && !trackedBoneFault.load() && layout.optionalGroupAvailable("fingerBones") &&
           InterlockedCompareExchange(&active, 0, 0) && InterlockedCompareExchange(&cameraReady, 0, 0);
}
static fingerbones::Root& trackedBoneEntry(uintptr_t root) {
    for (auto& entry : trackedBoneTable)
        if (entry.root == root) return entry;
    trackedBoneTable.push_back({root, {}});
    return trackedBoneTable.back();
}
static void trackedBoneRetireEntry(fingerbones::Root& entry, bool touch) {
    for (auto& hand : entry.hands) trackedBoneForget(hand, touch && hand.handNode != 0);
    entry = {};
}
static void trackedBoneRetire() {
    for (auto& entry : trackedBoneTable) trackedBoneRetireEntry(entry, entry.root && readable(entry.root, 0x1300));
    trackedBoneTable.clear();
    trackedBoneCount = 0;
    trackedBoneRootsDriven = 0;
    AcquireSRWLockExclusive(&trackedBonePublishLock);
    trackedBonePublished = {};
    ReleaseSRWLockExclusive(&trackedBonePublishLock);
}
// The world's characters, the dead too, as the game lists them; the player is among them.
static void trackedBoneRoster(std::vector<uintptr_t>& out) {
    out.clear();
    const uintptr_t world = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::world));
    if (!readable(world, field::worldObject::characters + sizeof(uintptr_t))) return;
    const uintptr_t roster = *reinterpret_cast<const uintptr_t*>(world + field::worldObject::characters);
    if (!readable(roster, field::list::items + sizeof(uintptr_t))) return;
    const int characters = *reinterpret_cast<const int*>(roster + field::list::count);
    const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(roster + field::list::items);
    if (characters < 1 || characters > 4096 || !readable(entries, size_t(characters) * sizeof(uintptr_t))) return;
    for (int i = 0; i < characters; ++i) {
        const uintptr_t character = *reinterpret_cast<const uintptr_t*>(entries + size_t(i) * sizeof(uintptr_t));
        if (character && readable(character, 0x1300)) out.push_back(character);
    }
}
// Drive one hand: bones made on first use (the rest skeleton from `rest`), re-synced after a skin
// rebuild, posed to `posed`. Both arrays are in the hand part's frame and the game's units. Returns
// false when the hand is left alone this tick (no skin, not a bone, no joints).
static bool trackedBoneDrive(uintptr_t root, unsigned side, fingerbones::Hand& hand, const fingerbones::Skin& skin, const fingerbones::Joints* rest, const fingerbones::Joints* posed, float reach, unsigned& count) {
    const uintptr_t part = trackedHandPart(root, side);
    if (!part || !readable(part, field::bodyPart::character + 8) || *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) != root) return false;
    const uintptr_t handNode = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!handNode || !readable(handNode, 0x420)) return false;
    if (hand.handNode != handNode) trackedBoneForget(hand, hand.handNode != 0);
    hand.handNode = handNode;
    const int handId = trackedBoneHandId(skin, handNode);
    if (handId < 1) return false;
    for (const auto& entry : trackedBoneTable) // a weight stream shared with another character's bones is left alone
        for (const auto& h : entry.hands)
            if (&h != &hand && h.weightData && h.weightData == skin.weights.data && entry.root != root) return false;
    if (!posed || !rest) { count += hand.created; return false; }
    if (hand.created == 0) {
        trackedBoneRestSkeleton(*rest, hand);
        for (unsigned b = 0; b < fingerbones::perHand; ++b) {
            if (!trackedBoneCreate(hand, b, skin)) return false;
            hand.created = b + 1;
        }
    }
    if (hand.skinNode != skin.node && !trackedBoneSync(hand, skin, handId, reach)) { trackedBoneFault = true; return false; }
    hand.heldPoseValid = false;
    trackedBonePose(hand, *posed);
    count += hand.created;
    return true;
}
// A rigid glove or gauntlet on the hand: a piece carrying the hand's part code in its name (17.. right,
// 18.. left) hung on the hand node or bound to the part. It does not bend with the skin, so the skin
// under it stays at rest rather than poking through.
static bool handGloved(uintptr_t root, unsigned side) {
    const uintptr_t part = trackedHandPart(root, side);
    if (!part || !readable(part, field::bodyPart::meshNode + 8)) return false;
    const uintptr_t own = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!own || !readable(own, field::sceneNode::name + 16)) return false;
    const int last = *reinterpret_cast<int*>(own + field::sceneNode::lastChild);
    const uintptr_t children = *reinterpret_cast<uintptr_t*>(own + field::sceneNode::children);
    if (last < 0 || last > 256 || !readable(children, (size_t(last) + 1) * 8)) return false;
    for (int k = 0; k <= last; ++k) {
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(children + size_t(k) * 8);
        if (!node || !readable(node, field::sceneNode::name + 16)) continue;
        const char* name = reinterpret_cast<const char*>(node + field::sceneNode::name);
        if (name[0] == '1' && name[1] == (side ? '7' : '8') && !skinPieceId(node, own)) return true; // rigid: not merged into the skin
    }
    return false;
}
// The held item's node, when it hangs on the hand node: a wielded weapon or a carried item.
static uintptr_t heldItemNode(uintptr_t root, unsigned side) {
    if (!root || !readable(root, 0x1300)) return 0;
    const uintptr_t hand = trackedHandPart(root, side);
    uintptr_t part = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8);
    const int weaponCount = *reinterpret_cast<int*>(root + field::character::weaponCount);
    if (!part && weaponCount > 0) {
        for (int w = 0; w < (std::min)(weaponCount, 2); ++w) {
            const uintptr_t weapon = *reinterpret_cast<uintptr_t*>(root + field::character::weapons + w * 8);
            if (readable(weapon, field::bodyPart::parentPart + 8) && *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::parentPart) == hand) { part = weapon; break; }
        }
    }
    if (!part && weaponCount > 0) part = *reinterpret_cast<uintptr_t*>(root + field::character::weapons);
    if (!part || !readable(part, field::bodyPart::meshNode + 8) || !hand || !readable(hand, field::bodyPart::meshNode + 8)) return 0;
    const uintptr_t node = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode), own = *reinterpret_cast<uintptr_t*>(hand + field::bodyPart::meshNode);
    if (!node || !own || !readable(node, field::sceneNode::name + 16)) return 0;
    const uintptr_t parent = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent);
    if (parent != own) {
        const uintptr_t otherHand = trackedHandPart(root, 1 - side);
        const uintptr_t otherOwn = otherHand && readable(otherHand, field::bodyPart::meshNode + 8) ? *reinterpret_cast<uintptr_t*>(otherHand + field::bodyPart::meshNode) : 0;
        if (parent != otherOwn) return 0;
    }
    return node;
}
// The long axis of the held item in the hand part's frame: its longest half extent's local axis taken
// through its transform under the hand node, pointed to the thumb's side (+x on the right hand). The
// joint model wraps the fingers round +x across the palm, so the curl is turned onto this axis.
static bool heldItemAxis(uintptr_t root, unsigned side, XrVector3f& axis) {
    const uintptr_t node = heldItemNode(root, side);
    if (!node) return false;
    const float* e = reinterpret_cast<const float*>(node + 0x30);
    const float* local = reinterpret_cast<const float*>(node + field::sceneNode::localTransform);
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(e[i]) || e[i] < 0) return false;
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(local[i])) return false;
    const unsigned longest = e[0] >= e[1] && e[0] >= e[2] ? 0 : e[1] >= e[2] ? 1 : 2;
    if (e[longest] < 2 * (std::max)(e[(longest + 1) % 3], e[(longest + 2) % 3])) return false; // no clear axis: a round or flat thing
    const XrVector3f row = {local[longest * 3], local[longest * 3 + 1], local[longest * 3 + 2]};
    if (xd::length(row) < 1e-4f) return false;
    axis = xd::unit(row, {1, 0, 0});
    const float thumbSide = side == 1 ? 1.f : -1.f;
    if (axis.x * thumbSide < 0) axis = xd::scale(axis, -1);
    return true;
}
// The pose of one character's hand for the held-pose driver: what it holds; a dead hand holding
// nothing falls slack.
static xd::HandPose heldDrivenPose(uintptr_t root, unsigned side) {
    const xd::HandPose held = heldHandPose(root, side);
    if (held == xd::HandPose::open) {
        if (readable(root, field::character::stateFlags + 4) && *reinterpret_cast<unsigned*>(root + field::character::stateFlags) == fp::deadState) return xd::HandPose::limp;
        return xd::HandPose::open;
    }
    return held;
}
static void heldTargetCurls(xd::HandPose pose, float itemRadius, float (&out)[5]) {
    float curlBase = 0.f;
    switch (pose) {
    case xd::HandPose::open: curlBase = 0.f; break;
    case xd::HandPose::flat: curlBase = 0.05f; break;
    case xd::HandPose::limp:
        out[0] = 0.25f; out[1] = 0.30f; out[2] = 0.35f; out[3] = 0.40f; out[4] = 0.45f;
        return;
    case xd::HandPose::fist: curlBase = 1.f; break;
    case xd::HandPose::hilt:
        curlBase = std::clamp(0.92f - itemRadius * 4.f, 0.25f, 0.95f);
        break;
    case xd::HandPose::hold:
        curlBase = std::clamp(0.95f - itemRadius * 8.f, 0.15f, 0.95f);
        break;
    }
    for (float& c : out) c = curlBase;
}
static bool trackedBoneWorldCurrent(const fingerbones::Hand& hand) {
    if (!hand.created || !hand.handNode || !readable(hand.handNode, field::sceneNode::transform + 48)) return false;
    const uintptr_t boneNode = hand.bones[0].node;
    if (!trackedBoneNodeValid(boneNode, hand.handNode)) return false;
    const float* parent = reinterpret_cast<const float*>(hand.handNode + field::sceneNode::transform);
    const float* local = reinterpret_cast<const float*>(boneNode + field::sceneNode::localTransform);
    const float* world = reinterpret_cast<const float*>(boneNode + field::sceneNode::transform);
    const XrVector3f wantRow0 = {local[0] * parent[0] + local[1] * parent[3] + local[2] * parent[6],
                                 local[0] * parent[1] + local[1] * parent[4] + local[2] * parent[7],
                                 local[0] * parent[2] + local[1] * parent[5] + local[2] * parent[8]};
    const XrVector3f wantOrigin = fingerbones::apply12(parent, {local[9], local[10], local[11]});
    const XrVector3f gotRow0 = {world[0], world[1], world[2]};
    const XrVector3f gotOrigin = {world[9], world[10], world[11]};
    return xd::finite(wantRow0) && xd::finite(wantOrigin) &&
           xd::length(xd::sub(wantRow0, gotRow0)) < 1e-3f &&
           xd::length(xd::sub(wantOrigin, gotOrigin)) < 1e-2f;
}
static bool trackedBoneReuseHeld(uintptr_t root, unsigned side, fingerbones::Hand& hand, const fingerbones::Skin& skin,
                                 xd::HandPose pose, uintptr_t heldNode, const float (&curls)[5], float itemRadius,
                                 float units, bool hasAxis, const XrVector3f& axis, bool hasGrip, const XrVector3f& grip,
                                 unsigned& count) {
    if (!hand.heldPoseValid || hand.created != fingerbones::perHand || hand.skinNode != skin.node) return false;
    const uintptr_t part = trackedHandPart(root, side);
    if (!part || !readable(part, field::bodyPart::character + 8) || *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) != root) return false;
    const uintptr_t handNode = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!handNode || hand.handNode != handNode || !readable(handNode, 0x420)) return false;
    if (trackedBoneHandId(skin, handNode) < 1) return false;
    if (hand.lastPose != pose || hand.lastHeldNode != heldNode ||
        std::fabs(hand.lastRadius - itemRadius) >= 1e-4f || std::fabs(hand.lastUnits - units) >= 1e-3f ||
        hand.lastHasAxis != hasAxis || hand.lastHasGrip != hasGrip) return false;
    for (unsigned f = 0; f < 5; ++f)
        if (std::fabs(curls[f] - hand.lastCurl[f]) >= 1e-4f) return false;
    if (hasAxis && xd::length(xd::sub(hand.lastAxis, axis)) >= 1e-4f) return false;
    if (hasGrip && xd::length(xd::sub(hand.lastGrip, grip)) >= 1e-3f) return false;
    for (const auto& entry : trackedBoneTable)
        for (const auto& h : entry.hands)
            if (&h != &hand && h.weightData && h.weightData == skin.weights.data && entry.root != root) return false;
    for (unsigned b = 0; b < hand.created; ++b)
        if (!trackedBoneNodeValid(hand.bones[b].node, handNode)) return false;
    if (!trackedBoneWorldCurrent(hand)) {
        for (unsigned b = 0; b < hand.created; ++b)
            if (!trackedBonePropagateCall(hand.bones[b].node)) return false;
    }
    count += hand.created;
    return true;
}
// The held-item poses of one character's two hands, laid out by the joint model, into its bones.
static void trackedBoneDriveHeld(fingerbones::Root& entry, const fingerbones::Skin& skin, float units, unsigned& count, uintptr_t player) {
    for (unsigned side = 0; side < 2; ++side) {
        auto& hand = entry.hands[side];
        const xd::HandPose pose = heldDrivenPose(entry.root, side);
        const uintptr_t heldNode = heldItemNode(entry.root, side);
        XrVector3f grip{};
        float itemRadius = 0;
        const bool hasGrip = heldItemGrip(entry.root, side, grip, itemRadius);
        if (!hasGrip) itemRadius = heldRadiusFromExtents(entry.root, side);
        const bool gripPose = pose == xd::HandPose::hilt || pose == xd::HandPose::hold;
        XrVector3f axis{};
        const bool hasAxis = gripPose && heldItemAxis(entry.root, side, axis);
        float curls[5]{};
        heldTargetCurls(pose, itemRadius, curls);
        if (!trackedBoneReuseHeld(entry.root, side, hand, skin, pose, heldNode, curls, itemRadius, units,
                                  hasAxis, axis, gripPose && hasGrip, grip, count)) {
            fingerbones::Joints rest{}, posed{};
            float reach = 0, unused = 0;
            const bool laid = fingerbones::modelJoints(xd::HandPose::open, side == 1, 0, units, rest, unused) &&
                              fingerbones::modelJoints(pose, side == 1, itemRadius, units, posed, reach);
            const bool gripped = laid && gripPose;
            if (gripped && hasAxis) {
                // each finger's curl turns about its own base so the fingers wrap round the item's axis; the
                // bases stay on the palm, and the turn is capped where a real hand would move the wrist instead
                const float thumbSide = side == 1 ? 1.f : -1.f;
                const XrVector3f from = {thumbSide, 0, 0};
                const float angle = std::acos((std::min)(1.f, (std::max)(-1.f, xd::dot(from, axis)))), cap = 80.f * 3.14159265f / 180.f;
                const float t = angle > cap ? cap / angle : 1.f;
                const XrQuaternionf q = xd::directionRotation(from, xd::unit(xd::add(xd::scale(from, 1 - t), xd::scale(axis, t)), from));
                for (unsigned finger = 0; finger < 5; ++finger) {
                    const xd::FingerChain c = xd::fingerChain(finger);
                    for (unsigned j = c.first + 1; j <= c.first + c.segments; ++j) posed[j] = xd::add(posed[c.first], xd::rotate(q, xd::sub(posed[j], posed[c.first])));
                }
            }
            if (gripped && hasGrip) {
                // the fingers close where the item's grip is: the model's wrap centre (the index and middle
                // fingers' middle joints and the thumb's tip) is moved onto the grip point, across the item's axis only,
                // by at most three centimetres, the bases staying on the palm
                XrVector3f centre = xd::scale(xd::add(xd::add(posed[8], posed[13]), posed[5]), 1.f / 3.f);
                XrVector3f shift = xd::sub(grip, centre);
                if (xd::length(axis) > .5f) shift = xd::sub(shift, xd::scale(axis, xd::dot(shift, axis)));
                const float len = xd::length(shift), cap = 3.f;
                if (len > cap) shift = xd::scale(shift, cap / len);
                if (xd::finite(shift))
                    for (unsigned finger = 0; finger < 5; ++finger) {
                        const xd::FingerChain c = xd::fingerChain(finger);
                        for (unsigned j = c.first + 1; j <= c.first + c.segments; ++j) posed[j] = xd::add(posed[j], shift);
                    }
            }
            if (trackedBoneDrive(entry.root, side, hand, skin, laid ? &rest : nullptr, laid ? &posed : nullptr, reach, count) &&
                !trackedBoneFault.load()) {
                hand.heldPoseValid = true;
                hand.lastPose = pose;
                hand.lastHeldNode = heldNode;
                for (unsigned f = 0; f < 5; ++f) hand.lastCurl[f] = curls[f];
                hand.lastRadius = itemRadius;
                hand.lastUnits = units;
                hand.lastHasAxis = hasAxis;
                hand.lastAxis = axis;
                hand.lastHasGrip = gripPose && hasGrip;
                hand.lastGrip = grip;
            }
        }
        if (entry.root == player) {
            if (side == 0) {
                trackedPlayerPoseLeft = static_cast<int>(pose);
                trackedPlayerGlovedLeft = handGloved(entry.root, side);
                trackedPlayerGrippedLeft = hasAxis;
                trackedPlayerFittedLeft = hand.fitted;
                trackedPlayerVerticesLeft = static_cast<unsigned>(hand.saved.size());
            } else {
                trackedPlayerPoseRight = static_cast<int>(pose);
                trackedPlayerGlovedRight = handGloved(entry.root, side);
                trackedPlayerGrippedRight = hasAxis;
                trackedPlayerFittedRight = hand.fitted;
                trackedPlayerVerticesRight = static_cast<unsigned>(hand.saved.size());
            }
        }
        if (trackedBoneFault.load()) return;
    }
}
static void trackedBonePublish(uintptr_t player) {
    PublishedBones out{};
    for (const auto& entry : trackedBoneTable) {
        if (entry.root != player || !player) continue;
        out.root = player;
        for (const auto& hand : entry.hands) {
            if (!hand.created || !hand.skinNode || !hand.handNode || !readable(hand.handNode, 0x180)) continue;
            if (out.skinNode && out.skinNode != hand.skinNode) continue; // both hands on one skin, else the draw keeps dropping the body
            out.skinNode = hand.skinNode;
            out.ids[out.count++] = *reinterpret_cast<int*>(hand.handNode + 0x17c);
            for (unsigned g = 0; g < hand.pieceCount; ++g) out.ids[out.count++] = hand.pieceIds[g];
            for (unsigned b = 0; b < hand.created; ++b)
                if (trackedBoneNodeValid(hand.bones[b].node, hand.handNode)) out.ids[out.count++] = *reinterpret_cast<int*>(hand.bones[b].node + 0x17c);
        }
    }
    AcquireSRWLockExclusive(&trackedBonePublishLock);
    trackedBonePublished = out;
    ReleaseSRWLockExclusive(&trackedBonePublishLock);
}
static inline PublishedBones trackedBoneSnapshot() { // inline: the plain build has no reader
    AcquireSRWLockShared(&trackedBonePublishLock);
    const PublishedBones out = trackedBonePublished;
    ReleaseSRWLockShared(&trackedBonePublishLock);
    return out;
}
static void trackedBonesEnd(unsigned count, unsigned roots) {
    trackedBoneCount = count;
    trackedBoneRootsDriven = roots;
    if (trackedBoneFault.load()) { trackedBoneFaults.fetch_add(1, std::memory_order_relaxed); trackedBoneRetire(); }
}
static bool trackedBoneActorWorld(uintptr_t root, float (&worldOut)[3], bool requireShown) {
    if (!root || !readable(root, field::character::sceneNode + 8)) return false;
    const uintptr_t actor = *reinterpret_cast<const uintptr_t*>(root + field::character::sceneNode);
    if (!actor || !readable(actor, field::sceneNode::sector + 8)) return false;
    const uintptr_t sector = *reinterpret_cast<const uintptr_t*>(actor + field::sceneNode::sector);
    if (!sector || !readable(sector, 0x31c)) return false;
    if (requireShown && *reinterpret_cast<const int*>(sector + 0x318) == 0) return false;
    const float* t = reinterpret_cast<const float*>(actor + field::sceneNode::transform) + 9;
    const float* o = reinterpret_cast<const float*>(sector + field::sector::origin);
    const float wx = t[0] + o[0], wy = t[1] + o[1], wz = t[2] + o[2];
    if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) return false;
    worldOut[0] = wx;
    worldOut[1] = wy;
    worldOut[2] = wz;
    return true;
}
static bool trackedBoneCameraWorld(float (&worldOut)[3]) {
    if (!TryAcquireSRWLockShared(&cameraLock)) return false;
    bool ok = false;
    __try {
        uintptr_t view = 0, camera = 0;
        currentCamera(view, camera);
        if (camera && readable(camera + field::camera::pose, 48)) {
            const float* eye = reinterpret_cast<const float*>(camera + field::camera::pose) + 9;
            if (std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2])) {
                worldOut[0] = eye[0];
                worldOut[1] = eye[1];
                worldOut[2] = eye[2];
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    ReleaseSRWLockShared(&cameraLock);
    return ok;
}
// The held-pose driver, every build: the VR hands' item poses laid out by the joint model, for nearby
// visible characters the world lists (the player first, then the list's order, up to maxRoots). In VR the
// drive's tick ran just before this one from the same hook; when it drove the player's bones, the
// player is left to it here.
static void fingerPosesTick(uintptr_t player) {
    const bool vr = trackedBoneDrivenByVr;
    trackedBoneDrivenByVr = false;
    if (!trackedBonesWanted(fingerPoses.load(std::memory_order_relaxed) != 0)) {
        if (!vr && !trackedBoneTable.empty()) trackedBoneRetire();
        return;
    }
    static std::vector<uintptr_t> worldRoster;
    static std::vector<uintptr_t> roster;
    const bool othersOn = fingerPosesOthers.load(std::memory_order_relaxed) != 0;
    if (othersOn || !trackedBoneTable.empty()) trackedBoneRoster(worldRoster);
    else worldRoster.clear();
    if (!player || !readable(player, 0x1300) || player != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot))) player = 0;
    if (player && std::find(worldRoster.begin(), worldRoster.end(), player) == worldRoster.end()) worldRoster.push_back(player);
    // entries whose root the world no longer lists are dropped untouched; retired non-player entries restore weights
    for (auto& entry : trackedBoneTable) {
        if (std::find(worldRoster.begin(), worldRoster.end(), entry.root) == worldRoster.end()) entry = {};
        else if (!othersOn && entry.root != player) trackedBoneRetireEntry(entry, readable(entry.root, 0x1300));
    }
    trackedBoneTable.erase(std::remove_if(trackedBoneTable.begin(), trackedBoneTable.end(), [](const fingerbones::Root& e) { return e.root == 0; }), trackedBoneTable.end());
    const float units = static_cast<float>(vrUnitsPerMetre.load(std::memory_order_relaxed));
    const float scaleUnits = (std::isfinite(units) && units > 0.f) ? units : 100.f;
    const float maxDist = 15.f * scaleUnits;
    const float maxDistSq = maxDist * maxDist;
    float refWorld[3]{};
    const bool hasRef = trackedBoneCameraWorld(refWorld) || (player && trackedBoneActorWorld(player, refWorld, false));
    roster.clear();
    if (player) roster.push_back(player);
    if (othersOn) {
        for (const uintptr_t root : worldRoster) {
            if (roster.size() >= fingerbones::maxRoots) break;
            if (root == player) continue;
            float actorWorld[3]{};
            if (!trackedBoneActorWorld(root, actorWorld, true)) continue;
            if (hasRef) {
                const float dx = actorWorld[0] - refWorld[0], dy = actorWorld[1] - refWorld[1], dz = actorWorld[2] - refWorld[2];
                if (dx * dx + dy * dy + dz * dz > maxDistSq) continue;
            }
            roster.push_back(root);
        }
    }
    unsigned count = 0, driven = 0;
    for (const uintptr_t root : roster) {
        if (vr && root == player) { // the VR tick already posed the player; count its bones
            for (const auto& hand : trackedBoneEntry(root).hands) count += hand.created;
            ++driven;
            continue;
        }
        fingerbones::Skin skin{};
        if (!trackedBoneSkin(root, skin)) continue;
        trackedBoneDriveHeld(trackedBoneEntry(root), skin, units, count, player);
        ++driven;
        if (trackedBoneFault.load()) break;
    }
    for (const auto& entry : trackedBoneTable) {
        if (std::find(roster.begin(), roster.end(), entry.root) != roster.end()) continue;
        for (const auto& hand : entry.hands) count += hand.created;
    }
    fingerPoseTicks.fetch_add(1, std::memory_order_relaxed);
    trackedBonesEnd(count, driven);
    trackedBonePublish(player);
}
