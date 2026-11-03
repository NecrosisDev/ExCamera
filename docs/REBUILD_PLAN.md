# ExCamera and Exanima Modding Platform: Parked Rebuild and Release Plan

> **Status: parked research.** This plan describes a clean rebuild that was prototyped but is not part of the shipped ExCamera v0.3.0, `build.py`, or CI. The shipped project remains the tracked single-translation-unit implementation described in [ARCHITECTURE.md](ARCHITECTURE.md). The rebuild's acceptance records and release gates below do not validate the current release. Keep this as a design reference only if the rebuild is resumed.

## 1. Purpose

Build a maintainable first-/third-person camera mod and, beneath it, a reusable Exanima knowledge base and modding API.

This is a clean rebuild, not a continuation of the shipped implementation. Existing ExCamera and ExanimaLua source was reference material only for this design. Code, offsets, comments, tests, and compatibility claims must earn their way into the new system through reproducible evidence.

“Bug-free” cannot be proven for an injected native mod. The release standard is therefore:

- no known reproducible critical or major defect in the supported test matrix;
- no unexplained runtime fault, leaked hook, leaked lock, stuck input state, or failed restoration;
- unsupported builds and unproven capabilities fail closed;
- every supported capability has automated checks plus an explicit in-game acceptance record;
- release documentation states residual coverage limits instead of implying universal correctness.

## 2. Evidence established during the audit

### Game and tooling

- The supplied `Exanima.exe` is Windows x64, 3,981,312 bytes, SHA-256 `af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70`.
- This is the standard 0.9.5.2g executable identified by the old source. No Hellmode executable was supplied, so Hellmode was not an initial rebuild release target.
- REA 6.8.0, Ghidra 12.1.4, JDK 21, and Node 24.22.1 are operating locally against an immutable workspace copy of the game.
- Ghidra reports 8,550 procedures and 5,147 strings in the executable.
- Initial REA/Ghidra dossiers cover `navSampler`, `headRenderSample`, `playerBehavior`, `cameraMouse`, `copyCameraSnapshots`, `pickRay`, `queueWork`, `guiMaintenance`, `guiOptionsOpen`, and `renderViewport`.
- The old ExCamera tables resolve all 233 targets and pass all 356 recorded static contracts against this executable. These checks validate recorded bytes and operands, not semantics or runtime safety.

### ExCamera tree at the start of the rebuild

- The tree is roughly 42,000 lines, with a single ordered-include translation unit and more than 100 hook-installation references.
- It compiles, but emits relevant shadowing and dead-code warnings. Compilation is not a behavioral baseline.
- Existing documentation repeatedly says in-game validation is outstanding. Old status and fault files are diagnostic clues, not reproducible acceptance tests.
- Camera, input, settings, inventory, rendering, combat, and XR concerns share private global state and native calls. This makes isolation, fault reasoning, and independent testing unnecessarily difficult.

### ExanimaLua tree at the start of the rebuild

- Its own API documentation says none of the API has run in the game. Owned-process tests do not establish startup, gameplay, or coexistence.
- The API already advertises 72 capability names and has reached bit 63 of its fixed `uint64_t` capability set. A new registry must not have a 64-capability ceiling.
- Existing-detour validation accepts an unknown non-jump target without comparing the expected instruction prefix. That is an explicit trust decision, not proof of ABI compatibility, and must not be the default in the new hook broker.
- The full native build/test command did not complete within the five-minute audit window. It is uncompleted, not a claimed pass or failure.

## 3. Product scope and release slices

### Slice A: stable ExCamera core

The rebuild's first planned public release was to contain only:

- standalone startup, validated attachment, clean stop, and restart;
- first-person, third-person, and vanilla overhead modes;
- mouse look and mode switching;
- base and third-person FOV;
- correct native interaction and cursor behavior;
- settings with Apply, Cancel, Defaults, and persistent preferences;
- structured diagnostics and an in-game acceptance checklist.

Third-person obstruction handling is part of the core if the simple camera cannot behave correctly without it. Head stabilization, cosmetic headwear rendering, and other feel/render enhancements follow only after the basic camera is accepted.

### Slice B: reusable platform and read-only Lua API

After Slice A is stable:

- versioned native SDK;
- read-only lifecycle, camera, player, scene, and nearby-world snapshots;
- copied events with sequence numbers and explicit overflow reporting;
- addon settings, storage, logging, timers, and bounded tasks;
- a small set of complete Lua examples and native SDK examples.

### Slice C: individually reviewed capabilities

Each optional feature is a separate work item with its own native contract and acceptance tests. No feature is ported merely because it existed before.

Initially quarantined:

- OpenXR and body/hand driving;
- attack variants, manual-defense changes, jumps, kicks, and held throwing;
- wound, blood, stone-skin, fire, blade, and silhouette modifications;
- supersampling and screen-effect replacement;
- custom headwear rendering and skin/bone rewrites;
- broad inventory, equipment, NPC, power, world, and save-mutating Lua APIs;
- EMTK-hosted startup and Settings-form sharing.

Quarantine means excluded from the release build and unsupported, not silently disabled at runtime while appearing available.

## 4. Target architecture

### Design rules

1. **One runtime owner.** One DLL owns session lifecycle, MinHook, native contracts, scheduling, logging, and module registration.
2. **One hook per native seam.** Every detour has a named owner, admission contract, forwarding rule, teardown order, and coexistence policy.
3. **Generated ABI boundary.** Native addresses, layouts, signatures, and instruction witnesses are generated from knowledge-base records, never scattered through gameplay code.
4. **Domain logic is native-agnostic.** Camera math, cursor policy, settings transactions, and addon manifests do not read game memory and do not call Win32 or OpenGL unless through explicit ports.
5. **Callbacks do minimal work.** No file I/O, Lua execution, unbounded allocation, or blocking waits in native hooks.
6. **Copies cross trust boundaries.** Public snapshots and events are bounded copies. Raw pointers are confined to reviewed native adapters.
7. **Generations invalidate stale state.** Player, scene, window, OpenGL context, addon, and session identities include generations.
8. **Unknown is not false.** Unreadable or unproven state remains unknown and causes a safe fallback.
9. **No fault is hidden.** SEH boundaries restore borrowed state and record diagnostics; they do not turn arbitrary access violations into apparently successful calls.
10. **Small supported surface.** A capability ships only when its static and runtime acceptance records are complete.

### Proposed repository layout

```text
include/exanima/             Public, versioned C SDK headers
knowledge/
  schema/                    JSON Schemas for builds, functions, layouts, and evidence
  builds/<exe-sha256>/       Reviewed native knowledge records
  capabilities/              Capability-to-contract mapping and acceptance state
src/
  core/                      Result types, logging, configuration, bounded queues
  abi/                       Generated declarations and native contract resolver
  runtime/                   Lifecycle, hook broker, modules, scheduler, diagnostics
  services/                  Snapshots, events, settings, storage
  lua/                       Sandbox and Lua bindings over public services
  excamera/
    domain/                  Camera math and state machines; no game dependencies
    native/                  Reviewed game/OpenGL/Win32 adapters
  launcher/                  Thin standalone bootstrap
  platform/                  Explicit Win32, OpenGL, and EMTK ports
tests/
  unit/
  contract/
  integration/
  acceptance/                In-game scenarios and result records
tools/
  kb.py                      Import, validate, generate, and document knowledge
  rea/                       Reproducible local REA session helpers
  build.py                   Reproducible build/package entry point
docs/
  architecture.md
  api/
  acceptance/
```

The old `src/mod` include chain is removed. Normal `.cpp` translation units have explicit dependencies and can be tested independently. CMake with MSVC and CTest is the canonical native build; a small Python entry point preserves convenient packaging and CI use. Public C ABI headers remain consumable by C and C++.

### Runtime components

#### Build identity and resolver

- Hash the executable before attachment.
- Resolve only the contracts assigned to that exact build.
- Verify loaded instruction windows again immediately before enabling a hook.
- Refuse unknown builds in release binaries.
- Keep standard and Hellmode records separate; similarity does not imply support.

#### Lifecycle state machine

```text
Unloaded -> Attaching -> Dormant -> Active -> Stopping -> Stopped
                  \-> Refused       Active -> Faulted -> Stopping
```

- `DllMain` records the module and disables thread notifications only.
- Attachment owns all hook creation and module initialization.
- `Dormant` is valid at the title screen or whenever gameplay ownership is absent.
- Stop admits no new callbacks, drains admitted work, restores borrowed native state, disables hooks in reverse dependency order, and only then releases module state.
- Restart creates a new session generation; no pointer or readability result crosses generations.
- Partial attachment rolls back to one consistent state.

#### Hook broker

Each registration declares:

- stable contract ID and owning module;
- target and complete admission requirements;
- detour and trampoline ABI;
- required thread/context;
- install/enable/disable ordering;
- fault policy and restoration obligations;
- coexistence rule and required capability dependencies.

Default coexistence is **untouched entry or refusal**. An existing detour is a named conflict unless a reviewed adapter explicitly supports its exact contract. The broker never assumes that a non-jump target proves compatibility.

#### Work and event scheduling

- Native callbacks publish bounded records and return.
- Consumers execute only on their documented thread and lifecycle state.
- Every queue has capacity, overflow policy, sequence number, and drain semantics.
- Callback admission counts remain positive until callback finally-restoration completes.
- Gameplay mutations are staged with session, scene, and object generations.

The observed native 128-entry work ring is research evidence only. It is not used by the platform until its consumer, task ABI, full behavior, and thread rules are independently established.

#### Diagnostics

- Machine-readable session status, capability admission/refusal reasons, fault counters, queue drops, and restoration state.
- Human-readable rotating logs outside hot callbacks.
- Optional crash context contains mod build, game hash, contract ID, session generation, and owning thread; no save data or unrelated personal information.
- Debug symbols are separate from the runtime ZIP.

## 5. ExCamera design

### Domain model

```text
CameraMode = FirstPerson | ThirdPerson | Overhead
SessionState = Inactive | WaitingForPlayer | Active | Suspended | Restoring | Faulted
CursorOwner = Native | UserLook | Settings | NativePanel | FocusLoss
```

These are explicit state machines with logged transitions and tested exit actions. A bit mask may represent simultaneous internal reasons, but one arbiter computes the externally visible cursor state. Independent flags must not disagree about clipping, visibility, or mouse ownership.

### Native camera adapter

The initial candidate seam is the reviewed native camera sampler (`0x100094720` in this executable). Static analysis shows it computes and publishes native camera state. This does not yet prove it is sufficient or the best seam.

The vertical slice will:

1. establish the native caller, thread, object lifetimes, arguments, return behavior, and output layout;
2. call and preserve native behavior where appropriate;
3. derive an immutable frame snapshot;
4. compute the requested view in domain code from that snapshot;
5. publish through a bounded native adapter with finally-restoration;
6. verify picking, projection, and interaction before adding another hook.

If this seam cannot support correct mouse look and interaction, add the smallest reviewed input seam. Do not reproduce the old hook inventory preemptively.

### Camera math

- Pure, deterministic transforms with explicit coordinate spaces and units.
- No direct game pointers, global settings, or native calls.
- Finite-value and matrix-invertibility checks at system boundaries.
- Golden tests for position, orientation, projection, FOV, near plane, third-person pivot, obstruction, and mode transitions.
- Frame-rate-independent smoothing tested at several time steps; no smoothing until the unsmoothed baseline is correct.

### Input and interaction

- Vanilla input remains authoritative unless a specific ownership reason is active.
- User look, native UI, Settings, and focus loss compose through one policy.
- Every ownership acquisition has a matching generation-scoped release.
- Mouse warping, clipping, visibility, and raw-input registration are restored on every exit path.
- Picking uses native behavior until a reviewed discrepancy proves an adapter is required.

### Settings and UI

- One settings model owns definitions, validation, draft, published revision, and persisted revision.
- Apply is one transaction; Cancel discards the draft; Defaults is another explicit draft operation.
- Persistence is atomic and asynchronous; UI callbacks never wait on disk.
- The standalone UI has exactly one native-form owner.
- Hosted ExanimaLua/EMTK Settings are a later adapter behind the same presentation interface, not a second simultaneous owner.

### Rendering

The core release changes no rendering beyond what the camera contract requires. OpenGL interception, custom resources, headwear, wounds, and effects are separate optional adapters with their own context generations and pixel/state tests.

## 6. Knowledge base and reverse-engineering workflow

### Record types

**Build record**

- game version and Steam build when known;
- executable SHA-256, size, PE architecture, image base, and section map;
- analysis tool versions and source provenance;
- support status and acceptance coverage.

**Function contract**

- stable namespaced ID;
- RVA/VA and reviewed signature patterns;
- Ghidra procedure identity and body ranges;
- calling convention, arguments, return, reads/writes, and callees;
- caller/thread/context requirements;
- preconditions, postconditions, lifetime rules, and failure behavior;
- hook admission and restoration obligations when relevant;
- evidence references and independent review status.

**Layout contract**

- stable type ID and size/alignment evidence;
- field offset, type, valid range, read/write owner, and lifetime;
- instruction witnesses proving the field claim;
- unknown/padding regions retained rather than guessed.

**Capability contract**

- public capability name and version;
- required function/layout contracts;
- static and runtime acceptance state;
- fallback/refusal behavior and supported builds;
- owning adapter and test scenarios.

### Evidence states

Keep these independent rather than using one ambiguous “verified” flag:

- `observed_static`: a tool observed bytes, references, or decompiler output;
- `reviewed_static`: a reviewer checked the relevant assembly and derived semantics;
- `byte_validated`: recorded instruction witnesses match the selected executable;
- `observed_runtime`: a live run observed the stated behavior;
- `validated_runtime`: a repeatable in-game scenario passed;
- `hypothesis`: plausible but not yet established;
- `rejected`: disproved, with the contrary evidence retained.

Raw REA dossiers and full decompilations remain local research artifacts. The publishable knowledge base contains derived contracts, concise evidence, and necessary instruction witnesses—not the game executable, assets, or wholesale decompiled source.

### Repeatable workflow

1. Pin the game hash and analysis tool versions.
2. Use REA to open an immutable game copy and retain evidence IDs.
3. Inspect pseudocode and assembly; mark decompiler-derived types as hypotheses.
4. Trace callers, callees, references, and object ownership.
5. Record only reviewed claims and explicit unknowns.
6. Generate signatures/layout declarations; diff generated output.
7. Run static contract checks against the executable.
8. Add a focused runtime probe or acceptance scenario.
9. Promote evidence states only when the corresponding check passed.
10. Keep rejected approaches and limitations so later work does not rediscover them.

## 7. Public modding API

### Native C ABI

The SDK exposes a function-table query rather than unstructured exports:

```c
ExaResult ExaQueryInterface(
    const ExaUuid* interface_id,
    uint32_t version,
    void* out_table,
    uint32_t out_table_size);
```

Rules:

- C-compatible types, fixed-width integers, explicit struct sizes and versions;
- namespaced, versioned capabilities rather than a fixed 64-bit mask;
- opaque handles with generations;
- explicit `ExaResult` values such as `Unavailable`, `StaleHandle`, `WrongThread`, `Overflow`, `Refused`, and `Fault`;
- caller-owned buffers with required/actual counts;
- documented callback thread, duration, reentrancy, and registration lifetime;
- no C++ exceptions, STL objects, or C++ ownership across the ABI;
- no arbitrary memory read/write, arbitrary native-call, or “execute on random thread” API.

The first public interfaces are logging, build information, lifecycle events, bounded snapshots, settings, storage, and task submission. Camera control remains a privileged ExCamera interface until its semantics are stable enough to promise to other mods.

### Lua API

Lua is a consumer of the same services, not a second native implementation.

- One isolated Lua state per addon with memory, instruction, module, callback, and log limits.
- No `io`, `os`, `package`, `debug`, arbitrary native calls, or raw pointers.
- Declared required/optional capabilities with clear refusal diagnostics.
- Snapshot rows and events are copies; method tickets expire with their callback.
- Writes, when later introduced, are staged after successful callback return and revalidated immediately before execution.
- Reload cancels timers, tasks, registrations, and pending writes from the old addon generation.
- Errors disable only the offending addon and preserve diagnostics.

Initial Lua scope is deliberately small: lifecycle, timers/tasks, logging, settings/storage, and read-only snapshots. Existing write APIs are not inherited automatically.

## 8. Verification strategy

### Automated on every change

- JSON Schema validation and deterministic knowledge generation;
- generated-source freshness checks;
- native unit tests with `/W4 /WX` and CTest;
- camera golden and property tests;
- cursor/settings/lifecycle state-machine transition tests;
- bounded-queue, cancellation, overflow, and generation tests;
- hook fixtures using owned executable memory: normal, conflicting, chained, cyclic, malformed, and changed-code cases;
- fault injection at every borrowed-state restoration point;
- manifest/path-confinement and Lua sandbox tests;
- DLL exports, package contents, licenses, and clean-machine dependency checks.

### Native integration gates

- start/stop/restart cycles without leaked hooks, threads, locks, contexts, or callbacks;
- attachment refusal leaves the process untouched;
- delayed and reentrant callbacks cannot run freed module state;
- scene/player/window/context generations invalidate stale work;
- existing-detour policy is exercised with real fixture detours;
- fault tests demonstrate finally-restoration and useful diagnostics.

### In-game acceptance matrix

Record game hash, mod build, scenario, result, logs, and tester/date for at least:

- title startup and settled attachment;
- new game, save load, save creation, and return to title;
- first/third/overhead transitions and repeated rapid switching;
- mouse look, FOV changes, controller input, and rebinding;
- walking, combat, knockdown, death, and respawn;
- inventory, looting, containers, shops, dialogue, and arena forms;
- doors, dragging, pickups, and item placement;
- scene travel and arena entry/exit;
- Settings Apply/Cancel/Defaults and preference restart;
- focus loss, alt-tab, windowed/fullscreen changes, and resolution/UI-scale changes;
- stop/restart in gameplay and at title;
- coexistence with each supported mod/loader configuration;
- a 30-minute normal-play soak and a 30-minute transition stress soak.

Static checks and offline fixtures never substitute for these scenarios.

## 9. Implementation phases and exit gates

### Phase 0: evidence reset and scope quarantine

- Adopt this plan as authoritative.
- Exclude game files, raw research, reference clones, and toolchains from release inputs.
- Create the supported-build manifest and initial research inventory.
- Remove quarantined features from the new build definition rather than carrying dormant code.

**Exit:** one authoritative plan, reproducible research environment, explicit release scope, no accidental game/research artifacts in packaging.

### Phase 1: knowledge-base foundation

- Implement schemas, validation, REA import, deterministic generation, and human-readable contract pages.
- Import the ten initial function dossiers with their actual evidence states.
- Add camera, lifecycle, input, UI, and render research questions with owners and unknowns.
- Reject unreviewed compatibility claims in generated runtime tables.

**Exit:** schema tests pass; generated output is deterministic; every native claim has provenance and an evidence state; no runtime code consumes unreviewed records.

### Phase 2: clean build and runtime skeleton

- Establish CMake/CTest, public include tree, module boundaries, logging, results, lifecycle, and diagnostics.
- Implement the hook broker against synthetic fixtures without attaching to Exanima.
- Implement thin standalone startup with build refusal and no-op attachment.
- Restore public automated tests; private game fixtures may remain separate, but production tests are versioned with source.

**Exit:** clean warning-free build and tests; no-op start/stop/restart; package smoke test; hook fixtures pass; zero game hooks installed by default.

### Phase 3: ExCamera vertical slice

- Review and record the exact camera contracts.
- Implement pure camera math and one minimal native adapter.
- Add first/third/overhead switching, mouse look, and FOV.
- Keep native picking/rendering/input wherever correct.
- Run the in-game core scenarios before adding any optional feature.

**Exit:** core camera acceptance passes with no runtime faults and clean stop/restart; unsupported features are absent from package and UI.

### Phase 4: settings and input completion

- Implement settings transactions, persistence, native-form ownership, and cursor arbitration.
- Complete interaction, focus, panel, and resolution/UI-scale acceptance.
- Add fault injection and repeated transition stress tests.

**Exit:** no stuck cursor/input, wrong table restoration, lost Apply, or stale-panel callback in the supported matrix.

### Phase 5: public SDK and read-only Lua

- Stabilize the initial C ABI and capability registry.
- Implement copied lifecycle/camera/player/scene snapshots and events.
- Implement Lua sandbox, timers/tasks, settings/storage, diagnostics, and examples.
- Add reload/disable/error isolation tests and live addon smoke tests.

**Exit:** SDK examples and Lua examples work in game; all public contracts have tests and documentation; no write API is exposed by default.

### Phase 6: coexistence and optional adapters

- Validate EMTK startup and Settings ownership before enabling either.
- Port optional features one at a time through research, contracts, fixtures, and in-game acceptance.
- Require independent performance and teardown validation for rendering/XR work.

**Exit:** each shipped optional capability has its own green acceptance record; all others remain excluded.

### Phase 7: release candidate

- Freeze features and contracts.
- Run the complete automated and in-game matrices on the exact candidate binaries.
- Triage all defects; no known critical/major issue remains open.
- Verify installation, upgrade, rollback, preference preservation, and uninstall.
- Produce reproducible ZIPs, SHA-256 files, licenses, release notes, support matrix, and known limitations.
- Tag only after candidate acceptance; CI reproduces and verifies the same artifacts.

**Exit:** signed-off release evidence for the exact hash being published.

## 10. Immediate implementation batch

The first code batch after this plan is intentionally limited:

1. `knowledge/schema` and `tools/kb.py` with validation and deterministic generation tests.
2. Standard-build manifest and ten imported function records marked only with the evidence actually established.
3. CMake/CTest skeleton with core result/log/lifecycle primitives and warning-as-error builds.
4. Hook-broker synthetic tests, especially unknown-detour refusal and finally-restoration.
5. Thin no-op runtime/launcher package proving clean attachment refusal and restart before camera hooks return.

No gameplay feature is considered started until its contract record and acceptance scenario exist.

## 11. Decisions that shaped the rebuild

- The old single-translation-unit architecture is discarded.
- The old phase labels claiming “code complete” do not count as completion.
- ExanimaLua is not made the mandatory owner of ExCamera or Settings before it passes live validation.
- Standalone operation remains a first-class boot path; EMTK is an optional later adapter.
- The new runtime does not inherit ExanimaLua’s broad API or ExCamera’s optional features by default.
- Public tests return to the source repository. Private game files and raw reverse-engineering evidence remain outside release artifacts.
- Initial support is the supplied standard executable only. Hellmode requires its own reviewed binary and acceptance run.
