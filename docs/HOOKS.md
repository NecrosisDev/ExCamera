# Native contracts and hook ownership

The runtime's callable game targets and globals are listed by name in
[`tools/targets.json`](../tools/targets.json), with RVAs from the reviewed source build
(Exanima 0.9.5.2g, Steam build 25803178). [`src/game_signatures.h`](../src/game_signatures.h) holds the
generated signatures used to resolve those addresses. Native field contracts separately check the
reviewed instructions that access object fields and binding slots before activation.

In runtime code, a target is `gameAt(fp::sig::name)`: the loaded module base plus its resolved RVA.
The source executable's preferred base, `0x100000000`, is not an address assumption for another build.

## Native field contracts

[`src/game_structs.h`](../src/game_structs.h) names the object-field offsets. Their evidence is recorded in
[`tools/native_contracts.json`](../tools/native_contracts.json): native owner, instruction RVA, encoded
operand width, and the pointer path that establishes the owning object. A matching displacement on a
different object is not evidence for the field. For an address-taking instruction such as `LEA`, operand
width describes the address operand; native copies or inspected consumers establish an array's extent.
Affine aliases identify their witnessed matrix and component offset.

[`tools/native_contracts.py`](../tools/native_contracts.py) generates
[`src/game_contracts.h`](../src/game_contracts.h) only from the archive identified by the evidence's hash.
Generation requires each selected instruction to start on a decoded boundary in its reviewed owner,
checks its operand, and keeps the emitted window inside that owner. Derived offsets must reach native
field or global-reference evidence without cycles. The binding scan count is an explicit mod bound tied
to the witnessed slot list; it does not assert where the game's entire binding table ends. Generated
static assertions keep the named C++ offsets aligned with the reviewed values.

Both the launcher and DLL run [`src/native_contracts.h`](../src/native_contracts.h) after resolving game
targets from the executable file. Each owner must resolve uniquely; a callable owner must agree with its
resolved target when that target is available. Each field window preserves the reviewed opcodes,
registers and field displacements while allowing relocation-sensitive operands to move. Binding-slot
references also decode their RIP-relative displacement and require the result to equal the resolved
`movementKeys` base plus the reviewed slot offset, with the native access width inside the image.

A required contract failure rejects the candidate layout. A contract declared for `manualDefense`
withholds all four optional targets together. The checks use a candidate copy, so a rejected required
layout cannot publish a partially accepted set of addresses. The file image supplies instruction
evidence; live hook-entry ownership is checked separately before installation or re-arm.

These checks cover the recorded accesses, references and derivations. They do not establish every
ancestor-pointer path, callee behavior or control-flow meaning in an arbitrary future executable.
Compatibility remains conditional on those instruction contracts and the existing callback health
checks during play; it is not a claim of complete semantic equivalence with the source build.

## Hooked functions (MinHook)

| Target | RVA (source build) | Area |
|---|---|---|
| `cameraOrbit`, `cameraZoom` | `0x91F00`, `0x92630` | overhead orbit/zoom; gate direct controller callers in close views |
| `cameraMouse` | `0x11EF40` | native camera input gate; retains `copyCameraSnapshots` at `0x11EB50` |
| `pollInput` | `0xAA080` | input; per tick |
| `cursorAim` | `0xBF680` | aim |
| `cursorPosition` | `0x22710` | cursor; told apart by return site |
| `cursorWarp` | `0x22780` | cursor; the pick-up warp is skipped |
| `explore` | `0xBFEA0` | movement (out of combat) |
| `combatStep` | `0xC13F0` | movement (combat) |
| `defenseReactive` | `0x163880` | optional Manual: native defensive response entry/continuation |
| `defenseMotionSelect` | `0x156FB0` | optional Manual: threat-conditioned native motion selection |
| `defenseAttackUpdate` | `0x166580` | optional Manual: retained interception in native attack execution |
| `travelTarget` | `0x19390` | movement |
| `walkGait` | `0x163EF0` | movement |
| `poseBuilder` | `0x1760F0` | skeleton: spine bend, crouch, head stabilisation; per tick |
| `partSettle` | `0x89800` | skeleton: head stabiliser; per tick |
| `sectorUpdate` | `0x58CD0` | rendering: hides meshes that would cover the view |
| `coverPass`, `coverDepth` | `0x144610`, `0x1444A0` | rendering: tile covers skipped in first person |
| `pickRay`, `planeHit`, `grabPick` | `0x8F720`, `0x8F530`, `0x8FB30` | picking and grabbing |
| `dragPlace`, `dragDrop` | `0x1183C0`, `0x119C30` | held objects |
| `doorHandler`, `hingeHandler` | `0x11B400`, `0x11BC50` | doors and lids (pass-through; the trampolines drive them each frame) |
| `opengl32!wglSwapBuffers` | (export) | per frame: settings window, crosshair, swing cue |
| `opengl32!wglMakeCurrent` | (export) | retires cue resources before an observed core context switch or unbind |
| `opengl32!wglDeleteContext` | (export) | tracks native deletion and ends the old cue context identity on success |

## Replaced vtable slots

| Slot | Holds (checked) | Replaced by |
|---|---|---|
| `navSlot` | `navSampler` | `nav`: the camera; per tick |
| `behaviorSlot` | `playerBehavior` | `behavior`: aim and input; per tick |
| `pointSlot` | `aimPoint` | `point`: the aim point |

## Called, not hooked

`rebuildProjection`, `rebuildInverse`, `levelRay`, `levelRay2`, `headingFromDirection`,
`aimFilterPush`, `aimFilterUpdate`.

`defenseCleanup` (`0x162E80`) belongs to the optional `manualDefense` group. It is called only on native
actor callbacks when active or remembered assistance must be withdrawn. It restores native constraints,
hand state and response history, and can select another native posture. The attack wrapper preserves
that transition instead of executing the old attack body afterward. UI, window and worker threads never
call it or directly rewrite defensive capability/eligibility.

## Optional manual-defense admission

The four-target `manualDefense` group is optional. Target resolution or a native contract failure for
that capability withholds all four, without concealing ambiguous or contradictory matches. A zero
optional RVA must never be called or passed to a hook API. Before creation, all four live entries must
match 16 bytes of the file image. Existing foreign defense detours do not become chains under
`OtherHooks=allow`; initial admission keeps Automatic effective instead.

The three optional hooks are created disabled as one capability. Partial creation is removed in reverse
order; only a complete rollback allows core startup with Automatic. Once created, their RVAs and retained
trampolines keep their identity. A changed entry on re-arm, including cleanup, requires restart before any
new enable batch. No retained trampoline is freed to create an apparent fallback.

`test_manual_admission.cpp` exercises admission, native forwarding, cleanup entry validation, complete
rollback and retained-entry conflict using executable storage in its own process. `test_combat_controls.cpp`
exercises the production policy and native callback contracts. Neither executes archived game code.

## Activation acknowledgement

The launcher uses `enableWithStatus` for both a fresh load and a DLL already mapped in the game.
The export identifies the request; [`mod/session.inl`](../src/mod/session.inl)'s worker owns startup,
stopping and re-arm. A request arriving during a clean stop can queue re-arm. A retained fault refuses
the request and requires restart. EMTK's existing `enable`/`disable` exports retain their off/on behavior.

[`activation_status.h`](../src/activation_status.h) defines the shared acknowledgement. The launcher
accepts only a top-level status containing the current PID, process creation time, request token,
revision and build, with `activation_state` equal to `running`. A successful export call confirms that
the request was accepted; the worker publishes completion after arming. `starting`, `stopping` and
`stopped` remain pending; `failed` and a mapped-build mismatch are explicit failures. A stale status
from another request or process cannot acknowledge the new launch.

The six phases are `idle`, `starting`, `running`, `stopping`, `stopped` and `failed`. Status treats a
running phase with a raised stop flag as stopping, closing the interval before the worker updates its
phase. The launcher waits at most ten seconds for the matching result and reports a timeout without
claiming successful activation. Clean F7 re-arm reuses the admitted layout and retained trampolines,
rechecks live native and dispatch ownership, and starts a fresh callback-health observation window.

## Input ownership

[`input_ownership.h`](../src/input_ownership.h) owns the input history shared by the window adapter and
native poll adapter. Cross-thread event mailboxes and invalidation counters are atomic. The native input
thread alone changes poll history: the gameplay context, button handoff, optional binding holds and
consumed-button state. A context includes the controlled character, head, scene, perspective, combat
ownership, binding values and policy revision.

There are two different release obligations. `releaseGameplay()` advances the generation and clears
transient mailboxes, so stale gameplay publications cannot regain ownership. It preserves window-owned
presses whose key-up still needs to be consumed. `newWindowSession()` clears those window obligations
when a new subclass session starts. A complete `resetQuiescent()` is reserved for a state with no active
callbacks; normal handoffs use the narrower operations.

Optional held bindings keep their physical key and generation. Acquiring gameplay, changing the bound
key or crossing an ownership boundary while the key is down requires release before that hold becomes
active. Combat button suppression follows the identity of the physical LMB press, including a complete
click between native polls. Retiring an older sampled suppression cannot clear a newer press. Guard
admission, buffered-button filtering and native RMB consumption use the same poll-owned state.

## Raw mouse registration

[`raw_registration.h`](../src/raw_registration.h) separates the remembered registration candidate from
fresh evidence that it still belongs to ExCamera. The worker observes the process's mouse usage
registration before retaining, removing or replacing it; our registration requires the expected window
and zero flags. It also rechecks while raw input is active, so a later registration by another component
does not leave the old `rawRegistered` latch authoritative.

Loss or uncertainty publishes raw input as unavailable, clears accumulated relative input and resumes
cursor-delta sampling. A foreign registration observed in the query is preserved. Window migration
retires an observed owned registration, queries again, and installs only when the slot is empty; the
successful installation is queried once more before raw input becomes effective. A failed query retains
the remembered candidate so a later observation can resolve its ownership. If shutdown cannot retire
that candidate, it reports `raw_retirement_pending` with callbacks retained.

Windows provides no atomic compare-and-remove operation for this process-wide registration. These
checks act on fresh observations; they do not exclude another component changing the registration
between the query and the following native API call.

## Hook ownership when stopping or re-arming

MinHook is retained after F7 so a callback already in flight can finish through its trampoline.
An unchanged, approved chain can be enabled again with F6. If another program changes an entry while
ExCamera is stopped, that retained trampoline is no longer evidence of the current code: re-arm
reports `hook_entry_changed` and requires a restart. `OtherHooks=allow` does not override this check.

The vendored MinHook has a small ExCamera ownership guard (`MH_ERROR_CONFLICT`). It remembers the
original instruction bytes consumed by each trampoline, including any immediate beyond the five
bytes patched by a jump. It also checks the hot-patch area when that form is used. During enable,
disable, or queued/batch changes, it suspends the enumerated threads, checks every affected entry,
and only then relocates suspended instruction pointers and writes code. A rejected batch changes
neither entries nor suspended instruction pointers. The source changes are in `hook.c`,
`trampoline.c`, `trampoline.h`, and `MinHook.h`; the upstream licence remains intact.

If another program takes over an entry during play, stopping preserves that replacement. ExCamera
removes the hooks whose entries it still owns individually, retains its allocated trampolines,
and reports incomplete removal with a restart requirement. The camera restoration gate still runs
first; a pending or uncertain camera restoration is never reported as complete. The compatibility
health check starts a fresh observation window on each successful re-arm.

The worker releases input and reconciles raw registration, then waits for camera restoration and cue
retirement before disabling hooks. These waits are bounded. If an owning callback does not return in
time, the module stays mapped with its callbacks and trampolines retained and reports the specific
pending state. It does not report a clean stop or accept re-arm after that fault. Startup rollback with
no published camera or cue resources can complete without waiting for a render context.

This protects the ordinary in-process installation/removal paths. Thread enumeration and suspension
cannot make a guarantee about newly created or unsuspendable threads, or an unrelated writer outside
the process changing executable memory concurrently. Such interference still requires a restart.

`test_chaining.cpp` exercises preserved existing chains, changed entries during stop and play,
direct/all/queued APIs, suspended instruction pointers, relocated immediates, and hot-patch prefixes
using owned toy functions only. `tools/test_signatures.py` checks that the command-line resolver
rejects out-of-image targets and mismatched vtable slots; it uses no game or optional Python packages.

## OpenGL cue lifetime

[`cue_resources.h`](../src/cue_resources.h) owns the arc's program, VAO and VBO, temporary shader objects,
and any bindings borrowed during incomplete setup. Failed shader compilation, linking or buffer setup
retires what was allocated. Successful linking detaches and deletes both shader objects. There is no
allocation for the unused highlight pass.

[`mod/overlay.inl`](../src/mod/overlay.inl) associates those resources and their GL entry points with a
context identity. `cueLock` serializes preparation, drawing and retirement; an API table cannot be
cleared beneath a draw. Native WGL switch/delete calls run after releasing the lock. Up to eight
simultaneous or pending context records are retained; additional contexts omit the optional cue.

| Observed event | Resource lifetime |
|---|---|
| Another frame in the same context, or a successful unchanged context/DC bind | Reuse the existing arc resources. |
| Core `wglMakeCurrent` switches context, changes DC or unbinds | Retire resources while the previous context is still current. Returning through A → B → A intentionally rebuilds A's cue. |
| A context switch fails | Resources already retired stay retired. If an unchanged bind fails and unbinds the context, its existing ownership remains pending until that context returns or deletion succeeds. |
| Core `wglDeleteContext` | Retire directly only when the target is current on this thread. Keep native deletion outside the lock, but block preparation until its result is recorded. Successful deletion ends the old identity before cue preparation can use a reused handle. |
| Stop reaches a frame or core WGL callback with the owner current | Retire through that context, including a frame with no visible cue. |

Construction or drawing failure disables further cue attempts for that binding lifetime. Drawing runs
borrowed-state restoration from its `finally` path, including when the draw throws. A cleanup exception
keeps incomplete ownership visible to the stop drain.

The worker requests retirement and waits at most two seconds for resources and WGL callbacks to drain.
It never makes a game context current on its own thread. A context that remains unavailable produces
`cue_retirement_pending`; its cleanup callbacks and trampolines remain mapped. Completed retirement
releases that context record and its failure latch. The ordinary stop drain clears remaining empty
records only after resources and callbacks have drained; successful native deletion separately ends the
old context identity.

This lifecycle observation covers the core `wglMakeCurrent` and `wglDeleteContext` exports. It does not
observe arbitrary ARB/EXT context-switch functions or establish every share-group lifetime. An
unobserved switch can leave resources pending, and native deletion of an inactive context is not
independent proof that shared GL objects have been reclaimed. Those paths do not justify forgetting
live handles during the ordinary stop drain.

## Return sites

Some hooks act only for particular callers, which they recognise by return address: `uiPollCursorReturn`,
`crosshairCursorReturn`, `exploreTravelReturnA/B`, `pickupWarpReturn`, `dropSpotPickReturn`,
`grabStartPickReturn`, `hoverPickReturn`, `doorPlaneReturn`, `hingePlaneReturnA/B/C`.

## Globals

Globals, read or written (`playerRoot`, `currentScene`, `keyWord`, `buttonWord`, `dragManager`,
`mainViewport`, ...). See `tools/targets.json`. Each is found through two independent RIP-relative
references in the game's code.

## Adding a target

1. Add it to `tools/targets.json` with its RVA in the build you have.
2. `python tools/signatures.py generate <that Exanima.exe>`.
3. Use it as `gameAt(fp::sig::yourName)`. A hook's detour that runs every tick should also count itself
   (`healthHit`) if the mod cannot work without it. Add it to `fp::HealthHook` in that case.

## Changing a native field

Review the actual owning object and instruction before changing `game_structs.h`. Update
`tools/native_contracts.json` with the source archive, owner range, instruction, operand width and
pointer provenance. Array extents and aliases need native copy/consumer evidence; an existing mod helper
is not independent proof of its own memory contract. Record optional capability ownership where it
applies.

Run `python tools/native_contracts.py generate <reviewed Exanima.exe>` to regenerate the declaration
assertions and instruction windows together. `python tools/native_contracts.py check <Exanima.exe>`
checks a candidate executable with the generated target signatures and field contracts. Checking a new
build uses its instructions and references; regeneration requires the archive named by the evidence.
