// Blade contact behaviours, every build, for every character: a blade edge drawn across a body deals
// damage, and a thrust sinks into a soft target instead of stopping at its surface.
//
// Both ride the game's own impact path. The physics calls the contact collector 1590D0(weapon part,
// other part, a, b, point) for every weapon contact and keeps the one it accepts in the attacker's
// impact bookkeeping (+C18 kind, +C20 target, +C28 point, +C38 weapon); the per-character commit
// 1592D0 turns that into the weapon impact 159A60, which reads the velocity cache, the weapon values,
// the contact position along the blade (the edge window), the struck part's armour layers and the
// victim's state, and applies 159700. The collector only accepts a contact while the weapon carries
// the swing's active bit, so a blade resting on or drawn across a body never counts.
//
// Drag: a weapon contact that persists while the blade slides along its own axis fast enough is
// replayed every 150 ms through the collector under borrowed bookkeeping (the same borrow the VR
// physical contacts use), with the sliding velocity as the impact velocity, and the game's impact
// runs on it: its own speed threshold, edge window, cut and armour maths, blood and reaction. No
// damage number is invented; the only knob is a gain on the velocity, 100% by default (a Combat row).
// One continuous draw is one cut that deepens: a drag replays once per edge segment it crosses on a
// target part (four segments along the blade), never twice on the same segment of the same stroke,
// and a sinking thrust replays per 5 cm of travel, so neither reads as a flurry of hits.
//
// Sink: a thrust (motion 0xB) whose point meets a soft part (armour rows carry little pierce
// protection; never the head) has the weapon's collision bit (+8 bit 0x800, the game's own
// collision-off flag, set and cleared by the physics and the character code) set for up to 20 cm of
// travel, 300 ms, or until the blade slows or withdraws; the commit replays the contact along the
// way so the thrust keeps cutting through the game's point-damage path. The bit is restored exactly.
// Off by default: the bit's meaning is inferred from its readers, not proven live.
//
// The hooks on 1590D0 and 1592D0 are the VR drive's in the headset build (which binds this file to
// its trampolines) and this file's own in the plain build. Sim thread for contacts, the commit thread
// for events; one lock between them; every game call in its own SEH frame; a fault latch.
#include "../xr/physical_contact.h"
namespace xd = fp::xr;
static std::atomic<unsigned> bladeDrags{0}, bladeSinks{0}, bladeFaults{0};
static std::atomic<bool> bladeFault{false}, bladeReady{false};
static float bladeGain() { return static_cast<float>((std::min)(400, (std::max)(10, bladeGainPercent.load(std::memory_order_relaxed)))) / 100.f; } // the Combat row
using BladeContactFn = void (*)(uintptr_t, uintptr_t, const float*, const float*, const float*);
static BladeContactFn bladeOriginalContact = nullptr; // the game's collector (through the trampoline)
static void (*bladeImpact)(uintptr_t) = nullptr;      // 159A60
namespace blade {
// Speeds below are centimetres a second; the parts' tetra velocities are per physics step and are
// scaled by the step rate (physicsContext::frequencyHz) before comparing.
constexpr float dragMinSpeed = 40.f;    // the edge must move along itself this fast to be drawn
// The weapon impact 159A60 takes the velocity cache per step, doubles its square, takes the root and
// does nothing below 10 (31BF68) and no more above 30 (31C4E0): per step, 7.07 and 21.2. A drawn edge is
// nowhere near a swing, so the draw is mapped onto that scale: a draw at `dragReferenceSpeed` becomes
// the impact's minimum cutting speed, faster draws proportionally more; the gain row scales it.
constexpr float impactMinStepSpeed = 10.f / 1.41421356f;
constexpr float dragReferenceSpeed = 100.f; // cm/s of draw that just cuts at 100% gain
constexpr float dragEdgeLow = .2f;      // the contact must sit on the blade, not the grip: fraction of the rest length from the hilt end
constexpr float dragEdgeHigh = 1.05f;
constexpr uint64_t dragHoldMs = 80;     // a contact seen again within this is the same drag
constexpr uint64_t dragEveryMs = 150;   // never two replays closer than this on one drag
constexpr unsigned dragSegments = 4;    // edge segments along the blade: a new one on the same stroke deepens the cut
constexpr float sinkStepDepth = 5.f;    // native units of travel per replay while sinking
constexpr float sinkMinSpeed = 150.f;   // a thrust this fast along the blade, point first
constexpr float sinkPointFrom = .8f;    // the contact near the point
constexpr float sinkDepth = 20.f;       // native units the point may travel past the entry
constexpr float sinkStopSpeed = 20.f;   // slower than this along the blade ends the sink
constexpr uint64_t sinkMaxMs = 300;
constexpr float softPierce = .25f;      // more pierce protection than this on a layer stops the blade
constexpr unsigned collisionOffBit = 0x800; // body +8
struct Event {
    uintptr_t root = 0, part = 0, other = 0, otherCharacter = 0;
    std::array<float, 3> a{}, b{}, point{};
    XrVector3f velocity{};
    uint64_t tick = 0;
};
struct Drag { uintptr_t part = 0, other = 0; uint64_t lastSeen = 0, lastEmit = 0; int lastSegment = -1; };
struct Sink {
    uintptr_t root = 0, part = 0, other = 0, otherCharacter = 0;
    XrVector3f axis{}, entryTip{}, centre{};
    float radius = 0; // the struck part's reach from its tetra centroid: the sink ends when the point leaves it
    std::array<float, 3> a{}, b{}, point{};
    uint64_t start = 0, lastEmit = 0;
    float lastDepth = 0;
    unsigned savedFlags = 0;
    bool bitSet = false;
};
} // namespace blade
static SRWLOCK bladeLock = SRWLOCK_INIT;
static std::array<blade::Event, 16> bladeEvents;
static std::array<blade::Drag, 16> bladeDragLedger;
static std::array<blade::Sink, 4> bladeSinkTable;
// The physics step rate, for per-step velocities; 60 when the context cannot be read.
static float bladeStepHz() {
    const uintptr_t scene = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::currentScene));
    if (!readable(scene, field::scene::physicsContext + 8)) return 60.f;
    const uintptr_t context = *reinterpret_cast<uintptr_t*>(scene + field::scene::physicsContext);
    if (!readable(context, field::physicsContext::frequencyHz + 4)) return 60.f;
    const int hz = *reinterpret_cast<int*>(context + field::physicsContext::frequencyHz);
    return hz >= 10 && hz <= 1000 ? float(hz) : 60.f;
}
// The impact velocity (per step, as the cache holds it) for a draw or a sink at `speed` cm/s along the blade.
static XrVector3f bladeImpactVelocity(XrVector3f axis, float speed) {
    return xd::scale(axis, blade::impactMinStepSpeed * (speed / blade::dragReferenceSpeed) * bladeGain());
}
static bool bladeVelocityAt(uintptr_t part, XrVector3f point, XrVector3f& velocity) {
    if (!readable(part + field::bodyPart::points, 48) || !readable(part + field::bodyPart::velocityTetra, 48)) return false;
    std::array<XrVector3f, 4> tetra{}, values{};
    memcpy(tetra.data(), reinterpret_cast<void*>(part + field::bodyPart::points), 48);
    memcpy(values.data(), reinterpret_cast<void*>(part + field::bodyPart::velocityTetra), 48);
    return xd::contactVelocity(tetra, values, point, velocity);
}
// The blade line in the world: the rest tetra's longest edge taken through the live tetra, its hilt
// end the one nearer the holding hand's pose origin.
static bool bladeLine(uintptr_t weapon, XrVector3f& hilt, XrVector3f& tip, XrVector3f& axis, float& length) {
    if (!readable(weapon, field::bodyPart::character + 8) || !readable(weapon + field::bodyPart::restTetra, 48) || !readable(weapon + field::bodyPart::points, 48)) return false;
    const float* rest = reinterpret_cast<const float*>(weapon + field::bodyPart::restTetra);
    const float* live = reinterpret_cast<const float*>(weapon + field::bodyPart::points);
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(rest[i]) || !std::isfinite(live[i])) return false;
    float best = 0;
    unsigned bi = 0, bk = 1;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned k = i + 1; k < 4; ++k) {
            const float len = xd::length({rest[k * 3] - rest[i * 3], rest[k * 3 + 1] - rest[i * 3 + 1], rest[k * 3 + 2] - rest[i * 3 + 2]});
            if (len > best) { best = len; bi = i; bk = k; }
        }
    if (best < 10.f || best > 300.f) return false; // ten centimetres to three metres
    XrVector3f p = {live[bi * 3], live[bi * 3 + 1], live[bi * 3 + 2]}, q = {live[bk * 3], live[bk * 3 + 1], live[bk * 3 + 2]};
    const uintptr_t hand = *reinterpret_cast<uintptr_t*>(weapon + field::bodyPart::parentPart);
    if (!hand || !readable(hand, field::bodyPart::pose + 48)) return false;
    const float* pose = reinterpret_cast<const float*>(hand + field::bodyPart::pose);
    const XrVector3f handAt = {pose[9], pose[10], pose[11]};
    if (!xd::finite(handAt)) return false;
    if (xd::length(xd::sub(q, handAt)) < xd::length(xd::sub(p, handAt))) std::swap(p, q);
    hilt = p;
    tip = q;
    length = xd::length(xd::sub(tip, hilt));
    if (!(length > 1e-3f)) return false;
    axis = xd::scale(xd::sub(tip, hilt), 1 / length);
    return xd::finite(axis);
}
// A part the blade may sink into: little pierce protection on both armour rows, and not the head.
static bool bladeSoftPart(uintptr_t other, uintptr_t victim) {
    if (!readable(other, 0x6A8 + 0x18) || !readable(victim, field::character::headPart + 8)) return false;
    if (other == *reinterpret_cast<uintptr_t*>(victim + field::character::headPart)) return false;
    const float* outer = reinterpret_cast<const float*>(other + 0x6A8);
    const float* under = reinterpret_cast<const float*>(other + 0x690);
    for (unsigned i = 0; i < 6; ++i)
        if (!std::isfinite(outer[i]) || !std::isfinite(under[i])) return false;
    return outer[0] * outer[4] < blade::softPierce && under[4] < blade::softPierce;
}
static bool bladeQueue(const blade::Event& event) {
    if (!TryAcquireSRWLockExclusive(&bladeLock)) return false;
    bool queued = false;
    for (auto& slot : bladeEvents)
        if (!slot.root || event.tick - slot.tick > 200) { slot = event; queued = true; break; }
    ReleaseSRWLockExclusive(&bladeLock);
    return queued;
}
// Sim thread, after the game's collector saw the contact.
static void bladeContact(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    const bool drag = bladeDrag.load(std::memory_order_relaxed), sink = stabSink.load(std::memory_order_relaxed);
    if (!bladeReady.load() || bladeFault.load() || (!drag && !sink)) return;
    if (!readable(part, field::bodyPart::character + 8) || !readable(other, field::bodyPart::character + 8)) return;
    if (!(*reinterpret_cast<unsigned*>(part + field::bodyPart::flags) & 1u) || !(*reinterpret_cast<unsigned*>(other + field::bodyPart::flags) & 0x2000u)) return; // a weapon on a body part
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(part + field::bodyPart::character), victim = *reinterpret_cast<uintptr_t*>(other + field::bodyPart::character);
    if (!root || !victim || root == victim || !readable(root, 0x1300) || !readable(victim, 0x1300)) return;
    if (!readable(reinterpret_cast<uintptr_t>(a), 12) || !readable(reinterpret_cast<uintptr_t>(b), 12) || !readable(reinterpret_cast<uintptr_t>(point), 12)) return;
    XrVector3f hilt{}, tip{}, axis{}, at{};
    float length = 0;
    memcpy(&at, point, 12);
    if (!xd::finite(at) || !bladeLine(part, hilt, tip, axis, length)) return;
    const float u = xd::dot(xd::sub(at, hilt), axis) / length;
    if (u < blade::dragEdgeLow || u > blade::dragEdgeHigh) return; // the grip, or off the blade
    XrVector3f ours{}, theirs{};
    if (!bladeVelocityAt(part, at, ours) || !bladeVelocityAt(other, at, theirs)) return;
    const XrVector3f relative = xd::scale(xd::sub(ours, theirs), bladeStepHz()); // cm/s
    const float along = xd::dot(relative, axis); // towards the point when positive
    if (!std::isfinite(along)) return;
    const uint64_t now = GetTickCount64();
    if (drag && std::abs(along) >= blade::dragMinSpeed) {
        blade::Drag* entry = &bladeDragLedger[0];
        for (auto& item : bladeDragLedger) {
            if (item.part == part && item.other == other) { entry = &item; break; }
            if (item.lastSeen < entry->lastSeen) entry = &item;
        }
        if (entry->part != part || entry->other != other) *entry = {part, other, 0, 0, -1};
        const bool sustained = entry->lastSeen && now - entry->lastSeen <= blade::dragHoldMs;
        if (!sustained) { entry->lastEmit = 0; entry->lastSegment = -1; } // a new stroke cuts again
        entry->lastSeen = now;
        const int segment = (std::min)(int(blade::dragSegments) - 1, (std::max)(0, int((u - blade::dragEdgeLow) / (blade::dragEdgeHigh - blade::dragEdgeLow) * float(blade::dragSegments))));
        if (sustained && segment != entry->lastSegment && now - entry->lastEmit >= blade::dragEveryMs) {
            blade::Event event{};
            event.root = root;
            event.part = part;
            event.other = other;
            event.otherCharacter = victim;
            memcpy(event.a.data(), a, 12);
            memcpy(event.b.data(), b, 12);
            memcpy(event.point.data(), point, 12);
            event.velocity = bladeImpactVelocity(axis, along); // the draw along the edge is what cuts
            event.tick = now;
            if (bladeQueue(event)) { entry->lastEmit = now; entry->lastSegment = segment; }
        }
    }
    if (sink && u >= blade::sinkPointFrom && along >= blade::sinkMinSpeed && *reinterpret_cast<int*>(root + field::character::motion) == 0xB && bladeSoftPart(other, victim)) {
        if (!TryAcquireSRWLockExclusive(&bladeLock)) return;
        bool held = false;
        blade::Sink* free = nullptr;
        for (auto& s : bladeSinkTable) {
            if (s.part == part) held = true;
            if (!s.root && !free) free = &s;
        }
        if (!held && free && readable(part + field::bodyPart::motionFlags, 4)) {
            unsigned& flags = *reinterpret_cast<unsigned*>(part + field::bodyPart::motionFlags);
            blade::Sink s{};
            s.root = root;
            s.part = part;
            s.other = other;
            s.otherCharacter = victim;
            s.axis = axis;
            s.entryTip = tip;
            { // the struck part's extent, so the blade stops inside it rather than passing out the far side
                const float* pts = reinterpret_cast<const float*>(other + field::bodyPart::points);
                XrVector3f c{};
                for (unsigned i = 0; i < 4; ++i) c = xd::add(c, {pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]});
                c = xd::scale(c, .25f);
                float r = 0;
                for (unsigned i = 0; i < 4; ++i) r = (std::max)(r, xd::length(xd::sub({pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]}, c)));
                s.centre = c;
                s.radius = r + 3.f;
            }
            memcpy(s.a.data(), a, 12);
            memcpy(s.b.data(), b, 12);
            memcpy(s.point.data(), point, 12);
            s.start = s.lastEmit = now;
            s.savedFlags = flags;
            flags |= blade::collisionOffBit; // the blade passes the soft body; restored by the commit
            s.bitSet = true;
            *free = s;
            bladeSinks.fetch_add(1, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&bladeLock);
    }
}
static void bladeContactSafe(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    try { bladeContact(part, other, a, b, point); } catch (const std::exception&) { bladeFault = true; }
}
static void bladeContactGuarded(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) { // SEH-only frame
    __try { bladeContactSafe(part, other, a, b, point); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bladeFault = true; bladeFaults.fetch_add(1, std::memory_order_relaxed); }
}
static void bladeSinkEnd(blade::Sink& s) {
    if (s.bitSet && readable(s.part + field::bodyPart::motionFlags, 4) && !(s.savedFlags & blade::collisionOffBit))
        *reinterpret_cast<unsigned*>(s.part + field::bodyPart::motionFlags) &= ~blade::collisionOffBit; // only our own set is undone
    s = {};
}
// Native impact bookkeeping borrowed for one replayed contact and restored byte for byte (the VR drive's borrow, generic).
struct BladeBorrow {
    struct Field { uintptr_t address = 0; size_t size = 0; std::array<unsigned char, 48> saved{}; };
    std::array<Field, 8> fields{};
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
static bool bladeImpactContext(const blade::Event& event, BladeBorrow& borrow) {
    const uintptr_t root = event.root, part = event.part, other = event.other;
    if (!readable(root, 0x1300) || !readable(part, field::bodyPart::weaponData + 8) || !readable(other, field::bodyPart::character + 8)) return false;
    if (*reinterpret_cast<uintptr_t*>(part + field::bodyPart::character) != root || *reinterpret_cast<uintptr_t*>(other + field::bodyPart::character) != event.otherCharacter) return false;
    if (*reinterpret_cast<int*>(root + field::character::impactKind)) return false; // the game has its own impact pending: ours waits
    if (!readable(*reinterpret_cast<uintptr_t*>(part + field::bodyPart::weaponData), 0x70)) return false;
    const std::array<unsigned char, 40> emptyContact{};
    std::array<float, 8> velocity{};
    memcpy(velocity.data(), &event.velocity, 12);
    velocity[3] = xd::dot(event.velocity, event.velocity);
    const std::array<uintptr_t, 2> parts{part, 0};
    const int count = 1;
    const unsigned zero = 0;
    const unsigned flags = *reinterpret_cast<unsigned*>(part + field::bodyPart::flags) | 0x10001u;
    return borrow.write(root + field::character::impactKind, emptyContact.data(), 40) && borrow.write(root + field::character::impactVelocity, velocity.data(), 32) &&
           borrow.write(root + field::character::activePartCount, &count, 4) && borrow.write(root + field::character::activeParts, parts.data(), 16) &&
           borrow.write(root + field::character::swingWeapon, &part, 8) && borrow.write(root + field::character::swingBits, &zero, 4) &&
           borrow.write(part + field::bodyPart::flags, &flags, 4);
}
static bool bladeImpactContextSafe(const blade::Event& event, BladeBorrow& borrow) {
    try { return bladeImpactContext(event, borrow); } catch (const std::exception&) { return false; }
}
static void bladeImpactEvent(const blade::Event& event) { // SEH-only frame
    BladeBorrow borrow{};
    __try {
        if (bladeImpactContextSafe(event, borrow)) {
            bladeOriginalContact(event.part, event.other, event.a.data(), event.b.data(), event.point.data());
            if (*reinterpret_cast<int*>(event.root + field::character::impactKind) == 1 && *reinterpret_cast<uintptr_t*>(event.root + field::character::impactWeapon) == event.part &&
                *reinterpret_cast<uintptr_t*>(event.root + field::character::impactTarget) == event.other) {
                bladeImpact(event.root);
                bladeDrags.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { bladeFault = true; bladeFaults.fetch_add(1, std::memory_order_relaxed); }
    borrow.restore();
}
// The commit thread, before the game's own commit for this character: sinks advance or end, and the
// queued contacts of this attacker are replayed.
static void bladeCommit(uintptr_t root) {
    if (!bladeReady.load() || !root) return;
    const uint64_t now = GetTickCount64();
    std::array<blade::Event, 16> events{};
    unsigned count = 0;
    if (TryAcquireSRWLockExclusive(&bladeLock)) {
        for (auto& s : bladeSinkTable) {
            if (s.root != root) continue;
            XrVector3f hilt{}, tip{}, axis{}, velocity{};
            float length = 0;
            bool alive = !bladeFault.load() && stabSink.load(std::memory_order_relaxed) && now - s.start <= blade::sinkMaxMs &&
                         readable(s.other, field::bodyPart::character + 8) && *reinterpret_cast<uintptr_t*>(s.other + field::bodyPart::character) == s.otherCharacter &&
                         bladeLine(s.part, hilt, tip, axis, length) && bladeVelocityAt(s.part, tip, velocity);
            const float depth = alive ? xd::dot(xd::sub(tip, s.entryTip), s.axis) : 0, along = alive ? xd::dot(velocity, s.axis) * bladeStepHz() : 0;
            const bool inside = alive && xd::length(xd::sub(tip, s.centre)) <= s.radius;
            if (!alive || !inside || !std::isfinite(depth) || !std::isfinite(along) || depth > blade::sinkDepth || depth < -2.f || along < blade::sinkStopSpeed) { bladeSinkEnd(s); continue; }
            if (depth - s.lastDepth >= blade::sinkStepDepth && now - s.lastEmit >= blade::dragEveryMs && count < events.size()) { // the thrust cuts deeper per step of travel
                blade::Event event{};
                event.root = root;
                event.part = s.part;
                event.other = s.other;
                event.otherCharacter = s.otherCharacter;
                event.a = s.a;
                event.b = s.b;
                const XrVector3f point = xd::add({s.point[0], s.point[1], s.point[2]}, xd::scale(s.axis, depth));
                event.point = {point.x, point.y, point.z};
                event.velocity = bladeImpactVelocity(s.axis, along);
                event.tick = now;
                events[count++] = event;
                s.lastEmit = now;
                s.lastDepth = depth;
            }
        }
        for (auto& event : bladeEvents)
            if (event.root == root && count < events.size()) { events[count++] = event; event = {}; }
            else if (event.root && now - event.tick > 200) event = {};
        ReleaseSRWLockExclusive(&bladeLock);
    }
    if (bladeFault.load()) {
        if (TryAcquireSRWLockExclusive(&bladeLock)) { for (auto& s : bladeSinkTable) if (s.root) bladeSinkEnd(s); ReleaseSRWLockExclusive(&bladeLock); }
        return;
    }
    for (unsigned i = 0; i < count; ++i) bladeImpactEvent(events[i]);
}
static void bladeCommitSafe(uintptr_t root) {
    try { bladeCommit(root); } catch (const std::exception&) { bladeFault = true; }
}
static void bladeCommitGuarded(uintptr_t root) { // SEH-only frame
    __try { bladeCommitSafe(root); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bladeFault = true; bladeFaults.fetch_add(1, std::memory_order_relaxed); }
}
static void bladeBind(BladeContactFn original, void (*impact)(uintptr_t)) {
    bladeOriginalContact = original;
    bladeImpact = impact;
    bladeReady = original && impact;
}
#ifndef FP_OPENXR_EMULATION
// The plain build hooks the collector and the commit itself; the headset build's VR drive owns them and binds us.
static BladeContactFn bladeOriginalHook = nullptr;
static void (*bladeOriginalCommit)(uintptr_t, uint32_t) = nullptr;
static void bladeContactHook(uintptr_t part, uintptr_t other, const float* a, const float* b, const float* point) {
    bladeOriginalHook(part, other, a, b, point);
    bladeContactGuarded(part, other, a, b, point);
}
static void bladeCommitHook(uintptr_t root, uint32_t tick) {
    bladeCommitGuarded(root);
    bladeOriginalCommit(root, tick);
}
static bool createBladeHooks() {
    if (!layout.optionalGroupAvailable("xrPhysicalContacts")) return true;
    if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrPhysicalContact)), reinterpret_cast<void*>(bladeContactHook), reinterpret_cast<void**>(&bladeOriginalHook)) != MH_OK) return false;
    if (MH_CreateHook(reinterpret_cast<void*>(gameAt(fp::sig::xrPhysicalCommit)), reinterpret_cast<void*>(bladeCommitHook), reinterpret_cast<void**>(&bladeOriginalCommit)) != MH_OK) return false;
    bladeBind(bladeOriginalHook, reinterpret_cast<void (*)(uintptr_t)>(gameAt(fp::sig::xrPhysicalImpact)));
    return true;
}
#else
static bool createBladeHooks() { return true; }
#endif
