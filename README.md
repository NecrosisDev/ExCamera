# ExCamera

A first-person camera for Exanima, plus a proper third-person one while I was at it.

Your view *is* your character's head. Move the mouse, the head turns and the body follows. No floating camera bolted onto your face.

> **Beta.** It works on my machine. Expect rough edges and tell me about them.

## Download

Grab the latest zip from [Releases](https://github.com/NecrosisDev/exanima-first-person/releases). Like it? [Patreon](https://www.patreon.com/cw/NecrosisDev).

## What it does

- **F6** cycles first person, third person and the game's normal overhead view.
- **Head stabiliser** keeps the view steady instead of wobbling like you've had a few.
- **Spine follows your aim.** Look down and you lean in, look up and you lean back, so swings go where you're looking.
- **Crosshair** that matches the game's own combat cursor, with a wind-up ring.
- **Grab things in place.** Hold RMB for a cursor, LMB to grab, move the mouse to swing it around you, wheel to push or pull it, WASD to rotate it, let go to drop it right there.
- **Doors** that open when you want them to.
- **Crouch** with X outside combat.
- **Inventory, containers, dialogue and shops** give you the cursor and freeze your view, so you're not spinning around while you loot.
- **F8 Settings**, saved between launches: FOV (30-130), near clip, head steadiness, spine lean, third-person distance / height / shoulder, grab range and controls.

## Install

1. Unzip it somewhere normal: Desktop, Documents or Downloads. Not Program Files.
2. Run **ExCamera.exe**. If Exanima isn't running, it starts it through Steam.
3. Play. Press **F8** for Settings.

Windows may warn you about an unsigned app. That's expected for a hobby mod. Click **More info**, then **Run anyway**.

No game files are changed. Nothing is installed.

## Controls

| Key | What it does |
| --- | --- |
| F6 | First person → third person → overhead |
| F7 (hold) | Turn the mod off |
| F8 | Settings |
| RMB (hold) | Show the cursor |
| LMB | Grab what's under the cursor |
| Mouse wheel | Push / pull a held object |
| WASD | Rotate a held object |
| X | Crouch (outside combat) |

## Using EMTK?

The mod works **alongside** the [Exanima Modding Toolkit](https://codeberg.org/ExanimaModding/Toolkit). Start the game through EMTK like normal, then run ExCamera.exe. Your asset mods and first person load together.

If you run EMTK's older `rust-plugins` build, you can drop the zip's `mods\exanima-first-person` folder into `<Exanima>\mods\` instead.

This isn't an EMTK mod, it just plays nice with it.

## Settings file

`Documents\ExaniMate\first-person.ini`. Delete it to reset everything to defaults.

## Uninstall

Delete the folder you unzipped. Optionally delete `Documents\ExaniMate\` too.

## Known issues

- The view drifts a few degrees sideways when looking far up or down.
- Third-person height is capped by low ceilings indoors.
- Running alongside EMTK hasn't been tested in game yet.
- On EMTK's `rust-plugins` route, pressing F6 while EMTK has the mod switched off does nothing useful, and the "stopped" notice mentions F7 either way.
- No free cam yet.

## Compatibility

- Exanima **0.9.5.2g** (Steam). Other versions may refuse to load, on purpose, rather than crash.
- Windows 10 / 11. No extra runtimes needed.

## Bugs and feedback

Open an [issue](https://github.com/NecrosisDev/exanima-first-person/issues). Say what you were doing, which view you were in (first / third / overhead) and what happened.

## Support

If you like it and want more of this, you can chip in on [Patreon](https://www.patreon.com/cw/NecrosisDev). Totally optional, the mod stays free.

## License

MIT. See [LICENSE](LICENSE).

Exanima is made by Bare Mettle Entertainment. This is an unofficial fan mod and isn't affiliated with or endorsed by them.
