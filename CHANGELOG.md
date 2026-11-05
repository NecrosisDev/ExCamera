# Changelog

Unreleased

- Jump is a real hop now: the upward push lasts several physics steps (up to about 20 cm of rise) instead of one, which the game's balance took straight back, and it no longer plays the game's jump clip, so the next jump is ready after under a second instead of about two. Kick blends in more slowly and needs both feet planted, to stop it toppling you; it plays motion 0xD through the game's own motion player.
- A Kick or Jump key press that does nothing now says why in a short message (at most every 3 seconds): stand still in the fighting stance with most of your stamina for Kick, stand still with half your stamina for Jump, not while the pointer is free, or that the key belongs to the game's own Controls. first-person-status.json counts each reason (kick_refused, jump_refused).
- Alt + left mouse stabs again when Kick or Jump was bound to Alt. The game uses Alt for its thrust, and those keys took it away from the game. Kick, Jump and Swap shoulder now leave any key the game's Controls use to the game, and Settings refuses such a key for them. If you had Kick on Alt, choose another key for it.
- After an arena fight you can look around again while the result window (Next Fight) is up, so you can walk over and loot your opponent. To click Next Fight or the other result buttons, press Cursor Look (the game's binding, Space by default) to free the pointer, and press it again to look around.
- Portable copy: put an empty portable.txt beside ExCamera.exe and the DLL, and that copy keeps its settings in first-person.ini beside it instead of Documents\ExCamera (for testing without touching your own settings). Safe start in such a copy backs up and resets that file.
- Safe start: hold Shift while starting ExCamera.exe (or start it with `--safe`) to play with default settings. Your current settings are copied to `first-person.backup-<date>-<time>.ini` beside them first. If the last session ran into an error, the launcher offers the same, with a note that it may not help.
- F6 can switch between first and third person only: set Settings > Camera > F6 cycle to "1st/3rd only". Then a tap flips the two views, and holding F6 for half a second shows the game's own camera. The default still steps through all three.
- New Hit reaction slider (Settings > Camera > First person): how hard the first-person view jolts when you are hit, from 0 (still) to 100 % (as before). Hits now also tilt the view a little, as they were meant to. A VR headset view never jolts.
- New Swap shoulder key (Settings > Controls > Keys, unbound by default): in third person it slides the camera over your other shoulder and remembers the side. Changing the Shoulder setting's side slides too, instead of jumping. It can't share a key with Rotate, Kick, Jump or Recenter, and a hand-edited ini that binds one key to two of these keeps only the first.
- A greyed-out setting now says why in its help line: "Not available on this game version." when this Exanima build lacks what it needs (for example far rooms or finger poses on Hellmode, screen effects or manual defense where the game code differs), or "Turned off after an error. Restart the game to retry." when the feature stopped itself after a fault.
- Third person follows your look angle out of combat: looking down raises the camera and moves it back a little so you see the ground ahead, looking up brings it closer and lower. It eases in and out and walls still pull the camera in. Turn it off with Third person: follow look angle.
- Knocked down in first person, the view now switches to third person so you can see where you fell, and back to first person a moment after you are up. Pressing F6 meanwhile keeps your choice. Dying never switches to third person, and if a knockdown had switched you, dying, the next round or a new scene puts you back in first person. Settings > Camera > Switching views > Knocked down can keep you in first person instead.

v0.3.0 (revision 60)

- Fixed level loading hitches and stuttering: eliminated synchronous shader double-compilation in glShaderSource, gated sector traversals and roster scans during level population, precompiled wound/fire/bake shaders, paused worker disk logging while loading, and generated procedural stone textures asynchronously on a background worker thread.
- Removed the mod's Screen-space reflections: the game already draws its own reflections from each surface's specular data, and ours added a second, guessed one on top. An old `View/ScreenSpaceReflections` value is ignored.
- Held-item detail takes each pixel's material from the game's own item textures (the procedural material of every layer the game composes), no longer from colour. Armour, object and level detail stay off until they are rebuilt the same way.
- Armour and clothing detail is back on the same data: worn pieces take each pixel's material from the game's own composed textures. Object and level detail stay off.
- Contact shadows now darken evenly instead of fading out on bright pixels; they are a visual addition on top of the game's own occlusion.
- Fixed procedural wounds and impact feedback: hooked damage and physical impact detours to enable procedural cuts, blunt bruises, bloody weapon drips, directional camera impact kicks, and damage flash vignettes.
- Fixed finger posing and sector transfers: activated dynamic stack node movement hook to prevent native stack overflow crashes and enable full finger bone physics drive.
- Added live telemetry toast and wound debug mode (F12) to inspect real-time active wounds, drips, blade blood saturation, and finger poses with vibrant neon highlights.
- Enabled native in-game Manual First Person page hook on the main menu.
- Added native in-game Manual page: "First Person" guide page dynamically injected into the main menu MANUAL tab, complete with rich-text formatting, keybindings, combat guidance, and object manipulation tips without modifying on-disk archives.
- Fixed character movement and turning jitter/rubberbanding by propagating head stabilization deltas into the physical history buffer so integration velocities remain continuous.
- Fixed visual artifacts and ghosting/black rings in perspective post-processing effects (ambient occlusion and contact shadows) at full native resolution with all graphics settings enabled.
- Fixed container and inventory item drops to the ground: items dragged out of open containers or bags can now be dropped directly onto the floor or level surfaces freely.
- Polished 3D object manipulation: holding RMB while dragging an item rotates it freely along pitch and yaw in true 3D space.
- Fresh installs no longer lose the Mod Settings tab. The default third-person FOV now says "Same", which fits its slider; any other value too long for it is just shortened.
- Dropping an item from your inventory next to a wall tries a closer spot on the same line, instead of cancelling. If there's no clear spot, the item stays in your inventory.
- Items dropped while looking down no longer fail because of the height check, but only when your view starts above the drop height.
- Aim oval: a mouse move splits the same way on a slow frame as on fast ones, and resizing the window keeps your aim instead of recentring it.
- Clicking away while binding a key no longer binds the left mouse button.
- Rotate, Kick, Jump and Recenter can no longer share a key. F8 in the overhead view no longer opens Settings later. The By category switch only shows with other mods installed. `fault-log.txt` rolls over at 256 KB.
- Head tracking moved to Controls, so it can be switched off without a VR build. Blade draw gain reaches its full 400 %. Defaults restores finger poses for other characters too.
- Removed the Lean key row, which did nothing. Wound marks (procedural cuts, punctures, bruises, and running blood) are fully integrated natively into ExCamera.
- Fixed "Task fatal error" after arena rounds: moving a character between scenes overflowed a fixed list in the game once finger bones were on. Finger poses need the standard game build for this fix and stay off on others.
- World object detail is off by default, because it draws thin dark lines across furniture. Turn it on under Surface detail if you don't mind them.
- Far rooms: the game only draws rooms next to the one you're in, so a far doorway could show solid black in first person. One more room now shows through doorways. The Draw far rooms setting switches it off. Standard build only; Hellmode keeps the game's rule.
- Hair and headwear weighted partly to the neck no longer jitter in first person: worn pieces stay attached to the view, and hair merged into the head is hidden with it.
- A slow title load no longer shows a false "did not finish starting" error; the launcher now waits as long as the mod does.
- A short in-game message confirms the mod started and shows its keys. The launcher's errors are in plain language, and an unsupported game version says to look for a new release.
- The ZIP now includes a README.txt. The per-spawn debug log is dev-only.
- ExCamera operates completely standalone with its own integrated native settings and procedural damage passes; ExanimaLua is not required.

- If the Settings integration refuses a changed native owner, restore the game's original Settings table when ownership is still proven; leave unknown replacement tables untouched.
- If the displayed-pointer sample becomes stale during a native click, fall back to the game's own pick coordinates instead of consuming the pickup. Record how often that fallback occurs.
- Dying no longer freezes the game. A hook that faulted while reading your freshly dead character used to keep its lock forever; now it lets go.
- Blood and decals stop flickering in first person with supersampling on. The depth correction was shrinking their offset to about two depth steps; it now keeps at least sixteen.
- Depth of field and ambient occlusion, drawn before the UI. With supersampling they run once on the averaged image. Admitted only when every hooked function matches byte for byte, so Hellmode qualifies too.
- New F8 rows, all off by default: edge smoothing, helmet view, a see-through ghost when something blocks the third-person camera, and a hurt/winded overlay read from health and stamina.
- OpenXR VR, only in `build.py --openxr-sdk` builds: per-eye AO behind a "VR: effects" row, a stab modifier on the left trigger, a mirror mode, a comfort vignette (off by default), and rows for grip angle, haptics strength, panel width and panel distance. Half-resolution AO/SSR now hands back the framebuffer it found, which the headset eyes needed. Normal builds don't contain it and play exactly as before.
- Settings gets a VR tab with its own Defaults, CONTROLS is just Controls now, and the window is wider. VR also has a hands-only body mode, and a "VR: main menu" row that shows the title level in stereo with the menu on a panel standing in it.
- Hellmode is a recognised build now, so it skips the "untested version" banner. Banners that do show wrap instead of getting cut off.
- Third-person camera stops pumping in and out when a pillar or someone walks past: after a pull-in it waits a quarter second before easing back, and eases back slower the higher Camera lag is set. Its follow point also rides vertical head bob more softly.
- Fewer pointless memory checks every frame and every mouse message. Same results, less asking Windows if memory is still there.
- The settings panel reads the game's GUI size from `hm_saves` on Hellmode installs instead of assuming AppData.
- `build.py --dev`: dev-only build with a readiness file for automation and a save-slot loader driven by `dev-load.ini`. Never packaged; the loader hasn't loaded a slot in game yet.
- Casts and comments tidied; camera math untouched.
- VR: recentering and the wrist menu no longer write `first-person.ini` from the render thread. The session worker saves them, so the headset doesn't drop a frame on a recenter and a wrist-menu change can't overwrite a settings save made at the same moment.
- VR: a damaged `RecenterOrigin` line (nan, inf or a zero rotation) is ignored instead of handed to the headset runtime.
- Checked with MSVC, the generated tables, and offline admission checks on both EXEs. See [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md) for the release build's observed status. VR, the effects and the decal fix still need eyes in game. FPS gains unmeasured.

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
