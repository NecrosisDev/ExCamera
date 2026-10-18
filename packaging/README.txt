ExCamera 0.1.0-beta3-input16
First-person and third-person cameras for Exanima

Windows x64. Targets Exanima 0.9.5.2g (Steam build 25803178).

INSTALL

Extract this ZIP to a writable folder such as Documents or Downloads. Keep
ExanimaFirstPerson.dll beside ExCamera.exe, then run ExCamera.exe. It starts
Exanima through Steam or connects to an already running game. No game files
are changed. Close Exanima before replacing an older build.

The launcher is unsigned, so Windows may show a SmartScreen prompt.


CONTROLS

In first and third person:

Mouse             Aim and look
Movement keys     Move relative to the view; strafe outside combat too
Left mouse        Exploration: click to interact; hold and drag objects or
                  doors; double-click containers within reach
Left mouse        Combat: attack using the game's timing
F6                First person -> third person -> overhead
F7 (hold 1 sec)   Switch ExCamera off; F6 turns it back on
F8                Settings and free cursor
Right mouse       Outside combat: hold to free the cursor
Right mouse       Manual defense: hold to guard in ordinary combat
Right mouse       While holding an object: hold and move mouse to rotate
Mouse wheel       Held object: push farther/pull nearer; release while
                  pushing to throw
Alt + mouse       Lean; release to straighten
Duck key (X)      Crouch outside combat; toggle by default
F9 / F10 / F11    Base field of view down / up / reset to 75 degrees

Movement, run, Duck, alternate attack, combat stance and interactions use your
Exanima bindings. Overhead keeps the game's controls. Ordinary right-mouse
cursor walking is disabled in first and third person.

Inventory, dialogue and other supported game windows free the cursor and
suspend gameplay input by default.


AIM AND DEFENSE

F8 has separate aim-box width/height settings for each perspective. Mouse
movement inside the box aims; reaching its edge turns the view. Set an axis
to zero for direct turning. These settings apply in exploration and combat.
Third person: aiming mode offers Aim box or Screen centre. Screen centre
turns directly and disables the third-person aim-box size controls.

Each view also has Swing selection: Mouse direction (default) or Native facing.
Mouse direction chooses the swing's starting side from left/right mouse
movement before you click, including with small boxes or Screen centre.
Native facing lets Exanima choose from the angle between aim and body;
small aim boxes can limit side changes.

F8 -> Aim & combat lets you choose defense separately for each view:

- Automatic (default): the game's automatic defense.
- Manual (hold RMB): hold right mouse to ready the weapon and enable
  attack-specific native interception after recovery. Aim
  and movement remain available. Equipment, reach and recovery still decide
  whether a parry succeeds. Guard wins over an overlapping attack; release
  left mouse before starting another attack.

Interactions and held-object rotation keep priority. If Manual is unavailable,
F8 reports Manual as unavailable and keeps your saved preference while
Automatic remains effective.


SETTINGS

F8 has three pages: Camera (First person, Third person, Head and body),
Controls (mouse, keys and interaction), and Aim & combat.

F8 controls field of view, head steadiness, sensitivity, crosshair and swing
cues, crouch, lean, grab distance and third-person framing. Changes apply
immediately. Scroll for more options; Reset page stays at the bottom.
Hover an inactive control to see which setting enables it. You can
configure either perspective without switching views.

Right-mouse free cursor can use hold, toggle or off. Lean can use a modifier
plus mouse or separate left/right keys, which start unbound. Lean and held-
object rotation bindings are configurable. In combat, the game's alternate-
attack binding takes priority over lean: Alt retains alternate attacks
with the default bindings. Third person can override the base field of
view with its own setting.

Settings: Documents\ExCamera\first-person.ini
They survive updates. Older Documents\ExaniMate settings are copied on first
use. F8 reports a save failure, keeps the live change and retries automatically.
Direct INI edits are read when the game next starts; F7/F6 does not reload them.


COMPATIBILITY

Required compatibility checks must pass before ExCamera starts. Passing them
does not guarantee gameplay compatibility. If another mod changes the same
game functions, the launcher asks before running together. Set OtherHooks to
ask, allow or refuse under [Compat] in first-person.ini to choose a policy.

If raw mouse input becomes unavailable, ordinary mouse input takes over.
Restart Exanima when switching builds or when a restart is requested.

Controller movement uses view yaw; a complete controller controlset is not implemented. EMTK loading routes still need
in-game testing. Native carrying and movement restrictions remain.

EMTK is optional. Choose one route:
- Launcher: start the game through EMTK, then run ExCamera.exe.
- Legacy native-DLL loader only: copy mods\exanima-first-person from this ZIP
  into Exanima's mods folder. Do not also run ExCamera.exe. If EMTK disables
  the mod, use EMTK's switch to enable it again.


UNINSTALL

Close Exanima and delete the extracted folder. If you used the legacy EMTK
route, also delete mods\exanima-first-person from the game folder. Delete
Documents\ExCamera\first-person.ini only if you want to remove your settings.

Updates and source: https://github.com/NecrosisDev/ExCamera
License: MIT (LICENSE.txt); MinHook: BSD-2 (LICENSE-MinHook.txt).
ExCamera is unaffiliated with Bare Mettle Entertainment and ships no game files.
