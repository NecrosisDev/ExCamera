# Lifecycle and defeat acceptance

## Scope

Player/scene identity changes and an actual defeat, including clean stop while the native camera sampler is no longer running. The final test uses the three-hook build with a presentation restoration fallback.

## Identity handling

A `LifecycleTracker` detects changes to player, scene, head, camera, and controller identities. On a changed generation the adapter:

- drops the old ownership snapshot;
- clears look validity and reinitializes from the new head;
- resets cursor deltas and the input state machine;
- resets presentation timing before evaluating the new frame.

The live story-to-arena transition exercised this reset path rather than carrying the previous scene's look into the new one.

## Defect found and corrected

The first defeat test reached `You have been defeated` with the camera coherent, but stop timed out. The camera sampler had stopped, so its restoration request could not run. This was a real lifecycle failure, not a test-environment issue.

A presentation-boundary fallback was added at `RVA 0x21E60`. It forwards the original presentation call, then services pending camera and cursor restoration. Restoration therefore no longer depends on either the camera sampler or mouse callback continuing during death or menus.

## Final observed result

- Practice restarted with the fallback adapter active: `state=active`, `installed_hooks=3`, `active_hooks=3`, `module_faults=0`, `restoration_failed=0`.
- The opponent defeated the character and the defeat screen appeared.
- Stop at the defeat screen completed immediately: `state=stopped`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`.
- The game was then exited through the native defeat and title Exit rows.

## Offline checks

Lifecycle tests cover initial identity, repeated identity, every player/scene/head/camera/controller change, and reset requiring fresh initialization. All nine CTest suites pass.

## Evidence limits

- The story-to-arena transition is an identity/scene generation test, not proof of every in-game level-exit path.
- Death, resolution/fullscreen changes, inventory/shops/dialogue, and long soak coverage remain separate work.
- EMTK, ExanimaLua, Hellmode, and other builds were not tested.
