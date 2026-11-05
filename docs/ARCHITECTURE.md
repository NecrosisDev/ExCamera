# Architecture

## Build boundaries

`build.py` builds two Windows x64 binaries: the launcher (`src/launcher.cpp`) and the DLL (`src/first_person.cpp`). The DLL also links `src/native_settings.cpp`, `src/settings_presentation.cpp`, the bundled MinHook C sources, and `src/supersampling_shim.asm` (plus `src/xr_host.cpp` when `--openxr-sdk` is passed).

The files under `src/mod/` intentionally form **one translation unit** through ordered includes in `first_person.cpp`; `state.inl` also includes `activation.inl`. They share private state and helpers. Do not add these `.inl` files to the compiler's source list or reorder them as an organizational cleanup.

## Where to start

Paths in this section are relative to `src/`.

- `mod/state.inl`, `activation.inl`, `session.inl`: shared state, activation checks, hooks, and session lifetime.
- `mod/optional_contracts.inl`, `held_throw_admission.inl`: exact loaded optional witnesses and held-body hook admission/retirement.
- `mod/input.inl`, `input_policy.h`, `input_ownership.h`, `raw_registration.h`: input routing and ownership.
- `mod/camera.inl`, `view.inl`, `frame.inl`, `camera_motion.h`, `head_stabiliser.h`: camera pose, transitions, and per-frame behavior.
- `mod/combat.inl`, `attack_variants.inl`, `picking.inl`, `held_object.h`: combat and interaction integration.
- `mod/inventory.inl`: observed ordinary inventory-preview transactions and native commit cancellation.
- `mod/held_throw.inl`: release intent, native body lifetime, and one-shot physics delivery.
- `mod/supersampling.inl`, `supersampling_depth.h`, `supersampling_shim.asm`: rendering integration.
- `mod/wounds.inl`, `mod/detail.inl`, `mod/blade.inl`: native procedural wounds, blood, surface detail, and blade drag/stab-sink physics.
- `mod/overlay.inl`, `panel.inl`, `settings.inl`: overlay, settings integration, and persistence.
- `native_settings.*`, `native_widgets.h`, `native_widget_contract.h`: native widget adapter and event contracts.
- `settings_model.h`, `settings_draft.h`, `settings_presentation.*`: settings values, draft/Apply/Cancel handling, and display mapping.
- `xr/`, `mod/xr_stereo.inl`, `xr_host.cpp`: OpenXR math, whole-function native contracts, and optional stereo/VR host.
- `tracking.h`, `tracking_udp.h`: tracking data and receiver.

## Native compatibility is more than addresses

`game_signatures.h` contains precomputed signature tables. `game_layout.h` resolves game targets. Finding a target is not proof that every field or call contract still matches the game.

`game_structs.h` describes the layouts used by the mod. `game_contracts.h` contains precomputed owner/field evidence; `native_contracts.h` validates those contracts. `xr/native_contracts.h` captures whole-function byte patterns and the frustum template required before OpenXR replay is admitted. `native_build.h` supplies the supported build identity. Native widget event semantics are separate again: `native_widget_contract.h` distinguishes checkbox selected-state handling from slider callback handling rather than treating all widgets identically.

Signature, field-contract, and XR-contract tables are checked in alongside their authoritative specifications in `tools/targets.json`, `tools/native_contracts.json`, and `tools/xr_contracts.json`. `python tools/check_generated.py` verifies source/header consistency across all three specifications offline using only the Python standard library; CI runs it before compiling. The optional native generators additionally require the reviewed executable, `pefile`, and `capstone`, and are not needed for a normal build. See `tools/README.md`. Updating compatibility requires fresh native review, not simply changing the executable hash or accepting a successful signature match.

Core field witnesses validate the executable file; loaded entry/dispatch checks are separate. The optional capabilities documented below additionally compare their critical loaded instruction windows. These checks do not establish that every required function interior in memory is unchanged by another component.

## Camera query performance

The shared mesh queue checks the submitted pointer before validating a possible head drawable's back-pointer and name. The live root and head reads remain guarded on every eligible call; no presentation-long head cache delays replacement detection.

`walkSceneVisit` offers a synchronous visitor after each current node's full-header readability check. Self-pick masking uses that validation for its narrower flags access, falling back to the original narrow check for partial leaves. The visitor can compact only consumed queue entries; it calls no native code. Ordinary scene discovery uses the same walker without a visitor. Pick flags remain query-scoped and finally-restored, with pre-existing skip bits preserved.

The near-plane solver may cap proved-clear results at twice the caller's maximum near plane. Its initial segment ends at an endpoint of the original six-step bisection tree, so even a quantized result beyond it already saturates the filter. A hit uses the unchanged six-step search; unknown queries retain the original fallback. Props are still queried unless level clearance already forces the minimum plane. Every temporal sample retains current geometry queries and its own physical head pose, camera transform and filter evaluation. Held placement passes its first predicate result into clearance refinement and still checks final clearance before accepting an output.

The native regression suites used during development compile production owners with owned-memory/native-boundary fixtures and compare against a pinned pre-change oracle. Their retained integration fixtures cover temporal sample order, extra requests, source faults, replacement, TP obstruction ownership and native view boundaries. They do not launch or attach to the game.

## Screen-effect quality diagnostics

`first-person-status.json` reports the screen-effect input and intermediate target dimensions for ambient occlusion and depth of field, plus `view_effects_low_tier`. These targets match the full view dimensions; the low-tier flag remains false because screen effects no longer reduce resolution under load. Nonzero dimensions describe the most recently supported rendered view, and are cleared when the effects skip or target creation fails. Use these values with the per-stage CPU/GPU timings to optimize shader and memory work without reducing output resolution.

## Settings boundaries and `ScriptRuntime.dll` coexistence

The native UI edits a draft. Apply publishes accepted values and queues per-row revisions under a short settings lock; Cancel discards draft edits. Only the session worker persists INI values, outside that lock, then acknowledges the exact revision written. Edits arriving during a write remain pending. Failed writes remain pending: running and cleanly stopped sessions retry on the same worker; a fault exit makes a final best-effort pass after its notice/recovery work ends. Native GUI callbacks never wait for disk I/O. Status files are best-effort snapshots and may remain stale if publication fails. The presentation layer maps internal values to labels without making the native controller Controls table into an ExCamera page. Preferences are external to the source and runtime package.

ExCamera operates **100% standalone** — all camera modes, settings persistence, combat cues, and procedural wounds, blood, and fire run natively inside `ExanimaFirstPerson.dll` without requiring ExanimaLua (`ScriptRuntime.dll`). When `ScriptRuntime.dll` is loaded in the process, ExCamera optionally coexists with it by hosting third-party mods' registered Settings tabs inside ExCamera's widened native Options form and binding `ScriptRuntime` inventory exports when available for the VR hip pouch.

Explicit leaning has no runtime input or pose path. Its four hidden legacy INI preferences, existing bulk-policy invalidation, and false/zero lean status fields remain for compatibility. The pose-cadence diagnostic (`held.physics_step`) also remains; it is not native release/throw timing. Active exploration spine-follow is separate and retains its native pose borrowing and restoration.

ExCamera-owned checkboxes adapt the native scrolling-coordinate round trip for preview and release events. The game still performs hit testing, capture, checked-state changes, and action dispatch. This is not a global GUI patch; vanilla descriptors retain their native handlers. Handler dispatch lifetime is guarded separately from action callbacks so a native activation can still update the draft.

## Thread and input boundaries

Diagnostic names are copied under a short leaf lock, drag status uses atomic flags, and camera-owned diagnostic values are copied under the camera lock before JSON/file I/O. Tracking transport failures retire the current sample and exit the receiver so its lifecycle owner can restart after backoff.

Guard priority suppresses attack only during the overlap. Releasing guard admits a still-held native attack without clearing unrelated ownership masks or inventing a queued attack. Native timing and recovery still decide when an attack can execute.

Window callbacks capture their forwarding procedure and installation generation. Native destruction is forwarded before retiring that exact installation, so a recycled HWND does not inherit the old registration. Raw-input retirement retries fresh ownership observations for a bounded interval; unknown or foreign registrations are not removed. This does not make Win32 procedure replacement atomic against arbitrary concurrent subclassers.

## Runtime ownership and diagnostics

Camera ownership is decided under `cameraLock`; native callbacks and status producers observe copies rather than racing the saved frame. The status value `camera_ownership_state` is `0` unknown, `1` owned, `2` parked, `3` restoring, `4` restored, or `5` faulted. A park records the gameplay (root and scene) it belongs to. Released camera records are invalidated when that identity changes. Restoration is sticky-unknown when a released camera of the current gameplay can no longer be reclaimed, when pointer equality cannot prove lifetime, or when an earlier restoration failed. A clean re-arm is the only path back to normal ownership. Stop waits for `savedCamera`, `pendingRestore`, and admitted render scopes to drain before it may report `restored`; a timeout or unknown record reports failure instead.

Cursor freedom is one atomic reason bitmask: Cursor Look, an automatically selected native panel, and the F8 panel. Actions and clipping test the aggregate, while each owner changes only its own bit, so one owner cannot clear another's reason. Native Cursor Look remains user-owned data. The status file reports the aggregate and the cumulative transition counter.

Native Settings owns a form only while its captured root and the applicable child table still match the recorded identities. An Apply publishes one draft transaction; Cancel never mutates native tables. Ownership/action refusals are counted, and the last refused descriptor is reported separately as `kind<<24 | flags`. Recovery restores the original table only when ownership is still proved; an unknown replacement form is left untouched. The atomic status file is a best-effort snapshot, not proof that the game or the mod is healthy.

Hook-fault counters are regression signals, not normal operating conditions. A counter is incremented once at the owning hook boundary, whether that callback contains the fault or lets it propagate to the game's handler; inner helpers do not increment it again. The top-level hook counters are `camera_faults`, `swap_faults`, `behavior_faults`, `pose_faults`, `window_faults`, `cursor_faults`, `view_faults`, `body_part_faults`, `room_view_faults`, and `node_move_faults`; feature-specific counters such as `headwear_render_faults`, `wound_skin_faults`, and the nested `supersampling`, `openxr`, `blade`, and `finger_bones` counters follow the same rule. Every fault counter and flag must stay zero, as must `faults_seen`, `camera_restoration_unknown`, and `settings_fault_at`; `faults_seen` and `fault_last` summarize distinct exception records. A nonzero count with `restoration=failed` or `settings_fault_at` requires a restart, not continued play.

## Inventory preview boundary

The inventory adapter observes one ordinary source/payload/job transaction. Only an unchanged completed preview with a positively observed placement failure may use the native cancellation sentinel at commit. The game still owns loading, completion, source handling and disposal. Merchant/special sources, changed scenes and replaced jobs remain native; this is not a general asynchronous inventory lifetime repair.

## Bounded native attack variants

The optional `attackVariants` capability preserves the native classifier inside only the two ordinary attack-family starters. The existing Manual-defense gate remains unchanged outside an admitted invocation. Unsupported starts fall back to that gate, rather than disabling Manual defense.

Ordinary native two-handed weapon configurations do not consult this classifier in these starters, so they do not need the exception. The adapter does not broaden admission merely to cover synthetic grip/equipment combinations.

Admission requires the exact controlled native actor, its head/behaviour/scene identities, the native physics thread while its published scene's callback loop is active, a live non-retiring scene, entry motion 3–12, inactive defense with no existing incoming-motion request, and a native grip-table entry with no callback. Motions outside 3–12 are deliberately unsupported by this initial adapter; this is not the native motion range. Every nested starter pushes its own scope, including rejected calls, so same-actor reentry cannot inherit an outer grant.

The original starter executes once and selects the variant, motion and RNG state. On a validated normal return, the adapter re-establishes physical ownership and obtains fresh Manual/held/native permission. If still denied, it writes only the newly published incoming-motion request to absent. It does not call native defense cleanup or save/restore hand, contact, constraint, recovery or pose state. Input-generation changes signal a handoff, not an actor lifetime lock. Unknown normal-return ownership disables this optional capability until process restart and does not attempt an old-pointer rollback.

This initial capability additionally requires the supported executable hash, freshly checked on installation and re-arm. It is deliberately unavailable on merely signature-compatible, unreviewed game builds; the existing Manual gate remains the fallback. File contracts and exact loaded instruction windows cover the reviewed family bodies/tails and immediate classifier, transition, grip and child-traversal helpers, along with critical scheduler, lifetime and thread witnesses. Optional creation failures preserve the existing Manual capability when rollback succeeds. Retained hooks must pass fresh source and loaded-code checks on re-arm even after a runtime ownership fault. These checks do not establish universal compatibility with another mod patching unrelated interior callees, calling actors off-scheduler, or destroying native objects concurrently.

Shared thread/context/scene locators and ten ownership witnesses belong to the independent optional `physicsOwnership` group. Attack admission requires its fifteen local witnesses plus those ten shared witnesses, preserving the original twenty-five-window proof. Withholding attack never clears common or held-throw locators. A missing common dependency withholds dependent optional capabilities, not Manual defense or core gameplay.

## Optional held-body hook lifetime

Throwing is limited to the reviewed generic, owned root body with no child bodies or external attachment constraints. Native release retains all eight settling steps and its collision flags. At the next eligible integration, a uniform velocity delta is added to the older point history, letting the native integrator apply damping, gravity and collision response. It does not replace current geometry or existing angular motion.

The same serial remains published through queued work, native release and settling. Destruction, another release, any new grab, owner/input changes, a changed settling sequence, or the absolute 250 ms queue deadline discard the optional impulse. Unsupported or interrupted cases keep the native drop; articulated objects are not covered. The original 120 ms wheel-gesture freshness is separate from this delivery deadline.

`heldThrow` is independently admitted using the same fresh supported-source check, its own loaded windows, shared physics ownership, and five exact native metadata cells. Neither attack preservation nor Manual defense is a prerequisite. Creation installs the body destructor hook before integration; an optional failure rolls back only those hooks, retaining removal identities and aborting the session if rollback fails.

Session activation, cancellation and stop are separate from runtime queue ownership. Before an arm attempt, stop or fault cleanup, readiness is withdrawn, any MinHook suspension debt is discharged outside gameplay locks, and only then does the runtime's cancellation lock clear older queued serials. A suspended callback may own that lock, so every cancellation path uses this ordering rather than relying on an earlier caller to have recovered. Only a fully successful session arm restores readiness. Native callbacks are not made under that queue lock. Retained source/code changes require restart before any all-hooks re-enable, rather than silently disabling the feature while reusing unsafe trampolines.

## Optional cue rendering boundary

The attack cue owns its OpenGL resources and capability decisions per current native presentation context. Where supported, it changes only draw-buffer zero and viewport zero, preserving floating-point viewport coordinates; global setters are used only for genuinely global state. Drawing requires a default framebuffer with only output zero active, without changing native routing. Nondefault or multi-output frames are temporarily skipped and can resume on a later swap. Missing or uncertain required APIs, failed program creation, or a draw error omit the optional cue for that context; owned resources remain subject to the existing context retirement paths. This boundary does not replace native rendering or promise recovery from arbitrary driver exceptions.

## Validation boundary

Offline compatibility checks (`tools/check_generated.py`), Python tool unit tests (`tools/tests/`), shipped C++ header unit tests (`tools/run_cpp_tests.py` under `/W4 /WX`), `--dev` and release `/WX` builds, and DLL export checks run automatically in CI (`build.yml` and `release.yml`). They catch build, contract, packaging, and pure-policy regressions, while live camera, input, and rendering behavior in a running game is tracked in `docs/RELEASE_CHECKLIST.md`.
