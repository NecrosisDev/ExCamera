# Changelog

## 0.1.0-beta3-input16

- Share combat targeting/crosshair screen projection; remove duplicate third-person ray reconstruction. Native rays drive both free and locked third-person head gaze.
- Correct viewport/view/camera identity checks that rejected native aim in input15, and pass the viewport to gaze and camera collision picks. Consolidate trial-pose picks in the camera owner.
- Retain overhead, physical-head following, head/look separation, mouse capture, interaction handoffs and native targeting. Correct fixtures to model distinct native object types; gameplay testing remains pending.


## 0.1.0-beta3-input15

- Restore native opponent/body target selection for first/third-person combat, adapting the existing plane/pick owners to preserve selected depth and actual view rays.
- Retain native assistance blending and world queries; remove the overhead height restriction on close-view surface hits. Preserve overhead, physical-head camera, native attack tracking and existing guard/carrying behavior.
- Validate native geometry/frame contracts with Ghidra and add production-owner offline regressions. Gameplay acceptance remains with the owner.

## 0.1.0-beta3-input14

- Remove automatic gaze-driven spine bending from first/third-person combat. Native attack aiming remains in control; explicit lean and exploration spine following are preserved.
- Review cursor/target, thrust, swing selection, movement, cues and input ownership against native paths. Retain the close-view target adapter because native fallback aiming still uses a horizontal-plane intersection.
- Preserve overhead and the physical-head camera. Gameplay testing remains with the owner.

## 0.1.0-beta3-input13

- Consolidate duplicated guard readiness checks without changing input12 guard behavior. Native code continues to own interception tracking, retained poses and transitions.
- Cover native target updates that retain a defensive pose as well as native pose changes. Gameplay acceptance remains with the owner.

## 0.1.0-beta3-input12

- Held Manual guard actively requests native attack-specific interception from rest/ready, preserving native weapon targeting, motion selection, ongoing defense, attack recovery and overhead behavior.

## 0.1.0-beta3-input11 - 2026-10-17

- Held Manual guard now requests the native ready posture, retaining balance, active parry and attack recovery; Automatic and overhead stay native.

- Feed current close-view aim to native opponent scoring without steering the physical head toward the free-aim cursor. Native selection, defense eligibility and physical facing remain authoritative.
- Restore gaze after the native selector, including exceptions and handoffs; preserve overhead, Manual-defense permission, carrying, fading and menu behavior.
- Add production-owner regression coverage and stamped aim ownership. Gameplay acceptance remains pending; see docs/GAMEPLAY-CHECKS-INPUT11.md.

## 0.1.0-beta3-input10 - 2026-10-17

- Correct native overhead obstruction fading in first/third person. Existing faded masonry recovers through native fade timing; overhead keeps its original behavior.
- Scope only the native camera owner and its two obstruction tests. Preserve unrelated transparency, materials, draw passes, TP blockers and input9 carrying.
- Add regression coverage for faded meshes, view transitions, nested cameras and fault cleanup. Gameplay acceptance remains pending; see docs/NATIVE-REVIEW-INPUT10.md.

## 0.1.0-beta3-input9 - 2026-10-17

- Bound native held-item placement to the real cursor ray and pickup distance, including inventory-to-world placement. Exclude the player from that scoped collision query.
- Preserve native shape collision, accepted-pose history and release; cover distant third-person cameras with consecutive native sweeps.
- Expand regression coverage beyond query output to downstream placement. Gameplay acceptance remains pending; see docs/NATIVE-REVIEW-INPUT9.md.

## 0.1.0-beta3-input8 � 2026-10-17

- Remove remaining overhead placement assumptions from inventory-to-world dragging and free physical constraints in first/third person. Keep native placement, springs, collision and release.
- Bound physical door/hinge targets, keep separate histories for each hinge plane, and use the visible reticle during frame updates. Pause held movement and rotation while UI or focus owns input; retain actual-pointer dragging during configured RMB free cursor.
- Use view-yaw movement axes in both native controller providers without repolling devices or changing their scalar values, gait, buttons or separate aim channels.
- Consolidate F8 into Camera, Controls and Aim & combat, with section headings, clearer descriptions and compact spacing. Preserve settings IDs/defaults; Reset page affects the displayed page.

See [native safety review](docs/NATIVE-REVIEW-INPUT8.md) and [gameplay checks](docs/GAMEPLAY-CHECKS-INPUT8.md). Offline checks do not establish gameplay acceptance.

## 0.1.0-beta3-input7 — 2026-10-17

- Preserve the input6 repairs and reconcile them into the standalone source.
- Use the actual first/third-person view ray and held distance for carrying objects. Remove the synthetic downward ray and overhead waist-plane admission from close-view holding.
- Keep native collision, accepted-pose history, rotation propagation and release handling. Overhead retains native placement and slot snapping.
- Reject blocked, invalid or stale held queries without publishing a replacement placement; preserve ownership and pick masks through nested calls and faults.

Ghidra review and offline validation are recorded in [Native review](docs/NATIVE-REVIEW-INPUT7.md). Gameplay acceptance remains pending.

## 0.1.0-beta3-input6 — 2026-10-17

- Restore exploration clicks, object and door dragging, container double-clicks, and inventory mouse input.
- Choose either swing direction with mouse movement, including small aim boxes; show the wind-up arc on its starting side and preserve Alt alternate attacks.
- Align the camera with the game’s render samples and keep third-person smoothing consistent across those samples.
- Correct held-object placement, distance, rotation, and release handling.
- Clarify settings names and tooltips, keep every row reachable, show inactive dependencies, and retry failed saves.
- Fix third-person obstruction handling and release head and spine adjustments when returning to the native view.

## 0.1.0-beta3-input5 — 2026-10-17

- Add independent **Automatic / Manual defense** settings for first and third person. Manual uses held right mouse to request the game's defensive assistance; Automatic remains the default.
- Separate combat aim from movement: right mouse no longer cursor-walks in close views, and keyboard footwork follows the view's horizontal direction.
- Add independent aim-box dimensions and optional left/right lean keys. Third-person free aim now works outside combat too.
- Use fresh combat aim and clear stale movement or attack requests when controls, views, menus or player ownership change.
- Fix held-object handoffs, interaction-key handling, crouch ownership, third-person obstruction tracking and camera transitions.
- Improve launcher reentry, mouse fallback, settings saving and graphics cleanup.
- Strengthen game-update compatibility checks. Unsupported Manual defense falls back to Automatic without changing the saved setting.

Builds and automated tests pass. **This update has not been tested in game.** Controller movement is not fully adapted; native movement, balance, recovery and carrying restrictions remain.

## 0.1.0-beta3

First release with public source.

- Add game-update checks and a choice when another mod uses the same game functions.
- Improve first-person head positioning, helmet visibility and nearby clipping.
- Improve third-person camera clearance, tight-space framing and free aim.
- Add held-object distance, rotation and throwing controls, plus modifier-and-mouse leaning.
- Organize F8 settings into six pages with individual resets.
- Rename the mod to ExCamera and move settings to `Documents\ExCamera`, importing older settings automatically.

Play-tested on Exanima 0.9.5.2g (Steam build 25803178), Windows x64.

## Earlier betas

See [GitHub Releases](https://github.com/NecrosisDev/ExCamera/releases).
