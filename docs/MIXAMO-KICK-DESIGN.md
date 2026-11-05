# Mixamo kick: design (Phase 1)

Goal: a kick that looks like Hunter's Mixamo clips in first and third person, while the game's physics,
balance, damage and stamina stay native. Setting: **Kick style: Classic / Animated** (default Classic).

Everything below was traced in this worktree (branch `claude/mixamo-kick`, from `ebe9a5b`) and in the
reviewed executable (Exanima 0.9.5.2g, SHA-256 af9033c7…, `game/Exanima/Exanima.exe`). RVAs are from that
build. **No new game offset or signature is needed**; see "What the design reuses".

## 1. Summary

* Animated is an *overlay on the Classic kick*. The key still starts the game's own kick clip (`0xD`) exactly
  as now (`src/mod/motions.inl:32-67`), so gates, stamina, motion-busy state, AI reactions and contact damage
  stay native. While that clip runs, the mod replaces the **kicking leg's** motor targets (thigh, shin, foot)
  with the Mixamo leg, inside the pose builder, through the same compose hook the VR body driver uses.
* Any check failing (setting off, group withheld, a contract mismatch, part graph unexpected, VR body driver
  owning the legs) leaves the step untouched: the player gets the Classic kick, never no kick.
* Timing follows the native kick's own progress (`character::motionProgress`, `0xA28`), evaluated every physics
  step, so the Mixamo motion is stretched to the native clip and is frame-rate independent.
* The support leg, pelvis and balance stay native. Torso lean (chest/belly, small) is a later phase.
* Recommended clip: **Standing Melee Attack Kick Ver. 2** (right-leg front push kick; support foot moves 2 cm;
  torso lean about 14°), lowered from 105 cm to about hip height by scaling hip flexion. Backup: **Sword And
  Shield Kick** (shorter, 1.2 s, support foot moves 4 cm).

## 2. Input data (Phase 0, done)

* FBX files copied from `C:\Users\hunte\Downloads` to `E:\ExCamera\.worktrees\mixamo-input\` (outside git).
* `tools/mixamo_fbx2json.py` (stdlib only): binary FBX 7.x reader, LimbNode bones, OO/OP connections,
  `PreRotation * Lcl Rotation` (eEulerXYZ = Rz·Ry·Rx), 30 fps linear sampling, globals composed down the
  hierarchy. Output: `mixamo-input/json/*.json` (22 bones, local and global quats, global positions, Hips
  translation). Space: +Y up, +Z character forward, +X character left, cm. Checked: toes point +Z, left leg at +X.

| clip | frames (30 fps) | kick leg | foot peak | peak at | hip flex / knee at peak | support foot slide | Hips XZ travel |
|---|---|---|---|---|---|---|---|
| Standing Melee Attack Kick Ver. 2 | 43 (1.43 s) | right | 105 cm | f22 (0.73 s) | 107° / 8° (push kick) | **2 cm** | 41 cm |
| Sword And Shield Kick | 37 (1.23 s) | right | 112 cm | f15 (0.50 s) | 117° / 30° | 4 cm | 52 cm |
| Great Sword Kick (1) | 46 | right | 109 cm | f20 | 102° / 6° | 28 cm | 39 cm |
| Great Sword Kick | 53 | left | 117 cm | f24 | 98° / 19° | 81 cm (steps) | 57 cm |
| Mma Kick | 49 | left | 125 cm | f18 | 106° / 32° | 54 cm (pivots) | 29 cm |

The Mixamo character's hips are ~85-95 cm high, so every clip kicks at chest/head height. For Exanima a kick
to the belly is more believable and much kinder to the native balance: target foot height ~80-90 cm (scale the
thigh's flexion by ~0.7-0.8, keep the knee angle). All clips end where they started (0 cm), so the blend-out
lands on the stance pose.

## 3. The game side

### 3.1 Body parts

* A character owns `partCount` (`character::partCount`, 0x910) parts in `character::parts` (0x918). The armour
  and wound tables use 18 parts in this order (reference/ExanimaLua `API.md:303`): pelvis, right thigh, right
  shin, right foot, left thigh, left shin, left foot, belly, chest, head, right shoulder, right upper arm, right
  forearm, left shoulder, left upper arm, left forearm, right hand, left hand.
* The mod never relies on that index order. It identifies parts structurally, as the VR driver does
  (`src/mod/xr_drive.inl:866-918`): `parts[0]` is the pelvis (its `rootPart` and `parentPart` are itself, flag
  0x40); `character::legs` (0x9E0, `legCount` 2) holds the two shins (`bodyPart::role` 0x10); a thigh is the
  shin's `parentPart` whose parent is the pelvis; the foot is the part whose parent is the shin and role 0x13;
  the side is the sign of the hip in the pelvis frame (`hipLocal.x > 0` is left, `xr_drive.inl:920-923`).
  Upper body: `chestPart` (0xD48), its parent (belly), `headPart` (0x11E0), `handParts` (0xE78, slot 1 is left).
* Per part (`src/game_structs.h:90-117`): achieved pose `pose` (0x2A0, float[12], three axes then origin, local
  to the sector; `trackedPartPose`, `xr_drive.inl:716-726`, adds `sector::origin` and converts to a quaternion
  in metres), `animationPose` (0x550, the composed motor candidate), `targetPose` (0x4F0, the motor target),
  `parentPart` (0x428), `flags` (0xC; 0x40 = root/pelvis chain, 0x4000 = apply `extraRotation` 0x5EC),
  `contactCount/contacts` (0x338/0x340, support contacts, flags at `contact::flags`).
* The mod reads parts and never writes a pose directly. It writes only: motor *inputs* inside the compose hook
  (VR), the spine bend's borrowed `extraRotation` (`input.inl:1380-1410`), and the jump's velocity change
  through the game's rigid-body push `0x89240` (`motions.inl:41-54`).

### 3.2 How a target pose becomes motion (traced in the exe)

1. **Per physics step**, the character update `0x169200` (`balanceUpdate` in `tools/xr_contracts.json`) calls
   the pose builder `0x1760F0` with mode 0 when the body is simulated (`character::physicsLive`, 0x4C8)
   (`0x16CFF0`), then `motionUpdate 0x178350`, then the support code (`supportSettle 0x160070`,
   `supportHeight 0x16E1D0` → `standingLegs 0x15F080`, which calls `footAngle 0x151CA0` per support foot).
   The step rate is `physicsContext::frequencyHz` (read by `bladeStepHz`, `blade.inl:85-92`).
2. The **pose builder** reads the current motion (`character::motion` 0x9B8, clip table at root+0x9B0),
   computes the normalized clip time into `motionProgress` (0xA28 = (time - start) / clip length,
   `0x1761CC-0x1761F7`), finds the two keyframes around it and an ease mode (root+0x1208), and per part:
   * interpolates the keyed local quaternion (`0x9D570`, 20-byte rows), cross-fades with the previous motion
     by the blend weight set by `motionPlay` (the Classic kick's 200 ms, `motions.inl:12,34`), optionally
     adds `extraRotation` (flag 0x4000);
   * builds the local affine from the quaternion and the part's rest offset (`0x1CF20`), and **composes it with
     the parent's animationPose** through `0x18B60` (`xrCompose`): the call at `0x176C69` returns to
     `0x176C6E` (`xrMotorComposeReturn`) and writes `animationPose` (0x550);
   * for the pelvis (flag 0x40) composes animationPose with the **balance frame** (`character::balanceFrame`,
     0xDFC) at `0x176CB2` → `0x176CB7` (`xrPelvisComposeReturn`) into `targetPose` (0x4F0);
   * for legs/feet, `footAngle` and the composes at `0x177366/0x177387` → `0x17738C`
     (`xrFootComposeReturn`) build the foot's `targetPose`;
   * `weightedAnchor 0x1C900` and `motorWeights 0x1CAF0` turn the targets into joint anchors and weights for
     the solver; the tetrahedron integrator (`0x88D30`) moves the points (Verlet).
3. So a native clip *is* a per-part local quaternion track, and the motors chase `targetPose`/`animationPose`
   with native stiffness, contacts and balance. The only clean injection point is the compose call: replace
   its `local` input for a planned part at a planned call site.

### 3.3 The VR full-body path and what it already solves

* `poseBuilderOwned` (`src/mod/input.inl:1415-1467`, hook created at `session.inl:1214`) opens a scope with
  `trackedMotorBegin(root)` before the native pose builder and closes it in `__finally` (`trackedMotorEnd`).
* `trackedMotorPlan`/`trackedLowerPlan` (`xr_drive.inl:731-951`) read achieved part poses, compute desired world
  orientations (thigh/shin by `directionRotation` from the achieved bone direction to the solved one, foot
  limited to 0.6 rad) and push `TrackedMotorEntry` rows (`xr_drive.inl:685-692`: part, parent, `lowerKind`
  0 upper/1 pelvis/2 thigh-shin/3 foot, world orientation, optional position).
* `trackedComposeFor` (`xr_drive.inl:1019-1087`), the MinHook detour on `xrCompose`, matches the return address
  (one of the three reviewed sites), the output address (`animationPose`/`targetPose` of that part), the parent
  address and the part's parent pointer, and the stack range of `local`; then it converts the desired world
  orientation into the parent frame (`motorLocal`, body frame 0x9F0 removed), composes, and copies the scratch
  back so the native joint-anchor builder sees the same local. Anything unexpected falls through to the native
  call unchanged.
* `BodyPoseDriver` (`src/xr/body_driver.h:17-44`) is only the tracker-to-native pose mapping for VR (bind-time
  deltas); the kick does not need it. What the kick reuses is the **scope + compose replacement**, i.e. the
  `lowerKind` 2/3 rows and their checks.

### 3.4 Compiling that path in desktop builds

`xr_drive.inl` is compiled only with `--openxr-sdk` (`first_person.cpp:40-99,122-125`); desktop builds get stubs
(`trackedMotorBegin` returns false, `first_person.cpp:86`). The XR maths (`src/xr/math.h`, `upper_body.h`,
`motor_math.h`, `native_pose.h`) already compiles without OpenXR (math.h declares the value types itself), and
the hook targets (`xrCompose`, `xrMotorComposeReturn`, `xrPelvisComposeReturn`, `xrFootComposeReturn`, group
`xrDrive`) are resolved by the shared signature table in every build (`game_signatures.h:208-217,718-728`).

Plan, without changing VR behaviour:

1. New `src/mod/pose_drive.inl` (every build, included after `blade.inl`, before `motions.inl`): owns one
   `xrCompose` detour and a thread-local *kick scope* (entries in the same `TrackedMotorEntry` shape), plus a
   pure header `src/kick_retarget.h` for the maths (unit-tested).
2. One detour, two clients: in VR builds `createTrackedDriveHooks` keeps installing `trackedCompose` exactly as
   now; its final fall-through (`xr_drive.inl:1086`) calls `kickComposeFor(caller, out, local, parent)` instead
   of the original directly. In desktop builds (or VR builds with the VR drive off) `pose_drive.inl` installs
   its own detour on `xrCompose` that calls `kickComposeFor`. Only one detour is ever created on the target.
   The VR scope is consulted first and returns when it replaced, so VR output is bit-identical.
3. `poseBuilderOwned` calls `kickPoseBegin(root)` only when `trackedMotorBegin` returned false, and
   `kickPoseEnd()` in the same `__finally` chain.
4. Admission for Animated: groups `kick` and `xrDrive` available, and the reviewed whole-function contracts for
   `poseBuilder`, `motorCompose`, `footAngle`, `motorWeights` (a subset of `fp::xr::contracts`,
   `src/xr/native_contracts.h`) match the loaded image. These are existing reviewed contracts, now also checked
   in desktop builds; any mismatch withholds Animated (Classic keeps working) and is reported in the status file
   and the settings row, like other withheld optional features.
5. The detour is installed when Animated is first selected (session worker) and stays; its body gates itself
   (return-address check first, so the many other `xrCompose` callers pay one compare).

## 4. Retarget

### 4.1 Offline bake (tools)

`tools/kick_bake.py` (stdlib) reads one clip JSON and writes `src/kick_clip.h` (generated, checked by
`tools/check_generated.py`): per frame, for the kicking leg only, three **unit bone directions** in the clip's
heading frame (thigh: UpLeg→Leg, shin: Leg→Foot, foot: Foot→ToeBase), the stance-frame directions (frame 0),
the peak frame, frame count and fps. Directions, not rotations: they need no knowledge of the game's part rest
frames or twist axes, and Hips root motion drops out (only differences of positions are used). A mirrored copy
(swap left/right, negate X) is baked too, so either leg can kick.

### 4.2 Run time, per physics step (inside the pose builder)

1. **Frame mapping.** Mixamo heading frame (X lateral, Y up, Z forward) → native world: Y to world up, Z to the
   character's facing at kick start (`character::facing`/body frame), X to the measured lateral axis (the left
   hip minus the right hip, projected). The lateral sign is measured, never assumed, so a mirrored native
   handedness cannot flip the kick. Captured once at kick start, so the leg does not swing with body turn noise.
2. **Time.** `p = motionProgress` (native, per step). A piecewise-linear warp maps native progress to clip time:
   `[0, p_hit, 1] → [0, f_peak, f_end]`, with `p_hit` measured from the Classic kick in Phase 2 (where the native
   foot peaks). Sample with linear interpolation of directions + renormalize (30 fps bake, ~60+ Hz steps).
3. **Height.** Rotate the thigh direction toward straight-down about the lateral axis by `(1 - s)` of its
   flexion (s ≈ 0.75); the shin and foot keep their angles relative to the thigh. Foot peak ≈ 80-90 cm.
4. **Blend.** Weight `w(p)` ramps 0→1 over the first ~12% and 1→0 over the last ~15% (smoothstep). Desired
   direction = slerp(achieved direction, clip direction, w). Outside `0 ≤ p ≤ 1` (start dwell) the weight is 0.
5. **Targets.** As `trackedLowerPlan` does: thigh orientation = `directionRotation(achieved thigh dir, desired) *
   achieved orientation`, same for shin, foot limited to 0.6 rad per step (`limitMotorRotation`). Rows
   `lowerKind` 2 (thigh, shin) and 3 (foot, no position): the motors chase them with native stiffness.
6. **Support foot.** Not driven: the native kick clip already plants it, and `standingLegs`/`footAngle` keep it
   on the ground. Mixamo's own support leg and Hips motion are ignored.

### 4.3 Which leg

The baked clip's leg must be the leg the native kick lifts. Phase 2 measures it (the shin whose contacts drop to
none during the Classic kick, `bodyPart::contacts`), then picks the original or mirrored bake at run time.

## 5. Balance

* The pelvis target is `animationPose ∘ balanceFrame` (0xDFC) and the support legs are solved by the native
  support code after the pose builder (`0x16E058` → `standingLegs`). Driving only the kicking leg keeps all of
  that native: the balance frame, support height and foot angles react to the leg's mass moving forward as they
  do for the Classic kick.
* Risk: a high, extended leg moves the centre of mass further forward than the native kick → stumble. Mitigations,
  in order: lower the kick (s ≈ 0.75); cap the thigh's change per step (0.35 rad, as the VR pelvis); keep the
  Classic blend (200 ms, `motions.inl:10-12`); later, a small backward chest/belly lean (`lowerKind` 0 rows,
  ≤ 0.15 rad) as Mixamo does. No pelvis or support-leg override is planned.
* `standingLegs` runs after our compose for the support leg only; if it also rewrites the swing leg's foot target
  in some state, the status counters (rows applied vs. composes seen) will show it in Phase 2.

## 6. Camera and views

First person rides the head, which stays native (the leg is visible when looking down, as with Classic). Third
person shows the whole body. Nothing in the camera code changes; VR body driving (`trackedBodyOwns`) disables
Animated (the VR legs are the player's own).

## 7. What the design reuses (no new offsets)

Fields: `character::{parts, partCount, legs, legCount, motion, motionProgress, facing, bodyFrame,
sceneNode}`, `bodyPart::{pose, parentPart, rootPart, role, flags, character, meshNode,
contacts}`, `sceneNode::{bodyPart, sector}`, `sector::origin` — all witnessed in `native_contracts.json`.
Signatures: `playerRoot`, `motionPlay` (group `kick`), `poseBuilder`, `xrCompose` and the three compose return sites (group
`xrDrive`). Contracts: `poseBuilder`, `motorCompose`, `footAngle`, `motorWeights` from `xr_contracts.json`.

## 8. Phases, effort, risk

| phase | work | effort |
|---|---|---|
| 2 | Setting row (Classic/Animated, ini `KickStyle`), `kick_bake.py` + generated header + check, `kick_retarget.h` maths with C++ tests (frame mapping, warp, height scale, blend, mirror), `pose_drive.inl` scope + shared detour, kicking leg only, status counters (`kick_animated`: admitted, steps driven, rows applied, refusals by reason). Build minimal + dev + VR, all tests, `check_game.py`, update camera-test only. | 1.5-2 days |
| 3 | Hunter's play check, then tune: `p_hit`, height scale, blend widths, which leg, foot limit. Status file guides each round. | 1-2 days (paced by play sessions) |
| 4 | Torso lean and arm counter-swing (`lowerKind` 0 rows on belly/chest/upper arms, small), optional second clip. | ~1 day |
| 5 | VR coexistence check, release checklist rows, README/CHANGELOG, manual text. | 0.5 day |

**Patch risk.** An Exanima update that moves the pose builder or its compose sites fails the `xrDrive` signatures
or the whole-function contracts → Animated is withheld, Classic stays. Fields are contract-witnessed and adapt
or withhold. No raw RVAs are added. **Run-time risks:** toppling on a high kick (mitigated as above), a swing leg
that fights `standingLegs` (measured in Phase 2), and the kick touching an opponent harder or softer than
Classic (damage stays native, but foot speed changes the impulse).
