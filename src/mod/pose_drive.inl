// The Animated kick (docs/MIXAMO-KICK-DESIGN.md), every build. The kick key still plays the game's own kick
// (motions.inl), so its gates, damage and reactions stay native; while that clip runs, every physics
// step's motor targets are blended toward a baked motion-captured kick (kick_clip.h): the whole body, or the hips
// and legs only, by what the player holds (fp::kick::classRules). The targets go in where the VR body driver puts
// its own: the pose builder's affine composition (xrCompose) at its three reviewed call sites, for one planned part
// at a time; anything unexpected leaves the game's own composition untouched, so a refusal is the Classic kick.
// The Animated jump (jumpPlan below) drives the same rows from a baked jump, without the hips' position.
// Pose-builder thread only, apart from the status counters.
#include "../kick_clip.h"
#include "../xr/native_pose.h"
namespace kk = fp::kick;

using KickCompose = void (*)(float*, float*, const float*);
static KickCompose kickComposeOriginal = nullptr;
static bool kickComposeChained = false; // the VR drive owns the xrCompose detour and hands its fall-through here
static std::atomic<bool> kickDriveReady{false}, kickDriveFault{false};
static std::atomic<unsigned> kickAnimated{0}, kickBlendedIn{0}, kickBlendedOut{0}, kickCutShort{0}, kickStaggered{0}, kickSteps{0},
    kickRows{0}, kickRefusedGraph{0}, kickRefusedHeading{0}, kickNativeLegAgree{0}, kickNativeLegDisagree{0}, kickHeldArms{0};
static std::atomic<int> kickLastClip{-1}, kickLastClass{-1}, kickLastMirror{-1}, kickLastMode{-1}, kickNativePeakMilli{-1};
static constexpr float kickUnitsPerMetre = 100.f; // game units per metre, as the VR pose maths assumes
static int kickRotation[2] = {-1, -1};            // the clip each set played last (pose-builder thread)

enum class KickSite : uint8_t { motor, foot, pelvis };
struct KickRow {
    uintptr_t part = 0, parent = 0;
    KickSite site = KickSite::motor;
    XrQuaternionf world{0, 0, 0, 1};
    XrVector3f position{};
    bool positioned = false;
    float weight = 0;
};
struct KickScope {
    uintptr_t root = 0;
    unsigned count = 0;
    std::array<KickRow, 18> rows{};
};
static thread_local KickScope kickScope;
static thread_local bool kickScopeOpen = false;

// The player's body, found by structure as the VR driver finds it (never by index): [0] left, [1] right.
struct KickPart { uintptr_t part = 0; XrPosef pose{{0, 0, 0, 1}, {}}; };
struct KickBody {
    KickPart pelvis, belly, chest, head;
    KickPart shoulder[2], upper[2], fore[2], hand[2], thigh[2], shin[2], foot[2];
    bool supported[2]{};
};
static bool kickOwned(uintptr_t part, uintptr_t root) {
    return part && readable(part, 0x600) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) == root;
}
static uintptr_t kickParentOf(uintptr_t part) { return *reinterpret_cast<uintptr_t*>(part + field::bodyPart::parentPart); }
// A part's pose in world metres; false (counted, no throw: this runs every step of a kick) when its mesh node does
// not name it back (node ownership) or the node's sector cannot be read.
static std::atomic<unsigned> kickNodeOwnershipFails{0}, kickSectorFails{0};
static bool kickPartPose(uintptr_t part, XrPosef& out) {
    fp::xr::NativePose pose{};
    memcpy(pose.data(), reinterpret_cast<void*>(part + field::bodyPart::pose), 48);
    const auto node = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    if (!readable(node, field::sceneNode::bodyPart + 8) || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::bodyPart) != part) {
        kickNodeOwnershipFails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
    if (!readable(sector, field::sector::origin + 12)) {
        kickSectorFails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (unsigned i = 0; i < 3; ++i) pose[9 + i] += reinterpret_cast<float*>(sector + field::sector::origin)[i];
    out = fp::xr::nativePoseMetres(pose, kickUnitsPerMetre);
    return true;
}
// What the first part of the player's list is when the pelvis check refuses it (status): its bind role, whether it is
// the player's, whether it is its own root part and parent, and its flags.
static std::atomic<int> kickFirstPartRole{-1};
static std::atomic<unsigned> kickFirstPartFlags{0}, kickFirstPartBits{0};
// A support contact on the shin, as the VR leg planner counts them.
static bool kickShinSupported(uintptr_t shin) {
    const int n = *reinterpret_cast<int*>(shin + field::bodyPart::contactCount);
    const auto contacts = *reinterpret_cast<uintptr_t*>(shin + field::bodyPart::contacts);
    if (n < 1 || n > 8 || !readable(contacts, size_t(n) * 0x98)) return false;
    for (int i = 0; i < n; ++i)
        if ((*reinterpret_cast<unsigned*>(contacts + size_t(i) * 0x98 + field::contact::flags) & 0x12) == 2) return true;
    return false;
}
// Which check of kickReadBody refused the body last (status kick_animated.refused_body_reasons, by check number in
// the order they appear below; 14 is the hands' and hips' sides disagreeing).
static constexpr unsigned kickBodyChecks = 18;
static std::atomic<unsigned> kickBodyRefusals[kickBodyChecks]{};
static bool kickBodyFail(unsigned check) {
    if (check < kickBodyChecks) kickBodyRefusals[check].fetch_add(1, std::memory_order_relaxed);
    return false;
}
static bool kickReadBody(uintptr_t root, KickBody& b) {
    const int count = *reinterpret_cast<int*>(root + field::character::partCount), legs = *reinterpret_cast<int*>(root + field::character::legCount);
    const auto parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts), legArray = *reinterpret_cast<uintptr_t*>(root + field::character::legs);
    if (count < 8 || count > 64 || legs != 2 || !readable(parts, size_t(count) * 8) || !readable(legArray, 16)) return kickBodyFail(0);
    auto take = [&](KickPart& p, uintptr_t part) {
        if (!kickOwned(part, root)) return kickBodyFail(1);
        p.part = part;
        return kickPartPose(part, p.pose);
    };
    auto plain = [](uintptr_t part) { return !(*reinterpret_cast<unsigned*>(part + field::bodyPart::flags) & 0x40); };
    const auto pelvis = *reinterpret_cast<uintptr_t*>(parts);
    // The pelvis: owned (2), its own root part (15), its own parent (16), and flagged 0x40 (17).
    if (readable(pelvis, 0x600)) {
        kickFirstPartRole = *reinterpret_cast<int*>(pelvis + field::bodyPart::role);
        kickFirstPartFlags = *reinterpret_cast<unsigned*>(pelvis + field::bodyPart::flags);
        kickFirstPartBits = (*reinterpret_cast<uintptr_t*>(pelvis + field::bodyPart::character) == root ? 1u : 0u) |
                            (*reinterpret_cast<uintptr_t*>(pelvis + field::bodyPart::rootPart) == pelvis ? 2u : 0u) |
                            (kickParentOf(pelvis) == pelvis ? 4u : 0u);
    }
    if (!take(b.pelvis, pelvis)) return kickBodyFail(2);
    if (*reinterpret_cast<uintptr_t*>(pelvis + field::bodyPart::rootPart) != pelvis) return kickBodyFail(15);
    if (kickParentOf(pelvis) != pelvis) return kickBodyFail(16);
    if (plain(pelvis)) return kickBodyFail(17);
    const auto chest = *reinterpret_cast<uintptr_t*>(root + field::character::chestPart), head = *reinterpret_cast<uintptr_t*>(root + field::character::headPart);
    if (!take(b.chest, chest) || !take(b.head, head) || kickParentOf(head) != chest || !take(b.belly, kickParentOf(chest)) || kickParentOf(b.belly.part) != pelvis ||
        !plain(chest) || !plain(head) || !plain(b.belly.part)) return kickBodyFail(3);
    for (unsigned side = 0; side < 2; ++side) { // the native bind graph puts the left hand in slot 1
        if (!take(b.hand[side], *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8)) || !take(b.fore[side], kickParentOf(b.hand[side].part)) ||
            !take(b.upper[side], kickParentOf(b.fore[side].part)) || !take(b.shoulder[side], kickParentOf(b.upper[side].part)) || kickParentOf(b.shoulder[side].part) != chest)
            return kickBodyFail(4);
        for (const auto* p : {&b.hand[side], &b.fore[side], &b.upper[side], &b.shoulder[side]}) if (!plain(p->part)) return kickBodyFail(5);
    }
    unsigned seen = 0;
    for (unsigned leg = 0; leg < 2; ++leg) {
        const auto shin = *reinterpret_cast<uintptr_t*>(legArray + leg * 8);
        if (!kickOwned(shin, root) || *reinterpret_cast<int*>(shin + field::bodyPart::role) != 0x10) return kickBodyFail(6);
        const auto thigh = kickParentOf(shin);
        if (!kickOwned(thigh, root) || kickParentOf(thigh) != pelvis) return kickBodyFail(7);
        uintptr_t foot = 0;
        for (int i = 1; i < count; ++i) {
            const auto part = *reinterpret_cast<uintptr_t*>(parts + size_t(i) * 8);
            if (kickOwned(part, root) && kickParentOf(part) == shin && *reinterpret_cast<int*>(part + field::bodyPart::role) == 0x13) {
                if (foot) return kickBodyFail(8);
                foot = part;
            }
        }
        if (!foot || !plain(thigh) || !plain(shin) || !plain(foot)) return kickBodyFail(9);
        KickPart t{}, s{}, f{};
        if (!take(t, thigh) || !take(s, shin) || !take(f, foot)) return kickBodyFail(10);
        const auto hip = fp::xr::rotate(fp::xr::conjugate(b.pelvis.pose.orientation), fp::xr::sub(t.pose.position, b.pelvis.pose.position));
        if (std::abs(hip.x) < .02f) return kickBodyFail(11);
        const unsigned side = hip.x > 0 ? 0 : 1; // positive native X is left
        if (seen & (1u << side)) return kickBodyFail(12);
        seen |= 1u << side;
        b.thigh[side] = t;
        b.shin[side] = s;
        b.foot[side] = f;
        b.supported[side] = kickShinSupported(shin);
    }
    if (seen != 3) return kickBodyFail(13);
    // The hands' slots and the hips' sides must agree on which side is left.
    return fp::xr::dot(fp::xr::sub(b.upper[0].pose.position, b.upper[1].pose.position), fp::xr::sub(b.thigh[0].pose.position, b.thigh[1].pose.position)) > 0 ||
           kickBodyFail(14);
}

// One kick, from the step the game's kick clip starts to the step it ends (pose-builder thread).
struct KickRun {
    bool active = false, driving = false, blendedIn = false, fell = false;
    uintptr_t root = 0, scene = 0;
    int clip = -1, nativeLeg = -1;
    bool mirror = false;
    kk::Mode mode = kk::Mode::full;
    kk::Heading heading{};
    float lastProgress = -1, lastWeight = 0, nativePeak = -1, nativePeakProgress = -1;
    uint64_t endedAt = 0;
};
static thread_local KickRun kickRun;

static void kickRunEnd(KickRun& run) {
    if (run.driving) {
        if (run.lastWeight > .05f) kickCutShort.fetch_add(1, std::memory_order_relaxed);
        else kickBlendedOut.fetch_add(1, std::memory_order_relaxed);
        if (run.nativeLeg >= 0) (run.nativeLeg == (run.mirror ? 0 : 1) ? kickNativeLegAgree : kickNativeLegDisagree).fetch_add(1, std::memory_order_relaxed);
    }
    if (run.nativePeakProgress >= 0) kickNativePeakMilli.store(int(run.nativePeakProgress * 1000), std::memory_order_relaxed);
    const bool driving = run.driving, fell = run.fell;
    run = {};
    run.driving = driving; // remembered for the fall watch after the kick
    run.fell = fell;
    run.endedAt = GetTickCount64();
}
// A stagger during an animated kick, or within 1.5 s after it: the game plays its recover motion 3, as it does when
// balance runs low (169C5C) or at death (17B26A). (The state word is 0x8000 throughout motion 0xD, 16B09C, so it
// cannot tell a fall.)
static void kickWatchFall(uintptr_t root, KickRun& run) {
    if (!run.driving || run.fell) return;
    if (!run.active && GetTickCount64() - run.endedAt > 1500) { run.driving = false; return; }
    if (*reinterpret_cast<int*>(root + field::character::motion) == 3) {
        run.fell = true;
        kickStaggered.fetch_add(1, std::memory_order_relaxed);
    }
}

static kk::WeaponClass kickClassOf(uintptr_t root) {
    return kk::classify(*reinterpret_cast<int*>(root + field::character::weaponCount), *reinterpret_cast<unsigned*>(root + field::character::gripFlags),
                        *reinterpret_cast<uintptr_t*>(root + field::character::shieldPart) != 0,
                        *reinterpret_cast<uintptr_t*>(root + field::character::specialHeldPart) != 0);
}
// The game's own stance at the start of each kick, per class (status: compares the hold with the clips'): hands
// from the mid-hip in cm (across to the left, up, forward), which foot leads, and the shoulders' turn from the hips.
struct KickStance { std::atomic<int> v[8]{}; std::atomic<bool> seen{false}; };
static KickStance kickStances[size_t(kk::WeaponClass::count)];
static void kickCaptureStance(uintptr_t root, const KickBody& body) {
    const auto facing = reinterpret_cast<const float*>(root + field::character::facing);
    const auto h = kk::headingFrom({facing[0], facing[1], facing[2]}, fp::xr::sub(body.thigh[0].pose.position, body.thigh[1].pose.position));
    if (!h.valid) return;
    const auto mid = fp::xr::scale(fp::xr::add(body.thigh[0].pose.position, body.thigh[1].pose.position), .5f);
    auto& s = kickStances[size_t(kickClassOf(root))];
    for (unsigned side = 0; side < 2; ++side) {
        const auto d = fp::xr::sub(body.hand[side].pose.position, mid);
        s.v[side * 3 + 0] = int(std::lround(fp::xr::dot(d, h.left) * 100));
        s.v[side * 3 + 1] = int(std::lround(fp::xr::dot(d, h.up) * 100));
        s.v[side * 3 + 2] = int(std::lround(fp::xr::dot(d, h.forward) * 100));
    }
    s.v[6] = fp::xr::dot(fp::xr::sub(body.foot[0].pose.position, body.foot[1].pose.position), h.forward) > 0 ? 0 : 1;
    const auto across = [&](XrVector3f v) { return std::atan2(fp::xr::dot(v, h.forward), fp::xr::dot(v, h.left)); };
    s.v[7] = int(std::lround((across(fp::xr::sub(body.upper[0].pose.position, body.upper[1].pose.position)) -
                              across(fp::xr::sub(body.thigh[0].pose.position, body.thigh[1].pose.position))) * 57.2958f));
    s.seen = true;
}
// The rows of one step: every part's target from a frame of a clip in world axes (the kick's or the jump's), the whole
// body or the hips and legs only. hip: where the mid-hip should be (the kick), or nullptr to leave the hips' position
// to the game (the jump: physics alone lifts the body). heldArms counts the steps an arm with something in its hand
// was driven (status only).
static void kickPlanRows(uintptr_t root, const KickBody& body, const kk::Frame& f, kk::Mode mode, float weight, const XrVector3f* hip,
                         std::atomic<unsigned>& heldArms) {
    KickScope& scope = kickScope;
    scope = {};
    scope.root = root;
    auto add = [&](const KickPart& p, uintptr_t parent, KickSite site, XrQuaternionf world, float w) {
        if (scope.count < scope.rows.size()) scope.rows[scope.count++] = {p.part, parent, site, world, {}, false, w};
    };
    auto across = [](const KickPart* pair) { return fp::xr::sub(pair[0].pose.position, pair[1].pose.position); };
    const XrVector3f midHip = fp::xr::scale(fp::xr::add(body.thigh[0].pose.position, body.thigh[1].pose.position), .5f);
    const XrVector3f hipsAcross = across(body.thigh), shouldersAcross = across(body.upper);
    const XrVector3f pelvisUp = fp::xr::sub(body.chest.pose.position, midHip), chestUp = fp::xr::sub(body.head.pose.position, body.chest.pose.position);
    // The hips: turned like the clip's, and moved to `hip` (the mid-hip wanted) when there is one.
    KickRow pelvisRow{body.pelvis.part, root, KickSite::pelvis,
                      kk::frameTarget(body.pelvis.pose.orientation, pelvisUp, hipsAcross, f.v[kk::pelvisUp], f.v[kk::pelvisAcross]),
                      hip ? fp::xr::add(body.pelvis.pose.position, fp::xr::sub(*hip, midHip)) : XrVector3f{}, hip != nullptr, weight};
    scope.rows[scope.count++] = pelvisRow;
    if (mode == kk::Mode::full) {
        const auto bellyUp = fp::xr::add(kk::normalised(pelvisUp), kk::normalised(chestUp)), bellyAcross = fp::xr::add(kk::normalised(hipsAcross), kk::normalised(shouldersAcross));
        add(body.belly, body.pelvis.part, KickSite::motor,
            kk::frameTarget(body.belly.pose.orientation, bellyUp, bellyAcross, fp::xr::add(f.v[kk::pelvisUp], f.v[kk::chestUp]),
                            fp::xr::add(f.v[kk::pelvisAcross], f.v[kk::chestAcross])), weight);
        const auto chestQ = kk::frameTarget(body.chest.pose.orientation, chestUp, shouldersAcross, f.v[kk::chestUp], f.v[kk::chestAcross]);
        add(body.chest, body.belly.part, KickSite::motor, chestQ, weight);
        const auto chestTurn = fp::xr::multiply(chestQ, fp::xr::conjugate(body.chest.pose.orientation));
        add(body.head, body.chest.part, KickSite::motor, fp::xr::multiply(chestTurn, body.head.pose.orientation), weight);
        const unsigned shoulderSlot[2] = {kk::leftShoulder, kk::rightShoulder}, upperSlot[2] = {kk::leftUpperArm, kk::rightUpperArm},
                       foreSlot[2] = {kk::leftForearm, kk::rightForearm};
        const auto held = reinterpret_cast<const uintptr_t*>(root + field::character::heldItems);
        for (unsigned side = 0; side < 2; ++side) {
            if (held[1 - side]) heldArms.fetch_add(1, std::memory_order_relaxed); // played fully (Hunter): status only
            const float w = weight;
            add(body.shoulder[side], body.chest.part, KickSite::motor,
                kk::boneTarget(body.shoulder[side].pose.orientation, fp::xr::sub(body.upper[side].pose.position, body.shoulder[side].pose.position), f.v[shoulderSlot[side]]), w);
            add(body.upper[side], body.shoulder[side].part, KickSite::motor,
                kk::boneTarget(body.upper[side].pose.orientation, fp::xr::sub(body.fore[side].pose.position, body.upper[side].pose.position), f.v[upperSlot[side]]), w);
            const auto foreQ = kk::boneTarget(body.fore[side].pose.orientation, fp::xr::sub(body.hand[side].pose.position, body.fore[side].pose.position), f.v[foreSlot[side]]);
            add(body.fore[side], body.upper[side].part, KickSite::motor, foreQ, w);
            add(body.hand[side], body.fore[side].part, KickSite::motor,
                fp::xr::multiply(fp::xr::multiply(foreQ, fp::xr::conjugate(body.fore[side].pose.orientation)), body.hand[side].pose.orientation), w);
        }
    }
    const unsigned thighSlot[2] = {kk::leftThigh, kk::rightThigh}, shinSlot[2] = {kk::leftShin, kk::rightShin};
    for (unsigned side = 0; side < 2; ++side) {
        const auto thighQ = kk::boneTarget(body.thigh[side].pose.orientation, fp::xr::sub(body.shin[side].pose.position, body.thigh[side].pose.position), f.v[thighSlot[side]]);
        const auto shinQ = kk::boneTarget(body.shin[side].pose.orientation, fp::xr::sub(body.foot[side].pose.position, body.shin[side].pose.position), f.v[shinSlot[side]]);
        add(body.thigh[side], body.pelvis.part, KickSite::motor, thighQ, weight);
        add(body.shin[side], body.thigh[side].part, KickSite::motor, shinQ, weight);
        add(body.foot[side], body.shin[side].part, KickSite::foot,
            fp::xr::multiply(fp::xr::multiply(shinQ, fp::xr::conjugate(body.shin[side].pose.orientation)), body.foot[side].pose.orientation), weight);
    }
}
static bool kickPlan(uintptr_t root) {
    KickRun& run = kickRun;
    const uintptr_t scene = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene));
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    if (run.active && (motion != kickMotion || run.root != root || run.scene != scene)) kickRunEnd(run);
    kickWatchFall(root, run);
    if (motion != kickMotion) return false;
    const float progress = *reinterpret_cast<float*>(root + field::character::motionProgress);
    if (!std::isfinite(progress)) return false;
    if (run.active && progress + .25f < run.lastProgress) kickRunEnd(run); // the clip restarted
    KickBody body{};
    if (!kickReadBody(root, body)) {
        if (!run.active || run.lastProgress < 0) kickRefusedGraph.fetch_add(1, std::memory_order_relaxed);
        run.lastProgress = progress;
        return false;
    }
    if (!run.active) {
        const bool fell = run.fell && run.driving;
        run = {};
        run.fell = fell;
        run.active = true;
        run.root = root;
        run.scene = scene;
        kickCaptureStance(root, body); // every style: the game's own stance as the kick starts
    }
    run.lastProgress = progress;
    // The game's own kicking leg: the first one it lifts while the other stands (status: does ours agree).
    if (run.nativeLeg < 0 && body.supported[0] != body.supported[1]) run.nativeLeg = body.supported[0] ? 1 : 0;
    if (!run.driving) { // the native foot's highest point, for tuning the hit progress
        for (unsigned side = 0; side < 2; ++side) {
            const float lift = body.foot[side].pose.position.y - body.foot[1 - side].pose.position.y;
            if (lift > run.nativePeak) { run.nativePeak = lift; run.nativePeakProgress = progress; }
        }
    }
    if (kickStyle.load(std::memory_order_relaxed) != 1 || !kickDriveReady.load(std::memory_order_relaxed) || kickDriveFault.load(std::memory_order_relaxed))
        return false;
    if (run.clip == -1) { // first step of this kick: weapon class, clip, heading and stance (-2: refused for this kick)
        run.clip = -2;
        const auto facing = reinterpret_cast<const float*>(root + field::character::facing);
        run.heading = kk::headingFrom({facing[0], facing[1], facing[2]}, fp::xr::sub(body.thigh[0].pose.position, body.thigh[1].pose.position));
        if (!run.heading.valid) { kickRefusedHeading.fetch_add(1, std::memory_order_relaxed); return false; }
        const auto weaponClass = kickClassOf(root);
        const auto rule = kk::classRules[size_t(weaponClass)];
        const int pick = kk::nextClip(kk::clips, kk::clipCount, rule.set, kickRotation[rule.set]);
        if (pick < 0) return false;
        run.clip = kickRotation[rule.set] = pick;
        run.mode = rule.mode;
        const bool leftLeads = fp::xr::dot(fp::xr::sub(body.foot[0].pose.position, body.foot[1].pose.position), run.heading.forward) > 0;
        run.mirror = kk::mirrorFor(kk::clips[run.clip], leftLeads);
        kickLastClip = run.clip;
        kickLastClass = int(weaponClass);
        kickLastMode = int(rule.mode);
        kickLastMirror = run.mirror ? 1 : 0;
        kickAnimated.fetch_add(1, std::memory_order_relaxed);
    }
    if (run.clip < 0) return false;
    const float weight = kk::weightAt(progress);
    run.lastWeight = weight;
    if (weight <= 0) return false;
    run.driving = true;
    if (weight > .99f && !run.blendedIn) { run.blendedIn = true; kickBlendedIn.fetch_add(1, std::memory_order_relaxed); }
    const auto& clip = kk::clips[run.clip];
    const kk::Frame f = kk::worldFrame(clip, run.heading, progress, run.mirror);
    // The hips: carried over the supporting foot by the clip's hip travel.
    const unsigned support = run.mirror ? 1 : 0;
    const XrVector3f midHip = fp::xr::scale(fp::xr::add(body.thigh[0].pose.position, body.thigh[1].pose.position), .5f);
    const XrVector3f footPos = body.foot[support].pose.position;
    const float legMetres = fp::xr::length(fp::xr::sub(body.shin[support].pose.position, body.thigh[support].pose.position)) +
                            fp::xr::length(fp::xr::sub(footPos, body.shin[support].pose.position));
    const auto hip = kk::hipTarget(footPos, f.v[kk::hipOffset], clip.legLengthCm, legMetres, midHip);
    kickPlanRows(root, body, f, run.mode, weight, &hip, kickHeldArms);
    kickSteps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// The Animated jump (jump_warp.h), the same overlay on the same body read: motions.inl runs the warp on the game
// thread and publishes the clip, frame, weight and tuck; here each step turns them into targets, never the hips'
// position (physics alone lifts the body). The heading and the mirror are taken on the jump's first step. A body
// refused on that step refuses the jump's clip, and motions.inl, told through jumpDriveRefusedSerial, starts the push
// at once rather than waiting out a windup nobody sees.
static bool jumpDriveAvailable() {
    return kickStyle.load(std::memory_order_relaxed) == 1 && kickDriveReady.load(std::memory_order_relaxed) &&
           !kickDriveFault.load(std::memory_order_relaxed);
}
struct JumpRun { unsigned serial = 0; bool planned = false, refused = false, mirror = false; kk::Mode mode = kk::Mode::full; kk::Heading heading{}; };
static thread_local JumpRun jumpRun;
static std::atomic<unsigned> jumpDriveSteps{0}, jumpRefusedBody{0}, jumpRefusedHeading{0}, jumpHeldArms{0};
static std::atomic<int> jumpLastMirror{-1};
static bool jumpPlan(uintptr_t root) {
    if (!jumpAnimDrive.load() || !jumpDriveAvailable()) return false;
    const unsigned serial = jumpAnimSerial.load();
    const int clipIndex = jumpAnimClip.load();
    if (!serial || clipIndex < 0 || clipIndex >= jw::clipCount) return false;
    JumpRun& run = jumpRun;
    if (run.serial != serial) {
        run = {};
        run.serial = serial;
    }
    if (run.refused) return false;
    auto refuse = [&](std::atomic<unsigned>& counter) {
        run.refused = true;
        counter.fetch_add(1, std::memory_order_relaxed);
        jumpDriveRefusedSerial.store(serial);
        return false;
    };
    KickBody body{};
    if (!kickReadBody(root, body)) return run.planned ? false : refuse(jumpRefusedBody);
    const auto& clip = jw::clips[clipIndex];
    if (!run.planned) {
        const auto facing = reinterpret_cast<const float*>(root + field::character::facing);
        run.heading = kk::headingFrom({facing[0], facing[1], facing[2]}, fp::xr::sub(body.thigh[0].pose.position, body.thigh[1].pose.position));
        if (!run.heading.valid) return refuse(jumpRefusedHeading);
        const bool leftLeads = fp::xr::dot(fp::xr::sub(body.foot[0].pose.position, body.foot[1].pose.position), run.heading.forward) > 0;
        const int supported = body.supported[0] != body.supported[1] ? (body.supported[0] ? 0 : 1) : -1;
        run.mirror = jw::mirrorFor(clip, leftLeads, clipIndex == jw::travellingClip ? supported : -1);
        run.mode = kk::classRules[size_t(kickClassOf(root))].mode; // a polearm or carried object keeps the game's arms
        run.planned = true;
        jumpLastMirror = run.mirror ? 1 : 0;
    }
    const float weight = jumpAnimWeight.load();
    if (!(weight > 0)) return false;
    const kk::Frame f = jw::worldFrame(clip, run.heading, jumpAnimFrame.load(), jumpAnimTuck.load(), run.mirror);
    kickPlanRows(root, body, f, run.mode, std::fmin(weight, 1.f), nullptr, jumpHeldArms);
    jumpDriveSteps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static bool kickPlanSafe(uintptr_t root) {
    try { return kickPlan(root) || jumpPlan(root); }
    catch (const std::exception&) { kickScope = {}; return false; }
}
// Before the game's pose builder, for the player only (input.inl poseBuilderOwned), when the VR drive did not plan:
// the kick's targets, else the jump's.
static bool kickPoseBegin(uintptr_t root) {
    kickScopeOpen = false;
    __try {
        if (!root || !readable(root, 0x1300)) return false;
        kickScopeOpen = kickPlanSafe(root);
        return kickScopeOpen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kickDriveFault = true;
        kickScopeOpen = false;
        return false;
    }
}
static void kickPoseEnd() {
    kickScopeOpen = false;
    kickScope.count = 0;
}

// The composition for one planned part at one reviewed call site: the game's own local target blended toward the
// clip's by the row's weight. true when it composed; false leaves the call to the caller.
static bool kickComposeImpl(uintptr_t caller, float* out, float* local, const float* parent, KickCompose original) {
    const KickScope& scope = kickScope;
    for (unsigned i = 0; i < scope.count; ++i) {
        const KickRow& row = scope.rows[i];
        const bool pelvis = row.site == KickSite::pelvis, foot = row.site == KickSite::foot;
        const auto site = pelvis ? fp::sig::xrPelvisComposeReturn : (foot ? fp::sig::xrFootComposeReturn : fp::sig::xrMotorComposeReturn);
        const auto output = row.part + (pelvis || foot ? field::bodyPart::targetPose : field::bodyPart::animationPose);
        const auto parentAddress = pelvis ? scope.root + field::character::balanceFrame : row.parent + (foot ? field::bodyPart::targetPose : field::bodyPart::animationPose);
        if (caller != gameAt(site) || reinterpret_cast<uintptr_t>(out) != output || reinterpret_cast<uintptr_t>(parent) != parentAddress) continue;
        if (!kickOwned(row.part, scope.root) || kickParentOf(row.part) != (pelvis ? row.part : row.parent)) return false;
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        const auto at = reinterpret_cast<uintptr_t>(local);
        if (pelvis ? at != row.part + field::bodyPart::animationPose : (at < low || at > high - 48)) return false;
        if (!readable(at, 48) || !readable(parentAddress, 48)) return false;
        fp::xr::NativePose scratch{}, frame{}, p{};
        memcpy(frame.data(), reinterpret_cast<void*>(scope.root + field::character::bodyFrame), 48);
        memcpy(p.data(), parent, 48);
        memcpy(scratch.data(), local, 48);
        const auto frameQ = fp::xr::nativePoseMetres(frame, kickUnitsPerMetre).orientation;
        const auto parentPose = fp::xr::nativePoseMetres(p, kickUnitsPerMetre);
        fp::xr::NativePose nativeRotation = scratch;
        nativeRotation[9] = nativeRotation[10] = nativeRotation[11] = 0;
        const auto nativeLocal = fp::xr::nativePoseMetres(nativeRotation, kickUnitsPerMetre).orientation;
        const auto wantedLocal = fp::xr::multiply(fp::xr::conjugate(parentPose.orientation), fp::xr::multiply(fp::xr::conjugate(frameQ), row.world));
        const auto axes = fp::xr::nativeItemPose({kk::blend(nativeLocal, wantedLocal, row.weight), {}});
        memcpy(scratch.data(), axes.data(), 36);
        if (row.positioned) {
            const auto node = *reinterpret_cast<uintptr_t*>(scope.root + field::character::sceneNode);
            if (!readable(node, field::sceneNode::sector + 8)) return false;
            const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
            if (!readable(sector, field::sector::origin + 12)) return false;
            XrVector3f origin{};
            memcpy(&origin, reinterpret_cast<void*>(sector + field::sector::origin), 12);
            auto worldFrame = fp::xr::nativePoseMetres(frame, kickUnitsPerMetre);
            worldFrame.position = fp::xr::add(worldFrame.position, fp::xr::scale(origin, 1 / kickUnitsPerMetre));
            const auto wanted = fp::xr::compose(fp::xr::inverse(parentPose), fp::xr::compose(fp::xr::inverse(worldFrame), XrPosef{row.world, row.position})).position;
            for (unsigned k = 0; k < 3; ++k)
                scratch[9 + k] = local[9 + k] + ((&wanted.x)[k] * kickUnitsPerMetre - local[9 + k]) * row.weight;
        }
        original(out, scratch.data(), parent);
        if (!pelvis) memcpy(local, scratch.data(), row.positioned ? 48 : 36); // the joint-anchor builder reads it too
        kickRows.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
static bool kickComposeSafe(uintptr_t caller, float* out, float* local, const float* parent, KickCompose original) {
    try { return kickComposeImpl(caller, out, local, parent, original); }
    catch (const std::exception&) { return false; }
}
static bool kickComposeFor(uintptr_t caller, float* out, float* local, const float* parent, KickCompose original) {
    if (!kickScopeOpen || !kickScope.count || !original || !active || stopping) return false;
    __try { return kickComposeSafe(caller, out, local, parent, original); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        kickDriveFault = true;
        kickScopeOpen = false;
        return false;
    }
}
static void kickCompose(float* out, float* local, const float* parent) {
    if (!kickComposeFor(reinterpret_cast<uintptr_t>(_ReturnAddress()), out, local, parent, kickComposeOriginal)) kickComposeOriginal(out, local, parent);
}
// After the stereo hooks: the VR drive may already own the xrCompose detour (then it chains to kickComposeFor).
static bool createKickDriveHook() {
    kickDriveReady = false;
    // kickClasses pins the game's weapon-class bits the clip sets are chosen by: changed, and the kick stays Classic.
    if (!layout.optionalGroupAvailable("kick") || !layout.optionalGroupAvailable("xrDrive") || !layout.optionalGroupAvailable("kickClasses")) return true;
    if (!kickComposeChained &&
        MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrCompose)), reinterpret_cast<void*>(kickCompose), reinterpret_cast<void**>(&kickComposeOriginal)) != MH_OK)
        return false;
    kickDriveReady = true;
    return true;
}
