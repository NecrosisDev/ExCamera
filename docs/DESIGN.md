# Design rules

The rules ExCamera's behaviour is built on. Changes that break one of these need a discussion in an
issue first, not just a pull request.

## The view

- **First person is your head.** The mouse turns your character's head and body; the camera simply
  sits at the eyes and shows exactly what the head does, roll included. The camera has no input or
  smoothing of its own, so a fast turn never shows your own head.
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
  mod. F8 frees the cursor and opens the settings window. F9/F10/F11 change or reset the field of view. Tab stays
  the game's.
- Out of combat, right mouse frees the cursor (hold by default; toggle or off in Settings).
- No other new keys. Optional bindings ship unassigned, and the mod never chooses a key for you.

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
- The mod restores the game's camera and removes its hooks when it stops or hits a fault, and says so.
- The mod runs on any game build where every game function it uses is found by its code signature. If
  another mod already hooks one of those functions, the player decides (`[Compat] OtherHooks`: ask,
  allow, refuse); it is never done silently. On an untested build, or on top of another mod, its per-tick
  hooks must be seen running during play, or it stops and restores the game's camera.
