# v0.3.0 in-game checklist

Test the exact `build/ExCamera-0.3.0-minimal.zip`, extracted to a new folder. Use a truly default ini: an empty `Documents\ExCamera\first-person.ini`. Deleting the file isn't enough, because the mod then copies an old ini from `Documents\ExaniMate` if one exists.

After each section, `first-person-status.json` should show `faults_seen: 0` and every fault counter or flag at zero: the top-level `*_faults` fields, nested counters and flags such as `supersampling.faults`, `openxr.*_faults`, `blade.faults`/`fault`, `finger_bones.faults`/`fault`, `motions.kick_fault`/`jump_fault`, and `skin_merge.fault`. A nonzero value is a release blocker even when the fallback or recovery appears healthy.

## Results so far (2026-10-31)

Passed on the dev build of the same source (Arena001 unless noted), driven by key messages and read from the status file:

- Launcher starts the game through Steam (release ZIP) and exits cleanly.
- The release ZIP's launcher and DLL are byte-identical to the tested install in `build/test-0.3.0`. That install ran in Arena001 for 18,256 samples with zero faults and no `fault-log.txt` entry. This covers startup and one level load only; it does not cover the interaction checks below.
- Mod Settings tab opens with a default ini, no fault.
- F6 cycles views. F9, F10 and F11 change the FOV. Hold F7 stops, and a real F6 press restarts.
- F8 is ignored in the overhead view, and nothing opens later.
- Five fresh starts through level loads with zero faults: Arena001 three times, Arena004, and story save Exanima005.
- First-person camera feel: Hunter played it and approved.

- Far rooms: Hunter confirmed the arena hub's far doorway now shows its room from where it used to go black.

Fixed from testing: a false "did not finish starting" launcher error on slow title loads, far doorways showing black, and hair jitter (headwear now draws attached to the view; confirm by eye).

Known issue: World object detail draws thin, screen-horizontal dark lines across tables and counters, so it ships off by default. The cause isn't found. Hunter describes them as a copy of the mesh edge pulled toward the camera. They're one pixel thick, sometimes broken, and appear and vanish with viewing angle, which suggests a real gap: Hunter sees the desktop floating slightly above its base, so these pixels may look through it while still carrying the desk's mask (unconfirmed). They fade with the setting, so the composite shader draws them. Four changes didn't help: skipping depth steps, skipping the 2 m close-up switch, removing the mask replay's slope offset, and guarding the output against NaN.

Frame rate at idle, about 20 s each, one location: Arena001 default 58 fps twice, Arena001 with detail off plus draw distance off and 6 lights 49 fps, Arena004 default 45, story default 42. The defaults aren't slower than that near-vanilla setup, but this doesn't measure detail on its own or combat load.

Not tested, needs a person: everything below that takes the mouse or a fight, including Settings clicks, item placement, deaths and the 30-minute session. The tested install has only had the single Arena001 run above; its launcher and DLL match the ZIP byte for byte.

## Must pass before publishing

**Startup**
- [ ] With the game closed, run `ExCamera.exe`: the game starts and the "ready" message shows the keys.
- [ ] With the game running, run `ExCamera.exe`: it attaches.

**Settings (fresh ini)**
- [ ] Mod Settings opens and every category shows: Camera, Graphics, Combat, Controls.
- [ ] Third-person FOV shows "Same" at its default; the slider moves and returns to it.
- [ ] Blade draw gain reaches 400 %.
- [ ] Apply, Cancel and Defaults work. Finger grip Off, then Defaults, restores "Everyone".
- [ ] While binding a key, a left click doesn't bind it.
- [ ] Rotate, Kick, Jump and Recenter refuse a key another of them already uses; Escape cancels.
- [ ] With no other mods, Mod Settings shows no "By category" switch.
- [ ] Wound marks toggle enables and disables procedural cuts, punctures, bruises, and running blood.
- [ ] Head tracking shows its live status while enabled.
- [ ] Head tracking shows under Controls and can be switched off.
- [ ] Settings survive a game restart.

**Aim**
- [x] First-person aim feels good at normal frame rate (Hunter, 2026-10-31).
- [ ] Draw far rooms: check frame rate in a busy room, and that nothing pops in while walking between rooms. Standard build only.
- [ ] Hair and headwear don't jitter in first person, including looking down and while running.
- [ ] Sideways flicks feel the same at a low frame rate (cap it, or use heavy settings).
- [ ] Resizing the window or changing fullscreen keeps the aim, with no jump.

**Item placement (changed code, highest risk)**
- [ ] Drag an item from inventory to the floor while looking straight down.
- [ ] Drag an item out with your back to a wall, then facing a wall from up close.
- [ ] The item never appears at the world origin, past a wall or under the floor.
- [ ] If a drop has no clear spot, the item stays in your inventory.
- [ ] Hold and place objects in the world still works, in first and third person.
- [ ] `inventory_placement_failures` stays low in the status file.

**Crash watch**
- [ ] Enter and leave levels at least five times, and die and respawn. The tested install's Arena001 run logged no crash. Earlier dev builds on 2026-10-31 logged two reports at game `0x1f4f76` and one at ExCamera `0x5aa07` while this work was in progress; the changelog's scene-move finger-bone fix landed before the release build. Any new `0xc0000005` in the exact ZIP's `fault-log.txt` blocks release.

**Core**
- [ ] F6 cycles all three views; hold F7 stops cleanly; F6 restarts.
- [ ] F8 does nothing in the overhead view or after F7, and nothing pops up later, including F8 pressed during a load. It still opens at the title screen.
- [ ] Alt-tab, inventory, shop, dialogue, arena windows, knockdown and death behave normally.
- [ ] A 30-minute normal play session has no faults.

## Decide before publishing

- **Frame rate.** See the idle numbers above. Check a busy fight with the defaults before publishing.
- **View effects.** Ambient occlusion, reflections and depth of field are off by default and still need a look in game.
