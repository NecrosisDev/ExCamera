# ExCamera

First- and third-person camera mod for the Windows x64 version of Exanima, with settings integrated into the game's native Settings window.

## Compatibility

This source targets **Exanima 0.9.5.2g, Steam build 25803178**. The launcher checks the game executable; other builds are not supported by this distribution.

Supported `Exanima.exe` SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

Current mod build: `0.2.0` (revision 42).

## Build

Requires Windows x64, Python 3, Visual Studio 2022 C++ Build Tools with the x64 C++ tools, and the Windows SDK. The build script locates MSVC through Visual Studio's `vswhere.exe`. No game executable or game assets are needed to compile.

From this folder:

```powershell
py build.py --package
```

Outputs:

- `build/ExCamera.exe` — launcher.
- `build/ExanimaFirstPerson.dll` — mod.
- `build/ExCamera-0.1.0-beta3-input38-native-fixes2-minimal.zip` — runtime ZIP containing those two files, `LICENSE.txt`, and `LICENSE-MinHook.txt`.

Use `py build.py` to build without making the ZIP. Build intermediates stay in `build/`. The package name follows `FP_BUILD` in `src/build_identity.h`.

## Run and configure

1. Extract the runtime ZIP, keeping the launcher and DLL together.
2. Start the supported Exanima build, then run `ExCamera.exe`.
3. Open the game's Settings window to configure ExCamera.

Controls:

- **F6:** cycle first-person, third-person, and the game's overhead camera.
- **F7:** stop the mod.
- **F8:** release or recapture the cursor.
- **F9 / F10:** decrease or increase FOV by 5 degrees.
- **F11:** reset FOV.

Camera and Combat settings are available in keyboard and controller modes. ExCamera key bindings appear under keyboard-mode Controls; the native controller Controls page remains unchanged. Apply commits the settings draft; Cancel discards it. Preferences are stored in `Documents/ExCamera/first-person.ini`.

Fully exit the game before replacing a loaded DLL. Keep your existing preferences when updating; the runtime ZIP does not include an INI file.

## Current limits and validation

Leaning is disabled. Eye setback is fixed at 16 cm; the camera near plane adapts between 1 and 3 cm. F6–F11 are reserved mod controls.

This revision corrects scroll-coordinate validation for ExCamera-owned checkboxes while retaining the game's native hit testing and activation. Vanilla controls are not wrapped.

This revision has **not been built with Windows/MSVC or tested in a running game**. Source-level and portable checks do not establish in-game correctness. Settings interaction and outdoor culling/lighting changes still require in-game verification. The build script checks the DLL's required exports, but does not run a test suite; development tests are not bundled in this minimal source distribution.

## Source layout

- `build.py` — Windows build and minimal runtime packaging.
- `src/first_person.cpp`, `src/mod/` — DLL entry point and camera/input/game integration.
- `src/launcher.cpp` — launcher and activation flow.
- `src/native_settings.*`, `src/settings_*` — native settings UI, presentation, and draft model.
- `src/game_*`, `src/native_contracts.h`, `src/native_build.h` — game compatibility data and validation.
- `src/vendor/minhook/` — bundled x64 hook library.

See [Architecture](docs/ARCHITECTURE.md) for the compilation model and native integration boundaries.

## License

ExCamera is licensed under the [MIT License](LICENSE). Bundled MinHook retains its [own license](src/vendor/minhook/LICENSE.txt). Runtime packages include both licenses. No game executable or assets are distributed here.
