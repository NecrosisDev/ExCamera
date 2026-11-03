# Collision performance baseline

## Scope

This is a measured baseline for the instrumented static-level collision adapter in the disposable `CameraStory` room. It is not a release performance guarantee or proof that every historical sampling burst is fixed.

The supplied standard Exanima executable was used. All measurement and shutdown work completed before the game was closed.

## Measurement contract

`ExaCameraGetDiagnostics` now appends three fields to the existing snapshot:

- `camera_samples`: calls into the adapter's post-native `apply` path. This is not a rendered-frame counter and must not be reported as FPS.
- `collision_mesh_traces`: actual native mesh-trace calls attempted after memory and ABI checks.
- `collision_nanoseconds`: cumulative time inside `level_probe`, including level-grid access, both trace directions, and any original-wrapper validation in that interval.

Existing `collision_queries` counts complete grid queries. In this scene each enabled entry has primary and secondary geometry, so a bidirectional query produces four mesh traces.

The original fields through `back[3]` occupy 56 bytes (`EXA_CAMERA_DIAGNOSTICS_BASE_SIZE`). The expanded structure is 72 bytes. ABI version 1 is retained: `struct_size` selects the consumer's layout, and the native-independent copy routine writes only the shared prefix while preserving the caller's size and unknown trailing bytes.

Tests cover a full snapshot, a 56-byte legacy destination, an oversized destination with an unknown tail, rejected undersized buffers, and a rejected short producer. Canary bytes prove there is no write beyond the selected layout. The dormant runtime accepts the legacy size and rejects invalid arguments correctly.

## Reproduction

Attach to the supported game, enter a safe scene, then run:

```powershell
.venv\Scripts\python tools\camera_metrics.py --mode third --look 0 -0.12 --distance 240 --seconds 30
```

The script reads `--camera-status` at a bounded interval, validates process identity and counter monotonicity, and prints deltas and rates. Optional `--mode`, `--look`, and `--distance` controls are applied before the first snapshot. Ordinary control responses are not parsed as camera diagnostics.

## Observed results

### Thirty-second steady third-person interval

| Measurement | Observed value |
| --- | ---: |
| Elapsed time | 30.0327718 s |
| Camera samples | 5,226 |
| Grid queries | 5,226 |
| Mesh traces | 20,904 |
| Collision failures | 0 |
| Collision time | 1,429,315,800 ns |
| Camera samples/s | 174.0099 |
| Grid queries/s | 174.0099 |
| Mesh traces/s | 696.0397 |
| Mean collision time/query | 273.5009 µs |
| Grid queries/camera sample | 1.0 |
| Mesh traces/grid query | 4.0 |

The final clearance and applied distance were both 240 cm. No query or module failure occurred.

### Control changes

A distance change to 900 cm produced 180 samples, 180 grid queries, and 720 mesh traces over 1.0325594 seconds. Collision time was 50,436,100 ns (280.2006 µs/query). Clearance was 853.658 cm while the camera was easing toward it.

A look-only change at the same 900 cm requested distance produced 180 samples, 180 grid queries, and 720 mesh traces over 1.0354500 seconds. Collision time was 47,465,000 ns (263.6944 µs/query). Clearance was 900 cm.

A screenshot followed by two measurement intervals did not change the one-query-per-sample relationship or produce a burst.

### Bounded CPU contention

Six CPU workers ran for five seconds and were joined before measurement. The next 1.0332613-second interval contained 180 samples, 180 grid queries, and 720 mesh traces, with 48,764,900 ns of collision work (270.9161 µs/query). The following two-second interval remained at one query per sample and four traces per query.

### Mode handoff and shutdown

- First person: zero grid queries, zero mesh traces, and zero collision time over the measured one-second interval.
- Overhead: the same zero-collision-work result; native cursor and camera presentation resumed.
- Third person: collision work resumed with the expected one-query/four-trace relationship.
- Stop: `state=stopped`, `generation=1`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`, `abi=2`.
- Persisted defaults after shutdown: first FOV 75, third FOV 82, sensitivity 0.003, third distance 240, shoulder 32, height 28, near plane 3.

All 12 CTest suites passed in Debug and Release after the frustum follow-up. All 16 Python tests passed, including measurement parsing, zero-work intervals, missing metrics, invalid intervals, and counter regression handling.

## Limits and next gates

Earlier large counter bursts did not reproduce in this instrumented session, including during bounded CPU contention. That is evidence about these conditions, not proof of a fix. The new counters must remain available during longer play soaks, scene travel, graphics stress, and EMTK/ExanimaLua coexistence testing.

The baseline does not establish acceptable performance in more complex levels, with additional frustum probes, or at a particular rendered frame rate. Ground and near-plane/frustum coverage remain separate correctness work before final performance acceptance.
