# Camera adapter acceptance

## Scope

First-/third-/native-overhead camera integration against the real Exanima process, using disposable Arena practice characters. This acceptance covers the reviewed sampler adapter and its teardown, not full mouse ownership, collision, combat, or release readiness.

Game executable SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

## Implemented boundary

- One hook at the reviewed native camera sampler (`RVA 0x94720`).
- Native head render sampling (`RVA 0x8B6E0`) and projection rebuild (`RVA 0x9F2E0`).
- Exact executable hashing before attachment and again before camera activation.
- First-/third-/overhead mode controls and explicit look controls for deterministic testing.
- Generation-scoped native camera snapshots and safe fallback to the original camera.
- Restoration requested on Exanima's camera thread, with a bounded wait and a safe retry on failure.
- ABI 2 status with installed/active hooks, module faults, and restoration failure.

## Defects found and corrected during acceptance

### Paused transitions did not complete

The first third-person selection remained in transition while the game was paused because transition time came from the native sample offset, which is zero in that state. The adapter now uses bounded wall-clock deltas for controller transitions and retains native sample offset only for physical interpolation. Third-person and overhead transitions subsequently completed while paused, with no unpaused interval.

### Head render-transform stride was misread

The head render transform stores four floats per row; only the first three are significant. The initial adapter treated the rows as a packed 12-float pose and shifted the up/forward axes. Conversion moved into `excamera::strided_pose` in the domain math layer, and a regression test now proves that padding is ignored correctly.

## Final observed results

The final corrected build was tested in a fresh paused Arena practice session:

- Attach: `state=active`, `installed_hooks=1`, `active_hooks=1`, `module_faults=0`, `restoration_failed=0`, `abi=2`.
- First person: physical eye-mounted view rendered; an explicit level look was accepted and rendered.
- Third person: the player character was visible from behind, with the camera offset applied. The transition completed while paused.
- Overhead: native overhead presentation resumed. The transition completed while paused.
- Stop: `state=stopped`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`.
- Practice process was closed through its native Close control.

Earlier repetitions also confirmed first-/third-/overhead presentation and clean stop before the two fixes. Only the final corrected results are treated as acceptance for this adapter.

## Offline checks

- Six CTest suites pass, including the new strided-render-layout regression.
- Controller tests cover invalid heads, generation changes, collision timing, invalid settings, NaN input, native handoff, and repeated mode cycles.
- Package exports include `ExaRuntimeStart`, `ExaRuntimeStop`, `ExaRuntimeGetStatus`, `ExaCameraSetMode`, and `ExaCameraSetLook`.

## Evidence limits

- Mouse look, cursor ownership, rebinding, and settings UI have not been implemented or accepted.
- Third-person geometry clearance is still a domain input, not a live native collision query.
- Head/headwear hiding and visual filtering are not implemented; visible body/head geometry in first person is not a finished visual policy.
- Movement/combat alignment, interaction/picking, death, scene travel, focus changes, resolution changes, and long soaks remain separate acceptance work.
- EMTK, ExanimaLua, Hellmode, and other game builds were not tested.
