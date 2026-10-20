# Changelog

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
