# Settings and FOV acceptance

## Scope

FOV controls and preference persistence against a disposable story character, including a complete game process restart. The implementation uses validated camera settings, same-directory atomic INI replacement, and a revision-coalescing worker.

## Controls and persistence

- F9 decreases first- and third-person FOV by 5 degrees, within validated bounds.
- F10 increases both by 5 degrees.
- F11 restores first-person 75 degrees and third-person 82 degrees.
- `ExCamera.exe --settings` prints the current persisted values without requiring the game.
- Preferences are stored in `Documents/ExCamera/controller.ini` and replaced atomically through a same-directory temporary file.
- Worker writes are coalesced by revision and flushed on stop. Disk I/O never runs in the native camera or mouse callbacks.

## Observed sequence

1. Attach loaded defaults: first FOV 75, third FOV 82, sensitivity 0.003.
2. F10 produced 80/87 after an observed key edge.
3. F9 restored 75/82.
4. Two F10 changes produced 85/92, and stop completed with zero hooks and zero faults.
5. The game process was closed and the same story save was reloaded.
6. Re-attach loaded 85/92 from disk, proving persistence across a full process restart.
7. F11 restored 75/82; final stop had zero hooks, zero module faults, and no restoration failure.

A very short synthetic keypress was not observed by the game-thread poll, so the acceptance uses ordinary observed key edges rather than assuming every injected press reaches the game.

## Offline checks

- Atomic round-trip preserves FOV, sensitivity, distance, and shoulder settings.
- Rapid worker requests coalesce and the final value is the one saved.
- Invalid files and out-of-range values fall back or clamp safely.
- Live controller updates and third-person FOV inheritance pass.
- All eight CTest suites pass after settings integration.

## Evidence limits

- There is no native settings UI yet; `--settings` and the INI file are the inspection interfaces.
- Sensitivity and third-person placement are persisted but do not yet have dedicated hotkeys or a UI.
- Rebinding, controller input, collision, death, scene travel, and long soak behavior remain separate work.
- EMTK, ExanimaLua, Hellmode, and other builds were not tested.
