# ExCamera

First-person and third-person cameras for **Exanima**, with the game's overhead view a keypress away. First person follows your character's head; movement, attacks and collisions still use Exanima's physics.

**Current development build: 0.1.0-beta3-input5 · Windows x64.** Automated tests and compatibility checks against the archived game executable pass. **This update has not been tested in game.** The earlier beta was play-tested on Exanima **0.9.5.2g** (Steam build **25803178**).

## Install

1. Download a ZIP from [Releases](https://github.com/NecrosisDev/ExCamera/releases).
2. Extract it to a writable folder, such as Documents or Downloads. Keep `ExanimaFirstPerson.dll` beside `ExCamera.exe`.
3. Run `ExCamera.exe`. It starts Exanima through Steam, or connects to the game if it is already running. No game files are changed.

Release ZIPs may be older than this source; see [Building](#building) for the development build. Close Exanima before replacing an older build. Your settings carry over. ExCamera runs on its own; it does not require ExaniMate or EMTK.

## Controls

These controls apply in first and third person. Overhead keeps the game's controls.

| Control | Action |
|---|---|
| Mouse | Aim and look |
| Your movement keys | Move relative to the view, including strafing outside combat |
| Left mouse | Attack using the game's timing and movement |
| F6 | Cycle first person → third person → overhead |
| F7, held for one second | Switch ExCamera off; F6 turns it back on |
| F8 | Open settings and free the cursor |
| Right mouse, outside combat | Hold to free the cursor; configurable as hold, toggle or off |
| Right mouse, with Manual defense | Hold to guard in ordinary combat |
| Right mouse, while holding an object | Hold and move the mouse to rotate the object |
| Mouse wheel, while holding an object | Push farther or pull nearer; release while pushing to throw |
| Alt + mouse | Lean left, right, forward or back; release to straighten |
| Your Duck key (normally X) | Crouch outside combat; toggle by default |
| F9 / F10 / F11 | Decrease / increase / reset base field of view to 75°; third person can override it in F8 |

Run, alternate attacks, combat stance and interactions keep your Exanima bindings. Ordinary right-mouse cursor walking is disabled in both close views. Inventory, dialogue and other supported game windows free the cursor and suspend gameplay input by default.

See [Controls](docs/CONTROLS.md) for aim, movement and button combinations.

## Aim and defense

Each perspective has its own **aim-box width and height** in F8. Move within the box to aim; reach its edge to turn the view. Set either dimension to zero for direct turning on that axis. Third person also has a free-aim switch.

Choose defense separately for each view under **F8 → Crosshair and combat**:

- **Automatic** is the default and preserves the game's automatic defense.
- **Manual** enables the game's defensive assistance while you hold right mouse. You can still aim and move; equipment, reach and recovery determine whether a parry succeeds. Guard takes priority over an overlapping attack—release left mouse before starting a new attack.

Interactions and held-object rotation keep priority. If Manual is unavailable on your game build, F8 shows **Auto (fallback)** and keeps your saved preference.

## Settings

F8 includes field of view, head steadiness, sensitivity, crosshair and swing cues, crouch, lean, grab distance, and third-person framing. Changes apply immediately. Each page has its own reset button.

Lean can use Alt + mouse or separate left/right keys; the latter start unbound. Held-object rotation and lean bindings are configurable.

Settings are saved to `Documents\ExCamera\first-person.ini`. Older `Documents\ExaniMate` settings are copied on first use. F8 reports a save failure without discarding the live change.

## Compatibility and troubleshooting

- Game updates are checked at startup. Missing required support prevents activation; passing these checks does not establish gameplay compatibility.
- If another mod changes the same game functions, the launcher asks before running together. Set `[Compat] OtherHooks=ask`, `allow` or `refuse` in the settings file to choose a policy.
- If raw mouse input becomes unavailable, ExCamera falls back to ordinary mouse input.
- Restart Exanima when switching builds or when the launcher reports a restart is required.
- Controller movement is not fully adapted. The current build's gameplay feel and both EMTK loading routes still need in-game testing. Native carrying and movement restrictions remain.

For EMTK setup and uninstall instructions, see the [packaged readme](packaging/README.txt).

## Building

Requires Windows, Python 3, and Visual Studio 2022 or its Build Tools with x64 C++ support. Use MSVC for the mod's Windows memory-fault handling.

```powershell
python build.py            # Build both binaries and run tests
python build.py --package  # Also package a ZIP
```

See [Contributing](CONTRIBUTING.md) for development guidance.

[Changelog](CHANGELOG.md) · [License](LICENSE) · [Support development](https://www.patreon.com/cw/NecrosisDev)

ExCamera is an independent mod, unaffiliated with Bare Mettle Entertainment. It includes no game files. MinHook is included under its own BSD-2 license.
