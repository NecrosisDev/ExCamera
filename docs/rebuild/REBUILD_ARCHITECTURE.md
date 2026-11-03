# Rebuild Architecture (parked)

> **Status: parked research.** This document describes the prototype rebuild, which is not shipped and not part of the current build or CI. For the shipped v0.3.0 architecture, see [../ARCHITECTURE.md](../ARCHITECTURE.md).

## Status and authority

This document describes the clean rebuild prototype. The prototype implementation and acceptance sequence were owned by [the rebuild plan](../REBUILD_PLAN.md).

At the time of the prototype, the new runtime had live camera, mouse, settings, lifecycle, and static-level collision, pivot, near-plane, and transition-origin adapters validated in Arena practice and a safe story scene. In-game ground coverage, doorway, dynamic-object, full-performance, remaining lifecycle edge, coexistence, and Lua integration work remained incomplete. The shipped v0.3.0 runtime (`src/first_person.cpp` and the `src/mod/` include chain) is documented in [../ARCHITECTURE.md](../ARCHITECTURE.md); the rebuild never replaced it.

## Dependency rule

Dependencies flow inward:

```text
knowledge records and generated ABI
             |
native build resolver and platform ports
             |
runtime lifecycle, hook broker, bounded services
             |
public native interfaces and Lua bindings
             |
ExCamera domain and other mod consumers
```

ExCamera domain code must not depend on game addresses, MinHook, Lua, or global native state. Conversely, the runtime and knowledge layers do not contain camera feel, rendering effects, or feature-specific gameplay policy.

## Source boundaries

- `include/exanima/`: C-compatible public types and function declarations. No STL, C++ exceptions, or native object pointers cross this boundary.
- `src/runtime/`: session lifecycle, hook broker, and later module/scheduler ownership.
- `src/launcher/`: thin standalone package bootstrap and self-test; no game policy.
- `src/excamera/domain/`: native-independent camera math and controller state; no game addresses, Win32, OpenGL, MinHook, or per-frame heap allocation.
- `knowledge/`: executable identity, native research, reviewed contracts, and generated documentation.
- `tests/`: public synthetic, unit, contract, and integration tests.
- `tools/kb.py`: evidence import and deterministic knowledge generation.

Normal `.cpp` files are compiled as independent translation units. Ordered `.inl` inclusion and shared mutable globals are not part of the new design.

## Session lifecycle

```text
Unloaded -> Attaching -> Dormant -> Active -> Stopping -> Stopped
                  \-> Refused       Active -> Faulted -> Stopping
```

`Dormant` is the no-hook running state used by the package self-test and by hosts without an admitted camera capability. `Active` is the state after the reviewed camera adapter is admitted.

A successful start creates a new generation. Stop closes new admission, drains work, restores borrowed state, disables and removes hooks, and only then publishes `Stopped`. A teardown error publishes `Faulted`, not a false clean stop. Restart after a clean stop receives another generation; old pointers, readability results, callbacks, and object identities cannot cross it.

The C ABI exports `ExaRuntimeStart`, `ExaRuntimeStop`, `ExaRuntimeGetStatus`, camera mode/look/distance controls, and a versioned `ExaCameraGetDiagnostics` snapshot. Runtime status includes lifecycle state, generation, hook counts, last error, module faults, and restoration failure. Camera diagnostics expose bounded values rather than native pointers: current mode, ownership flags, level availability, query and failure counts, clearance, applied distance, pivot, back direction, camera samples, mesh traces, and cumulative collision time. `struct_size` selects the consumer layout: the original 56-byte prefix remains valid, while newer fields occupy the expanded 72-byte structure. The native-independent copy path preserves unknown trailing bytes. `DllMain` only disables thread notifications; attachment never starts there.

The standalone launcher hashes the running executable, detects an existing runtime, resolves remote exports relative to the actual remote module base, and performs bounded remote calls. `--attach`, `--status`, and `--stop` return without leaving a launcher thread. Stop deliberately leaves the DLL loaded for a safe later restart. The observed zero-hook sequence is recorded in [standalone lifecycle acceptance](../acceptance/standalone-lifecycle.md).

## Hook broker

The broker is the only MinHook owner. A registration contains a stable ID, target, detour, trampoline destination, and retained expected entry bytes.

Current admission is deliberately narrow:

- exact expected original entry bytes are required;
- modified entries are refused;
- existing foreign detours are refused;
- cyclic, malformed, non-executable, and unreadable targets are refused;
- duplicate IDs are refused without changing the live registration;
- failed installation preserves the caller's previous trampoline value;
- re-enable checks the current entry bytes again;
- stop disables and removes in reverse order and reports failure rather than claiming completion.

A future reviewed coexistence adapter must be a separate explicit contract. Following an unknown jump and assuming its destination is ABI-compatible is forbidden.

Native callback admission and quiescence are mandatory before a game hook is admitted. The camera adapter closes new writes before teardown and performs restoration on Exanima's camera thread. A bounded restoration wait must succeed before hooks are removed; otherwise stop remains retryable. Hook installation alone is never proof of safe teardown.

## Knowledge and generated ABI

`knowledge/builds/<sha256>/` is keyed by the complete executable hash. Function records separate:

- provider-observed symbol and body information;
- hypothesized role;
- ABI, thread, and memory semantics;
- caller/callee relationships and analysis limitations;
- evidence IDs and analysis-profile digests;
- evidence states and runtime-generation eligibility.

An imported REA/Ghidra dossier supplies `observed_static` only. It does not infer a callable signature, object lifetime, thread affinity, or safe hook. `tools/kb.py validate` enforces schema and cross-record invariants, including RVA/VA consistency and evidence requirements for runtime eligibility.

`tools/kb.py generate` deterministically produces the public index and combined research records. Full decompilations and raw dossiers remain local research artifacts.

## Testing boundaries

Synthetic tests prove only their stated contracts:

- executable-memory fixtures exercise real MinHook behavior, not game behavior;
- session tests exercise lifecycle and generation ownership;
- the packaged self-test exercises DLL loading, exports, and repeated start/stop;
- knowledge and camera-metrics tests exercise import, schemas, semantic checks, deterministic generation, and frozen report summaries;
- ground tests exercise support selection, downward-only clearance enforcement, stale contacts, and finite bounds.

Synthetic tests alone are not described as live validation. Camera presentation and paused transitions are recorded in [camera adapter acceptance](../acceptance/camera-adapter.md). Mouse look, F6, pause-safe ownership, and focus recovery are recorded in [input acceptance](../acceptance/input-adapter.md). FOV controls and persistence across a full game restart are recorded in [settings acceptance](../acceptance/settings-adapter.md). Identity changes and defeat-safe teardown are recorded in [lifecycle acceptance](../acceptance/lifecycle-adapter.md). The central static-level boom and its native cross-check are recorded in [collision acceptance](../acceptance/collision-adapter.md), with pivot, near-plane corner, and transition-origin coverage in [frustum acceptance](../acceptance/frustum-adapter.md). Interaction, ground coverage, doorway and dynamic-object behavior, full performance validation, remaining lifecycle edges, coexistence, and play-soak acceptance remain separate gates.

## ExCamera target boundary

The camera slice separates:

- pure camera math and coordinate-space transforms;
- camera/cursor/settings state machines;
- immutable native frame snapshots;
- small reviewed Win32/game/OpenGL adapters;
- explicit generation-scoped restoration.

The native camera sampler at RVA `0x94720` is admitted only for the supplied standard executable and the reviewed `NativeCamera` adapter. Its static function record remains research evidence, while `excamera.camera.core` links it and the two helper contracts to the live acceptance and remaining limitations.

The collision domain separates scalar segment clearance from `CameraFrustum` near-plane evaluation. The native adapter clamps the shoulder pivot before querying the center boom and four near-plane corners, with a bounded refinement budget and no per-frame allocation. Frame-level query failures persist until the next camera sample. Mode transitions blend orientation and lens settings but use the newly validated target position rather than interpolating an unverified camera path. These are static-level corner checks, not a general swept-volume or dynamic-object API.

Optional combat, inventory, rendering-effect, wound, headwear, and XR features have no place in the core dependency graph. Each must be an optional module with independent contracts and acceptance evidence.
