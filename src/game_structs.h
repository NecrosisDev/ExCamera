#pragma once
// Field offsets in the game's objects, by name. Exanima 0.9.5.2g (Steam build 25803178); identified by
// decompiling the functions that use them and by reading them live. Signatures (game_signatures.h) find
// the game's functions and globals in other builds, but not these: a game update that changes an object's
// layout changes them (README, "Game versions").
#include <cstddef>

namespace fp::game {

// A character (playerRoot holds the controlled one). Positions are local to its scene sector.
namespace character {
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
inline constexpr size_t chestPart = 0xD48;      // the part the arms hang from
inline constexpr size_t stateFlags =
    0xDF4; // knocked down / falling / getting up bits; 0x20: right mouse is the game's
inline constexpr size_t lifeFlags = 0xDF8; // 0x100: dead
inline constexpr size_t contextualTarget = 0xE50;
inline constexpr size_t handParts = 0xE78;   // [2] body parts
inline constexpr size_t heldItems = 0xE88;   // [2] what each hand holds
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
inline constexpr size_t combatStance = 0x12FE; // u8: in combat mode
} // namespace character

// A character's behaviour (character::behaviour); the player's runs the input step.
namespace behaviour {
inline constexpr size_t character = 0x28;
inline constexpr size_t hoverState = 0x30; // 4: hovering something usable
inline constexpr size_t inactive = 0x58;   // u8
inline constexpr size_t swingState = 0x90;
inline constexpr size_t aimHeading = 0x1B0;    // float: what the next new press's side classifier reads
inline constexpr size_t aimPoint = 0x854;      // float[3]
inline constexpr size_t pressFlags = 0x890;    // bit 0: a press in progress
inline constexpr size_t view = 0x898;          // the UI view gameplay input belongs to
inline constexpr size_t lastPressTime = 0x8E4; // game clock ms
inline constexpr size_t aimFilter = 0x90C;     // the native aim smoothing (aimFilterPush/aimFilterUpdate)
inline constexpr size_t moveIntent = 0x96C;    // u8
inline constexpr size_t pressBlocked = 0x96E;  // u8
} // namespace behaviour

// A physics body part (the head, the chest, a hand).
namespace bodyPart {
inline constexpr size_t flags = 0xC;             // 0x4000: the pose builder applies extraRotation
inline constexpr size_t meshNode = 0x18;         // its scene node
inline constexpr size_t poseCoefficients = 0xB0; // turn the points into a pose
inline constexpr size_t points = 0xF0;           // float[4][3]: the physics tetrahedron
inline constexpr size_t pose = 0x2A0;            // float[12]: the achieved pose, three axes then the origin
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
inline constexpr size_t transform = 0x40;  // float[12], local to its sector
inline constexpr size_t passMask = 0xE0;   // which render passes and picks take it (rebuilt every frame)
inline constexpr size_t lastChild = 0x118; // int: index of the last child
inline constexpr size_t children = 0x120;
inline constexpr size_t parent = 0x128;
inline constexpr size_t sector = 0x130;   // the scene sector it is drawn in
inline constexpr size_t name = 0x140;     // char[16], not always terminated
inline constexpr size_t bodyPart = 0x160; // the part a character's mesh belongs to
inline constexpr size_t pose = 0x2A0;     // float[12]: where it is drawn, three axes then the origin
inline constexpr size_t poseOrigin = pose + 0x24;
inline constexpr size_t rayRoutine = 0x3B0; // the mesh type's ray test (game code)
inline constexpr size_t rayContext = 0x3B8;
} // namespace sceneNode

// A scene sector (sceneNode::sector) and its list of meshes.
namespace sector {
inline constexpr size_t origin = 0x64;    // float[3]: add to sector-local positions for global ones
inline constexpr size_t lastMesh = 0x1C8; // int
inline constexpr size_t meshes = 0x1D0;
} // namespace sector

namespace camera {
inline constexpr size_t pose = 0x40;        // float[12]
inline constexpr size_t pose2 = 0x70;       // float[12]: a second copy the game keeps; written with pose
inline constexpr size_t inverse = 0xA0;     // rebuilt from pose (rebuildInverse)
inline constexpr size_t dirty = 0xE4;       // 1: the projection is rebuilt
inline constexpr size_t controller = 0x160; // the sampler object driving it (navSlot's owner)
inline constexpr size_t fov = 0x1C8;        // float, degrees
inline constexpr size_t nearPlane = 0x1CC;  // float
} // namespace camera

namespace viewport {
inline constexpr size_t view = 0x1A8;
inline constexpr size_t pickCamera = 0x1C0; // the camera the pick rays unproject through
inline constexpr size_t sceneManager = 0x1F0;
} // namespace viewport
namespace view {
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
} // namespace pick

// The drag manager (dragManager): +8 its flags.
namespace drag {
inline constexpr size_t object = 0x10;
inline constexpr size_t pivot = 0xD0; // float[3]: a door's hinge
} // namespace drag

// The GUI: its manager (guiManager), tasks and their forms (the game's windows).
namespace gui {
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

// The world (the scene object, world global): its characters, the dead too.
namespace worldObject {
inline constexpr size_t characters = 0x1C28;
} // namespace worldObject
namespace list {
inline constexpr size_t count = 0x10C;
inline constexpr size_t items = 0x110;
} // namespace list

} // namespace fp::game
