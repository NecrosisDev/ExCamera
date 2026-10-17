// Production third-person blocker retention with owned native-object memory.
#ifdef NDEBUG
#error "The tests require assert()."
#endif
#include "first_person.cpp"
#include <cassert>
#include <limits>

int main() {
    std::vector<unsigned char> globals(0x500000), mesh(0x300), meshSector(0x100), cameraScene(0x100);
    base = uintptr_t(globals.data());
    const uintptr_t node = uintptr_t(mesh.data()), sector = uintptr_t(meshSector.data()),
                    scene = uintptr_t(cameraScene.data());
    *(uintptr_t*)gameAt(fp::sig::currentScene) = scene;
    *(uintptr_t*)(node + field::sceneNode::sector) = sector;
    float* origin = (float*)(node + field::sceneNode::transformOrigin);
    float* sectorOrigin = (float*)(sector + field::sector::origin);
    origin[0] = -1000;
    origin[1] = 0;
    origin[2] = 5;
    sectorOrigin[0] = 1000;
    // This obsolete body-part offset and the camera scene both disagree with the mesh's actual position.
    for (unsigned k = 0; k < 3; ++k) *(float*)(node + 0x2c4 + k * 4) = 9000;
    *(float*)(scene + field::sector::origin) = -2000;
    const float camera[3] = {0, 0, 0}, pivot[3] = {0, 0, 10};
    tpHideScene = scene;
    auto expired = [&] {
        tpHiddenCount = 1;
        tpHiddenTops[0] = {node, 0};
        tpHideKeep(camera, pivot, scene);
    };
    expired();
    assert(tpHiddenCount == 1 && tpHideTopCount == 1 && tpHideTops[0] == node);
    assert(tpHiddenTops[0].until > GetTickCount64());
    assert(tpHidePublishedScene == scene);

    origin[0] = -700; // Three metres off the sightline: an expired blocker becomes visible.
    expired();
    assert(!tpHiddenCount && !tpHideTopCount);
    origin[0] = -1000;
    *(uintptr_t*)(node + field::sceneNode::sector) = 0;
    expired();
    assert(!tpHiddenCount && !tpHideTopCount);
    *(uintptr_t*)(node + field::sceneNode::sector) = sector;
    origin[1] = std::numeric_limits<float>::quiet_NaN();
    expired();
    assert(!tpHiddenCount && !tpHideTopCount);
    origin[1] = 0;
    tpHideScene = 0;
    expired();
    assert(!tpHiddenCount && !tpHideTopCount);
    tpHideScene = scene;
    *(uintptr_t*)gameAt(fp::sig::currentScene) = 0;
    expired();
    assert(!tpHiddenCount && !tpHideTopCount);
    puts("TP HIDE ORIGIN: scene-node transform, own sector, expired distance, invalid geometry and "
         "scene handoff passed. Owned fixture only.");
}
