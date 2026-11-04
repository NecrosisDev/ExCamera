# ExCamera

First- and third-person camera mod for the Windows x64 version of Exanima, with settings integrated into the game's native Settings window.

Download the prebuilt Windows runtime from [Releases](https://github.com/NecrosisDev/ExCamera/releases/latest). Choose `ExCamera-<version>-minimal.zip`, extract it, and keep its files together. The automatically generated Source code downloads are for developers.

## Compatibility

This source targets **Exanima 0.9.5.2g, Steam build 25803178**. The launcher checks the game executable; other builds are not supported by this distribution.

Supported `Exanima.exe` SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70  0.9.5.2g
9a7839d5f948d3c3fa94d57de2a047fed77c8319007519ea6946d91ad6e85815  0.9.5.2g Hellmode
```

### Standalone operation and `ScriptRuntime.dll` coexistence

ExCamera runs **100% standalone** with zero external dependencies — camera modes, input, native Settings UI, and procedural wounds, blood, and fire are all built into `ExanimaFirstPerson.dll`. When `ScriptRuntime.dll` (ExanimaLua) is also installed, ExCamera **optionally coexists and integrates** with it: hosting other mods' registered Settings tabs inside ExCamera's widened native Options form and enabling VR hip-pouch inventory hooks when `ScriptRuntime` inventory exports are present.

## Changes

See [CHANGELOG.md](CHANGELOG.md) for changes since the 0.2.0 source baseline. The changelog is separate from the automatically generated GitHub release notes.

## Build

Requires Windows 10/11 x64, Python 3.10 or newer, Visual Studio 2022 C++ Build Tools with the x64 C++ tools, and the Windows SDK. The build script locates MSVC through Visual Studio's `vswhere.exe`. No game executable or game assets are needed to compile.

From this folder:

```powershell
python build.py --package --wx
```

Outputs:

- `build/ExCamera.exe` — launcher.
- `build/ExanimaFirstPerson.dll` — mod.
- `build/manifest.toml` — EMTK mod manifest stamped with `FP_BUILD`.
- `build/ExCamera-<version>-minimal.zip` — runtime ZIP containing `ExCamera.exe`, `ExanimaFirstPerson.dll`, `manifest.toml`, `README.txt` (from `docs/PLAYER_README.txt`), `LICENSE.txt`, and `LICENSE-MinHook.txt`.

Use `python build.py` to build without refreshing the ZIP, or pass `--openxr-sdk <path>` to compile the optional OpenXR stereo integration (staging `openxr_loader.dll` and packaging `ExCamera-<version>-openxr.zip`). Compilation and validation use isolated staging under `build/`; on failure, previous outputs are retained or restored. Exit the game and launcher before rebuilding. See [CONTRIBUTING.md](CONTRIBUTING.md) for full build flags, Python and C++ unit test commands, profiling, and failure recovery.

## Releases and automatic changelog

- **Updating `FP_BUILD` on `main`:** When `src/build_identity.h` changes on `main` (with a matching `version` in `manifest.toml`), `.github/workflows/auto-release.yml` validates the SemVer version, creates `v<FP_BUILD>` if absent, and dispatches the Release workflow. Test the exact build first; see [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md).
- **Direct tag push or manual dispatch:** Pushing an annotated `vX.Y.Z` (or `vX.Y.Z-rc.1`) tag also triggers `.github/workflows/release.yml` directly, and maintainers can retry publication for an existing tag via `workflow_dispatch` with `tag: vX.Y.Z`.
- **Verification gates:** Both `build.yml` and `release.yml` validate generated compatibility tables (`tools/check_generated.py`), run Python and C++ unit tests (`tools/tests` and `tools/run_cpp_tests.py`), compile with `/WX` (`--wx`), verify DLL exports, and validate the runtime ZIP before uploading or publishing. See [CONTRIBUTING.md](CONTRIBUTING.md) for step-by-step release and recovery instructions.

## Run and configure

1. Extract the runtime ZIP, keeping the launcher and DLL together.
2. Run `ExCamera.exe`. It starts Exanima through Steam, or attaches to a running game.
3. Open the game's Settings window to configure ExCamera.

Controls:

- **F6:** cycle first-person, third-person, and the game's overhead camera.
- **Hold F7 for one second:** stop the mod. A short press only shows a reminder.
- **F8 (first/third person or title screen):** open native Settings and free the cursor; press again to dismiss without applying ExCamera edits.
- **Hold Cursor Look (vanilla Controls binding, default Space):** free the cursor in first-/third-person exploration without opening Settings. Release to resume mouse look; combat and native UI retain their controls. Rebind it in the game's existing Controls row.
- **Hold RMB (combat):** engage Manual Guard to actively align your parry against incoming strikes.
- **Hold RMB (dragging objects):** rotate held physics objects along pitch and yaw in true 3D space.
- **Mouse Wheel:** adjust camera distance in third person; adjust hold reach when dragging objects.
- **F9 / F10:** decrease or increase the first-person (base) FOV by 5 degrees.
- **F11:** reset the base FOV to 75 degrees.
- **In-Game Manual:** open the native MANUAL tab on the main menu to read the complete First Person (ExCamera) reference guide.

FOV shortcuts work in active first/third-person views and apply immediately, outside the Settings draft. A separate third-person FOV overrides the base value; choose Same to share it.

Camera and Combat settings are available in keyboard and controller modes. All ExCamera Controls options appear only in keyboard mode; the native controller Controls page remains unchanged. Apply commits the ExCamera draft; Cancel discards those draft edits. Preferences are stored in `Documents/ExCamera/first-person.ini`.

Fully exit the game before replacing a loaded DLL. Keep your existing preferences when updating; the runtime ZIP does not include an INI file.

### First-person headwear

The Headwear setting controls helmets and other headwear in first person. **Hide all** removes them from the first-person view. **Show when clear** draws supported headwear attached to the view, including its inside faces. Either way, a piece that blocks the view is hidden. This changes only the first-person drawing: third person, the overhead camera, other characters and shadows are unchanged.

## Source layout

- `build.py` — Windows x64 build, export verification, and runtime packaging.
- `src/first_person.cpp`, `src/mod/` — DLL entry point and camera/input/rendering/game integration.
- `src/launcher.cpp` — launcher and activation flow.
- `src/native_settings.*`, `src/settings_*` — native settings UI, presentation, and draft model.
- `src/game_*`, `src/native_contracts.h`, `src/native_build.h`, `src/xr/` — game compatibility data, validation, and optional OpenXR host/contracts.
- `src/vendor/minhook/` — bundled x64 hook library.
- `tools/` — offline contract verification (`check_generated.py`), C++ header test runner (`run_cpp_tests.py`), release checks (`release.py`), profiling (`profile.py`), and Python unit tests (`tools/tests/`).
- `tests/` — shipped C++ header unit tests (`tests/unit/`).

See [CONTRIBUTING.md](CONTRIBUTING.md) for developer workflows, [Architecture](docs/ARCHITECTURE.md) for the compilation model and native integration boundaries, and the [documentation map](docs/README.md) for document status labels. *(An earlier experimental CMake prototype is archived separately on branch `archive/rebuild-prototype` and is not part of `main` or releases.)*

## License

ExCamera is licensed under the [MIT License](LICENSE). Bundled MinHook retains its [own license](src/vendor/minhook/LICENSE.txt). Runtime packages include both licenses. No game executable or assets are distributed here.
