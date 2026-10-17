# ExCamera: Exanima First Person

First person and third person for **Exanima**, plus the game's own overhead view, all on F6.
The camera is your character's head: the mouse turns the head and body, and you see exactly what the
head sees. Settings are on F8.

> **Beta. Windows x64.** Tested in play on **Exanima 0.9.5.2g (Steam build 25803178)**. Other game
> versions are supported when every game function the mod uses can still be found and its hooks are seen
> running (see [Game versions](#game-versions)). Otherwise it refuses to start and changes nothing.

ExCamera is a standalone camera mod. It is not part of the ExaniMate editor and does not need it, or
EMTK (it can run alongside EMTK, see the player readme).

## Download and install

**Players do not need to build the mod or install Visual Studio or Python.** Use the prebuilt
Windows x64 package:

1. Open [Releases](https://github.com/NecrosisDev/ExCamera/releases) and expand **Assets** on the
   release you want, including prereleases marked **Pre-release**.
2. Download **`ExCamera-0.1.0-beta3.zip`** for this version (future versions use
   `ExCamera-<version>.zip`). GitHub's **Source code (zip)** and **Source code (tar.gz)** downloads
   contain source files, not the runnable mod. If the ExCamera ZIP is missing, the binary package
   has not been attached to that release yet.
3. Extract the entire ZIP to a writable folder such as Downloads or Documents, outside Program
   Files and the Exanima game folder. Extract it before running anything.
4. Keep **`ExCamera.exe`** and **`ExanimaFirstPerson.dll`** together in the extracted folder, then
   run **`ExCamera.exe`**. It starts Exanima through Steam or attaches to it if it is already running.
   No game files are changed.
5. Use **F6** to switch first person / third person / overhead, **F8** for settings, and hold **F7**
   for one second to switch the mod off.

Read the included `README.txt` for EMTK installation options and known issues (also available as
the [player readme](packaging/README.txt)). Settings are saved separately, so a new download keeps
your settings. To uninstall, close the game and delete the extracted folder; if you used EMTK's
legacy loader, also remove its `mods\exanima-first-person` folder.

## Controls

| Key | What it does |
|---|---|
| Mouse | Look |
| F6 | First person → third person → overhead |
| F7 (hold 1 s) | Switch the mod off; F6 turns it back on |
| F8 | Cursor and the Settings window |
| Right mouse | Out of combat: hold to free the cursor (hold, toggle or off) |
| Mouse wheel | While holding an object: nearer / farther |
| Duck key (X) | Out of combat: crouch (toggle by default) |
| F9 / F10 / F11 | Field of view down / up / back to 75 |

Settings are saved in `Documents\ExCamera\first-person.ini`. Versions before the ExCamera name kept them in
`Documents\ExaniMate\`; that file is copied over the first time.

Some file names predate the name and are kept so existing installs keep working: `ExanimaFirstPerson.dll`
and the EMTK mod id `exanima-first-person`.

## Building

Requirements: Windows, Visual Studio 2019/2022 (or Build Tools) with the **x64 C++ tools**, Python 3.

```powershell
python build.py            # builds build\ExanimaFirstPerson.dll + build\ExCamera.exe and runs every test suite
python build.py --package  # also produces build\ExCamera-<version>.zip in the release layout
```

`build.py` finds MSVC with `vswhere`, so a normal PowerShell works (no Developer Prompt needed). CI runs
the same command on every push (`.github/workflows/build.yml`).

**Use MSVC.** The mod reads game memory inside `__try`/`__except` blocks. Only MSVC catches a fault from
an inline read inside the block itself; MinGW and clang accept the syntax but don't. So other compilers
build a DLL that can crash the game where the MSVC one recovers.

Set `EXANIMA_EXE` to a game executable (for example
`C:\Program Files (x86)\Steam\steamapps\common\Exanima\Exanima.exe`) and the tests also resolve every
signature against it. They only read the file.

The portable suites need no Windows headers:

```sh
for t in test_camera_motion test_yaw_stabilization test_view_boundaries test_game_layout; do
  g++ -std=c++17 src/$t.cpp -o $t && ./$t
done
```

## How it works

```
ExCamera.exe (src/launcher.cpp)
  ├─ finds or starts Exanima.exe (via Steam)
  ├─ resolves every game target in the exe file by signature        (src/game_layout.h)
  ├─ checks the three camera/control dispatch slots are the game's own
  ├─ injects ExanimaFirstPerson.dll and calls its exported enable()
  └─ waits for the DLL to report it is active (first-person-status.json)

ExanimaFirstPerson.dll (src/first_person.cpp)
  ├─ enable(): resolves the targets again from the game's file, then checks that no other program
  │            already patched the functions it hooks or calls (fp::codeMatchesFile)
  ├─ installs MinHook detours on the resolved functions (see docs/HOOKS.md)
  ├─ per frame: places the camera at the head's eye mount, applies FOV / near clip,
  │             draws the settings window and swing cue (wglSwapBuffers hook)
  ├─ watches its per-tick hooks during play (src/hook_health.h)
  └─ disable() / F7 / any fault: removes the hooks and restores the game's camera
```

The DLL exports `enable`, `disable` and `allowOtherHooks` (which the launcher calls before `enable` when the
player chose to start on top of another mod). EMTK's legacy `rust-plugins` loader also calls these, and it
crashes the game if either is missing, which is why the build checks for both. Hooks are never installed
from `DllMain`.

| File | Responsibility |
|---|---|
| `src/first_person.cpp` | The DLL. One translation unit: its includes, then the parts below, in order |
| `src/mod/state.inl` | Shared state: the resolved layout, settings, the status file, notices, faults |
| `src/mod/input.inl` | Window procedure (F6-F11, right mouse), mouse-look aim, input and behaviour hooks, head steadying, movement |
| `src/mod/picking.inl` | Picks past your own body, held-object distance and drop spot, grab reach, doors, the crosshair position |
| `src/mod/view.inl` | Hiding what blocks the eye (helmet, body wear, third-person blockers), tile covers |
| `src/mod/overlay.inl` | The attack wind-up arc (OpenGL), game windows that free the cursor, crouch |
| `src/mod/frame.inl` | The frame hook, installing and rolling back the dispatch slots, third-person aim |
| `src/mod/camera.inl` | The camera: the replaced sampler that puts the view at the head's eye or behind the shoulder |
| `src/mod/session.inl` | Activation, game build and other mods' hooks, F7/F6, the status file, the worker loop, exports |
| `src/mod/settings.inl`, `src/mod/panel.inl` | F8: settings read, applied live and saved; the Settings window |
| `src/status_json.h` | The status file's JSON writer |
| `src/game_structs.h` | Field offsets in the game's objects, by name (`field::character::combatStance`) |
| `src/pointer_set.h` | The visited set for walks over the game's scene trees |
| `src/game_signatures.h` | **Generated.** One code signature per game address (`tools/signatures.py`) |
| `src/game_layout.h` | Resolves the signatures in a game image; checks hooked entries are unpatched |
| `src/hook_health.h` | The live check that the per-tick hooks run during play |
| `src/native_build.h` | The hash of the verified game build (labels the build; does not gate it) |
| `src/camera_motion.h` | Pure math: head-mounted camera, third-person pivot, near plane, smoothing |
| `src/head_stabiliser.h` | Pure math: neck/head stabilisation and roll comfort |
| `src/input_policy.h` | Key/mouse rules: what the mod consumes vs. passes to the game |
| `src/settings_window.h`, `src/panel_ui.h`, `src/ui_art.h` | The F8 settings window |
| `src/launcher.cpp` | `ExCamera.exe`: start/attach, resolve, inject |
| `src/vendor/minhook` | [MinHook](https://github.com/TsudaKageyu/minhook), BSD-2 |
| `tools/targets.json` | Every game address the mod uses, named, with its RVA in the verified build |
| `tools/signatures.py` | Generates `game_signatures.h` from an exe; checks an exe against it |

### Tests

| Suite | Needs Windows | Covers |
|---|---|---|
| `test_camera_motion` | no | camera = head, third-person pivot, near clip |
| `test_yaw_stabilization` | no | head stabiliser, roll comfort |
| `test_view_boundaries` | no | numerical view limits |
| `test_status_json` | no | the status file writer: field shapes the launcher reads, number formats, escaping |
| `test_game_layout` | no | signature parsing and search, layout resolution, hook health; a real exe with `EXANIMA_EXE` |
| `test_fov_input` | yes | FOV keys and limits |
| `test_controls` | yes | input policy, settings file, notices, the activation checks (builds `first_person.cpp`) |
| `test_chaining` | yes | hooking on top of another program's detour, and handing it back |
| `test_runtime_lifecycle` | yes | enable / disable / fault / rearm against a fake game window |

These are offline tests. They do not prove in-game behaviour, which still needs testing with the game.

## Game versions

The mod does not check the version number of the game. At startup it checks things it needs in order
to work:

1. **Every game target resolves.** Each of the 73 functions, return sites, globals and vtable slots the
   mod uses has a code signature (`src/game_signatures.h`). The bytes are matched with everything that
   moves when the game is relinked masked out: branch targets, RIP-relative displacements, absolute
   addresses. Opcodes, registers and struct-field offsets are kept, so a match is the same code touching
   the same fields. Every signature must match exactly once. Each global is found through two independent
   references, which must agree, and each vtable slot must hold the function it is expected to hold.
   If anything fails, nothing is installed, and `first-person-status.json` lists what was not found.
2. **Another mod on its hook points: the player decides.** If another program has already detoured one of
   the functions the mod hooks or calls, or replaced one of its three dispatch slots, `[Compat] OtherHooks`
   in `first-person.ini` decides what happens:
   - `ask` (default): `ExCamera.exe` names the functions and asks whether to start on top of the other
     mod, for this game session. Without the launcher (a mod loader started the DLL) nothing is asked and
     the mod does not start.
   - `allow`: always start on top of it, with a notice naming what was already hooked.
   - `refuse`: never.

   On top means the mod's detour runs first and calls through to the other mod's, and its slot
   replacements call whatever the other mod put there. Stopping gives both back exactly
   (`test_chaining`). Mods that change other code are never a question.
3. **Its hooks run.** Once installed, the five hooks that run on every game tick (`pollInput`,
   `navSampler`, `playerBehavior`, `poseBuilder`, `partSettle`) must all fire during 5 seconds of game
   time in play. The game's own clock is used, so pauses, menus and loads never count. On an untested
   game build, or on top of another mod's hooks, a silent hook stops the mod and restores the game's
   camera. On the verified build the
   result is only reported (`"hook_health"` in the status file).

On an untested build that passes all three checks, the mod shows a one-time notice and runs. What the
signatures cannot prove is struct fields the mod reads from code it does not match. If a game update
changes those, things can look wrong without failing a check. That is what F7 and bug reports are for.

### After a game update

```sh
pip install pefile capstone
python tools/signatures.py check "C:\...\Exanima.exe"
```

`check` lists every target that moved (fine) and every one that no longer resolves (the mod will refuse
to start). To fix one, find the function again in the new exe (Ghidra/IDA, from a neighbouring function
that still resolves or a string reference), put its new RVA in `tools/targets.json`, and regenerate:

```sh
python tools/signatures.py generate "C:\...\Exanima.exe"   # rewrites src/game_signatures.h
```

`generate` needs every RVA in `targets.json` to be correct for the exe you give it. Then rebuild, play,
and check the status file says `"hook_health":"passed"`. To make a new build the verified one, put its
SHA-256 in `fp::executableHash` (`src/native_build.h`).

## Why a launcher that injects a DLL

The game has no mod API for code, so `ExCamera.exe` does what every native Exanima mod has to do. It opens the
running game, writes the DLL's path into it, starts `LoadLibraryW` there with `CreateRemoteThread`, then calls
the DLL's `enable` the same way. That is also how malware loads code, so antivirus tools may flag it, and the
exe is not code-signed. Everything it does is in `src/launcher.cpp` (about 300 lines). Build it yourself if
you would rather not run a downloaded binary. Without the launcher, EMTK's legacy loader can load the DLL
instead (see the player readme).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the design rules in [docs/DESIGN.md](docs/DESIGN.md).

## Changes

See [CHANGELOG.md](CHANGELOG.md).

## Known issues

- Looking up or down, the view drifts a few degrees sideways.
- Both EMTK routes are untested in game.
- Indoors, the third-person height is capped by the ceiling.

## License

MIT, see [LICENSE](LICENSE). MinHook keeps its own BSD-2 license (`src/vendor/minhook/LICENSE.txt`).
Exanima belongs to Bare Mettle Entertainment. This project isn't affiliated with them and ships no
game files.

Support the work: [patreon.com/cw/NecrosisDev](https://www.patreon.com/cw/NecrosisDev)
