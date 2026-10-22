# ExCamera

First- and third-person camera mod for the Windows x64 version of Exanima, with settings integrated into the game's native Settings window.

Download the prebuilt Windows runtime from [Releases](https://github.com/NecrosisDev/ExCamera/releases/latest). Choose `ExCamera-<version>-minimal.zip`, extract it, and keep the launcher, DLL and licenses together. The automatically generated Source code downloads are for developers.

## Compatibility

This source targets **Exanima 0.9.5.2g, Steam build 25803178**. The launcher checks the game executable; other builds are not supported by this distribution.

Supported `Exanima.exe` SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70  0.9.5.2g
9a7839d5f948d3c3fa94d57de2a047fed77c8319007519ea6946d91ad6e85815  0.9.5.2g Hellmode
```

## Changes

See [CHANGELOG.md](CHANGELOG.md) for changes since the 0.2.0 source baseline. The changelog is separate from the automatically generated GitHub release notes.

## Build

Requires Windows x64, Python 3.8 or newer, Visual Studio 2022 C++ Build Tools with the x64 C++ tools, and the Windows SDK. The build script locates MSVC through Visual Studio's `vswhere.exe`. No game executable or game assets are needed to compile.

From this folder:

```powershell
py build.py --package
```

Outputs:

- `build/ExCamera.exe` — launcher.
- `build/ExanimaFirstPerson.dll` — mod.
- `build/ExCamera-<version>-minimal.zip` — runtime ZIP containing those two files, `LICENSE.txt`, and `LICENSE-MinHook.txt`.

Use `py build.py` to build without refreshing the ZIP. The package name follows `FP_BUILD` in `src/build_identity.h`. Compilation and validation use isolated staging under `build/`; success publishes the runtime files and replaces the ZIP individually.

On failure, previous outputs are retained or restored, not newly built. If rollback cannot finish (for example, a running program locks a file), the error identifies retained recovery files; do not use the mixed outputs. Exit the game and launcher before rebuilding. Publication is not crash-atomic across multiple files. Unrelated files in `build/` are preserved; its persistent `.build.lock` file is only a coordination file, not a stale-lock sentinel.

## Releases and automatic changelog

Push an existing commit with a new version tag such as `v0.2.1`, `v0.3.0`, or `v0.3.0-rc.1`. The Release workflow validates the tag and sets `FP_BUILD` to its version in the CI checkout before compiling both binaries. It preserves `FP_REVISION` and does not edit your source commit or move the tag. No manual version edit is needed for each release; local builds continue to use the checked-in header.

Accepted tags are `vX.Y.Z` with an optional SemVer prerelease suffix. Numeric components cannot have leading zeros. Build metadata (`+...`) is not supported by the package naming convention. Prerelease tags are published as GitHub prereleases.

Release notes automatically list commit subjects since the prior reachable valid version tag selected by `git describe`, with commit links and a full comparison link. This uses Git's bounded candidate search, not version-number sorting or a guarantee of the globally closest tag. It includes direct commits, not only pull requests. The first tagged release lists the initial history. These are raw commit summaries, not inferred feature claims. Use clear commit subjects for useful notes.

Both workflows check generated compatibility tables, build the Windows runtime, check its exports and verify the runtime ZIP.

Only successful source checks, tooling tests, build, export check, ZIP check, and artifact upload can reach publication. The ZIP contains exactly the two runtime binaries and two licenses. Existing releases are not overwritten. See [release installation instructions](INSTALL.txt) for tagging and failure recovery.

## Run and configure

1. Extract the runtime ZIP, keeping the launcher and DLL together.
2. Start the supported Exanima build, then run `ExCamera.exe`.
3. Open the game's Settings window to configure ExCamera.

Controls:

- **F6:** cycle first-person, third-person, and the game's overhead camera.
- **Hold F7 for one second:** stop the mod. A short press only shows a reminder.
- **F8 (first/third person or title screen):** open native Settings and free the cursor; press again to dismiss without applying ExCamera edits.
- **Hold Cursor Look (vanilla Controls binding, default Space):** free the cursor in first-/third-person exploration without opening Settings. Release to resume mouse look; combat and native UI retain their controls. Rebind it in the game's existing Controls row.
- **F9 / F10:** decrease or increase the first-person (base) FOV by 5 degrees.
- **F11:** reset the base FOV to 75 degrees.

FOV shortcuts work in active first/third-person views and apply immediately, outside the Settings draft. A separate third-person FOV overrides the base value; choose Inherit to share it.

Camera and Combat settings are available in keyboard and controller modes. All ExCamera Controls options appear only in keyboard mode; the native controller Controls page remains unchanged. Apply commits the ExCamera draft; Cancel discards those draft edits. Preferences are stored in `Documents/ExCamera/first-person.ini`.

Fully exit the game before replacing a loaded DLL. Keep your existing preferences when updating; the runtime ZIP does not include an INI file.

### First-person headwear

The Headwear setting controls helmets and other headwear in first person. **Hide all** removes them from the first-person view. **Show when clear** draws supported headwear attached to the view, including its inside faces. Either way, a piece that blocks the view is hidden. This changes only the first-person drawing: third person, the overhead camera, other characters and shadows are unchanged.

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
