ExCamera 0.1.0-beta3 - first person for Exanima

First person and third person for Exanima, plus the game's own overhead view, all
on F6. The camera is your character's head: what you see is what the head sees.
Settings are on F8.

Tested on Exanima 0.9.5.2g (Steam build 25803178), Windows x64. Other versions
work if every game function the mod needs is still found; you then get a short
"untested Exanima version" notice. If something is missing, the mod says so and
changes nothing.


INSTALL

Unzip to a folder you can write to (Desktop, Documents, Downloads; not Program
Files, not the game folder). Run ExCamera.exe. It starts Exanima through Steam,
or attaches to it if it is already running. No character needs to be loaded
first. No game files are changed.

Keep ExanimaFirstPerson.dll next to ExCamera.exe.

ExCamera.exe isn't signed and it loads a DLL into the game, so Windows SmartScreen
or your antivirus may warn about it. That's expected ("More info" -> "Run anyway"
for SmartScreen).

Using EMTK?
- Current EMTK releases: start the game through EMTK as usual, then run
  ExCamera.exe. Your EMTK mods and first person run side by side. Not tested in
  game yet.
- EMTK's legacy "rust-plugins" build (the only one that loads DLL mods): copy the
  mods\exanima-first-person folder from this zip into <Exanima>\mods\. EMTK then
  loads it when the game starts. Switching it off in EMTK works like holding F7,
  and switching it back on works like F6. Not tested in game yet. Don't also run
  ExCamera.exe with this route.

This mod works alongside EMTK. It isn't part of it.

Other mods hooking the same game functions: ExCamera.exe tells you which, and
asks whether to start on top of them. If you always want one answer, set
OtherHooks=allow (or OtherHooks=refuse) under [Compat] in first-person.ini.
Started by a mod loader instead, it only starts on top of another mod with
OtherHooks=allow.


CONTROLS

Mouse          look
F6             first person -> third person -> overhead
F7             hold one second to switch the mod off (F6 turns it back on)
F8             cursor and the Settings window
Right mouse    hold to free the cursor out of combat (setting: hold, toggle, off)
Mouse wheel    while holding an object: nearer / farther
Duck key (X)   out of combat, crouch (toggle by default; stays down under low
               ceilings)
F9 / F10       field of view down / up
F11            field of view back to 75

Everything else is the game's own: movement, run, combat stance, swings, grabbing,
doors. In combat a thin arc by the crosshair shows a swing winding up.

Inventory, containers, item descriptions, loot, dialogue, the arena merchant and
the end-of-match menu free the cursor and hold the view while they are open.


SETTINGS

Everything is on F8: head steadiness, field of view, mouse, crosshair, the swing
arc, grab range, crouch, near clip and the third-person camera (distance,
shoulder, height, lag, combat framing, what to do with things in the way).
Changes apply at once.

They're saved in Documents\ExCamera\first-person.ini, so a new version keeps
them. Delete that file to go back to the defaults. Settings from versions
before the ExCamera name (Documents\ExaniMate) are copied over the first time.


UNINSTALL

Close the game. Delete this folder, and the mods\exanima-first-person folder if
you used the EMTK route. Delete Documents\ExCamera\first-person.ini if you don't
want to keep your settings.


KNOWN ISSUES

- It's a beta.
- Looking up or down, the view drifts a few degrees sideways (left looking down,
  right looking up).
- Both EMTK routes are untested in game.
- Under the legacy EMTK route, pressing F6 while EMTK has the mod switched off
  turns it on and straight off again, and the "stopped" notice mentions F7 even
  when EMTK stopped it.
- Indoors, the third-person height is capped by the ceiling.
- No free camera in this build.
- Editing tools (items, levels, NPCs) aren't included.

Files: ExCamera.exe, ExanimaFirstPerson.dll, first-person.ini (defaults), this
readme, LICENSE.txt (MIT), LICENSE-MinHook.txt, mods\exanima-first-person (EMTK legacy route) and
SHA256SUMS.txt.
