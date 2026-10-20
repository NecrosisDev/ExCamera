#pragma once
// Named field offsets reviewed against Exanima 0.9.5.2g (Steam build 25803178).
// tools/native_contracts.json traces each native owner and access; generated game_contracts.h checks
// these declarations at compile time. Both activation paths also validate the reviewed instruction
// windows in the executable file before using these layouts. See README, "Game versions".
#include <cstddef>
#include <cstdint>

namespace fp::game {

namespace bindings {
inline constexpr size_t stride = 0x10, count = 10; // runtime VK entries from movementKeys
inline constexpr size_t interact = 9 * stride;     // 0x2B7370 in the source build
} // namespace bindings

// A character (playerRoot holds the controlled one). Positions are local to its scene sector.
namespace character {
inline constexpr size_t metadata = 0; // optional native attack-variant ownership
inline constexpr size_t sceneNode = 0x18;    // the root of its mesh tree
inline constexpr size_t physicsLive = 0x4C8; // u8: its physics body is simulated
inline constexpr size_t weaponCount = 0x920; // int: weapons carried (1-2)
inline constexpr size_t weapons = 0x928;     // [2] pointers: slot 0 primary, slot 1 second
inline constexpr size_t swingWeapon = 0x988; // the weapon the swing's start routine chose
inline constexpr size_t motion = 0x9B8;      // int: accepted motion (5-12 attack families, set by 0x179EE0)
inline constexpr size_t motionProgress = 0xA28; // float: negative in the start dwell, the hit lands past 0.30
inline constexpr size_t travelTarget = 0xA30;   // float[3]: exploration's continuous travel target
inline constexpr size_t movementFacing = 0xA48; // float[3]
inline constexpr size_t position = 0xAB4;       // float[3]
inline constexpr size_t bodyHeading = 0xAD4;    // float, radians
inline constexpr size_t gaze = 0xCB0;           // float[3]: where the head is asked to look (unit)
inline constexpr size_t gazeDistance = 0xCBC;   // float
inline constexpr size_t aimDirection = 0xCC0;   // float[3]: the cursor aim's heading
inline constexpr size_t facing = 0xCFC;         // float[3]
inline constexpr size_t behaviour = 0xD40;      // its behaviour object (namespace behaviour)
inline constexpr size_t postureReadiness = 0xD58; // float: native B7560 readiness gate (>= .01)
inline constexpr size_t chestPart = 0xD48;      // the part the arms hang from
inline constexpr size_t stateFlags =
    0xDF4; // knocked down / falling / getting up bits; 0x20: right mouse is the game's
inline constexpr size_t lifeFlags = 0xDF8; // 0x100: dead
inline constexpr size_t aimOpponent = 0xE38; // native selected combat opponent
inline constexpr size_t contextualTarget = 0xE50;
inline constexpr size_t handParts = 0xE78;   // [2] body parts
inline constexpr size_t heldItems = 0xE88;   // [2] what each hand holds
inline constexpr size_t defenseIncomingMotion = 0xF5C; // int: native interception request, -1 absent
inline constexpr size_t defenseActive = 0x12F3;        // u8: reactive defense owns constraints/posture
inline constexpr size_t duckAmount = 0x100C; // float 0..1: the game's crouch, decays 0.02 a physics step
inline constexpr size_t swingBits = 0x1018;  // bounce/block bits (& 0x26)
inline constexpr size_t gripFlags =
    0x1068; // 0x80: the opposite family uses slot 1; nonzero: the wide threshold
inline constexpr size_t gripEntry = 0x1070;    // slot 0's entry in the grip table (gripTable)
inline constexpr size_t balance = 0x10B8;      // float
inline constexpr size_t balanceMax = 0x10BC;   // float
inline constexpr size_t gaitFlags = 0x10F4;    // bit 1: backward gait
inline constexpr size_t headPart = 0x11E0;     // the head's body part
inline constexpr size_t walking = 0x12EA;      // u8: exploration travel under way
inline constexpr size_t actionCallback = 0x1230; // native action dispatcher, owns LMB even outside0x20
inline constexpr size_t combatStance = 0x12FE; // u8: in combat mode
} // namespace character

// A character's behaviour (character::behaviour); the player's runs the input step.
namespace behaviour {
inline constexpr size_t character = 0x28;
inline constexpr size_t hoverState = 0x30; // 4: hovering something usable
inline constexpr size_t inactive = 0x58;   // u8
inline constexpr size_t swingState = 0x90;
inline constexpr size_t aimHeading = 0x1B0; // float: what the next new press's side classifier reads
inline constexpr size_t aimPoint = 0x854;   // float[3]
inline constexpr size_t pressFlags = 0x890; // bit 0: a press in progress
inline constexpr size_t view = 0x898;       // the UI view gameplay input belongs to
inline constexpr size_t pendingMove =
    0x8F8; // u32: combat's buffered directional request (0xC13F0), not an active physics step
inline constexpr size_t lastPressTime = 0x8E4; // game clock ms
inline constexpr size_t aimFilter = 0x90C;     // the native aim smoothing (aimFilterPush/aimFilterUpdate)
inline constexpr size_t moveIntent = 0x96C;    // u8
inline constexpr size_t interactionLatch = 0x96F; // u8: native Interact ownership can outlive its key
inline constexpr size_t pressBlocked = 0x96E;  // u8
} // namespace behaviour

// A physics body part (the head, the chest, a hand).
namespace bodyPart {
inline constexpr size_t metadata = 0, motionFlags = 8; // optional ordinary-body admission
inline constexpr size_t constraintCount = 0x31C, childCount = 0x430;
inline constexpr size_t releaseSettling = 0x450; // native eight-step release settling
inline constexpr size_t flags = 0xC;             // 0x4000: the pose builder applies extraRotation
inline constexpr size_t meshNode = 0x18;         // its scene node
inline constexpr size_t poseCoefficients = 0xB0; // turn the points into a pose
inline constexpr size_t points = 0xF0;           // float[4][3]: the physics tetrahedron
inline constexpr size_t history =
    0x120; // float[4][3]: the points a step before (Verlet: the velocity is the difference)
inline constexpr size_t history2 =
    0x150; // float[4][3]: two steps before (release shifts points -> history -> history2)
inline constexpr size_t pose = 0x2A0; // float[12]: the achieved pose, three axes then the origin
inline constexpr size_t poseForward = pose + 0x18;
inline constexpr size_t poseOrigin = pose + 0x24;
inline constexpr size_t rootPart = 0x420; // the body's root part (its own rootPart is itself)
inline constexpr size_t parentPart = 0x428;
inline constexpr size_t character = 0x580;     // the owner: a freed or reused part no longer names it
inline constexpr size_t extraRotation = 0x5EC; // float[4] quaternion (x y z w)
inline constexpr size_t slackCap = 0x77C;      // float: how far the head target may lag (head part)
} // namespace bodyPart

// A node in a scene tree: a mesh, a group, a placed object.
namespace sceneNode {
inline constexpr size_t queryFlags = 0xC;  // bit 0x200000: native held-body collision query candidate
inline constexpr size_t transform = 0x40;  // float[12], local to its sector
inline constexpr size_t transformOrigin = transform + 0x24;
inline constexpr size_t localTransform = 0x70; // float[12], relative to its parent
inline constexpr size_t renderTransform = 0xA0; // float[16]: native sampled pose, 16-byte row stride
inline constexpr size_t passMask = 0xE0;   // which render passes and picks take it (rebuilt every frame)
inline constexpr size_t lastChild = 0x118; // int: index of the last child
inline constexpr size_t children = 0x120;
inline constexpr size_t parent = 0x128;
inline constexpr size_t sector = 0x130;   // the scene sector it is drawn in
inline constexpr size_t name = 0x140;     // char[16], not always terminated
inline constexpr size_t bodyPart = 0x160; // its physics body (a character's part, a loose object's body)
inline constexpr size_t rayRoutine = 0x3B0; // the mesh type's ray test (game code)
inline constexpr size_t rayContext = 0x3B8;
} // namespace sceneNode

// A scene sector (sceneNode::sector) and its list of meshes.
namespace sector {
inline constexpr size_t scene = 0x220; // owning scene, used by native body release
inline constexpr size_t origin = 0x64;    // float[3]: add to sector-local positions for global ones
inline constexpr size_t lastMesh = 0x1C8; // int
inline constexpr size_t meshes = 0x1D0;
} // namespace sector

namespace scene {
inline constexpr size_t lifecycleFlags = 0x1C20; // optional native attack-variant ownership
inline constexpr size_t physicsContext = 0x1C28; // owning native physics context
} // namespace scene
namespace physicsContext {
inline constexpr size_t scene = 0x48, running = 0x159; // optional native attack-variant ownership
inline constexpr size_t frequencyHz = 0x6C; // signed int: native simulation frequency
} // namespace physicsContext

namespace camera {
inline constexpr size_t pose = 0x40;        // float[12]
inline constexpr size_t pose2 = 0x70;       // float[12]: a second copy the game keeps; written with pose
inline constexpr size_t inverse = 0xA0;     // expanded 4x4 pose; legacy name, not a matrix inverse
inline constexpr size_t dirty = 0xE4;       // 1: the projection is rebuilt
inline constexpr size_t controller = 0x160; // the sampler object driving it (navSlot's owner)
inline constexpr size_t fov = 0x1C8;        // float, degrees
inline constexpr size_t nearPlane = 0x1CC;  // float
inline constexpr size_t farPlane = 0x1D0;   // float; 0 selects the native infinite-far projection
inline constexpr size_t target = 0x1D8;     // native camera target node, terrain blend distance reference
} // namespace camera

namespace viewport {
inline constexpr size_t width = 0x19C, height = 0x1A0;
inline constexpr size_t view = 0x1A8;
inline constexpr size_t pickCamera = 0x1C0; // the camera the pick rays unproject through
inline constexpr size_t sceneManager = 0x1F0;
} // namespace viewport
namespace view {
inline constexpr size_t pickFarDepth = 0x120; // float: native far unprojection depth
inline constexpr size_t camera = 0x50;
} // namespace view
namespace sceneManager {
inline constexpr size_t flags = 0xC; // 0x2000000: it has a level grid
inline constexpr size_t levelGrid = 0x320;
} // namespace sceneManager

// The game's pick result (0x8F720 and 0x8FB30 fill it): +8 the hit point.
namespace pick {
inline constexpr size_t direction = 0x14; // float[3], unit
inline constexpr size_t origin = 0x20;    // float[3]
inline constexpr size_t distance = 0x2C;  // float, along direction
inline constexpr size_t scratchBank = 0x34; // u32 input: 0x8F720 saves this before clearing the result
inline constexpr size_t requiredSize = scratchBank + 4;
} // namespace pick

// Ordinary mesh ray at +3B0: 0x26A6C0 binds 0x26A910 with context +3B8.
// The first twelve bytes are traversal state, not a returned hit point.
namespace meshRayQuery {
inline constexpr size_t node = 0x0;              // u64: current BVH node, written by the backend
inline constexpr size_t minDistance = 0x8;       // float: traversal interval start
inline constexpr size_t distance = 0xC;          // float: caller limit, then closest hit distance
inline constexpr size_t origin = 0x10;           // float[3], mesh-local
inline constexpr size_t direction = 0x20;        // float[3], mesh-local
inline constexpr size_t inverseDirection = 0x30; // float[3], written by the backend
inline constexpr size_t traversalStack = 0x3C;   // u64: packed pointer, written by the backend
inline constexpr size_t scratchBank = 0x44;      // u32 input: selects the native traversal scratch
inline constexpr size_t requiredSize = scratchBank + 4;
} // namespace meshRayQuery

// Native held pick 0x117AE6 supplies 1 at result+34; 0x8F720 transfers it to
// query+44. The ray backend uses this bank separately from physics bank 0.
inline constexpr std::uint32_t viewQueryScratchBank = 1;
#pragma pack(push, 4)
struct alignas(8) MeshRayQuery {
    std::uint64_t node;
    float minDistance, distance;
    float origin[3], padOrigin;
    float direction[3], padDirection;
    float inverseDirection[3];
    std::uint64_t traversalStack;
    std::uint32_t scratchBank;
};
#pragma pack(pop)
static_assert(offsetof(MeshRayQuery, node) == meshRayQuery::node);
static_assert(offsetof(MeshRayQuery, minDistance) == meshRayQuery::minDistance);
static_assert(offsetof(MeshRayQuery, distance) == meshRayQuery::distance);
static_assert(offsetof(MeshRayQuery, origin) == meshRayQuery::origin);
static_assert(offsetof(MeshRayQuery, direction) == meshRayQuery::direction);
static_assert(offsetof(MeshRayQuery, inverseDirection) == meshRayQuery::inverseDirection);
static_assert(offsetof(MeshRayQuery, traversalStack) == meshRayQuery::traversalStack);
static_assert(offsetof(MeshRayQuery, scratchBank) == meshRayQuery::scratchBank);
static_assert(sizeof(MeshRayQuery) == meshRayQuery::requiredSize);

// The object the drag manager holds (drag::object) is a scene node; its physics body is sceneNode::bodyPart.

// The drag manager (dragManager): +8 its flags.
namespace drag {
inline constexpr size_t specialSource = 0x150; // native deferred source-side-effect flag
inline constexpr size_t acceptedKind = 0xC4; // u32: native last accepted placement kind
inline constexpr size_t acceptedQuery = 0x70; // 24-byte native successful placement query
inline constexpr size_t acceptedPoints = 0x88; // 48-byte native successful body-point snapshot
inline constexpr size_t flags = 0x8; // all native drag contexts
inline constexpr size_t object = 0x10;
inline constexpr size_t targetPoints =
    0x40; // float[4][3]: original grab snapshot used by native recovery and release
inline constexpr size_t viewport = 0x18;       // native drag viewport
inline constexpr size_t planeHeight = 0xC8;    // door plane height (hinges reuse it for angle offset)
inline constexpr size_t hingePlane = 0xE8;     // float[4], n dot p = d
inline constexpr size_t alternatePlane = 0xF8; // second native hinge plane
inline constexpr size_t pivot = 0xD0; // float[3]: a door's hinge
inline constexpr size_t invalidDrop = 0x14D; // u8: release falls back to the last valid placement
} // namespace drag

// Native work item allocated by 0x117000 and consumed/freed by 0x116EF0.
namespace heldRelease {
inline constexpr size_t body = 0x0; // pointer, followed by the four target points
} // namespace heldRelease

// The GUI: its manager (guiManager), tasks and their forms (the game's windows).
namespace gui {
inline constexpr size_t dragSource = 0x78; // native source view for current drag
inline constexpr size_t dragPayload = 0x90; // current drag payload pointer
inline constexpr size_t pointerX = 0x18; // LONG[2]: current client X/Y
inline constexpr size_t pressX = 0x20;   // LONG[2]: native press X/Y
inline constexpr size_t hoverView = 0x30; // widget under the pointer
inline constexpr size_t buttons = 0x40;  // mouse buttons and native drag-classification flags
inline constexpr size_t capture = 0x48;    // the view holding the mouse
inline constexpr size_t dragFormat = 0x70; // nonzero while an item is carried by the cursor
} // namespace gui
namespace guiTask {
inline constexpr size_t formCount = 0xA8;
inline constexpr size_t forms = 0xB0;
} // namespace guiTask
namespace form {
inline constexpr size_t visible = 0x79;  // u8
inline constexpr size_t kind = 0xB8;     // the game's window tag (windowInventory, ...)
inline constexpr size_t closing = 0x11B; // u8
} // namespace form

// Native generic GUI form. Constructor/layout/close witnesses are in the native settings review.
namespace nativeForm {
inline constexpr size_t contentLeft=0x30, contentTop=0x40, contentRight=0x44, contentBottom=0x3C;
inline constexpr size_t contentWidth=0x48, horizontalScroll=0xD8, verticalScroll=0xDC;
inline constexpr size_t scrollExtent = 0x4C;
inline constexpr size_t behaviorFlags = 0x68, skin = 0x130;
inline constexpr size_t stateFlags = 0x60;
inline constexpr size_t elementLast = 0x90, elements = 0x98;
inline constexpr size_t x=0x20, y=0x24, width=0x28, height=0x2C;
inline constexpr size_t layoutFlags=0x5C, frameFlags=0x64, title=0x80;
} // namespace nativeForm
namespace optionsForm {
inline constexpr size_t graphicsPane = 0x158, controlsPane = 0x160, controllerPane = 0x170, applied = 0x138;
}

// Native element and literal-string ABI consumed by native form/button code.
namespace nativeElement {
inline constexpr size_t stride=0x68, x=4, y=6, width=8, height=0xA, style=0xC;
inline constexpr size_t group=0xE, text=0x10, appearance=0x18, flags=0x1C;
inline constexpr size_t owner=0x20, action=0x28, handler=0x30, help=0x40;
}
namespace nativeString {
inline constexpr size_t referenceHeader=0x10, lengthHeader=8;
}

// The world (the scene object, world global): its characters, the dead too.
namespace worldObject {
inline constexpr size_t characters = 0x1C28;
} // namespace worldObject
namespace list {
inline constexpr size_t count = 0x10C;
inline constexpr size_t items = 0x110;
} // namespace list

namespace inventorySource {
inline constexpr size_t special = 0x3EB, merchant = 0x3EC; // source cancellation class guards
inline constexpr size_t finishSlot = 0xF0; // VTABLE byte offset, not object member
}
namespace inventoryPayload {
inline constexpr size_t source = 0, item = 0x10, flags = 0x14;
}
namespace inventoryJob {
inline constexpr size_t item = 0, flags = 4, sector = 8, object = 0x10;
}

namespace nativeThread { inline constexpr size_t id = 0x10; }
namespace grip { inline constexpr size_t callback = 0xB0, stride = 0xB8; }

} // namespace fp::game

// Backward offsets from AE2E0's RBP, recovered only from its plane argument
// at the reviewed AE739 call. Read-only, transient; never retained after return.
namespace fp::game {
namespace aimFrame {
inline constexpr size_t planeBack = 0x80;
inline constexpr size_t unassistedBack = 0xC0;
inline constexpr size_t unassistedInitBack = 0xC0;
inline constexpr size_t radiusBack = 0xB4;
inline constexpr size_t distanceBack = 0xAC;
inline constexpr size_t selectedBack = 0x8C;
inline constexpr size_t behaviorBack = 0x8;
inline constexpr size_t outputBack = 0x10;
} // namespace aimFrame
} // namespace fp::game
