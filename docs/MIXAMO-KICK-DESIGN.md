# Mixamo kick: design (Phase 1)

Goal: a kick that looks like Hunter's Mixamo clips in first and third person, while the game's physics,
balance, damage and stamina stay native. Setting: **Kick style: Classic / Animated** (default Classic).

Everything below was traced in this worktree (branch `claude/mixamo-kick`, from `ebe9a5b`) and in the
reviewed executable (Exanima 0.9.5.2g, SHA-256 af9033c7…, `game/Exanima/Exanima.exe`). RVAs are from that
build. **No new game offset or signature is needed**; see "What the design reuses".

## 0. Phase 2 revision (2026-11-05): full body, clip sets by weapon

Hunter changed the plan after Phase 1: the clips play **as authored on the whole body** (no lowering; only a
per-step safety cap), and the clip set depends on what the player holds. Sections 4, 5 and 8 are rewritten for
that; sections 3 and 7 still hold, and section 1 is the superseded leg-only summary. What Phase 2 built:

* **Clip sets and modes** (`src/kick_retarget.h`, `classRules`; superseded by the full class list and verdicts in section 9):

  | weapon class (detected) | set | body |
  |---|---|---|
  | unarmed | 0: Standing Melee v2, Sword and Shield, MMA | full |
  | one-handed, with or without a shield, dual wield | 0 | full |
  | two-handed | 1: Great Sword, Great Sword 1 | full |
  | polearm | 0 | hips and legs only (the game keeps the arms and the hold) |

  Within a set the clips rotate, never repeating the previous one. A polearm-specific clip is a new row/set.
* **Detection uses existing fields only**: `character::weaponCount`, `character::heldItems` (both hands holding
  the same item is a two-handed grip) and `character::gripFlags` (0x10 one hand, 0x20/0x30 two hands with a
  right/left lead, as `xr_weapon.inl` already reads them). **A polearm cannot be told from a two-handed sword with
  what the mod reads**: the polearm bits live in the held item's own record (ExanimaLua reads them from the item
  payload's first flag word, `flags & 0x30`), which ExCamera does not read or witness. Until that field is reviewed
  and added, spears, halberds and staves held in two hands get the two-handed (Great Sword) set, full body. The
  table row for polearms is in place.
* **Mirroring**: each baked clip is stored as a right-leg kick. At kick start the clip is mirrored when its stance
  (supporting foot ahead of or behind the hips) does not match which of the character's feet leads. The status file
  counts whether the game's own kick lifted the same leg (`native_leg_agree/disagree`).

### Stepping and pivoting clips (MMA, Great Sword, Great Sword 1)

These clips pivot the supporting foot or step (54-81 cm of slide) and turn the hips 90-120°. The game's supporting
foot is held by its contacts and support code, so **the step does not happen**: the foot stays planted where it
stood. What the drive does instead:

* The legs, torso and arms follow the clip's directions in the heading frame, so the kicking leg still travels its
  authored arc (a roundhouse comes round from the side) and the torso still turns.
* The hips follow the clip's offset from the supporting foot. Because that foot does not slide, the hips travel only
  as far as the planted foot allows; the per-step cap (10 cm) keeps the target near the body.
* The visible difference: a pivot kick done on a planted foot, i.e. the knee and hip twist where the clip's foot
  would have turned. That may read as stiff, or topple the character on the 120° Great Sword turn.

Recommendation: keep all five as authored for the first play check (as Hunter asked) and watch the status file's
`fell` and `cut_short` against `last_clip`. If the pivots topple or look wrong, the options are, in order: scale
the hip yaw for those clips only, trim them to the strike (drop the wind-up turn), or drop them from the rotation.
None needs new game data.

## 1. Summary (Phase 1, leg-only; superseded by section 0)

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
4. Admission for Animated: groups `kick` and `xrDrive` available, the same admission the VR motor drive uses
   (`createTrackedDriveHooks`). Built: the whole-function contracts were not added to desktop builds after all
   (their header is ~390 KB of text); a withheld group shows the settings row as unavailable and Classic stays.
5. Built: the detour is created with the other hooks at start (`createKickDriveHook`, after the stereo hooks) when
   the groups resolve; with no kick being animated it costs one thread-local check per call.

## 4. Retarget (full body, as built)

### 4.1 Offline bake

`tools/kick_bake.py bake <dir>` turns the five clip JSONs into `tools/kick_clips.json` (committed), and
`kick_bake.py header` generates `src/kick_clip.h` (checked by `tools/check_generated.py`). Per frame, in the clip's
heading frame, only chords between joints both skeletons have:

* pelvis up (mid-hip to Spine2) and across (left hip minus right hip); chest up (Spine2 to Head) and across (left
  shoulder joint minus right);
* unit directions of each shoulder, upper arm, forearm, thigh, shin and foot;
* the mid-hip's offset from the supporting foot, in cm (the authored hip travel).

Chords need no knowledge of the game's part rest frames, and the clip's travel across the floor drops out.

### 4.2 Run time, per physics step (`src/mod/pose_drive.inl`)

1. The game's kick clip is the carrier: the key, gates, stamina, damage and reactions are unchanged. While it runs,
   `kickPoseBegin` (from `poseBuilderOwned`, only when the VR drive did not plan) reads the 18 parts by structure.
2. First step of a kick: weapon class, rule, next clip of the set, heading (character facing, hips across), and the
   mirror from the stance.
3. Time: native `motionProgress` warped `[0, 0.35, 1] -> [0, peak, last]`; the clip is sampled at 30 fps with
   interpolation. Weight: smoothstep in over the first 12%, out over the last 15%.
4. Targets: trunk parts (pelvis, belly, chest) turn so their measured up/across chords match the clip's; limbs turn
   so their bone points along the clip's (twist kept); head, hands and feet turn with their parent. Every turn is
   capped at 0.35 rad per step (the only safety clamp; the motion is not lowered).
5. Hips: the pelvis target is placed so the mid-hip sits above the supporting foot by the clip's offset, scaled by
   leg length (character / clip), at most 10 cm from where it is per step.
6. In the composition detour, each planned part's native local target is blended toward the clip's by the weight
   (rotation slerp; the pelvis's position too). Lower-body mode plans only the pelvis and legs.
7. Arms: in full-body mode the arms play the clip fully, holding something or not (Hunter: the clips are played as
   authored; weapons whose hold does not match a set get lower-body mode instead). The grip joints stay native.

## 5. Balance (full body)

* The hips now move: the pelvis target follows the clip relative to the planted foot and is composed with the
  game's balance frame at the pelvis site, so the game's balance still adds its own correction. The support code
  (`standingLegs`) runs after the pose builder and keeps the planted foot.
* Risks, in order: toppling on high kicks and pivots (the clips are not lowered any more); the game's balance and
  the clip's hip travel disagreeing (seen as shaking during the blend); the game's kick lifting the other leg than
  the clip during the first 12% (counted as `native_leg_disagree`).
* Status (`first-person-status.json`, object `kick_animated`): `kicks`, `blended_in`, `blended_out`, `cut_short`
  (the kick ended while still blended, e.g. interrupted), `fell` (the state left upright during the kick or within
  1.5 s after), `last_clip`, `last_class`, `last_mode`, `last_mirrored`, `held_arm_steps`, `native_peak_progress`
  (where the game's own foot peaked, to tune the 0.35 hit point).
* Hands and weapons: arms play the clip fully. The grip joints are physical constraints, so the weapon is carried,
  not dropped; it can clip the body or a shield where the clip's guard differs from the game's hold. A weapon whose
  hold does not match its set belongs in lower-body mode (classRules); `held_arm_steps` counts armed steps.

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

## 8. Phases, effort, risk (revised for full body)

| phase | work | effort |
|---|---|---|
| 2 (built) | Setting, five baked clips in two sets, weapon-class table, full/lower-body drive, hips over the planted foot, status counters, tests, all builds. | done |
| 3 | Hunter's play checks per clip and weapon; tune hit point, blend widths, caps, held-arm weight; fix what topples. | 2-4 days (paced by play sessions) |
| 4 | Pivot clips: hip yaw scaling or trims; foot (toe) direction from the clip once the foot part's axes are measured. | 1-2 days |
| 5 | Polearm detection (needs the item-record flag field reviewed and witnessed, see section 0), VR check, docs, release rows. | 1 day |

**Patch risk** is unchanged: the drive uses only the `kick` and `xrDrive` groups and witnessed fields; any mismatch
withholds Animated and the kick stays Classic. **Run-time risk** is higher than the leg-only plan: the whole body
and the hips move as authored.

## 9. Weapon classes and which kicks suit them (2026-11-05)

### 9.1 How the game classifies a held weapon (reviewed exe 0.9.5.2g)

* **The flag word.** A weapon's item record carries a weapon chunk (tag 0x4000) whose first u32 is its flag word
  (ExanimaLua reads the same chunk). On equip, the posture selector `0x1523F0` copies the primary weapon's word
  (`weapon part +0x18` → node `+0x198` → payload, low 16 bits) into `character::gripFlags` (+0x1068) at `0x15241F`;
  `gripRefresh 0x152100` does the same at `0x1521A2`.
* **The decision** (`0x152531`..`0x1525F0`, now pinned by the reviewed signature `kickStanceClass`, group
  `kickClasses`), after two earlier branches: a shield part (`character::shieldPart` +0xBF8) selects the shield
  postures 0x100/0x200/0x300 by the shield's kind bits (+0xC40, `0x15245D`); a special held part
  (`character::specialHeldPart` +0xC00) selects posture 3 (`0x1524DD`). Otherwise by `gripFlags & 0xF0`:
  * 0x10 one-handed: two weapons → dual wield (gripFlags 0x90, posture 4, kind 6); else posture 1, kind 1.
  * 0x20 two-handed: flag word 0x521 → posture 0x30 (and +0x1309 set, the greatsword); flag bit 0 → posture 0x20;
    else posture 0x10; kind 2.
  * 0x30 → posture 0x40, kind 2.
  * anything else → unarmed: gripFlags 0x80, or 0xC0 when the character data's +0x58 bit 0x100 is set.
  Before this, a weapon whose item type passes `0x104440(type, 0x100)` takes posture 0x10010001 (`0x152527`): an
  item category the table below does not cover (unresolved: likely tools held as weapons).
* **The hold** (`gripLookup 0x155250`, pinned by `kickGripClass`): grip-table entry (0xB8 bytes at 0x2BAC50) =
  `(flags & 0x70) >> 4`, with 0x90 → entry 1 and the second weapon slot always entry 5. Entry 1: one hand, weapon on
  the right; entry 2: two hands, right lead (callback 0x152A30); entry 3: two hands, left lead, the haft across the
  body (callback 0x152E10); entry 5: the left (off) hand.
* **The items** (the game's item database `objdb.rdb` in `Resource.rpk`, 208 weapon chunks; names from the records,
  a few attributions are noisy):

  | flag word | class (gripFlags & 0x70) | examples |
  |---|---|---|
  | 0x11 | one-handed, cutting | swords (short, arming, gladius, Proctor), daggers, knives, cleaver, saw |
  | 0x12 | one-handed, chopping/crushing | flanged/knobbed maces, war hammer, horseman's axe, throwing axe, pickaxe, smithing hammer |
  | 0x13 | one-handed, blunt stick | clubs, spiked club, cudgel, baton |
  | 0x14 | one-handed, flail | flail |
  | 0x22 | two-handed, right lead (posture 0x10) | battle axe 2H, bardiche, sledgehammer, 2H mace, woodcutter's axe, shovel |
  | 0x23 | two-handed, right lead (posture 0x20) | metal bar, broom, crutch, improvised 2H items |
  | 0x421 | two-handed, right lead (posture 0x20) | two-handed swords, 2H falchion |
  | 0x521 | two-handed, right lead (posture 0x30) | the Greatsword |
  | 0x32 / 0x33 | polearm, left lead (posture 0x40) | poleaxe, halberd, glaive, guisarme, spetum, voulge, polehammer, awl pike, pitchfork, 2H bill |

  Spears, partisans and quarterstaffs are built by the item factories (`Factories.rpk`) and did not show as
  database chunks; by their hold they should be 0x3x (left-lead hafts). Confirm in play from the status
  (`stance_by_class`).

* **What the mod reads.** Only fields it already reads and witnesses: `character::weaponCount`, `gripFlags`,
  `shieldPart`, `specialHeldPart`. The class bits are `gripFlags & 0x70`, which survive the posture selector's
  final `& 0xF0` (`0x152618`). The finer two-handed split (axe vs. staff vs. sword vs. greatsword) is in the low bits
  that `0x152618` clears, so it is not used; it does not change the hold (all use grip entry 2).
* **Fail-safe.** New optional signature group `kickClasses` (`kickGripClass` at 0x155250, `kickStanceClass` at
  0x152531, both whole-instruction code patterns in `tools/targets.json`, checked by `check_game.py` and at
  start-up). If a game update changes either, Animated is withheld and the kick stays Classic.

The game's own names for these classes (table 0x2ED0F0, read at 0x258CE5; audit checklist 62c3d5b): One Handed
(0x10), 2W Light (0x90), Two Handed / 2H Light / Greatsword (0x20), Polearm (0x30); shield path 1H & Shield /
Shield C1 / Shield C2; special held 1H & Torch. See docs/GAME-FIELDS-AUDIT.md section 3.

### 9.2 The clips' arms and stances (measured from the converted clips)

Hands from the mid-hip in cm (across to the left, up, forward); lead foot at the start.

| clip | start: left hand / right hand, hand gap | strike: hands | lead foot | reads as |
|---|---|---|---|---|
| Standing Melee v2 | (19, 7, 44) / (-47, 10, -13), 87 cm | left up in front, right back | left | loose guard, right hand low behind: unarmed or a one-handed weapon held back |
| Sword and Shield | (-9, 25, 42) / (-38, 18, -22), 71 cm | arms wide for balance | left | shield up in front, sword cocked low behind |
| MMA | (6, 46, 45) / (-24, 44, 25), 35 cm | right arm swings down and back | left | both fists at the face: boxing guard |
| Great Sword | (6, 1, 30) / (-13, 8, 38), 23 cm | hands together, low right | left | two hands on one hilt, low in front, right hand on top |
| Great Sword 1 | same start, 23 cm | hands together, high left | left | same hold, swung the other way |

### 9.3 Verdicts (class × clip) and the default per class

full = full body OK; lower = lower body only (the game keeps the arms); no = not compatible.

| class | Standing Melee | Sword and Shield | MMA | Great Sword / 1 | default (code `classRules`) |
|---|---|---|---|---|---|
| unarmed | full: loose fists | full: an empty guard | full: boxing guard, best fit | no: hands clasp an invisible hilt | set 0, full |
| one-handed | full: weapon hand low behind | full: weapon cocked behind, best fit | full: weapon raised by the face (watch clipping) | no: second hand would grab the blade | set 0, full |
| one-handed + shield | full | full: shield up front, best fit | full: shield at the face | no | set 0, full |
| dual wield | full | full: left weapon forward | full | no | set 0, full |
| shield only | full | full, best fit | full | no | set 0, full |
| two-handed (0x2x) | lower: the arms pull 87-109 cm apart, against the grip | lower: same | lower: hands 35-83 cm apart | full: right-lead two-hand hold matches entry 2 | set 1, full |
| polearm (0x3x) | lower | lower | lower | no: right-lead hilt hold against a left-lead haft across the body | set 0, lower |
| special held | lower | lower | lower | lower | set 0, lower |

Caveats: the game's own stance pose was not extracted from its motion files (they are not decoded); the verdicts
compare the clips' measured arms with the hold the code defines (grip-table side and lead). The status file now
records the game's actual stance at each kick start per class (`kick_animated.stance_by_class`: hands in the same
cm frame as 9.2, lead foot, shoulder twist), so the table can be confirmed or corrected from one play session per
class. Two-handed axes, hammers and staves hold the hands farther apart on the haft than a hilt; if the Great Sword
arms fight their grip, that class's two-handed sub-kinds move to lower body (needs the low flag bits, see 9.1).

## 10. Toppling during the kick: what the game does to the legs (decode in progress)

Everything below is read from the reviewed exe; nothing is implemented yet.

* **Order each physics step** (`combatUpdate` 0x169200): `supportPosition` 0x163BB0 (0x16BB4C) → pose builder
  0x1760F0 (0x16CFF0, our hook) → `motionUpdate` 0x178350 (0x16D021) → `contactFrame` 0x1603F0 (0x16DDD0) →
  `supportSettle` 0x160070 only when +0x12EE is clear (0x16E014–0x16E04C) → `supportHeight` 0x16E1D0 every step
  (0x16E058), which calls `standingLegs` 0x15F080. Our hook runs before `contactFrame` rewrites the contacts, so it
  cannot change what `supportHeight` reads in the same step.
* **Motion 13 forces both legs into support.** `contactFrame` jumps to 0x1612EB when the local bl is set, motion ==
  13 (0x1612C9), character +0x12FC == 0, or state == 0x4000. That path sets every shin contact's flags (+0x340 → +0x10)
  to exactly 2 (0x161325) and its anchor +0x24 to its local point +0x30 through the current frame (0x18870 is a 3×4
  matrix × point; 0x16132C–0x161339); in state 0x300 below a progress threshold it clears bit 2 again
  (0x161344–0x1613AD). The normal path (0x1613C0 on) is the stepping logic: per leg it reads bit 8 and character
  +0x109C, clears bits 0xC (0x161436/0x16143E), updates the flags (0x161495) and copies anchor +0x3C to +0x24
  (0x161498), then builds the support frame from character +0x3C (0x19AE0, 0x162380).
* **What support (bit 2) changes**, in `supportHeight`: shin +0x40C = 2.0 for support without bit 4, 0 otherwise
  (0x16E6E8–0x16E746; constants 0x2BAAA0, 0x31BF28); +0x40C is read by the rigid-body physics 0x80CD0–0x88230. The
  ground query height is the contact's +0x90 minus 10 for a support, +0x90 otherwise (0x16EDA0–0x16EDF5), then
  0x150FF0. Bit 2 is read again at 0x16F1A5. `standingLegs` targets each foot from anchor +0x24 (or lerp(+0x3C, +0x24)
  with bit 4; 0x15F22E–0x15F300).
* **Not decoded yet**: what +0x40C is to the physics (the readers in 0x80CD0–0x88230), the rest of `supportHeight`
  (0x16F1A5 on, `heightForce` 0x16FDF0), and how the support height moves the pelvis. Until these are read, the
  cause of the toppling is not proven, and there is no proven moment between `contactFrame` and `supportHeight` for a
  fix: changing a kicking leg's flag would need a new hook there (a new signature target, to be reported first).
