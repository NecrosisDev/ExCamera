# Changelog

v0.2.3 (revision 58)

- Keep ExCamera's native keyboard Controls rows reachable by preserving scrolling while the settings integration is active.
- Make the Camera option's effect explicit: "Hide helmet and hair" toggles between hiding verified headwear and showing it when clear.
- Retain the original native Controls scroll setting when ExCamera's settings integration retires.
- Hide verified merged headwear from face and skin-weight ownership data alone; missing stable-display attributes no longer cause the native draw to reveal it.

v0.2.2-prof10 (local headwear-hide candidate, rev58)

- Separate hide-mode face classification from the extra vertex data and GPU stream used only to stabilize visible headwear.
- Keep the native keyboard Controls pane scrollable while ExCamera rows are present, restoring the game's original scroll behavior when the custom settings view closes.
- Label the visibility option "Hide helmet and hair" so its purpose is clear in Camera settings.

v0.2.2 (revision 56)

- Reduce repeated mesh, picking, collision, inactive-wear and empty throw-queue work while preserving current ownership, obstruction and lifetime checks.
- Keep cue and headwear GPU resources warm across context switches, with explicit owner-context deletion and safe teardown.
- Fix first-person helmet, hair and face-covering visibility inside merged character meshes. Stabilize supported visible headwear against the displayed camera sample and render inside faces.
- Validate the actual submitted mesh instance, including shared materials, so player headwear handling does not affect other actors.
- Restore exploration Cursor Look through the game's existing Controls binding.
- Add opt-in hook profiling and render diagnostics; profiling remains off by default.
- Keep public test sources and CI test steps out of the repository. Compatibility-table, build/export and ZIP checks remain.
- Verified with MSVC, native contract checks and offline GPU fixtures. Actual equipment coverage, gameplay feel and FPS gains remain subject to in-game validation.

v0.2.2-prof8 (local headwear/profiling candidate, rev56)

- Bind headwear classification to the actual submitted mesh instance and material index, including shared material groups. Revalidate the current table and physical ownership before drawing.
- Use exact reviewed native callers, a consumed single-sample ticket and scoped multisample provenance; retire state on nesting, exceptions and colour-pass exit.
- Reject unrelated mesh submissions with bounded hints before native-memory queries. Add headwear_submit profiling spans and shared-instance/lifetime regressions.

v0.2.2-prof7 (local headwear/profiling candidate, rev55)

- Filter head-bound faces in the controlled actor's merged skin at its first-person colour draw, preserving body/neck faces sharing the material, shadows and other views.
- Render verified visible headwear from copied bind geometry attached to the exact displayed camera sample; include inside faces. Retain native attachment rays and add two-sided obstruction tests of the stable geometry. Physics and camera algorithms are unchanged.
- Keep the optional vertex stream warm in the existing exact-context lifetime owner, including guarded context deletion and stop drain. Add four profiler spans and render-path status counters.
- Add owned Windows regression and real hidden-window WGL pixel checks. These do not establish acceptance for a player's equipped item or an in-game FPS gain.

v0.2.2-prof6 (local handoff/profiling candidate, rev54)

- Merge Cursor Look restoration from the supplied d39eff0 source handoff through the existing native Controls binding. Retain exploration-only ownership, release/rebind/focus/scene retirement, rotation priority and accepted held offsets.
- Guard the new held-state lock through SEH, retain cheap idle polling and retire active holds before stop removes input owners. Add no binding or setting and preserve all prof5 performance/lifetime work.
- Add latest headwear scan state/walk/rejection/bound diagnostics without extra memory queries; show explicit Show when clear / Hide all choices with unchanged saved semantics.
- Headwear visibility, hair, jiggle and backface issues remain unresolved; no classification, culling, physics or camera shortcut is included. Add Windows production callback and native Settings fixtures; gameplay acceptance remains outstanding.

v0.2.2-prof5 (local profiling candidate, rev53)

- Skip the throw table lock/scan for ordinary-body integration when the published queue is empty. Publish all queue mutations under the existing lock; nonempty callbacks retain every current-ticket check.
- Keep integration depth, native forwarding/fault cancellation, destruction lifetime invalidation, settle timing and history writes unchanged.
- Add full prof4 throw-owner parity, empty operation counts, cancellation/fault and controlled cross-thread lifetime/publication regressions. Profiling stays off by default; in-game FPS and throw feel remain unverified.

v0.2.2-prof4 (local profiling candidate, rev52)

- Stop hot health-counter increments after the existing verdict; reopen before each fresh hook arm without changing the game-clock health window or failure policy.
- Gather status-only head geometry on demand at running-status cadence, on the existing admitted native camera callback. Keep current pointer/range/finite checks and all camera samples.
- Clear inactive wear publications before early return; avoid head validation and body-root queries when wear cannot act. Preserve every active-path and application-time ownership check.
- Add frozen-prof3 parity/lifecycle regressions and diagnostic-demand camera integration. Keep profiling off by default and prof3 intact for in-game comparison. Offline checks do not establish FPS or camera feel.

v0.2.2-prof3 (local profiling candidate, rev51)

- Keep complete per-context cue resources warm across ordinary WGL switches and NULL unbinds; refresh actual DC bindings and retain the bounded cache.
- Retire unbound warm contexts through verified caller-thread bindings before deletion, restoring the caller first. Guard shared objects, handle reuse, nesting, faults and foreign-thread ownership.
- Close cue admission and drain owners before removing lifecycle hooks; block re-arm and retain pending teardown when ownership or restoration cannot be established. The worker performs no GL operations.
- Add modeled lifecycle regressions and an optional isolated native-driver WGL fixture. Camera algorithms, collision checks, cue geometry/state restoration and opt-in profiling boundaries remain unchanged.

v0.2.2-prof2 (local profiling candidate, rev50)

- Reuse one fresh protection-region query within eligible mesh callbacks when it fully covers the current head; keep live identity checks and separate-region fallback.
- Forward foreign pose builds before player window/input work, retaining native profiling and outer in-flight cleanup.
- Upload cue geometry once instead of five times, preserving vertex streams, draw order, state checks and restoration.
- Copy and reset only active TP-prop publication data, retaining lock coverage and current ownership validation.
- Return from WGL wrappers after their finally cleanup. Cue resource lifetime and cold re-creation behavior are unchanged.
- Keep opt-in profiling off by default and the previous profiling builds intact for comparison. Offline regressions do not establish in-game FPS or camera feel.

v0.2.2-perf1 (local candidate)

- Reject unrelated mesh submissions before head drawable validation, retaining current root/head checks and immediate replacement handling.
- Fuse self-pick masking with scene discovery's current range checks; preserve membership, pre-existing flags, nested calls and fault restoration.
- Limit near-plane searches once the original quantized result would saturate the same filter; retain closer-hit refinement and per-sample geometry queries. Skip prop queries only when they cannot tighten the minimum plane.
- Remove a duplicate held-placement predicate while retaining final obstruction validation.
- Add native performance regressions against the pinned original solver and retain focused camera, picking and render-boundary suites. Measured reductions are offline evidence; in-game FPS and feel remain unverified.

v0.2.1

### Input and native Settings

- Separate native GUI cursor/press coordinates from world-aim coordinates, retaining native hit testing, capture and checkbox activation.
- Keep arena management windows' cursor ownership tied to their lifetime rather than hover position.
- Preserve the Settings draft across focus handoffs; improve key-binding repeat/release ownership and window-handle retirement.
- Keep controller Controls Defaults from resetting hidden ExCamera keyboard options. Refresh raw-input fallback and tracking status captions without rebuilding controls or disturbing a drag.
- Use a single asynchronous, revision-tracked preferences writer so an older write cannot acknowledge a newer edit. Failed writes remain pending while the writer is active.

### Combat and held objects

- Allow a still-held attack to resume through native handling after guard is released, without replaying completed taps.
- Preserve native variant selection inside the admitted ordinary attack-family starters while retaining Manual-defense permission. Native motion, timing and equipment remain authoritative; this does not promise identical automatic-defense history.
- Match the native attack-side angle boundary without changing the camera's angle wrapping.
- Apply supported outward-wheel throw intent after native release settling, before the eligible native integration step. Preserve existing motion and native damping, gravity and collision handling; cancel stale intent on re-grab, destruction, owner changes or expiry.
- Improve held-object recovery and proportional door/hinge manipulation.
- Restrict inventory preview cancellation to an unchanged, completed ordinary transaction with a positively observed placement failure. Merchant/special and replaced transactions remain native.

### Camera and rendering

- Start collision probes at the eye and exclude the controlled character before nearest-hit selection, restoring borrowed camera state afterward. Existing camera controls and tuning are retained.
- Validate exact head, headwear, bodywear and third-person obstruction ownership before applying visibility masks. Head visibility is independent of the helmet option; changing helmet visibility invalidates stale probe results.
- Scope cover suppression and close-view rendering adjustments to the current native presentation, rather than a stale prior frame or unrelated viewport.
- Isolate the optional combat indicator's draw-buffer and viewport state, including fractional viewport coordinates and indexed blend/mask state. Unsupported or ambiguous contexts omit the cue instead of applying unsafe broadcast changes.

### Reliability and cleanup

- Strengthen optional native instruction/metadata checks and keep attack/throw capabilities independently admissible with explicit shared physics dependencies.
- Retain hook-recovery-before-cancellation ordering and generation-aware raw-input/window retirement. Broken tracking receivers retire their sample and can restart after backoff.
- Report an unconfirmed launcher activation as potentially pending, not definitely rejected.
- Remove unused standalone settings UI headers and inactive explicit-lean runtime code; retain legacy preference values for compatibility.

### Build and releases

- Add standard-library generated-table checks; normal compilation needs no game executable or assets. Python 3.8+ is required for builds/checks; native generation requires Python 3.10+ and its optional dependencies.
- Build and validate in isolated staging, serialize publication, and restore previous outputs after handled publication failures. Publication is not crash-atomic across files.
- Keep checkout paths out of batch-file bytes and command text; preserve compiler flags while explicitly handling child environment paths and UTF-8 Visual Studio discovery.
- Derive release versions from validated version tags, mark prereleases appropriately, and generate commit-based release notes. Existing releases are not overwritten.
- Run standard-library tooling regressions in both workflows before compilation. Qualify tag references in remote identity checks, local changelog ranges and comparison links so same-named branches cannot redirect them.
- Keep runtime ZIPs to the launcher, DLL and two licenses. Tests, research files and game assets are not runtime dependencies.

### Verification and remaining limits

Source/portable regression checks, standard-library tooling tests, and checks against the supplied supported game executable passed. **A full Windows/MSVC build, GitHub-hosted workflow run and live-game verification have not been performed.**

Throwing is limited to reviewed generic owned root bodies without child bodies or external attachment constraints; unsupported or interrupted cases retain native dropping. Outdoor lighting/culling symptoms still need live reproduction; the scoped rendering changes do not establish a universal outdoor fix. Preference/status writes remain best-effort under filesystem failure. See [Architecture](docs/ARCHITECTURE.md) for ownership and compatibility boundaries.
