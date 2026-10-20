# ExCamera

First- and third-person camera mod for the Windows x64 version of Exanima, with settings integrated into the game's native Settings window.

Download the prebuilt Windows runtime from [Releases](https://github.com/NecrosisDev/ExCamera/releases/latest). Choose `ExCamera-<version>-minimal.zip`, extract it, and keep the launcher, DLL and licenses together. The automatically generated Source code downloads are for developers.

## Compatibility

This source targets **Exanima 0.9.5.2g, Steam build 25803178**. The launcher checks the game executable; other builds are not supported by this distribution.

Supported `Exanima.exe` SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

Checked-in local build identity: `0.2.2-prof10` (revision 58), with the headwear hide and settings navigation fixes. Release builds derive their version from the Git tag; the checked-in local default does not pin future releases.

## Updating this repository

See [CHANGELOG.md](CHANGELOG.md) for changes since the explicitly identified 0.2.0 source baseline.

1. Commit or back up local edits before updating. Extract this source ZIP directly into the repository root, replacing supplied files and including `.github/` and `.gitignore`. Do not create an extra nested project folder or replace your `.git` directory.
2. Remove these obsolete tracked headers after checking for your own edits: `src/panel_ui.h`, `src/settings_window.h`, and `src/ui_art.h`. Extracting a ZIP alone cannot remove old files. No other files need deletion for this baseline update.
3. Review the Git diff, build using the command below, and commit the update. Keep your external `first-person.ini`; no preferences or built binaries are bundled in this source ZIP.

The supplied changelog is separate from automatically generated GitHub release notes. With no previous valid version tag, the first tagged release's generated notes include the initial commit history.

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

Both workflows check generated compatibility tables, build the Windows runtime, check its exports and verify the runtime ZIP. Test sources and harnesses are maintained outside this public repository. Build checks do not establish in-game camera feel, visual acceptance or FPS.

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
## Profiling candidate 0.2.2-prof10

This local build carries the performance changes plus opt-in sampled hook profiling. See [PROFILING-README.txt](PROFILING-README.txt) and [tools/profile.py](tools/profile.py) for capture controls and build-comparison guidance. Profiling defaults off; no key binding or camera setting is added. This is not a published release or evidence of an in-game FPS gain.

### First-person headwear in this local candidate

Headwear handles verified head-bound faces inside the player skin aggregate as well as separate head attachments. Hide all removes those faces. Show when clear draws supported headwear from its bind geometry, attached to the displayed camera sample, including its inside faces. Native attachment obstruction checks remain; stable triangles also receive a two-sided five-ray obstruction test. A blocking piece is hidden in either mode. Body and mixed neck faces keep their original draw.

This is a render-only treatment in the owned main first-person colour traversal. Native physics, camera sampling/input/collision, other actors, third person, overhead and shadows retain their owners. The additional GPU stream belongs to the existing warm exact-context table and joins its deletion/teardown drain. No game geometry or transform is edited.

The optional path requires the reviewed native source and unchanged loaded native bodies. Malformed or unsupported layouts forward to native rendering; `headwear_render_ready`, `headwear_render_draws`, `headwear_render_hidden_faces`, `headwear_render_stable_faces`, `headwear_render_fallbacks` and `headwear_render_faults` expose admission and results. The original attachment-scan counters do not count merged faces. A material first attached during a colour traversal can enter the rejection hints on the next traversal; every match is revalidated. Capacity uncertainty uses full validation.

The profiler adds `headwear_colour`, `headwear_colour.native`, `headwear_draw`, `headwear_draw.native`, `headwear_submit` and `headwear_submit.native`. Use revision 54 (prof6) for the immediate comparison and revision 56 (prof8) for this candidate, matching equipped items, settings, scene and actions. Both report variant `optimized`; compare build/revision rather than relabelling captures. All existing profiler metrics retain their identities.

Offline production fixtures and real hidden-window GPU pixel checks cover selective visibility, stable attachment, inside faces, restoration and context lifetime. Testing the actual helmet, hair and bevor in-game remains required.

Headwear rendering also checks the actual submitted mesh instance, including material groups shared with a model source or another actor. Unknown native callers and changed ownership retain native drawing. `headwear_submit` and `headwear_submit.native` profiling spans measure this guard separately.
