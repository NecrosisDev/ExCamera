// Physical collisions of the tracked body enter the native impact path. Nothing is
// synthesised: no attack request, no controller velocity, no fabricated damage.
using PhysicalContact = void (*)(uintptr_t, uintptr_t, const float*, const float*, const float*);
using PhysicalCommit = void (*)(uintptr_t, uint32_t);
static PhysicalContact originalPhysicalContact = nullptr;
static PhysicalCommit originalPhysicalCommit = nullptr;
static void (*physicalImpact)(uintptr_t) = nullptr;
static std::atomic<bool> physicalContactsReady{false};
static std::atomic<unsigned> physicalInFlight{0};
static std::atomic<unsigned> physicalImpactCalls{0};
static bool trackedPhysicalDrained() { return physicalInFlight.load() == 0; }
struct BodyMotorStamp {
    uintptr_t root = 0, weapon = 0, scene = 0;
    uint64_t stream = 0, tick = 0, epoch = 0;
    uint32_t generation = 0;
};
static SRWLOCK bodyMotorLock = SRWLOCK_INIT;
static BodyMotorStamp bodyMotorStamp;
static void trackedBodyMotorClear(uintptr_t root) {
    if (!TryAcquireSRWLockExclusive(&bodyMotorLock)) return;
    if (bodyMotorStamp.root == root) bodyMotorStamp = {};
    ReleaseSRWLockExclusive(&bodyMotorLock);
}
static void trackedBodyMotorPublish(const TrackedMotorScope& scope) {
    if (!scope.bodyDriven) { trackedBodyMotorClear(scope.root); return; }
    if (!TryAcquireSRWLockExclusive(&bodyMotorLock)) return;
    bodyMotorStamp = {scope.root, scope.weapon.weapon, scope.scene, scope.stream, GetTickCount64(), scope.epoch, scope.inputGeneration};
    ReleaseSRWLockExclusive(&bodyMotorLock);
}
static bool physicalEffector(uintptr_t part, uintptr_t& root, uintptr_t& profile, bool& weapon) {
    root = readable(part, field::bodyPart::character + 8) ? *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) : 0;
    if (!physicalContactsReady || !trackedPart(part, root) || !trackedBodyOwns(root)) return false;
    TrackedDriveFrame frame{};
    if (!trackedDriveRead(root, frame) || !TryAcquireSRWLockShared(&bodyMotorLock)) return false;
    const auto motor = bodyMotorStamp;
    ReleaseSRWLockShared(&bodyMotorLock);
    if (motor.root != root || motor.scene != frame.scene || motor.epoch != frame.epoch || motor.stream != frame.stream || motor.generation != frame.inputGeneration || GetTickCount64() - motor.tick > 100) return false;
    if (!readable(part, field::bodyPart::weaponData + 8)) return false;
    const auto left = *reinterpret_cast<uintptr_t*>(root + field::character::handParts + 8), right = *reinterpret_cast<uintptr_t*>(root + field::character::handParts);
    weapon = part == motor.weapon && motor.weapon;
    const bool foot = *reinterpret_cast<int*>(part + field::bodyPart::role) == 0x13;
    if (!weapon && part != left && part != right && !foot) return false;
    if (weapon) {
        TrackedWeaponStamp stamp{};
        xd::EquippedInput input{};
        if (!trackedWeaponRead(root, frame, stamp, input) || stamp.weapon != part) return false;
        for (unsigned side = 0; side < 2; ++side)
            if ((stamp.required & (1u << side)) && !frame.pressed[side]) return false;
    }
    if (foot) {
        const auto shin = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::parentPart);
        if (!trackedPart(shin, root) || *reinterpret_cast<int*>(shin + field::bodyPart::role) != 0x10) return false;
        const int count = *reinterpret_cast<int*>(root + field::character::partCount);
        const auto parts = *reinterpret_cast<uintptr_t*>(root + field::character::parts);
        if (count < 8 || count > 64 || !readable(parts, static_cast<size_t>(count) * 8)) return false;
        bool found = false;
        for (int i = 0; i < count; ++i) found |= *reinterpret_cast<uintptr_t*>(parts + i * 8) == part;
        if (!found) return false;
    }
    // +6E0 can also hold the shorter shield data. A foot keeps its own pointer and borrows
    // a hand's unarmed profile for the impact; it is never read as a 0x70-byte weapon profile.
    profile = foot ? 0 : *reinterpret_cast<uintptr_t*>(part + field::bodyPart::weaponData);
    if (foot)
        for (const auto hand : {right, left})
            if (trackedPart(hand, root) && readable(hand, field::bodyPart::weaponData + 8)) {
                const auto candidate = *reinterpret_cast<uintptr_t*>(hand + field::bodyPart::weaponData);
                if (readable(candidate, 0x70) && !(*reinterpret_cast<unsigned*>(candidate) & 0xF0)) { profile = candidate; break; }
            }
    return readable(profile, 0x70) && (weapon || !(*reinterpret_cast<unsigned*>(profile) & 0xF0));
}
static uintptr_t physicalSector(uintptr_t part) {
    if (!readable(part, field::bodyPart::meshNode + 8)) return 0;
    const auto node = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::meshNode);
    return readable(node, field::sceneNode::bodyPart + 8) && *reinterpret_cast<uintptr_t*>(node + field::sceneNode::bodyPart) == part ? *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector) : 0;
}
struct PhysicalEvent {
    uintptr_t root = 0, part = 0, other = 0, otherRoot = 0, otherCharacter = 0, scene = 0, sector = 0;
    uint64_t stream = 0, tick = 0, epoch = 0;
    uint32_t generation = 0;
    std::array<float, 3> a{}, b{}, point{};
    XrVector3f velocity{}, relative{};
};
static SRWLOCK physicalQueueLock = SRWLOCK_INIT;
static std::array<PhysicalEvent, 16> physicalEvents;
static xd::PhysicalContactLedger physicalLedger;
static uint64_t physicalLedgerStream = 0;
static bool physicalVelocity(uintptr_t part, const float* point, XrVector3f& velocity) {
    if (!readable(part + field::bodyPart::points, 48) || !readable(part + field::bodyPart::velocityTetra, 48) || !readable(reinterpret_cast<uintptr_t>(point), 12)) return false;
    std::array<XrVector3f, 4> tetra{}, values{};
    memcpy(tetra.data(), reinterpret_cast<void*>(part + field::bodyPart::points), 48);
    memcpy(values.data(), reinterpret_cast<void*>(part + field::bodyPart::velocityTetra), 48);
    XrVector3f p{};
    memcpy(&p, point, 12);
    return xd::contactVelocity(tetra, values, p, velocity);
}
static void physicalContactQueue(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    uintptr_t root = 0, profile = 0;
    bool weapon = false;
    if (!physicalEffector(part, root, profile, weapon) || !readable(other, field::bodyPart::character + 8) ||
        *reinterpret_cast<uintptr_t*>(other + field::bodyPart::rootPart) == *reinterpret_cast<uintptr_t*>(part + field::bodyPart::rootPart) ||
        !readable(reinterpret_cast<uintptr_t>(a), 12) || !readable(reinterpret_cast<uintptr_t>(b), 12)) return;
    TrackedDriveFrame frame{};
    if (!trackedDriveRead(root, frame)) return;
    PhysicalEvent event{};
    event.sector = physicalSector(part);
    if (!event.sector || physicalSector(other) != event.sector || !physicalVelocity(part, point, event.velocity)) return;
    XrVector3f otherVelocity{};
    if (!physicalVelocity(other, point, otherVelocity)) return;
    event.relative = xd::sub(event.velocity, otherVelocity);
    event.root = root;
    event.part = part;
    event.other = other;
    event.stream = frame.stream;
    event.generation = frame.inputGeneration;
    event.scene = frame.scene;
    event.epoch = frame.epoch;
    event.tick = GetTickCount64();
    event.otherRoot = *reinterpret_cast<uintptr_t*>(other + field::bodyPart::rootPart);
    event.otherCharacter = *reinterpret_cast<uintptr_t*>(other + field::bodyPart::character);
    memcpy(event.a.data(), a, 12);
    memcpy(event.b.data(), b, 12);
    memcpy(event.point.data(), point, 12);
    if (!TryAcquireSRWLockExclusive(&physicalQueueLock)) return;
    if (physicalLedgerStream != event.stream) { physicalLedger.reset(); physicalEvents = {}; physicalLedgerStream = event.stream; }
    if (physicalLedger.accept(part, event.otherRoot, event.tick, xd::length(event.relative)))
        for (auto& slot : physicalEvents)
            if (!slot.root || event.tick - slot.tick > 100) { slot = event; break; }
    ReleaseSRWLockExclusive(&physicalQueueLock);
}
static void physicalContactQueueSafe(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    try { physicalContactQueue(part, other, a, b, point); } catch (const std::exception&) {}
}
static void trackedPhysicalContact(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    ++physicalInFlight;
    __try {
        originalPhysicalContact(part, other, a, b, point);
        __try { physicalContactQueueSafe(part, other, a, b, point); }
        __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; }
        bladeContactGuarded(part, other, a, b, point); // the blade behaviours, every character (blade.inl)
    } __finally { --physicalInFlight; }
}
// Native impact bookkeeping is borrowed for one event and restored byte for byte.
struct PhysicalBorrow {
    struct Field { uintptr_t address = 0; size_t size = 0; std::array<unsigned char, 48> saved{}; };
    std::array<Field, 10> fields{};
    unsigned count = 0;
    bool write(uintptr_t address, const void* value, size_t size) {
        if (count >= fields.size() || size > 48 || !readable(address, size)) return false;
        auto& field = fields[count++];
        field.address = address;
        field.size = size;
        memcpy(field.saved.data(), reinterpret_cast<void*>(address), size);
        memcpy(reinterpret_cast<void*>(address), value, size);
        return true;
    }
    void restore() {
        while (count) {
            auto& field = fields[--count];
            if (readable(field.address, field.size)) memcpy(reinterpret_cast<void*>(field.address), field.saved.data(), field.size);
        }
    }
};
static bool physicalImpactContext(const PhysicalEvent& event, PhysicalBorrow& borrow) {
    uintptr_t root = 0, profile = 0;
    bool weapon = false;
    TrackedDriveFrame frame{};
    if (!trackedDriveRead(event.root, frame) || frame.scene != event.scene || frame.epoch != event.epoch) return false;
    if (!physicalEffector(event.part, root, profile, weapon) || root != event.root || event.stream != trackedDriveGeneration.load() || event.generation != inputOwner.generation() ||
        GetTickCount64() - event.tick > 100 || *reinterpret_cast<int*>(root + field::character::impactKind) || !readable(event.other, field::bodyPart::character + 8) ||
        *reinterpret_cast<uintptr_t*>(event.other + field::bodyPart::rootPart) != event.otherRoot || *reinterpret_cast<uintptr_t*>(event.other + field::bodyPart::character) != event.otherCharacter ||
        event.otherRoot == *reinterpret_cast<uintptr_t*>(event.part + field::bodyPart::rootPart) || physicalSector(event.part) != event.sector || physicalSector(event.other) != event.sector) return false;
    const std::array<unsigned char, 40> emptyContact{};
    std::array<float, 8> velocity{};
    memcpy(velocity.data(), &event.velocity, 12);
    velocity[3] = xd::dot(event.velocity, event.velocity);
    const std::array<uintptr_t, 2> parts{event.part, 0};
    const int count = 1;
    const unsigned zero = 0;
    const unsigned flags = *reinterpret_cast<unsigned*>(event.part + field::bodyPart::flags) | 0x10001u;
    const unsigned grip = weapon ? *reinterpret_cast<unsigned*>(root + field::character::gripFlags) : 0;
    return borrow.write(root + field::character::impactKind, emptyContact.data(), 40) && borrow.write(root + field::character::impactVelocity, velocity.data(), 32) &&
           borrow.write(root + field::character::activePartCount, &count, 4) && borrow.write(root + field::character::activeParts, parts.data(), 16) &&
           borrow.write(root + field::character::swingWeapon, &event.part, 8) && borrow.write(root + field::character::gripFlags, &grip, 4) &&
           borrow.write(root + field::character::swingBits, &zero, 4) && borrow.write(event.part + field::bodyPart::flags, &flags, 4) && borrow.write(event.part + field::bodyPart::weaponData, &profile, 8);
}
static bool physicalImpactContextSafe(const PhysicalEvent& event, PhysicalBorrow& borrow) {
    try { return physicalImpactContext(event, borrow); } catch (const std::exception&) { return false; }
}
// A pulse sized by the impact: the contact speed (native units a second; a full swing is near 800)
// sets amplitude from a quarter to full and the duration from 40 to 100 ms. Unknown speed: full.
static float trackedImpactSpeed(uintptr_t root) {
    if (!readable(root + field::character::impactVelocity, 16)) return -1;
    const float squared = *reinterpret_cast<const float*>(root + field::character::impactVelocity + 12);
    return std::isfinite(squared) && squared >= 0 ? std::sqrt(squared) : -1;
}
static constexpr float swingSpeed = 800.f, lightestPulse = .25f; // native units a second of a full swing; the floor of a pulse
static constexpr unsigned pulseBaseMs = 40, pulseSpanMs = 60;
static void trackedHapticScaled(unsigned side, float speed, float peak) {
    const float strength = speed < 0 ? 1.f : (std::min)(1.f, (std::max)(lightestPulse, speed / swingSpeed));
    trackedHapticRequest(side, peak * strength, pulseBaseMs + unsigned(float(pulseSpanMs) * strength));
}
// Our weapon or hand landing: the hand holding that part, both when the grip is not one hand's.
static void trackedHapticOurs(uintptr_t player, uintptr_t weapon, float speed) {
    if (!readable(player, field::character::heldItems + 16)) return;
    bool any = false;
    for (unsigned side = 0; side < 2; ++side)
        if (weapon && (weapon == *reinterpret_cast<uintptr_t*>(player + field::character::heldItems + (1 - side) * 8) || weapon == *reinterpret_cast<uintptr_t*>(player + field::character::handParts + (1 - side) * 8))) {
            trackedHapticScaled(side, speed, .8f);
            any = true;
        }
    if (!any) { trackedHapticScaled(0, speed, .8f); trackedHapticScaled(1, speed, .8f); }
}
static void physicalImpactEvent(const PhysicalEvent& event) {
    PhysicalBorrow borrow{};
    __try {
        if (physicalImpactContextSafe(event, borrow)) {
            originalPhysicalContact(event.part, event.other, event.a.data(), event.b.data(), event.point.data());
            if (*reinterpret_cast<int*>(event.root + field::character::impactKind) && *reinterpret_cast<uintptr_t*>(event.root + field::character::impactWeapon) == event.part && *reinterpret_cast<uintptr_t*>(event.root + field::character::impactTarget) == event.other) {
                ++physicalImpactCalls;
                physicalImpact(event.root);
                trackedHapticOurs(event.root, event.part, xd::length(event.velocity)); // the borrow restores the fields, so the commit read sees nothing
            }
        }
    } __finally { borrow.restore(); }
}
// The commit about to run carries a weapon impact when impactKind is set: ours landing (the
// hand holding that weapon) or someone's landing on us (both hands). Fields at the attacker.
struct HapticImpact { uintptr_t root = 0, target = 0, weapon = 0; uint64_t tick = 0; };
static HapticImpact lastHapticImpacts[4]; // commit thread only; one slot per recent attacker
static void trackedHapticImpact(uintptr_t root) {
    __try {
        const uintptr_t player = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot));
        if (!player || !readable(root, field::character::impactWeapon + 8) || !*reinterpret_cast<int*>(root + field::character::impactKind)) return;
        const uintptr_t target = *reinterpret_cast<uintptr_t*>(root + field::character::impactTarget), weapon = *reinterpret_cast<uintptr_t*>(root + field::character::impactWeapon);
        const uint64_t now = GetTickCount64();
        HapticImpact* slot = lastHapticImpacts;
        for (auto& candidate : lastHapticImpacts) {
            if (candidate.root == root) { slot = &candidate; break; }
            if (candidate.tick < slot->tick) slot = &candidate;
        }
        if (slot->root == root && slot->target == target && slot->weapon == weapon && now - slot->tick < 150) return;
        *slot = {root, target, weapon, now};
        const float speed = trackedImpactSpeed(root);
        if (root == player) {
            trackedHapticOurs(player, weapon, speed);
        } else if (readable(target, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(target + field::bodyPart::character) == player) {
            trackedHapticScaled(0, speed, 1.f);
            trackedHapticScaled(1, speed, 1.f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static thread_local unsigned physicalCommitDepth = 0;
static void trackedPhysicalCommit(uintptr_t root, uint32_t tick) {
    ++physicalInFlight;
    __try {
        if (!physicalCommitDepth && physicalContactsReady) {
            std::array<PhysicalEvent, 16> events{};
            unsigned count = 0;
            if (TryAcquireSRWLockExclusive(&physicalQueueLock)) {
                for (auto& event : physicalEvents)
                    if (event.root == root) { events[count++] = event; event = {}; }
                ReleaseSRWLockExclusive(&physicalQueueLock);
            }
            ++physicalCommitDepth;
            __try { for (unsigned i = 0; i < count; ++i) physicalImpactEvent(events[i]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; }
            --physicalCommitDepth;
        }
        bladeCommitGuarded(root); // sinks advance, drags replay, before the game's own commit
        trackedHapticImpact(root);
        originalPhysicalCommit(root, tick);
    } __finally { --physicalInFlight; }
}
