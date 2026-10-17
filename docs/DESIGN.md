# Design rules

The rules ExCamera's behaviour is built on. Changes that break one of these need a discussion in an
issue first, not just a pull request.

## The view

- **First person is your head.** The mouse turns your character's head and body; the camera simply
  sits at the eyes (or, with Eye position, that far straight back inside the head) and shows exactly what
  the head does, roll included. The camera has no input or smoothing of its own, so a fast turn never
  shows your own head.
- **Steadiness comes from the head.** Head stabilisation calms the neck itself (and pulls back large
  head roll); the camera keeps following the head exactly.
- **Third person is a shoulder camera on the same aim.** It follows from behind and over a shoulder,
  never goes through walls, hides people and props that get in the way, and is the only view with
  camera smoothing. Switching back to first person is an instant cut.
- **The overhead view is the game's own**, unchanged.
- **The mod never takes first person away from you** except for a real technical fault. Dying, the
  title screen and loading just pause it; it comes back by itself.

## Controls

- F6 cycles first person, third person and the game's overhead view. F7 (hold one second) stops the
  mod. F8 frees the cursor and opens the settings window. F9/F10/F11 change or reset the field of view.
  Tab stays the game's.
- Out of combat, right mouse frees the cursor (hold by default; toggle or off in Settings).
- No other new keys. Optional bindings ship unassigned, and the mod never chooses a key for you.

Close views give ordinary gameplay their own input policy. Overhead camera rotation and mouse-driven
locomotion cannot also consume a gesture owned by that policy. Native UI, interaction and held-object
actions retain their own handoffs. Automatic defense remains the default; optional Manual defense uses
RMB to request the game's defensive response only when the complete native capability is admitted.

An input hold belongs to its physical press. Changing perspective, character, bindings or input ownership
must not turn an already-held button into a new attack, guard or optional hold. Releasing a blocked hold
does not replay it. A window-message release owed for a consumed press survives a gameplay handoff until
the release arrives or that window-hook session ends.

## What you see

- **The crosshair is the game's own.** The mod only moves where it is drawn. Out of combat it shows
  only while right mouse frees the cursor (or always, if you choose); in combat it always shows.
- **Attack wind-up arc:** a thin arc beside the combat crosshair fills as a swing winds up, on the side
  it comes from, then fades. Nothing shows while idle.
- **Your helmet shows by default**, seen from inside; anything that would cover the middle of your view
  is hidden automatically. Hiding head gear is a setting.
- **Ceilings are off** until they are ready.

## Safety

- Nothing is installed into the game folder and no game file is changed. The one exception is loading
  through EMTK, where the mod sits in the game's own `mods\<id>\` folder.
- Startup requires the required game targets and reviewed native field contracts to match. A failed
  optional Manual-defense contract withholds that whole capability while retaining the admitted core.
  Instruction matches establish compatibility with those contracts; they do not prove every behavior of
  a new game build. See [Native field contracts](HOOKS.md#native-field-contracts).
- If another mod already hooks a required game function, the player decides (`[Compat] OtherHooks`:
  ask, allow, refuse). On an untested build, or on top of another mod, required per-tick hooks must be seen
  running during play; a missing hook requests a stop.
- Stopping releases input and requests camera restoration and resource retirement before removing
  callbacks. A bounded wait that cannot establish completion reports a pending or uncertain result and
  retains the callbacks and trampolines needed for safe continuation. A clean stop can be re-armed; a
  retained fault requires restarting the game.

## Ownership and lifetime

`InputOwnership` keeps cross-thread event mailboxes separate from native polling history. Gameplay
release invalidates stale publications without erasing a key-up still owed by the hooked window.
Raw mouse input is effective only while a fresh registration snapshot still identifies our window and
flags; losing that ownership clears accumulated raw input and restores cursor-delta sampling.

The wind-up cue owns every GL handle it allocates, including partial construction. Same-context frames
reuse the arc program and buffers. Observed core WGL switches retire them while their context is still
current, and context deletion ends the old handle identity. The worker never borrows a game context to
delete resources. Unobserved extension paths may leave retirement pending; they are outside the core
WGL lifecycle observation described in [OpenGL cue lifetime](HOOKS.md#opengl-cue-lifetime).

The launcher reports success only after the worker acknowledges the current process, mapped build and
activation request as running. Loading the DLL or accepting an export call is not that acknowledgement.
First launch and launch after a clean F7 stop use the same protocol.
