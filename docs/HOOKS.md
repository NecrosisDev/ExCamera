# Game targets

Every Exanima address ExCamera uses is listed by name in [`tools/targets.json`](../tools/targets.json),
with its RVA in the verified build (Exanima 0.9.5.2g, Steam build 25803178).
[`src/game_signatures.h`](../src/game_signatures.h) holds the generated code signature for each, and at
startup the mod finds them again in whatever game build is running (see "Game versions" in the
README). In code, a target is `gameAt(fp::sig::name)`, never a fixed address.

`Exanima.exe` is not ASLR-enabled (it always loads at `0x100000000`). The mod doesn't depend on that,
because every address is base + resolved RVA.

## Hooked functions (MinHook)

| Target | RVA (verified build) | Area |
|---|---|---|
| `pollInput` | `0xAA080` | input; per tick |
| `cursorAim` | `0xBF680` | aim |
| `cursorPosition` | `0x22710` | cursor; told apart by return site |
| `cursorWarp` | `0x22780` | cursor; the pick-up warp is skipped |
| `explore` | `0xBFEA0` | movement (out of combat) |
| `combatStep` | `0xC13F0` | movement (combat) |
| `travelTarget` | `0x19390` | movement |
| `walkGait` | `0x163EF0` | movement |
| `poseBuilder` | `0x1760F0` | skeleton: spine bend, crouch, head stabilisation; per tick |
| `partSettle` | `0x89800` | skeleton: head stabiliser; per tick |
| `sectorUpdate` | `0x58CD0` | rendering: hides meshes that would cover the view |
| `coverPass`, `coverDepth` | `0x144610`, `0x1444A0` | rendering: tile covers skipped in first person |
| `pickRay`, `planeHit`, `grabPick` | `0x8F720`, `0x8F530`, `0x8FB30` | picking and grabbing |
| `dragPlace`, `dragDrop` | `0x1183C0`, `0x119C30` | held objects |
| `doorHandler`, `hingeHandler` | `0x11B400`, `0x11BC50` | doors and lids (pass-through; the trampolines drive them each frame) |
| `opengl32!wglSwapBuffers` | (export) | per frame: settings window, crosshair, swing cue |

## Replaced vtable slots

| Slot | Holds (checked) | Replaced by |
|---|---|---|
| `navSlot` | `navSampler` | `nav`: the camera; per tick |
| `behaviorSlot` | `playerBehavior` | `behavior`: aim and input; per tick |
| `pointSlot` | `aimPoint` | `point`: the aim point |

## Called, not hooked

`rebuildProjection`, `rebuildInverse`, `levelRay`, `levelRay2`, `headingFromDirection`,
`aimFilterPush`, `aimFilterUpdate`.

## Return sites

Some hooks act only for particular callers, which they recognise by return address: `uiPollCursorReturn`,
`crosshairCursorReturn`, `exploreTravelReturnA/B`, `pickupWarpReturn`, `dropSpotPickReturn`,
`grabStartPickReturn`, `hoverPickReturn`, `doorPlaneReturn`, `hingePlaneReturnA/B/C`.

## Globals

28 globals, read or written (`playerRoot`, `currentScene`, `keyWord`, `buttonWord`, `dragManager`,
`mainViewport`, ...). See `tools/targets.json`. Each is found through two independent RIP-relative
references in the game's code.

## Not covered by signatures

Struct field offsets are named in `src/game_structs.h` (`field::character::headPart`,
`field::character::combatStance`, and so on). If an instruction inside a matched signature uses the same offset, the
match implies it is unchanged. Other offsets are not checked.

## Adding a target

1. Add it to `tools/targets.json` with its RVA in the build you have.
2. `python tools/signatures.py generate <that Exanima.exe>`.
3. Use it as `gameAt(fp::sig::yourName)`. A hook's detour that runs every tick should also count itself
   (`healthHit`) if the mod cannot work without it. Add it to `fp::HealthHook` in that case.
