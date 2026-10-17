# Contributing

Issues and pull requests are welcome. For anything that changes how the camera, keys, crosshair or
helmet behave, open an issue first and check it against [docs/DESIGN.md](docs/DESIGN.md).

## Every change

- `python build.py` passes: it builds both binaries and runs all C++ and Python test suites. CI runs the same.
- Say how you verified it: offline tests, reading the decompile, or playing the game. These are
  different kinds of evidence, so name the one you have.
- New key bindings ship unbound (ini value `0`). The mod never picks a key for the player.
- Behaviour changes and cosmetic changes go in separate pull requests.

## Touching game memory

- Any new hook or memory read/write needs a short note in the PR: the RVA, what the function is,
  how you identified it, and what happens if the hook runs while the mod is disabling.
- Never write a fixed game address. Add the target to `tools/targets.json`, regenerate
  `src/game_signatures.h` (`python tools/signatures.py generate <Exanima.exe>`), and use
  `gameAt(fp::sig::name)`. Hooked and called functions are then also checked for other programs' detours.
  See [docs/HOOKS.md](docs/HOOKS.md).
- Never write a raw struct offset either: name it in `src/game_structs.h` and say how it was identified.
- Record new fields in `tools/native_contracts.json` and regenerate `src/game_contracts.h` with
  `python tools/native_contracts.py generate <Exanima.exe>`. Keep native access widths and provenance.
- Taking a lock: keep the order documented at the top of `src/mod/state.inl`, and never wait while
  holding `cameraLock`.
- Settings controls send row actions to `applySetting`; displayed snapshots are only for display.
  FOV keys, settings edits and persistence share `settingsLock`; release it before waiting for raw input.
- Never install hooks from `DllMain`. Do it from `enable()`.
- Anything the mod changes must be restored on `disable()`, on F7, and on every fault path.

## Code style

- C++17, MSVC `/W4` clean (MSVC only: see "Building" in the README). Format with the repo's `.clang-format`.
- Comments explain *why*. Each decompiled game address gets one short comment saying what it is.
- Magic numbers become named constants. No dead or commented-out code, no leftover debug output.
- Keep math in the pure headers (`camera_motion.h`, `head_stabiliser.h`) so it stays testable without
  Windows or the game.
