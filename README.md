# ExCamera

**First- and third-person cameras for Exanima.** See the fight through your character's eyes, or follow them from close behind, with the game's own camera one key away.

<!-- Screenshots go here: first person in combat, third person exploring, the Settings tab. -->

**[Download the latest release](https://github.com/NecrosisDev/ExCamera/releases/latest)** and pick `ExCamera-<version>-minimal.zip`.

## What it does

- **First person** with a soft aim oval: the mouse moves your aim, and the view follows as you push toward the edge.
- **Third person** that stays close, frames you for combat, and eases around pillars and passers-by instead of pumping in and out.
- **Manual guard:** hold right mouse in combat to angle your parry yourself.
- **Object handling:** pick things up, rotate them freely in 3D, and drop items from your inventory onto the floor.
- **Wounds and blood:** wound marks and running blood where blows do damage.
- **Graphics extras,** all optional: ambient occlusion, reflections, depth of field, edge smoothing, a helmet view and a hurt overlay.
- **Settings inside the game's own Settings window,** with Apply, Cancel and Defaults. No config files to edit.

## Install

1. Extract the ZIP anywhere and keep its files together.
2. Run `ExCamera.exe` each time you play. It starts Exanima through Steam, or attaches if the game is already running.
3. A short message in game confirms it started.

To remove it, delete the folder and `Documents\ExCamera`. Your preferences live in `Documents\ExCamera\first-person.ini` and carry over when you update.

**Works with Exanima 0.9.5.2g** on Windows x64 (Steam build 25803178, standard and Hellmode). On any other game version it warns you or refuses to start, and changes nothing.

## Keys

| Key | What it does |
| --- | --- |
| **F6** | Cycle first person, third person and the game's camera |
| **F7** (hold 1 s) | Stop the mod |
| **F8** | Open Settings and free the cursor; press again to close without saving |
| **F9 / F10** | Lower / raise the field of view |
| **F11** | Reset the field of view to 75° |
| **Cursor Look** (Space by default) | Free the cursor while exploring |
| **Hold right mouse** | In combat: manual guard. While dragging an object: rotate it |
| **Mouse wheel** | Third-person camera distance, or reach while dragging an object |

The game's main menu has a **First Person** page under MANUAL with the full guide.

## Runs on its own

ExCamera needs no other mods. If ExanimaLua (`ScriptRuntime.dll`) is installed too, ExCamera shows other mods' settings tabs in its Settings window.

## Building from source

Windows 10/11 x64, Python 3.10+, and Visual Studio 2022 C++ Build Tools. No game files are needed to compile.

```powershell
python build.py --package --wx
```

Build options, tests, VR builds, profiling and the release process are in [CONTRIBUTING.md](CONTRIBUTING.md). How the mod fits together is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Changes per version are in [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE). The bundled MinHook library keeps [its own license](src/vendor/minhook/LICENSE.txt). No game files are distributed here.
