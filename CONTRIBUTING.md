# Contributing & Release Guide

ExCamera ships as two Windows x64 binaries built from a single runtime tree: `ExCamera.exe` (launcher) and `ExanimaFirstPerson.dll` (in-process camera, input, rendering, and native Settings integration). No game executable or proprietary assets are stored in this repository or required to compile and run the test suites.

## Prerequisites

- **OS:** Windows 10 or Windows 11 (x64)
- **C++ Toolchain:** Visual Studio 2022 with the **Desktop development with C++** workload (or Visual Studio 2022 Build Tools with `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` and the Windows 10/11 SDK). `build.py` locates `vcvars64.bat`, `cl.exe`, `ml64.exe`, and `dumpbin.exe` automatically via `vswhere.exe`.
- **Python:** Python 3.10 or newer (3.12 used in CI). Building, unit testing, offline contract verification, profiling, and release packaging use only the Python standard library.
- **Optional binary-review packages:** `pip install pefile capstone` is needed **only** when running `tools/signatures.py`, `tools/native_contracts.py`, or `tools/xr_contracts.py generate` against a local `Exanima.exe`.

## Supported game build

This source targets **Exanima 0.9.5.2g, Steam build 25803178**. The launcher checks the game executable's SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70  0.9.5.2g
9a7839d5f948d3c3fa94d57de2a047fed77c8319007519ea6946d91ad6e85815  0.9.5.2g Hellmode
```

## Source layout

- `build.py` — Windows x64 build, export verification, and runtime packaging.
- `src/first_person.cpp`, `src/mod/` — DLL entry point and camera/input/rendering/game integration (one translation unit; see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)).
- `src/launcher.cpp` — launcher and activation flow.
- `src/native_settings.*`, `src/settings_*` — native settings UI, presentation, and draft model.
- `src/game_*`, `src/native_contracts.h`, `src/native_build.h`, `src/xr/` — game compatibility data, validation, and optional OpenXR host/contracts.
- `src/vendor/minhook/` — bundled x64 hook library.
- `tools/` — contract verification, C++ test runner, release checks, profiling, and Python unit tests (`tools/tests/`).
- `tests/` — C++ header unit tests.
- `docs/PLAYER_README.txt` — becomes `README.txt` in the release ZIP.

## Building

Run `build.py` from the repository root:

```powershell
# Standard runtime build (publishes ExanimaFirstPerson.dll, ExCamera.exe, manifest.toml into build/)
python build.py

# Treat all C++ compiler warnings as errors (/WX, enforced in CI)
python build.py --wx

# Build and create build/ExCamera-<FP_BUILD>-minimal.zip
python build.py --package --wx

# Local development build with FP_DEV_TOOLS=1 (never packaged for release)
python build.py --dev --wx

# Optional OpenXR VR build (stages openxr_loader.dll; packages ExCamera-<FP_BUILD>-openxr.zip)
python build.py --openxr-sdk C:\path\to\openxr-sdk --package --wx
```

### Staging, Locking, and Recovery

- `build.py` coordinates concurrent invocations using `build/.build.lock`. If another build holds the lock, `build.py` exits immediately with a clear error rather than corrupting intermediate files.
- Compilation, export verification (`enable`, `disable`, `allowOtherHooks`, `enableWithStatus`), `manifest.toml` stamping, and archive integrity checks all happen inside a temporary staging directory under `build/` before replacing published outputs.
- Always close `Exanima.exe` and `ExCamera.exe` before rebuilding so Windows does not lock `build/ExanimaFirstPerson.dll` or `build/ExCamera.exe`. If an interruption or locked file prevents rollback, `build.py` preserves the recovery folder path in its error output.
- When `--openxr-sdk <path>` is used with `--package`, `openxr_loader.dll` must exist in the SDK (`bin/`, `x64/bin/`, `build/src/loader/Release/`, or root) because `src/mod/xr_stereo.inl` loads `openxr_loader.dll` from beside `ExanimaFirstPerson.dll`. OpenXR packages are written to `ExCamera-<version>-openxr.zip` so they never overwrite `ExCamera-<version>-minimal.zip`.

## Verification & Unit Tests

Run all offline checks and unit tests before opening a pull request or cutting a release:

```powershell
# 1. Verify generated signature, field-contract, and XR whole-function contract headers match JSON specs
python tools/check_generated.py

# 2. Run Python standard-library unit tests (build, release, and contract tooling)
python -m unittest discover -s tools/tests -v

# 3. Compile and run shipped C++ header unit tests under MSVC (/std:c++17 /EHsc /W4 /WX)
python tools/run_cpp_tests.py

# 4. Verify a locally packaged minimal release archive
python build.py --package --wx
python tools/release.py verify --local
```

After a game update, `python tools/check_game.py` finds `Exanima.exe` (or takes a path), identifies the build by hash, and runs the signature, field-contract and VR checks read-only. See [tools/README.md](tools/README.md).

`tools/run_cpp_tests.py` uses `build/_cpp_tests/` for object files and test executables so it never contends with `build/.build.lock`.

## Profiling (`tools/profile.py`)

`tools/profile.py` manages opt-in profiling captures through `Documents/ExCamera/profiling.ini` and compares JSON capture reports without attaching to or modifying game memory:

```powershell
# Request a 60-second capture in a loaded profiling session
python tools/profile.py start --label baseline --seconds 60 --sample-every 64

# Inspect current capture status or stop early
python tools/profile.py status
python tools/profile.py stop

# Compare two capture reports
python tools/profile.py compare before.json after.json --top 25 --output comparison.txt
```

## Releasing

Before publishing a release, complete [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md) and update [CHANGELOG.md](CHANGELOG.md).

Accepted versions follow strict SemVer (`X.Y.Z` or `X.Y.Z-prerelease`, tagged as `vX.Y.Z` or `vX.Y.Z-prerelease`, with no leading zeros on numeric identifiers and no `+build` metadata).

### Option 1: Bump `FP_BUILD` on `main` (Automatic Tag + Release)

1. Update `#define FP_BUILD "X.Y.Z"` in `src/build_identity.h` **and** `version = "X.Y.Z"` in `manifest.toml` in the same commit.
2. Push or merge to `main`.
3. `.github/workflows/auto-release.yml` runs when `src/build_identity.h` changes on `main`, validates SemVer syntax and `manifest.toml` parity, creates `refs/tags/vX.Y.Z` if absent, and dispatches `.github/workflows/release.yml`.

### Option 2: Push an Annotated Tag Directly

```powershell
git tag -a v0.3.1 -m "ExCamera v0.3.1"
git push origin v0.3.1
```

Pushing a `v*` tag triggers `.github/workflows/release.yml` directly. In CI, `tools/release.py prepare` stamps `FP_BUILD` to match the tag version in the ephemeral runner checkout (without mutating the tagged commit or `FP_REVISION`), runs all checks and unit tests, builds with `--wx`, verifies the archive, generates commit-based release notes, and publishes the GitHub Release (`--prerelease` automatically when the tag contains `-`).

### Failure Recovery

- Existing GitHub Releases and tags are never overwritten; `release.yml` re-verifies that the remote tag still resolves to the exact built commit immediately before calling `gh release create`.
- If `auto-release.yml` created the tag ref on GitHub but a transient API failure prevented `release.yml` from starting or finishing:
  1. Check the GitHub Releases page and delete any incomplete draft release for `vX.Y.Z` if one was left behind.
  2. Trigger **Release** manually from the GitHub Actions UI (`Run workflow`) and enter `vX.Y.Z` in the **tag** input (or re-run **Auto release** via `workflow_dispatch`).
- Never force-move an already-published version tag; bump the patch or prerelease version instead.
