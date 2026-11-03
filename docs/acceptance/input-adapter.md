# Input and cursor acceptance

## Scope

Mouse look, F6 mode cycling, native UI ownership, focus loss, and clean teardown against a disposable story character in a safe area. The tests use the corrected two-hook build and the standard executable identified in `camera-adapter.md`.

## Observed behavior

- Runtime attach: `state=active`, `installed_hooks=2`, `active_hooks=2`, `module_faults=0`, `restoration_failed=0`, `abi=2`.
- Mouse movement changed the live first-person view as expected.
- F6 transitioned first person to third person, third person to native overhead, and overhead back to first person.
- Pause opened the native menu with a free cursor. Moving the pointer to a corner did not change the fixed reference view.
- Focus moved from gameplay to File Explorer and back. Cursor recapture preserved the same reference view without a snap.
- Stop: `state=stopped`, `installed_hooks=0`, `active_hooks=0`, `module_faults=0`, `restoration_failed=0`.
- Game and File Explorer windows were closed after the test.

## Defect found and corrected

An early pause test showed a one-frame look snap: cursor capture was released only after the native UI had already changed pointer state, so a recenter delta could be applied. GUI capture and drag fields alone were not a sufficiently early ownership signal.

The policy now also reads the game's `uiOpenCount` (`RVA 0x499860`) before recentering. When it is nonzero, native UI owns the cursor. The corrected test shows identical reference views before and after pointer movement in pause.

## Ownership rules implemented

- Cursor capture requires a focused game window, valid client geometry, no native UI, and first-/third-person mode.
- Capture start and mode/focus transitions reset delta accumulation before any movement is applied.
- Cursor clipping and shape are changed only on Exanima's mouse thread.
- Native UI and focus loss release ownership on that same thread.
- Native mouse snapshots and key state are preserved when the adapter suppresses native mouse-look.
- Stop requests both camera and cursor restoration and waits for both before hook removal.

## Offline checks

Eight input state-machine tests cover initial capture, F6 edges and complete cycles, native UI release/recapture, focus loss, invalid clients, held F6 across focus changes, reset with a held key, and overhead behavior. All seven CTest suites pass after integration.

## Evidence limits

- Settings persistence, rebinding, FOV controls, and controller input are not covered.
- Death, scene travel, resolution changes, fullscreen changes, inventory/shops/dialogue, and long soaks remain separate edge-case work.
- Third-person live collision, visual filtering, and headwear policy are not covered.
- EMTK, ExanimaLua, Hellmode, and other builds were not tested.
