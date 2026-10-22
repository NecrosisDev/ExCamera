// Read-only equipped-grip admission for the XR pose owner; it adds no hook of its own.
static thread_local xd::EquippedDrive trackedEquippedDrive;
static bool trackedWeaponReady(uintptr_t root) {
    if (!trackedWeaponsRequested || !inputEnabled || cursorFreeNow() || (*reinterpret_cast<unsigned*>(gameAt(fp::sig::buttonWord)) & 3) || !readable(root, 0x1300) || *reinterpret_cast<uintptr_t*>(root + field::character::shieldPart) || *reinterpret_cast<uintptr_t*>(root + field::character::specialHeldPart) ||
        *(unsigned char*)(root + field::character::defenseActive) || *reinterpret_cast<uintptr_t*>(root + field::character::actionCallback) || (*reinterpret_cast<unsigned*>(root + field::character::swingBits) & 0xB0012)) return false;
    const auto drag = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::dragManager));
    if (readable(drag, field::drag::object + 8) && (*reinterpret_cast<unsigned*>(drag + field::drag::flags) || *reinterpret_cast<uintptr_t*>(drag + field::drag::object))) return false;
    const auto state = *reinterpret_cast<unsigned*>(root + field::character::stateFlags);
    const int motion = *reinterpret_cast<int*>(root + field::character::motion);
    return (state == 0 || state == 8) && (!*(unsigned char*)(root + field::character::combatStance) || motion == 3 || motion == 4);
}
// A grip anchor is the barycentric point of the part's rest tetrahedron the native joint row names.
static XrVector3f trackedGripPoint(uintptr_t coefficients, uintptr_t part) {
    if (!readable(coefficients, 16) || !readable(part + field::bodyPart::restTetra, 48)) throw std::runtime_error("grip tetra storage");
    const auto* weights = reinterpret_cast<const float*>(coefficients);
    const auto* tetra = reinterpret_cast<const float*>(part + field::bodyPart::restTetra);
    float sum = 0;
    XrVector3f point{};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(weights[i]) || std::abs(weights[i]) > 16) throw std::runtime_error("grip coefficients");
        sum += weights[i];
        point.x += weights[i] * tetra[i * 3];
        point.y += weights[i] * tetra[i * 3 + 1];
        point.z += weights[i] * tetra[i * 3 + 2];
    }
    if (std::abs(sum - 1) > .02f || !xd::finite(point)) throw std::runtime_error("invalid grip anchor");
    return xd::scale(point, 1 / stereoUnitsPerMetre);
}
static bool trackedWeaponCurrent(uintptr_t root, const TrackedWeaponStamp& stamp) {
    auto refuse = [] { trackedEquippedDrive.reset(); return false; };
    if (!stamp.weapon) return true;
    if (!trackedWeaponReady(root) || *reinterpret_cast<int*>(root + field::character::weaponCount) != 1 || *reinterpret_cast<uintptr_t*>(root + field::character::weapons) != stamp.weapon ||
        *reinterpret_cast<unsigned*>(root + field::character::gripFlags) != stamp.grip || !trackedPart(stamp.weapon, root) || !readable(stamp.weapon, field::bodyPart::weaponData + 8) ||
        *reinterpret_cast<uintptr_t*>(stamp.weapon + field::bodyPart::parentPart) != stamp.parent || *reinterpret_cast<uintptr_t*>(stamp.weapon + field::bodyPart::joints) != stamp.joints || *reinterpret_cast<int*>(stamp.weapon + field::bodyPart::jointCount) != stamp.count) return refuse();
    const auto node = *reinterpret_cast<uintptr_t*>(stamp.weapon + field::bodyPart::meshNode), data = *reinterpret_cast<uintptr_t*>(stamp.weapon + field::bodyPart::weaponData);
    if (!trackedPart(stamp.parent, root) || !readable(node, field::sceneNode::parent + 8) || !readable(data, 4) || (*reinterpret_cast<unsigned*>(data) & 0xF0) != stamp.grip ||
        !(*reinterpret_cast<unsigned*>(stamp.weapon + field::bodyPart::flags) & 0x2000) || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) != *reinterpret_cast<uintptr_t*>(stamp.parent + field::bodyPart::meshNode) ||
        *reinterpret_cast<uintptr_t*>(stamp.weapon + field::bodyPart::rootPart) != *reinterpret_cast<uintptr_t*>(stamp.parent + field::bodyPart::rootPart)) return refuse();
    for (unsigned side = 0; side < 2; ++side) {
        const auto hand = *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8), held = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8);
        if (hand != stamp.hands[side] || held != ((stamp.required & (1u << side)) ? stamp.weapon : 0)) return refuse();
        if (stamp.rows[side] && (!readable(stamp.rows[side], 0x68) || memcmp(reinterpret_cast<void*>(stamp.rows[side]), stamp.bytes[side].data(), 0x68))) return refuse();
    }
    return true;
}
// Weapon weight: the hands follow the controllers through a lag set by the weapon's own effective heft
// (the game's weapon values v[10] = 0.1 + 0.4 heft + 0.6 (1 - balance) + ..., at the weapon body's values
// block +0x28), so a heavy weapon trails a fast swing and a light one does not; two hands share the load.
// Filtered once per drive frame, whatever number of reads the frame gets. Row VR: weapon weight.
struct HeftLag { uintptr_t weapon = 0; uint64_t tick = 0; std::array<bool, 2> live{}; std::array<XrPosef, 2> pose{}; };
static HeftLag trackedHeftLag;
static constexpr float heftLagBase = .012f, heftLagPerHeft = .07f, heftLagMax = .12f; // seconds of time constant
static float trackedWeaponHeft(uintptr_t weapon) {
    if (!readable(weapon, field::bodyPart::weaponData + 8)) return 0;
    const uintptr_t values = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::weaponData);
    if (!readable(values, 0x70) || (*reinterpret_cast<unsigned*>(values) & 0x80000000u)) return 0; // a shield's short block (0x80000000 marker): no heft row
    const float heft = *reinterpret_cast<float*>(values + 10 * 4);
    return std::isfinite(heft) ? (std::min)(2.f, (std::max)(0.f, heft)) : 0.f;
}
static void trackedHeftFilter(uintptr_t weapon, const TrackedDriveFrame& frame, xd::EquippedInput& input) {
    HeftLag& lag = trackedHeftLag;
    if (!vrWeaponWeight.load(std::memory_order_relaxed)) { lag = {}; return; }
    if (lag.weapon != weapon) lag = {weapon};
    const float heft = trackedWeaponHeft(weapon);
    const float tau = (std::min)(heftLagMax, heftLagBase + heftLagPerHeft * (std::max)(0.f, heft - .3f)) * (input.twoHanded ? .6f : 1.f);
    const bool fresh = frame.tick != lag.tick;
    const float dt = lag.tick && fresh ? (std::min)(.1f, static_cast<float>(frame.tick - lag.tick) * .001f) : 0.f;
    if (fresh) lag.tick = frame.tick;
    for (unsigned side = 0; side < 2; ++side) {
        if (!input.tracked[side]) { lag.live[side] = false; continue; }
        if (!lag.live[side]) { lag.pose[side] = input.controllers[side]; lag.live[side] = true; continue; }
        if (fresh && dt > 0) {
            const float a = 1.f - std::exp(-dt / (std::max)(tau, 1e-3f));
            XrPosef& p = lag.pose[side];
            const XrPosef& c = input.controllers[side];
            p.position = xd::add(p.position, xd::scale(xd::sub(c.position, p.position), a));
            XrQuaternionf q = c.orientation; // the shorter way round, then a normalised blend
            if (p.orientation.x * q.x + p.orientation.y * q.y + p.orientation.z * q.z + p.orientation.w * q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
            XrQuaternionf r{p.orientation.x + (q.x - p.orientation.x) * a, p.orientation.y + (q.y - p.orientation.y) * a,
                            p.orientation.z + (q.z - p.orientation.z) * a, p.orientation.w + (q.w - p.orientation.w) * a};
            const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (n > 1e-6f && xd::finite(p.position)) p.orientation = {r.x / n, r.y / n, r.z / n, r.w / n};
            else p = c;
        }
        input.controllers[side] = lag.pose[side];
    }
}
static bool trackedWeaponRead(uintptr_t root, const TrackedDriveFrame& frame, TrackedWeaponStamp& stamp, xd::EquippedInput& input) {
    if (!readable(root, 0x1300) || *reinterpret_cast<int*>(root + field::character::weaponCount) != 1) return false;
    const auto grip = *reinterpret_cast<unsigned*>(root + field::character::gripFlags);
    if (grip != 0x10 && grip != 0x20 && grip != 0x30) return false;
    const auto weapon = *reinterpret_cast<uintptr_t*>(root + field::character::weapons);
    if (!trackedPart(weapon, root) || !readable(weapon, field::bodyPart::weaponData + 8) || !(*reinterpret_cast<unsigned*>(weapon + field::bodyPart::flags) & 0x2000)) return false;
    const auto data = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::weaponData);
    if (!readable(data, 4) || (*reinterpret_cast<unsigned*>(data) & 0xF0) != grip) return false;
    stamp = {};
    stamp.weapon = weapon;
    stamp.grip = grip;
    stamp.parent = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::parentPart);
    stamp.count = *reinterpret_cast<int*>(weapon + field::bodyPart::jointCount);
    stamp.joints = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::joints);
    if (stamp.count < 1 || stamp.count > 32 || !readable(stamp.joints, static_cast<size_t>(stamp.count) * 0x68)) return false;
    input = {};
    input.weapon = trackedPartPose(weapon);
    input.twoHanded = grip != 0x10;
    bool primary = false;
    for (unsigned side = 0; side < 2; ++side) {
        const auto hand = *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8);
        if (!trackedPart(hand, root) || hand == weapon) return false;
        stamp.hands[side] = hand;
        input.hands[side] = trackedPartPose(hand);
        if (hand == stamp.parent) { input.primary = side; primary = true; }
        const auto held = *reinterpret_cast<uintptr_t*>(root + field::character::heldItems + (1 - side) * 8);
        if (held && held != weapon) return false;
        if (held == weapon) stamp.required |= 1u << side;
        input.tracked[side] = frame.valid[xd::leftHand + side];
        input.pressed[side] = frame.pressed[side];
        if (input.tracked[side]) input.controllers[side] = xd::nativePoseMetres(frame.poses[xd::leftHand + side], stereoUnitsPerMetre);
    }
    if (!primary || stamp.required != (input.twoHanded ? 3u : (1u << input.primary))) return false;
    trackedHeftFilter(weapon, frame, input);
    // The grip slide moves the grip points along the hilt, so a weapon sits where the player holds it.
    const float classSlide = static_cast<float>(vrGripSlideCm.load(std::memory_order_relaxed)) * .01f;
    // The haft line: the longest edge of the weapon's rest tetrahedron, in weapon space, through the primary grip.
    if (!input.twoHanded && (vrHaftGrip.load(std::memory_order_relaxed) || vrGripSlideCm.load(std::memory_order_relaxed) != 0) && readable(weapon + field::bodyPart::restTetra, 48)) { // the haft line serves the haft grip and the slide
        const auto* tetra = reinterpret_cast<const float*>(weapon + field::bodyPart::restTetra);
        float best = 0;
        XrVector3f axis{};
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned k = i + 1; k < 4; ++k) {
                const XrVector3f edge = {tetra[k * 3] - tetra[i * 3], tetra[k * 3 + 1] - tetra[i * 3 + 1], tetra[k * 3 + 2] - tetra[i * 3 + 2]};
                const float len = xd::length(edge);
                if (std::isfinite(len) && len > best) { best = len; axis = edge; }
            }
        if (best > 0) {
            input.axis = xd::scale(axis, 1 / best);
            input.axisLength = best / stereoUnitsPerMetre;
            input.haftAllowed = xd::finite(input.axis) && input.axisLength > .1f && input.axisLength < 3.f;
        }
    }
    const auto node = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::meshNode), parentNode = *reinterpret_cast<uintptr_t*>(stamp.parent + field::bodyPart::meshNode);
    if (!readable(node, field::sceneNode::parent + 8) || *reinterpret_cast<uintptr_t*>(node + field::sceneNode::parent) != parentNode ||
        *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::rootPart) != *reinterpret_cast<uintptr_t*>(stamp.parent + field::bodyPart::rootPart)) return false;
    for (int i = 0; i < stamp.count; ++i) {
        const auto row = stamp.joints + static_cast<size_t>(i) * 0x68;
        const auto flags = *reinterpret_cast<unsigned*>(row + field::gripJoint::flags);
        if (!(flags & 0x10000) || (flags & 0x100000)) continue;
        const auto connected = *reinterpret_cast<uintptr_t*>(row + field::gripJoint::connectedPart);
        for (unsigned side = 0; side < 2; ++side)
            if ((stamp.required & (1u << side)) && connected == stamp.hands[side] && !stamp.rows[side]) {
                input.anchors[side] = trackedGripPoint(row, weapon);
                const auto handPoint = trackedGripPoint(row + field::gripJoint::connectedAnchor, connected);
                if (xd::length(xd::sub(xd::materialPoint(input.weapon, input.anchors[side]), xd::materialPoint(input.hands[side], handPoint))) > .15f) return false;
                stamp.rows[side] = row;
                memcpy(stamp.bytes[side].data(), reinterpret_cast<void*>(row), 0x68);
            }
    }
    if (classSlide != 0) { // along the hilt: the authored anchors move together, so the hands keep their spacing
        XrVector3f along = input.twoHanded ? xd::sub(input.anchors[1 - input.primary], input.anchors[input.primary]) : input.axis;
        if (xd::length(along) > 1e-4f) {
            along = xd::scale(along, 1 / xd::length(along));
            for (unsigned side = 0; side < 2; ++side)
                if (stamp.required & (1u << side)) input.anchors[side] = xd::add(input.anchors[side], xd::scale(along, classSlide));
        }
    }
    return true;
}
static bool trackedWeaponPlan(uintptr_t root, const TrackedDriveFrame& frame, TrackedWeaponStamp& stamp, xd::EquippedTarget& target) {
    auto refuse = [] { trackedEquippedDrive.reset(); return false; };
    if (!trackedWeaponReady(root) || *reinterpret_cast<int*>(root + field::character::weaponCount) != 1) return refuse();
    xd::EquippedInput input{};
    if (!trackedWeaponRead(root, frame, stamp, input)) return refuse();
    uint64_t token = 1469598103934665603ull; // FNV-1a over the grip rows: a changed constraint is a new owner
    for (unsigned side = 0; side < 2; ++side)
        if (stamp.required & (1u << side)) {
            if (!stamp.rows[side]) return refuse();
            for (const auto byte : stamp.bytes[side]) token = (token ^ byte) * 1099511628211ull;
        }
    target = trackedEquippedDrive.step({root, frame.scene, frame.epoch, frame.stream, stamp.weapon, token, frame.inputGeneration}, input);
    return target.valid;
}
