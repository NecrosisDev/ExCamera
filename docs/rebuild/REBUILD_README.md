# ExCamera and the Exanima Modding Platform

A clean rebuild of the Exanima first-/third-person camera mod, with a reusable native knowledge base, modding runtime, and planned Lua API.

> **Status: parked research.** This README documents a prototype rebuild that is not shipped and is not part of the current `build.py` or CI. The v0.3.0 release is the tracked single-translation-unit project documented in [ARCHITECTURE.md](../ARCHITECTURE.md). The validation statements below apply only to the rebuild prototype, not to the current release.

> **Prototype status: camera, mouse, settings, lifecycle, and static-level boom/pivot/frustum collision were validated in Arena practice and a safe story scene; the full controller was still in development.** Ground coverage, doorway and dynamic-object behavior, full performance validation, remaining edge cases, and Lua integration are not complete. The shipped release is not acceptance for the rebuild.

The design, scope, implementation phases, and release gates for the parked rebuild are in [the rebuild plan](../REBUILD_PLAN.md).

## Goals

- A small, tested ExCamera core: first person, third person, vanilla overhead, mouse look, FOV, native interaction, and reliable settings.
- One runtime lifecycle and hook owner instead of independent detours scattered across features.
- A versioned knowledge base whose native claims carry provenance and explicit evidence states.
- A versioned native C SDK and, later, a read-only Lua API built on the same bounded services.
- Fail-closed support for reviewed game builds, with no silent compatibility claims.

Optional rendering, combat, inventory, world-mutating, and OpenXR features are quarantined until each has its own contracts and acceptance evidence.

## Prototype foundation

- Local REA 6.8.0 and Ghidra 12.1.4 analysis pipeline for the supplied Exanima executable.
- One research build with 34 native function records and five camera, input, settings, lifecycle, and collision capability records.
- Deterministic knowledge generation, JSON Schema validation, and sixteen Python knowledge and camera-metrics tests.
- A native-independent first-/third-person controller with explicit coordinate conventions, physical eye mounting, obstruction pull-in/ease-out, quaternion transitions, bounded invalid-frame holds, ground clearance, and native handoff.
- Thirteen CTest suites covering math, controller state, collision, frustum refinement, ground clearance, diagnostics compatibility, input, lifecycle, settings, native hooks, and the package.
- A live two-hook camera/mouse adapter with first-/third-/overhead presentation, mouse look, F6 cycling, pause-safe cursor ownership, focus-loss recovery, and clean stop. See [camera adapter acceptance](../acceptance/camera-adapter.md) and [input acceptance](../acceptance/input-adapter.md).
- F9/F10/F11 FOV controls and atomic preference persistence verified across a complete game restart. See [settings acceptance](../acceptance/settings-adapter.md).
- A three-hook lifecycle path with identity resets and presentation-boundary restoration, verified through story-to-arena changes and an actual defeat. See [lifecycle acceptance](../acceptance/lifecycle-adapter.md).
- Bidirectional static-level collision with nearest-hit distances, pivot clamping, near-plane corner probes, and safe transition origins. See [collision acceptance](../acceptance/collision-adapter.md) and [frustum acceptance](../acceptance/frustum-adapter.md); in-game ground coverage, doorway, dynamic-object, and full performance gates remained open.
- Normal CMake translation units for the new runtime, launcher, public headers, and tests.
- Runtime start/stop/restart/status exports, camera controls and diagnostics, and a zero-hook package self-test.
- A MinHook broker that requires exact original entry bytes and rejects modified, foreign, cyclic, invalid, and non-executable targets.
- Debug and Release native tests for hook behavior, teardown, re-arm validation, duplicate registration, and session generations.

The supplied standard 0.9.5.2g executable is a research target only:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

Hellmode and other builds were not supported by the rebuild prototype.

## Build the new camera-enabled runtime

Requirements: Windows x64, Visual Studio 2022 C++ Build Tools, the Windows SDK, and CMake 3.24 or newer. No game files are needed to compile or run the synthetic tests.

From the repository root:

```powershell
cmake -S . -B build/rebuild -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build/rebuild --config Release --parallel 4
ctest --test-dir build/rebuild -C Release --output-on-failure
cmake --install build/rebuild --config Release --prefix build/rebuild-package
build\rebuild-package\ExCamera.exe --self-test
```

The self-test starts and stops the runtime through the packaged DLL for eight generations and verifies that no game hooks were installed. It does not launch or attach to Exanima.

The standalone launcher also supports a zero-hook live lifecycle smoke against the supported game:

```powershell
build\rebuild-package\ExCamera.exe --attach
build\rebuild-package\ExCamera.exe --status
build\rebuild-package\ExCamera.exe --stop
```

Attach and stop return immediately. Re-attach after stop reuses the loaded runtime and advances its generation. The observed live evidence is in [standalone lifecycle acceptance](../acceptance/standalone-lifecycle.md).

While attached, the launcher also exposes camera controls and a read-only diagnostics snapshot:

```powershell
build\rebuild-package\ExCamera.exe --mode third
build\rebuild-package\ExCamera.exe --look 0 -0.12
build\rebuild-package\ExCamera.exe --distance 240
build\rebuild-package\ExCamera.exe --camera-status
```

Look angles are radians. Distance is in centimetres, accepts 0–1200, and persists through the settings worker. `--camera-status` requires an active runtime and reports mode, ownership flags, level availability, query and failure counts, clearance, applied distance, camera geometry, camera samples, mesh traces, and cumulative collision time. See [collision acceptance](../acceptance/collision-adapter.md) for validated behavior and [the performance baseline](../acceptance/collision-performance.md) for interval measurements and remaining gates.

The install layout contains exactly:

- `ExCamera.exe` — standalone attach/status/stop launcher and package self-test.
- `ExanimaRuntime.dll` — camera-enabled lifecycle runtime.
- `LICENSE.txt` and `LICENSE-MinHook.txt`.

This layout is a test artifact, not a release package.

## Knowledge base

See [the knowledge-base guide](../../knowledge/README.md) and [the generated research index](../../knowledge/generated/index.md).

Set up the local Python tools and run their checks:

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r tools\requirements-kb.txt
.venv\Scripts\python tools\kb.py validate
.venv\Scripts\python tools\kb.py generate
.venv\Scripts\python tools\kb.py check
.venv\Scripts\python -m unittest discover -s tools\tests -p "test_*.py" -v
```

Imported REA observations begin with unknown ABI, thread, and memory semantics and cannot be emitted as runtime contracts. Promotion requires independent static review and byte validation; public runtime support additionally requires in-game acceptance.

Raw game files, reference clones, local toolchains, and full decompilations are ignored research inputs and are never release artifacts.

## New source layout

- `include/exanima/` — public, versioned C headers.
- `src/runtime/` — lifecycle and hook ownership.
- `src/excamera/domain/` — native-independent math, controller state, input, and collision policy.
- `src/excamera/native/` — reviewed game adapters and synchronized settings/diagnostics state.
- `src/launcher/` — thin standalone bootstrap and package self-test.
- `knowledge/` — schemas, research inputs, native records, and generated indexes.
- `tests/` — public unit, contract, and integration tests.
- `tools/kb.py` — knowledge import, validation, and generation.
- `../REBUILD_PLAN.md` — rebuild design and release plan.
- `../ARCHITECTURE.md` — shipped architecture.

The shipped source (`src/first_person.cpp`, `src/mod/`, the native settings/rendering files, and `build.py`) is the current release and is not an implementation baseline for this parked rebuild.

## License

ExCamera is licensed under the [MIT License](../../LICENSE). Bundled MinHook retains its [own license](../../src/vendor/minhook/LICENSE.txt). No game executable or assets are distributed.
