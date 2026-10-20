# Architecture

## Build boundaries

`build.py` builds two Windows x64 binaries: the launcher (`launcher.cpp`) and the DLL (`first_person.cpp`). The DLL also links `native_settings.cpp`, `settings_presentation.cpp`, the bundled MinHook C sources, and `supersampling_shim.asm`.

The files under `src/mod/` intentionally form **one translation unit** through ordered includes in `first_person.cpp`; `state.inl` also includes `activation.inl`. They share private state and helpers. Do not add these `.inl` files to the compiler's source list or reorder them as an organizational cleanup.

## Where to start

- `mod/state.inl`, `activation.inl`, `session.inl`: shared state, activation checks, hooks, and session lifetime.
- `mod/input.inl`, `input_policy.h`, `input_ownership.h`, `raw_registration.h`: input routing and ownership.
- `mod/camera.inl`, `view.inl`, `frame.inl`, `camera_motion.h`, `head_stabiliser.h`: camera pose, transitions, and per-frame behavior.
- `mod/combat.inl`, `picking.inl`, `held_object.h`: combat and interaction integration.
- `mod/supersampling.inl`, `supersampling_depth.h`, `supersampling_shim.asm`: rendering integration.
- `mod/overlay.inl`, `panel.inl`, `settings.inl`: overlay, settings integration, and persistence.
- `native_settings.*`, `native_widgets.h`, `native_widget_contract.h`: native widget adapter and event contracts.
- `settings_model.h`, `settings_draft.h`, `settings_presentation.*`: settings values, draft/Apply/Cancel handling, and display mapping.
- `tracking.h`, `tracking_udp.h`: tracking data and receiver.

## Native compatibility is more than addresses

`game_signatures.h` contains precomputed signature tables. `game_layout.h` resolves game targets. Finding a target is not proof that every field or call contract still matches the game.

`game_structs.h` describes the layouts used by the mod. `game_contracts.h` contains precomputed owner/field evidence; `native_contracts.h` validates those contracts. `native_build.h` supplies the supported build identity. Native widget event semantics are separate again: `native_widget_contract.h` distinguishes checkbox selected-state handling from slider callback handling rather than treating all widgets identically.

Signature and contract tables are checked in. Their original generation and research tooling is not bundled or required to build this minimal distribution. Updating compatibility for another game build requires fresh verification, not simply changing the executable hash or accepting a successful signature match.

## Settings boundaries

The native UI edits a draft. Apply commits an accepted snapshot; Cancel discards draft edits. The presentation layer maps internal values to labels without making the native controller Controls table into an ExCamera page. Preferences are external to the source and runtime package.

ExCamera-owned checkboxes adapt the native scrolling-coordinate round trip for preview and release events. The game still performs hit testing, capture, checked-state changes, and action dispatch. This is not a global GUI patch; vanilla descriptors retain their native handlers. Handler dispatch lifetime is guarded separately from action callbacks so a native activation can still update the draft.

## Validation boundary

Compatibility checks and DLL export checks catch specific integration failures; they do not prove camera, input, or rendering behavior in a running game. This revision still needs a Windows/MSVC build and in-game verification. Development test harnesses are not included in this distribution.
