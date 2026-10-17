# Changelog

## 0.1.0-beta3-input5 — 2026-10-17

- Add independent **Automatic / Manual defense** settings for first and third person. Manual uses held right mouse to request the game's defensive assistance; Automatic remains the default.
- Separate combat aim from movement: right mouse no longer cursor-walks in close views, and keyboard footwork follows the view's horizontal direction.
- Add independent aim-box dimensions and optional left/right lean keys. Third-person free aim now works outside combat too.
- Use fresh combat aim and clear stale movement or attack requests when controls, views, menus or player ownership change.
- Fix held-object handoffs, interaction-key handling, crouch ownership, third-person obstruction tracking and camera transitions.
- Improve launcher reentry, mouse fallback, settings saving and graphics cleanup.
- Strengthen game-update compatibility checks. Unsupported Manual defense falls back to Automatic without changing the saved setting.

Builds and automated tests pass. **This update has not been tested in game.** Controller movement is not fully adapted; native movement, balance, recovery and carrying restrictions remain.

## 0.1.0-beta3

First release with public source.

- Add game-update checks and a choice when another mod uses the same game functions.
- Improve first-person head positioning, helmet visibility and nearby clipping.
- Improve third-person camera clearance, tight-space framing and free aim.
- Add held-object distance, rotation and throwing controls, plus modifier-and-mouse leaning.
- Organize F8 settings into six pages with individual resets.
- Rename the mod to ExCamera and move settings to `Documents\ExCamera`, importing older settings automatically.

Play-tested on Exanima 0.9.5.2g (Steam build 25803178), Windows x64.

## Earlier betas

See [GitHub Releases](https://github.com/NecrosisDev/ExCamera/releases).
