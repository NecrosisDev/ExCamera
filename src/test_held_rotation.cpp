// Production 118920 adapter in an owned process. The placement spy implements
// the reviewed 118000 orientation -> tetrahedron -> histories -> drag88 sequence.
// It does not run the game, install hooks or claim collision simulation coverage.
#ifdef NDEBUG
#error "The held rotation tests require assert()."
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool fixtureForeground = true;
static HWND testForeground() { return fixtureForeground ? (HWND)1 : nullptr; }
static DWORD testWindowPid(HWND, DWORD* pid) {
    if (pid) *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow testForeground
#define GetWindowThreadProcessId testWindowPid
#include "first_person.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#include "test_controlled_player.h"
#include <cassert>

static uintptr_t fixtureObject = 0, fixtureBody = 0, fixtureDrag = 0;
static unsigned nativeRotations = 0, propagations = 0, cancellations = 0;
static const float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 12, 20, 30};
static const float tetra[4][3] = {{4, 0, 0}, {0, 6, 0}, {0, 0, 8}, {-2, -3, -4}};
static bool nativeRotate(uintptr_t, uintptr_t) {
    ++nativeRotations;
    return true;
}
static void nativePropagate(uintptr_t object) {
    ++propagations;
    const float* local = (const float*)(object + field::sceneNode::localTransform);
    float* world = (float*)(object + field::sceneNode::transform);
    const uintptr_t parent = *(uintptr_t*)(object + field::sceneNode::parent);
    if (!parent) {
        memcpy(world, local, 48);
        return;
    }
    const float* parentPose = (const float*)(parent + field::sceneNode::transform);
    for (int row = 0; row < 4; ++row)
        for (int axis = 0; axis < 3; ++axis) {
            world[row * 3 + axis] = row == 3 ? parentPose[9 + axis] : 0.f;
            for (int k = 0; k < 3; ++k)
                world[row * 3 + axis] += local[row * 3 + k] * parentPose[k * 3 + axis];
        }
}
static void nativePlacement(const float* position) {
    float* pose = (float*)(fixtureBody + field::bodyPart::pose);
    memcpy(pose + 9, position, 12);
    float* points = (float*)(fixtureBody + field::bodyPart::points);
    for (int point = 0; point < 4; ++point)
        for (int axis = 0; axis < 3; ++axis) {
            points[point * 3 + axis] = pose[9 + axis];
            for (int k = 0; k < 3; ++k)
                points[point * 3 + axis] += tetra[point][k] * pose[k * 3 + axis];
        }
    memcpy((void*)(fixtureBody + field::bodyPart::history), points, 48);
    memcpy((void*)(fixtureBody + field::bodyPart::history2), points, 48);
    memcpy((void*)(fixtureObject + field::sceneNode::localTransform), pose, 48);
    nativePropagate(fixtureObject);
    // Native 11F610 copies the accepted body's points to this last-valid backup.
    memcpy((void*)(fixtureDrag + 0x88), points, 48);
}
static void nativeCancel(uintptr_t drag) {
    ++cancellations;
    memcpy((void*)(fixtureBody + field::bodyPart::points),
           (const void*)(drag + field::drag::targetPoints), 48);
    *(unsigned*)(drag + field::drag::flags) = 0;
    *(uintptr_t*)(drag + field::drag::object) = 0;
}
static void closeEnough(float actual, float expected) {
    assert(std::fabs(actual - expected) < 2.e-4f);
}
int main() {
    fixture::NativeImage image(0x600000);
    assert(image);
    const uintptr_t actor = image.object(0x5b0000), behavior = image.object(0x5b2000),
                    head = image.object(0x5b4000), viewport = image.object(0x5b6000),
                    gui = image.object(0x5b8000), parent = image.object(0x5ba000);
    fixtureObject = image.object(0x5bc000);
    fixtureBody = image.object(0x5be000);
    fixtureDrag = image.object(0x5c0000);
    const fixture::ControlledPlayer player{actor, behavior, head, viewport};
    player.connect();
    *(uintptr_t*)gameAt(fp::sig::mainViewport) = viewport;
    *(uintptr_t*)gameAt(fp::sig::guiManager) = gui;
    *(uintptr_t*)gameAt(fp::sig::dragManager) = fixtureDrag;
    originalDragRotate = nativeRotate;
    originalDragDrop = nativeCancel;
    propagateHeldPose = nativePropagate;
    auto reset = [&]() {
        fixture::resetInput();
        fixtureForeground = true;
        *(int*)gameAt(fp::sig::uiOpenCount) = 0;
        *(int*)gameAt(fp::sig::pointerOverride) = 0;
        *(uintptr_t*)(gui + field::gui::capture) = viewport;
        *(unsigned*)(fixtureDrag + field::drag::flags) = 0x2000;
        *(uintptr_t*)(fixtureDrag + field::drag::object) = fixtureObject;
        *(uintptr_t*)(fixtureObject + field::sceneNode::bodyPart) = fixtureBody;
        *(uintptr_t*)(fixtureBody + field::bodyPart::meshNode) = fixtureObject;
        *(unsigned*)(fixtureBody + field::bodyPart::flags) = 0x10000000;
        *(uintptr_t*)(fixtureObject + field::sceneNode::parent) = 0;
        memcpy((void*)(fixtureObject + field::sceneNode::localTransform), identity, 48);
        memcpy((void*)(fixtureBody + field::bodyPart::pose), identity, 48);
        memcpy((void*)(fixtureDrag + field::drag::targetPoints), tetra, 48);
        memcpy((void*)(fixtureDrag + 0x88), tetra, 48);
        memcpy((void*)(fixtureBody + field::bodyPart::points), tetra, 48);
        memset((void*)(fixtureBody + field::bodyPart::history), 0x5a, 96);
        resetHeldGrab();
        viewYaw = viewPitch = 0.f;
        nativeRotations = propagations = 0;
    };
    for (int mode : {0, 1}) {
        reset();
        viewMode = mode;
        assert(gameplayHeldObject(viewport) == fixtureObject);
        // A physical movement key would be read inside native118920. No pending
        // rotate-key sample means no native call and no change of orientation.
        assert(!dragRotate(fixtureObject, viewport) && nativeRotations == 0);
        assert(memcmp((const void*)(fixtureObject + field::sceneNode::localTransform), identity, 48) == 0);
        unsigned char beforePhysics[144];
        memcpy(beforePhysics, (const void*)(fixtureBody + field::bodyPart::points), sizeof beforePhysics);
        heldTurnX = 100.f;
        assert(dragRotate(fixtureObject, viewport) && heldTurnX == 0.f && nativeRotations == 0);
        const float* pose = (const float*)(fixtureObject + field::sceneNode::localTransform);
        closeEnough(pose[0], std::cos(.6f));
        closeEnough(pose[2], std::sin(.6f)); // rightward mouse rotates about the view's up axis
        assert(memcmp(pose + 9, identity + 9, 12) == 0);
        assert(memcmp(pose, (const void*)(fixtureBody + field::bodyPart::pose), 48) == 0);
        assert(memcmp(beforePhysics, (const void*)(fixtureBody + field::bodyPart::points), sizeof beforePhysics) == 0);
        assert(memcmp((const void*)(fixtureDrag + field::drag::targetPoints), tetra, 48) == 0);
        assert(memcmp((const void*)(fixtureDrag + 0x88), tetra, 48) == 0);
        float rotated[12];
        memcpy(rotated, pose, 48);
        for (int frame = 0; frame < 100; ++frame) assert(!dragRotate(fixtureObject, viewport));
        assert(memcmp(rotated, pose, 48) == 0 && nativeRotations == 0);
        const float placed[3] = {40, 50, 60};
        nativePlacement(placed);
        const float* points = (const float*)(fixtureBody + field::bodyPart::points);
        closeEnough(points[0], placed[0] + 4.f * std::cos(.6f));
        closeEnough(points[2], placed[2] + 4.f * std::sin(.6f));
        assert(memcmp(points, (const void*)(fixtureBody + field::bodyPart::history), 48) == 0);
        assert(memcmp(points, (const void*)(fixtureBody + field::bodyPart::history2), 48) == 0);
        assert(memcmp(points, (const void*)(fixtureDrag + 0x88), 48) == 0);
        assert(memcmp((const void*)(fixtureDrag + field::drag::targetPoints), tetra, 48) == 0);
        dragDrop(fixtureDrag);
        assert(memcmp((const void*)(fixtureBody + field::bodyPart::points), tetra, 48) == 0);
        std::printf("held rotation: mode=%d, native placement consumes turned pose; original recovery points preserved\n", mode);
    }
    assert(cancellations == 2);
    reset();
    const float parentPose[12] = {0, 0, -1, 0, 1, 0, 1, 0, 0, 100, 0, 200};
    memcpy((void*)(parent + field::sceneNode::transform), parentPose, 48);
    *(uintptr_t*)(fixtureObject + field::sceneNode::parent) = parent;
    heldTurnY = -(3.14159265359f / 2.f) / fp::heldRotateRadiansPerPixel;
    assert(dragRotate(fixtureObject, viewport));
    const float* world = (const float*)(fixtureObject + field::sceneNode::transform);
    closeEnough(world[3], 0.f);
    closeEnough(world[4], 0.f);
    closeEnough(world[5], -1.f); // view-right rotation is correct even with a rotated parent
    for (int axis = 0; axis < 3; ++axis)
        closeEnough(((const float*)(fixtureObject + field::sceneNode::localTransform))[9 + axis], identity[9 + axis]);
    for (int turn = 0; turn < 1000; ++turn) {
        heldTurnX = 1.7f;
        heldTurnY = -.8f;
        assert(dragRotate(fixtureObject, viewport));
    }
    const float* pose = (const float*)(fixtureObject + field::sceneNode::localTransform);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
            float dot = 0.f;
            for (int k = 0; k < 3; ++k) dot += pose[a * 3 + k] * pose[b * 3 + k];
            closeEnough(dot, a == b ? 1.f : 0.f);
        }
    assert(memcmp((const void*)(fixtureDrag + field::drag::targetPoints), tetra, 48) == 0);
    puts("held rotation: parent frame and repeated-turn rigidity pass");

    // Only the native dynamic-body case mirrors the node pose to body+2A0.
    reset();
    *(unsigned*)(fixtureBody + field::bodyPart::flags) = 0;
    heldTurnX = 50.f;
    assert(dragRotate(fixtureObject, viewport));
    assert(memcmp((const void*)(fixtureBody + field::bodyPart::pose), identity, 48) == 0);
    reset();
    *(uintptr_t*)(fixtureBody + field::bodyPart::meshNode) = 0;
    heldTurnX = 50.f;
    assert(!dragRotate(fixtureObject, viewport) && propagations == 0 && heldTurnX == 0.f);
    assert(memcmp((const void*)(fixtureObject + field::sceneNode::localTransform), identity, 48) == 0);
    reset();
    heldTurnX = 100.f;
    resetHeldGrab();
    assert(!dragRotate(fixtureObject, viewport));
    assert(memcmp((const void*)(fixtureObject + field::sceneNode::localTransform), identity, 48) == 0);
    // A paused close-view world hold must not resume native overhead key rotation.
    for (int context = 0; context < 11; ++context) {
        reset();
        uintptr_t requestedObject = fixtureObject, requestedViewport = viewport;
        switch (context) {
        case 0: viewMode = 2; break;
        case 1: active = 0; break;
        case 2: stopping = 1; break;
        case 3: cameraReady = 0; break;
        case 4: cursorFree = 1; break;
        case 5: windowFree = 1; break;
        case 6: fixtureForeground = false; break;
        case 7: *(int*)gameAt(fp::sig::uiOpenCount) = 1; break;
        case 8: *(unsigned*)(fixtureDrag + field::drag::flags) = 0x4000; break;
        case 9: requestedViewport += 8; break;
        case 10: requestedObject += 8; break;
        }
        heldTurnX = 100.f;
        const bool paused = context >= 4 && context <= 7;
        const bool result = dragRotate(requestedObject, requestedViewport);
        assert(paused ? !result && nativeRotations == 0 : result && nativeRotations == 1);
        assert(propagations == 0);
        assert(memcmp((const void*)(fixtureObject + field::sceneNode::localTransform), identity, 48) == 0);
    }
    reset();
    rmbFree = 1;
    heldTurnX = 25.f;
    assert(dragRotate(fixtureObject, viewport) && nativeRotations == 0);
    puts("held rotation: native guard, stale identity, re-grab and ownership handoffs pass");
}
