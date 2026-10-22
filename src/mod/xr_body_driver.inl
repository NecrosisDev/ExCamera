// The tracked body is the desired pose of the physical character; motors and contacts achieve it.
static xd::BodyPoseDriver trackedBodyDriver;
static xd::BodyCameraDriver trackedBodyCamera;
static void trackedBodyPublishImpl(TrackedDriveFrame& frame) {
    if (!trackedBodyRequested || !frame.root || !manualDefenseReady || !xd::lowerValid(frame.lower, frame.lower.time) || !frame.valid[xd::leftHand] || !frame.valid[xd::rightHand] ||
        !bodyCalm(frame.root, *reinterpret_cast<uintptr_t*>(frame.root + field::character::headPart))) return;
    auto refuse = [&] { trackedBodyDriver.reset(); };
    const int count = *reinterpret_cast<int*>(frame.root + field::character::partCount), legs = *reinterpret_cast<int*>(frame.root + field::character::legCount);
    const auto parts = *reinterpret_cast<uintptr_t*>(frame.root + field::character::parts), legArray = *reinterpret_cast<uintptr_t*>(frame.root + field::character::legs);
    if (count < 8 || count > 64 || legs != 2 || !readable(parts, static_cast<size_t>(count) * 8) || !readable(legArray, 16)) return refuse();
    const auto pelvis = *reinterpret_cast<uintptr_t*>(parts);
    const auto node = *reinterpret_cast<uintptr_t*>(frame.root + field::character::sceneNode);
    if (!trackedPart(pelvis, frame.root) || !readable(node, field::sceneNode::sector + 8)) return refuse();
    const auto head = *reinterpret_cast<uintptr_t*>(frame.root + field::character::headPart);
    if (!trackedPart(head, frame.root)) return refuse();
    const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
    const auto pelvisPose = trackedPartPose(pelvis);
    std::array<XrPosef, xd::jointCount> physical{}, stage{};
    physical[xd::head] = trackedPartPose(head);
    physical[xd::waist] = pelvisPose;
    stage[xd::head] = frame.bodySample.head;
    stage[xd::waist] = frame.lower.trackers[0].pose;
    for (unsigned side = 0; side < 2; ++side) {
        const auto hand = *reinterpret_cast<uintptr_t*>(frame.root + field::character::handParts + (1 - side) * 8);
        if (!trackedPart(hand, frame.root)) return refuse();
        physical[xd::leftHand + side] = trackedPartPose(hand);
        stage[xd::leftHand + side] = frame.bodySample.hands[side].grip;
        stage[xd::leftFoot + side] = frame.lower.trackers[side + 1].pose;
    }
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const auto shin = *reinterpret_cast<uintptr_t*>(legArray + i * 8);
        if (!trackedPart(shin, frame.root)) return refuse();
        const auto thigh = *reinterpret_cast<uintptr_t*>(shin + field::bodyPart::parentPart);
        if (!trackedPart(thigh, frame.root) || *reinterpret_cast<uintptr_t*>(thigh + field::bodyPart::parentPart) != pelvis) return refuse();
        const auto hip = xd::rotate(xd::conjugate(pelvisPose.orientation), xd::sub(trackedPartPose(thigh).position, pelvisPose.position));
        if (std::abs(hip.x) < .02f) return refuse();
        const unsigned side = hip.x > 0 ? 0 : 1;
        if (seen & (1u << side)) return refuse();
        seen |= 1u << side;
        uintptr_t foot = 0;
        for (int p = 0; p < count; ++p) {
            const auto part = *reinterpret_cast<uintptr_t*>(parts + p * 8);
            if (trackedPart(part, frame.root) && *reinterpret_cast<uintptr_t*>(part + field::bodyPart::parentPart) == shin && *reinterpret_cast<int*>(part + field::bodyPart::role) == 0x13) {
                if (foot) return refuse();
                foot = part;
            }
        }
        if (!foot) return refuse();
        physical[xd::leftFoot + side] = trackedPartPose(foot);
    }
    const auto pose = trackedBodyDriver.step({frame.root, frame.scene, sector, frame.epoch, frame.stream, frame.inputGeneration}, stage, physical, frame.stageBasis);
    if (!pose.valid) return;
    if (xd::length(xd::sub(pose.joints[xd::waist].position, pelvisPose.position)) > 1.5f) {
        // A native teleport owns the character: retire the tracking reference rather than
        // dragging the physical body back to it.
        trackedBodyDriver.reset();
        trackedDriveClear();
        frame = {};
        return;
    }
    for (unsigned i = 0; i < xd::jointCount; ++i) frame.poses[i] = xd::nativeItemPose(pose.joints[i], stereoUnitsPerMetre);
    frame.bodyDriven = true;
}
static void trackedBodyPublishSafe(TrackedDriveFrame& frame) {
    try { trackedBodyPublishImpl(frame); }
    catch (const std::exception&) { trackedBodyDriver.reset(); frame.bodyDriven = false; }
}
static void trackedBodyPublish(TrackedDriveFrame& frame) {
    __try { trackedBodyPublishSafe(frame); }
    __except (EXCEPTION_EXECUTE_HANDLER) { trackedDriveFault = true; frame.bodyDriven = false; }
}
// The headset stays the camera; only the bind-time correction between the physical head
// and the tracked head moves the eyes, so achieved drift cannot feed back as camera motion.
static void trackedBodyStereo(fp::xr::StereoFrame& stereo) {
    TrackedDriveFrame frame{};
    if (!trackedBodyRequested || !trackedDriveRead(savedRoot, frame)) return;
    const auto node = *reinterpret_cast<uintptr_t*>(savedRoot + field::character::sceneNode);
    if (!readable(node, field::sceneNode::sector + 8)) return;
    const auto sector = *reinterpret_cast<uintptr_t*>(node + field::sceneNode::sector);
    if (!sector) return;
    const auto raw = xd::nativePoseMetres(frame.renderHead, stereoUnitsPerMetre);
    const auto desired = trackedBodyCamera.step({savedRoot, frame.scene, sector, frame.epoch, frame.stream, frame.inputGeneration}, frame.bodySample.head, raw, frame.stageBasis, frame.bodyDriven);
    const auto correction = xd::compose(desired, xd::inverse(raw));
    for (auto& eye : stereo.eyes) eye.pose = xd::nativeItemPose(xd::compose(correction, xd::nativePoseMetres(eye.pose)), stereoUnitsPerMetre);
    stereo.center.pose = xd::nativeItemPose(xd::compose(correction, xd::nativePoseMetres(stereo.center.pose)), stereoUnitsPerMetre);
}
static bool trackedBodyOwns(uintptr_t root) {
    __try {
        TrackedDriveFrame frame{};
        const auto state = *reinterpret_cast<unsigned*>(root + field::character::stateFlags);
        return trackedBodyRequested && trackedDriveRead(root, frame) && frame.bodyDriven && readable(root, 0x1300) && bodyCalm(root, *reinterpret_cast<uintptr_t*>(root + field::character::headPart)) &&
               (state == 0 || state == 1 || state == 8) && *reinterpret_cast<int*>(root + field::character::motion) >= 0 && *reinterpret_cast<int*>(root + field::character::motion) <= 4 && !*reinterpret_cast<uintptr_t*>(root + field::character::actionCallback);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Held objects follow the achieved physical hand, not a controller target that could
// already be on the far side of a collision. 0: not driven, 1: refused, 2: achieved pose.
static int trackedBodyHand(unsigned side, float* pose) {
    if (!trackedBodyRequested) return 0;
    __try {
        const auto root = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot));
        if (side > 1 || !trackedBodyOwns(root)) return 1;
        const auto hand = *reinterpret_cast<uintptr_t*>(root + field::character::handParts + (1 - side) * 8);
        if (!trackedPart(hand, root)) return 1;
        const auto native = xd::nativeItemPose(trackedPartPose(hand), stereoUnitsPerMetre);
        memcpy(pose, native.data(), 48);
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
