# Architecture

## Build boundaries

`build.py` builds two Windows x64 binaries: the launcher (`launcher.cpp`) and the DLL (`first_person.cpp`). The DLL also links `native_settings.cpp`, `settings_presentation.cpp`, the bundled MinHook C sources, and `supersampling_shim.asm`.

The files under `src/mod/` intentionally form **one translation unit** through ordered includes in `first_person.cpp`; `state.inl` also includes `activation.inl`. They share private state and helpers. Do not add these `.inl` files to the compiler's source list or reorder them as an organizational cleanup.

## Where to start

- `mod/state.inl`, `activation.inl`, `session.inl`: shared state, activation checks, hooks, and session lifetime.
- `mod/optional_contracts.inl`, `held_throw_admission.inl`: exact loaded optional witnesses and held-body hook admission/retirement.
- `mod/input.inl`, `input_policy.h`, `input_ownership.h`, `raw_registration.h`: input routing and ownership.
- `mod/camera.inl`, `view.inl`, `frame.inl`, `camera_motion.h`, `head_stabiliser.h`: camera pose, transitions, and per-frame behavior.
- `mod/combat.inl`, `attack_variants.inl`, `picking.inl`, `held_object.h`: combat and interaction integration.
- `mod/inventory.inl`: observed ordinary inventory-preview transactions and native commit cancellation.
- `mod/held_throw.inl`: release intent, native body lifetime, and one-shot physics delivery.
- `mod/supersampling.inl`, `supersampling_depth.h`, `supersampling_shim.asm`: rendering integration.
- `mod/overlay.inl`, `panel.inl`, `settings.inl`: overlay, settings integration, and persistence.
- `native_settings.*`, `native_widgets.h`, `native_widget_contract.h`: native widget adapter and event contracts.
- `settings_model.h`, `settings_draft.h`, `settings_presentation.*`: settings values, draft/Apply/Cancel handling, and display mapping.
- `tracking.h`, `tracking_udp.h`: tracking data and receiver.

## Native compatibility is more than addresses

`game_signatures.h` contains precomputed signature tables. `game_layout.h` resolves game targets. Finding a target is not proof that every field or call contract still matches the game.

`game_structs.h` describes the layouts used by the mod. `game_contracts.h` contains precomputed owner/field evidence; `native_contracts.h` validates those contracts. `native_build.h` supplies the supported build identity. Native widget event semantics are separate again: `native_widget_contract.h` distinguishes checkbox selected-state handling from slider callback handling rather than treating all widgets identically.

Signature and contract tables are checked in alongside their authoritative specifications in `tools/targets.json` and `tools/native_contracts.json`. `python tools/check_generated.py` verifies source/header consistency using only the Python standard library; CI runs it before compiling. The optional native generators additionally require the reviewed executable, pefile and capstone, and are not needed for a normal build. See `tools/README.md`. Updating compatibility requires fresh native review, not simply changing the executable hash or accepting a successful signature match.

Core field witnesses validate the executable file; loaded entry/dispatch checks are separate. The optional capabilities documented below additionally compare their critical loaded instruction windows. These checks do not establish that every required function interior in memory is unchanged by another component.

## Camera query performance

The shared mesh queue checks the submitted pointer before validating a possible head drawable's back-pointer and name. The live root and head reads remain guarded on every eligible call; no presentation-long head cache delays replacement detection.

`walkSceneVisit` offers a synchronous visitor after each current node's full-header readability check. Self-pick masking uses that validation for its narrower flags access, falling back to the original narrow check for partial leaves. The visitor can compact only consumed queue entries; it calls no native code. Ordinary scene discovery uses the same walker without a visitor. Pick flags remain query-scoped and finally-restored, with pre-existing skip bits preserved.

The near-plane solver may cap proved-clear results at twice the caller's maximum near plane. Its initial segment ends at an endpoint of the original six-step bisection tree, so even a quantized result beyond it already saturates the filter. A hit uses the unchanged six-step search; unknown queries retain the original fallback. Props are still queried unless level clearance already forces the minimum plane. Every temporal sample retains current geometry queries and its own physical head pose, camera transform and filter evaluation. Held placement passes its first predicate result into clearance refinement and still checks final clearance before accepting an output.

Run `python -B tests/run_performance.py` for native regression suites. They compile production owners with owned-memory/native-boundary fixtures and compare against the pinned pre-change oracle in `tests/original_performance.inl`. The retained integration fixtures cover temporal sample order, extra requests, source faults, replacement, TP obstruction ownership and native view boundaries. They do not launch or attach to the game. See `DECISIONS.md` for the reason cross-frame and cross-sample caches are deliberately absent.

## Screen-effect quality diagnostics

`first-person-status.json` reports the screen-effect input and intermediate target dimensions for ambient occlusion, reflections, and depth of field, plus `view_effects_low_tier`. These targets match the full view dimensions; the low-tier flag remains false because screen effects no longer reduce resolution under load. Nonzero dimensions describe the most recently supported rendered view, and are cleared when the effects skip or target creation fails. Use these values with the per-stage CPU/GPU timings to optimize shader and memory work without reducing output resolution.

## Settings boundaries

The native UI edits a draft. Apply publishes accepted values and queues per-row revisions under a short settings lock; Cancel discards draft edits. Only the session worker persists INI values, outside that lock, then acknowledges the exact revision written. Edits arriving during a write remain pending. Failed writes remain pending: running and cleanly stopped sessions retry on the same worker; a fault exit makes a final best-effort pass after its notice/recovery work ends. Native GUI callbacks never wait for disk I/O. Status files are best-effort snapshots and may remain stale if publication fails. The presentation layer maps internal values to labels without making the native controller Controls table into an ExCamera page. Preferences are external to the source and runtime package.

Explicit leaning has no runtime input or pose path. Its four hidden legacy INI preferences, existing bulk-policy invalidation, and false/zero lean status fields remain for compatibility. The pose-cadence diagnostic (`held.physics_step`) also remains; it is not native release/throw timing. Active exploration spine-follow is separate and retains its native pose borrowing and restoration.

ExCamera-owned checkboxes adapt the native scrolling-coordinate round trip for preview and release events. The game still performs hit testing, capture, checked-state changes, and action dispatch. This is not a global GUI patch; vanilla descriptors retain their native handlers. Handler dispatch lifetime is guarded separately from action callbacks so a native activation can still update the draft.

## Thread and input boundaries

Diagnostic names are copied under a short leaf lock, drag status uses atomic flags, and camera-owned diagnostic values are copied under the camera lock before JSON/file I/O. Tracking transport failures retire the current sample and exit the receiver so its lifecycle owner can restart after backoff.

Guard priority suppresses attack only during the overlap. Releasing guard admits a still-held native attack without clearing unrelated ownership masks or inventing a queued attack. Native timing and recovery still decide when an attack can execute.

Window callbacks capture their forwarding procedure and installation generation. Native destruction is forwarded before retiring that exact installation, so a recycled HWND does not inherit the old registration. Raw-input retirement retries fresh ownership observations for a bounded interval; unknown or foreign registrations are not removed. This does not make Win32 procedure replacement atomic against arbitrary concurrent subclassers.

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

Compatibility checks and DLL export checks catch specific integration failures; they do not prove camera, input, or rendering behavior in a running game. The prof8 source passed a Windows/MSVC build, native production fixtures and local real-OpenGL checks before import; in-game verification remains outstanding. The optional generator tests under `tools/` are not required to build. Gameplay regression harnesses are kept outside the runtime source and are not part of the release ZIP.
