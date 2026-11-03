# Collision adapter acceptance

## Scope and result

Third-person static-level collision was tested against the supplied standard Exanima executable in the disposable `CameraStory` story room. The game was closed after a clean stop. This record covers the central camera boom, not full frustum, ground, dynamic-object, or release acceptance.

Executable SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

## Defect found and corrected

The initial adapter used the legacy bool-only level-ray wrappers with bisection. It reported every tested 1200 cm direction as clear, and a downward view could reach an unusable low camera position. Those results were not accepted as proof of collision.

Review of the native callers and live grid established:

- The level has 153 records in this scene, with one enabled sector entry and primary and secondary mesh pointers.
- Head-local position plus sector origin matches the level's global coordinate convention. In this room the sector origin is zero.
- `RVA 0x19170` subtracts two three-float vectors; it is not a matrix transform.
- The bool wrappers at `RVA 0x1430F0` and `RVA 0x1431C0` subtract each mesh origin before tracing, use one-sided triangle intersection, and discard the hit distance.

The adapter now calls the reviewed mesh trace at `RVA 0x602D0` directly for both enabled geometry sets. Each segment is tested from its origin and from its end, which covers one-sided triangles and retains the nearest surface instead of depending on bisection across multiple walls. The adapter subtracts the mesh origin, uses a unit direction and centimetre distances, and never retains a native mesh pointer beyond the synchronized call.

The original wrappers are retained as a once-per-level cross-check of forward and reverse hit semantics. A mismatch, native exception, unreadable record, empty geometry set, or invalid hit produces zero clearance. The domain applies the 8 cm margin only when a surface exists; a clear segment retains its full requested length.

## Measured live results

The diagnostics use the versioned `ExaCameraDiagnostics` structure and `--camera-status`; no native pointers cross that interface.

- Default view: `clearance=240`, `applied_distance=240`, `level_available=1`, `query_failed=0`, `collision_failures=0`.
- A 1200 cm boom at yaw 0, pitch -0.12 was limited to `clearance=853.342`; the measured applied distance was still easing at `851.965` when sampled.
- Seven additional long-boom directions also clamped. Examples included `903.386`, `882.899`, `825.661`, `1156.31`, and `1179.31` cm. All had zero query failures.
- With a 900 cm requested boom, a clear direction reported `clearance=900`. An obstructed direction immediately reported `clearance=817.364`, `applied_distance=816.9`.
- Returning to the clear direction reported `clearance=900` and an intermediate `applied_distance=849.237`, followed by `897.667` on the next sample. This demonstrates eased release rather than an instant jump.
- Restoring the default produced `clearance=240`, `applied_distance=240` and the expected third-person framing.

The following images are local acceptance evidence, not claims of complete visual coverage:

- [Default 240 cm framing](media/collision-third-default.png)
- [Long-boom wall-limited framing](media/collision-wall-limited.png)

## Handoff and shutdown

After collision testing:

- First person: `custom_presented=1`, `level_available=0`, `applied_distance=0`; the collision counter did not advance.
- Overhead: `custom_presented=0`, `cursor_owned=0`; native presentation and cursor ownership resumed.
- Third person: `custom_presented=1`, `cursor_owned=1`, and static-level queries resumed.
- Stop: `state=stopped`, `generation=1`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`, `abi=2`.
- Persisted settings after the worker flush: first FOV 75, third FOV 82, sensitivity 0.003, third distance 240, shoulder 32, height 28, near plane 3.

All ten CTest suites passed in Release and Debug with the final collision implementation. The collision tests cover clear paths, exact forward/reverse distances, nearest surfaces, the 8 cm margin, invalid geometry and hit values, native-independent failure handling, and extreme lengths.

## Open gates

- **Floor and ground:** neither direction of these level meshes reported a floor hit in the tested downward view. Ground clearance or a justified pitch envelope remains required; floor protection is not claimed.
- **Frustum and transitions:** this is a central boom query. Near-plane corners, shoulder/pivot embedding, and the complete transition path still need coverage.
- **Doorways and props:** an actual doorway traversal and dynamic-object policy have not been accepted. Static level meshes alone are not a dynamic-object collision system.
- **Performance:** the [measured baseline](collision-performance.md) establishes one grid query and four mesh traces per third-person camera sample in the test room, with timing and legacy-buffer checks. Earlier bursts did not reproduce in that session; this is not proof of a fix or full performance acceptance.
- **Coverage:** other rooms, deaths and scene travel with this adapter, resolution changes, long soaks, EMTK/ExanimaLua coexistence, and other executable builds remain untested.

The collision core is therefore validated for the tested static-level path, but the complete controller is not release-ready.
