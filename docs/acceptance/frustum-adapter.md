# Frustum, pivot, and transition acceptance

## Scope

This follow-up to [collision adapter acceptance](collision-adapter.md) covers shoulder-pivot clamping, near-plane corner queries, and transition-origin policy in the disposable `CameraStory` room. Ground clearance, an actual doorway traversal, and dynamic-object policy are separate unfinished work.

The supplied standard Exanima executable was used. The game was closed after the final clean stop.

## Implemented boundaries

### Pivot clearance

The desired eye-to-pivot offset is queried before the camera boom. The shared `third_person_pivot` composition applies the resulting clearance to that offset, returning toward the eye when the offset is blocked or invalid. The 8 cm margin is applied only when an actual surface is found. An infinite clearance preserves the original offset; NaN, negative, or otherwise invalid clearance returns the eye origin.

The controller consumes the same `pivot_clearance` through `CameraFrame`, so native and native-independent paths share one composition rule rather than duplicating shoulder calculations.

### Near-plane corners

`CameraFrustum` carries orientation, vertical FOV, client aspect ratio, and near-plane distance. The adapter updates aspect ratio from the last valid focused client rectangle; focus loss does not invent a new aspect ratio.

The solver checks:

1. the central pivot-to-camera segment;
2. four segments from the pivot to the near-plane corners at the candidate distance;
3. at most two refinement passes after the initial corner evaluation.

Every accepted candidate is checked at its actual near-plane size. A candidate that cannot be verified within the bounded budget returns zero instead of an unverified distance. The domain helper performs at most 13 grid queries: one center query and three groups of four corner queries. A nonzero pivot offset adds one separate query, for a maximum of 14 in this path.

This is a static-level, near-plane corner approximation, not a general swept-volume or dynamic-object collision system.

### Transitions

Orientation, FOV, and near-plane settings can still blend during a mode transition. The camera position is taken from the newly validated target instead of interpolating between the old and new positions. This prevents a transition from sweeping the camera through a wall merely because its endpoints are valid. The positional change is therefore immediate; the transition is not presented as a fully smooth camera-path animation.

## Domain and compatibility tests

The new tests cover:

- clear paths requiring one center and four corner queries;
- central obstruction and the exact margin;
- near-plane obstruction failing inward;
- an oblique wall that requires corner refinement and rechecking;
- persistent obstruction reaching the bounded failure budget;
- zero near plane, invalid orientation/FOV/aspect/geometry, failed queries, and NaN hits;
- pivot clamping and invalid pivot clearance;
- transition origins using the validated target rather than a blended origin.

All 12 CTest suites passed in Release and Debug. The previous 16 Python tests also pass.

## Live results

### Default framing and cost

The default third-person view remained coherent at `clearance=240` and `applied_distance=240`. A 2.0341476-second interval produced:

| Measurement | Observed value |
| --- | ---: |
| Camera samples | 356 |
| Grid queries | 2,136 |
| Mesh traces | 8,544 |
| Collision failures | 0 |
| Collision time | 230,551,800 ns |
| Queries per camera sample | 6.0 |
| Mesh traces per grid query | 4.0 |
| Mean collision time per sample | 647.6174 µs |
| Mean collision time per grid query | 107.9362 µs |

The common path is one pivot query, one center query, and four corner queries. This measurement is specific to the enabled geometry and view in this room; it is not a global performance guarantee or an FPS claim.

[Restored default third-person framing](media/frustum-third-default.png)

### Obstruction and recovery

With a 900 cm requested boom:

- A clear direction reported `clearance=900` while release was still easing.
- The obstructed direction reported `clearance=824.527` and `applied_distance=823.427`.
- Returning to the clear direction reported `clearance=900`, with `applied_distance=896.655` and then `899.882` on later samples.
- The interval maintained six grid queries and four mesh traces per query, with zero failures.

Nine observed F10 presses raised third-person FOV to 127. At that FOV the clear direction remained at 900 cm; the obstructed direction reported `clearance=824.584`, with no additional refinement or failure in this room. F11 then restored first/third FOV to 75/82. Distance was separately restored to 240 cm.

### Paused transitions and shutdown

While the native pause menu remained open:

- Restored third-person settings produced `clearance=240` and `applied_distance=240`.
- Overhead stopped all grid queries and mesh traces.
- Third person resumed custom presentation and six grid queries per camera sample.
- The paused third-person diagnostics reported `cursor_owned=0` and `custom_presented=1`.

The window was recreated during the session. A fresh window observation confirmed that the game and runtime were healthy; this is not a claim of full resolution/fullscreen acceptance.

After closing the menu and stopping:

- `state=stopped`, `generation=1`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`, `abi=2`.
- Persisted settings were first FOV 75, third FOV 82, sensitivity 0.003, third distance 240, shoulder 32, height 28, and near plane 3.
- The test game was closed without a remaining Exanima process.

## Open gates

- A dedicated live shoulder/corner obstruction scene has not been isolated. The oblique-wall refinement is covered synthetically, while the live room validates normal framing and center/corner query execution without a distinct corner-only clamp.
- Ground/floor protection is not established by these level meshes in the tested downward view.
- Doorway traversal, moving geometry, dynamic props, and combat interactions remain untested.
- Long play soaks, other levels, scene travel with this adapter, other game builds, and EMTK/ExanimaLua coexistence remain open.
- The new six-query baseline is measured, but final performance acceptance is still pending. Earlier [central-boom measurements](collision-performance.md) have a different query scope and should not be compared as if they were the same workload.

The pivot/frustum core is validated for the tested static-level scenarios, but the complete controller is not release-ready.
