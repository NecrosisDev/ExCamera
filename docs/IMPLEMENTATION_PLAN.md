# ExCamera Implementation Plan

> **Status: historical (v0.3.0).** This plan records the hardening work that produced v0.3.0. It is not a current roadmap. Phases 0-4 produced the shipped code to the extent recorded below; Phase 5 (ExanimaLua-hosted settings) was abandoned and its unfinished path was removed; Phase 6 is the release-validation work now tracked in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). Unchecked in-game work remains there.

This plan breaks the requested hardening and unification work into verifiable phases. Each phase is intended to be completed and validated before the next phase begins. The goal is to fix the identified issues completely, without placeholders or untested changes.

## Ground rules

- Every code change must build successfully with `py build.py`.
- Every phase must pass `python tools/check_generated.py`.
- Every phase must include at least one focused verification step appropriate to the changed subsystem.
- No optional capability may be silently disabled to make a phase pass.
- Native compatibility claims must remain backed by generated tables and, where applicable, executable checks.
- ExCamera must remain usable without ExanimaLua installed.

## Current baseline

- `python tools/check_generated.py` passes.
- MSVC 2022 x64 BuildTools are available.
- The repository is a single-translation-unit DLL plus launcher.
- Optional features include attack variants, held-body throw, cue rendering, and OpenXR VR.

## Phase 0: Safety rails and diagnostics

Status: Complete (initial diagnostic counters, status fields, cursor transition counter, and two fault counters wired).

### Objective

Make regressions observable before changing behavior.

### Work

- Add fault counters for major game-thread hooks:
  - navigation/camera sampler
  - swap/present
  - behavior/input
  - pose/part rendering
  - window procedure
- Add transition counters for:
  - camera park/rearm
  - camera restoration unknown
  - cursor state transitions
  - settings ownership refusal
  - settings pending/failed writes
- Extend `first-person-status.json` output with these counters.
- Document state invariants for camera ownership, cursor state, and settings ownership.

### Files

- `src/mod/state.inl`
- `src/status_json.h`
- `src/mod/frame.inl`
- `src/mod/camera.inl`
- `src/mod/input.inl`
- `src/native_settings.h`
- `docs/ARCHITECTURE.md`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- Status file contains the new counters without changing existing successful activation behavior.

### Exit criteria

- Counters compile and are readable in status output.
- No behavior change in normal activation.

## Phase 1: Fault safety and camera restoration

Status: Code work complete for fault propagation and restoration observability. In-game transition validation remains.

### Objective

Prevent a fault in a hooked game-thread callback from permanently holding a lock or leaving camera ownership inconsistent.

### Work

- Audit every game-thread `AcquireSRWLockExclusive` and `AcquireSRWLockShared`.
- Classify each acquisition by owning thread and lock scope.
- Wrap all acquisitions that occur across game-memory reads in `__try/__finally`.
- Ensure every `Acquire` has a matching `Release` in `__finally`.
- Move game-memory traversal, VirtualQuery/readability checks, native calls, and scene traversal outside lock scope where practical.
- Prefer `TryAcquire` on hot paths and bail out cleanly on failure.
- Add per-hook fault counters around protected bodies.
- Make camera restoration an explicit state machine:
  - `owned`
  - `released`
  - `parked`
  - `restoring`
  - `restored`
  - `unknown`
- Invalidate retired camera slots when root, scene, or gameplay identity changes.
- Ensure `restorationUnknown` remains sticky until a clean re-arm.
- Ensure `awaitRestore` cannot report success while `savedCamera`, `pendingRestore`, or in-flight render scopes remain.

### Files

- `src/mod/state.inl`
- `src/mod/frame.inl`
- `src/mod/camera.inl`
- `src/mod/combat.inl`
- `src/mod/blade.inl`
- `src/mod/view.inl`
- `src/mod/picking.inl`
- `src/mod/input.inl`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- `py build.py --package`
- Fault-injection or synthetic-readability tests where available.
- Manual in-game checks:
  - repeated death
  - knockdown
  - arena entry/exit
  - shop/container/dialogue transitions
  - alt-tab/windowed and fullscreen switches
  - scene travel
- Confirm `first-person-status.json` reports no faults and camera restoration succeeds after each transition.

### Exit criteria

- No lock leak is observed in fault or transition testing.
- Camera restores after every tested transition.
- No optional capability is disabled.

## Phase 2: Cursor state unification

Status: Code work complete. In-game cursor transition validation remains.

### Objective:

Eliminate cursor state fragmentation and make cursor release policies explicit and consistent.

### Work

- Replace the separate volatile flags:
  - `cursorFree`
  - `rmbFree`
  - `windowFree`
  with one cursor-state enum.
- Store the reason/owner for the current state:
  - captured
  - free by Cursor Look
  - free by Settings
  - free by native panel
  - free by automatic policy
- Create one transition function used by all cursor state changes.
- Update clipping based on cursor state, not `inputEnabled`.
- Convert automatic cursor-release cases into an explicit panel-type policy table.
- Preserve the native Cursor Look toggle as user-owned data; do not overwrite it with mod policy.
- Preserve arena, shop, container, and company-screen behavior.
- Exclude dialogue from automatic release as documented.

### Files

- `src/mod/state.inl`
- `src/mod/input.inl`
- `src/native_settings.h`
- `src/native_settings.cpp`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- Manual in-game checks:
  - F8 open/dismiss
  - Cursor Look hold/release
  - inventory/loot/container interaction
  - shop/arsenal interaction
  - arena management windows
  - dialogue
  - focus handoffs
- Confirm cursor cannot become stuck free or stuck captured.

### Exit criteria

- All cursor transitions pass through one function.
- No desync between cursor state, clipping, and input ownership.

## Phase 3: Settings ownership and persistence hardening

Status: Code work already present in the unreleased settings integration; diagnostics and recovery paths verified at build level. In-game validation remains.

### Objective:

Make native Settings integration ownership explicit and make persistence reliable and observable.

### Work

- Replace inferred table ownership checks with an explicit owner ID/form generation.
- Refuse mutation unless ownership is proven.
- Restore the original native table only when ownership is still proven.
- Leave unknown replacement tables untouched.
- Separate draft edits from native table mutation until Apply.
- Publish Apply as one validated transaction.
- Make Cancel discard draft edits without touching native tables.
- Make persistence revision-aware:
  - UI revision
  - published revision
  - persisted revision
  - acknowledged revision
- Write preference files atomically using a temporary file and rename.
- Keep failed writes pending and retry on running/cleanly stopped sessions.
- Make status files atomic and mark them stale when publication fails.
- Remove hard-coded UI geometry assumptions:
  - read game UI scale
  - read Hellmode GUI size from `hm_saves`
  - derive frame dimensions from current game values
- Collapse callback state into an explicit state machine:
  - `idle`
  - `building`
  - `owned`
  - `closing`
  - `faulted`

### Files

- `src/native_settings.h`
- `src/native_settings.cpp`
- `src/settings_draft.h`
- `src/settings_model.h`
- `src/settings_presentation.h`
- `src/settings_presentation.cpp`
- `src/mod/settings.inl`
- `src/mod/overlay.inl`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- Manual in-game checks:
  - Apply
  - Cancel
  - Defaults
  - profile switch
  - focus handoff
  - native owner change/refusal
  - settings open during fault recovery
- Confirm no wrong table restoration and no lost edits.

### Exit criteria

- Ownership is explicit and recoverable.
- Pending/failed writes are visible and retried correctly.
- Geometry works across supported UI scales and Hellmode.

## Phase 4: UI coordinate and interaction cleanup

Status: Code work already present in the unreleased settings/picking integration; diagnostics exposed. In-game validation remains.

### Objective:

Fix coordinate mapping and make native UI interaction predictable.

### Work

- Create one coordinate transform function for ExCamera-owned rows.
- Use it for preview and release events.
- Add assertions that converted coordinates round-trip through native hit testing.
- Validate displayed-pointer samples before consuming pickups.
- Fall back to game pick coordinates when the sample is stale.
- Count stale-sample fallbacks in diagnostics.
- Make panel ownership lifetime-based with explicit:
  - `opened`
  - `active`
  - `closing`
- Perform cleanup exactly once on close.
- Make F8 semantics explicit:
  - open Settings
  - free cursor
  - dismiss without applying ExCamera edits
  - clearly indicate discard behavior
- Keep native handlers for vanilla descriptors.
- Keep ExCamera adapter thin and assert ownership before mutation.

### Files

- `src/native_settings.h`
- `src/native_settings.cpp`
- `src/mod/input.inl`
- `src/mod/picking.inl`
- `src/mod/panel.inl`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- Manual in-game checks:
  - checkbox activation
  - slider drag
  - row click
  - stale pointer sample during click
  - arena window open/close
  - settings open/dismiss
- Confirm no wrong activation and no pickup consumption on stale sample.

### Exit criteria

- One coordinate transform is used consistently.
- Stale samples fall back safely.
- Panel cleanup happens exactly once.

## Phase 5: Optional ExanimaLua settings backend (abandoned)

Status: Abandoned in v0.3.0. ExCamera uses its native settings form, keeps its own setting values, and ships native procedural wounds, blood, and fire standalone; the unfinished ExanimaLua-hosted ExCamera settings path was removed. When `ScriptRuntime.dll` (ExanimaLua) is present, ExCamera optionally hosts tabs registered by other mods inside its widened Options form and uses `ScriptRuntime` inventory exports for the VR hip pouch. The objective, work, and exit criteria below are retained as design history and will not be completed.

### Objective:

Unify settings UI where possible while keeping ExCamera standalone.

### Work

- Detect whether ExanimaLua `ScriptRuntime` is present and ready.
- Use hosted tabs when available:
  - `open`
  - `title`
  - `rows`
  - `change`
  - `event`
- Migrate settings row groups incrementally:
  1. Camera and View
  2. Input
  3. Combat
  4. ThirdPerson
  5. VR, when built with OpenXR
- Preserve native integration as fallback when ExanimaLua is absent.
- Keep ExCamera runtime independent:
  - camera
  - input
  - rendering
  - cursor
  - window handling
- Do not make ExanimaLua a hard dependency for core ExCamera functionality.

### Files

- `src/native_settings.h`
- `src/native_settings.cpp`
- `src/mod/settings.inl`
- ExanimaLua integration documentation

### Verification

- `python tools/check_generated.py`
- `py build.py`
- `py build.py --package`
- Test with ExanimaLua installed.
- Test without ExanimaLua installed.
- Confirm identical settings behavior in both configurations.

### Exit criteria

- Settings work in both standalone and ExanimaLua-hosted modes.
- No regression in settings values, Apply, Cancel, or persistence.

## Phase 6: Verification, cleanup, and final hardening

Status: Ongoing for v0.3.0. Automated build, package, generated-table, and export checks pass; the remaining in-game checks and known limitations are tracked in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). The exit criteria below are not yet met.

### Objective

Complete validation and remove only proven-dead compatibility paths.

### Work

- Run full build and package validation.
- Run generated compatibility checks.
- Run native contract checks against the supported executable when available.
- Execute in-game verification checklist:
  - repeated death
  - knockdown
  - arena management windows
  - shops and containers
  - dialogue
  - alt-tab
  - fullscreen/windowed switching
  - mod chaining
  - scene travel
  - settings Apply/Cancel/Defaults
  - profile switch
  - cursor Look hold/release
  - F8 settings open/dismiss
- Remove dead compatibility code only after all checks pass.
- Update documentation to reflect final architecture.

### Files

- `README.md`
- `docs/ARCHITECTURE.md`
- `CHANGELOG.md`
- `docs/IMPLEMENTATION_PLAN.md`

### Verification

- `python tools/check_generated.py`
- `py build.py`
- `py build.py --package`
- In-game checklist completed without faults.
- `first-person-status.json` reports no faults after checklist.

### Exit criteria

- All phases complete.
- Full build and package pass.
- No unresolved fault or settings/cursor/UI regression remains in tested scenarios.

## Recommended execution order

1. Phase 0
2. Phase 1
3. Phase 2
4. Phase 3
5. Phase 4
6. Phase 6 (skip the abandoned Phase 5)

## Notes

- Phase 1 should be completed before Phase 2 because cursor and UI changes depend on reliable fault recovery.
- Phase 3 should be completed before Phase 4 because settings ownership affects coordinate and interaction cleanup.
- Phase 5 was abandoned in v0.3.0. ExCamera must remain standalone, and ExanimaLua is optional.
- No phase is considered complete until its verification steps pass.
