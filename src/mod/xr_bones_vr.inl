// The VR driver of the finger bones (xr_bones.inl): the drive frame's hand joints, tracked or inferred
// from the controller, taken into the hand part's frame, for the player. Ticked from the drive after the
// kick, before the held-pose driver of the same hook, which leaves the player to it when it drove.
static void trackedBonesTick(uintptr_t root, bool owned, const TrackedDriveFrame& frame) {
    if (!trackedBonesWanted(vrFingerBones.load(std::memory_order_relaxed) != 0 && owned) || !root || !readable(root, 0x1300) ||
        root != *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot))) return;
    fingerbones::Skin skin{};
    if (!trackedBoneSkin(root, skin)) return;
    fingerbones::Root& entry = trackedBoneEntry(root);
    unsigned count = 0;
    for (unsigned side = 0; side < 2; ++side) {
        fingerbones::Joints local{};
        // a gloved hand is posed like a bare one: the glove piece is drawn bent by the same joints over it (xr_hand_render.inl)
        const bool laid = frame.handJoints[side].valid && trackedHandLocal(trackedHandPart(root, side), frame.handJoints[side], local);
        const float reach = (std::max)(frame.handJoints[side].radius[xd::indexTipJoint], .004f) * 2.5f;
        // the rest skeleton is read from the first tracked pose, straightened along each metacarpal
        if (trackedBoneDrive(root, side, entry.hands[side], skin, laid ? &local : nullptr, laid ? &local : nullptr, reach, count)) trackedBoneDrivenByVr = true;
        if (trackedBoneFault.load()) break;
    }
    if (trackedBoneFault.load()) { trackedBoneFaults.fetch_add(1, std::memory_order_relaxed); trackedBoneRetire(); }
    trackedBonePublish(root);
}
