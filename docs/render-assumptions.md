# Render assumptions audit

Branch `claude/project-thread-abkkik`, Exanima 0.9.5.2g (SHA-256 af9033c7…, the reviewed build), 2026-11-05.
This covers every render effect and setting on the View and Graphics pages, plus the hooks and data they rely on.

## Legend

- **PROVEN**: verified in this audit from the game code that reads or writes the value. The disassembly
  address is given (tools: capstone over the installed `Exanima.exe`, `.pdata` function bounds).
- **WITNESSED**: the offset is pinned by a startup contract (`tools/native_contracts.json`, checked by
  `python tools/check_game.py` and at every arm). The contract proves the game code at that address reads or
  writes the offset. Its *meaning* comes from the contract's provenance text, written at an earlier review, and
  was **not** re-derived in this audit.
- **BYTE-EXACT**: the whole game function is checked byte for byte, in the file and in the loaded image,
  before its hook or call is made (an optional group in `targets.json`).
- **UNPROVEN**: no game code has been shown to give the value the meaning the mod uses.
- **WRONG**: game code shows a different meaning.
- **OURS**: a choice the mod makes (shader look, thresholds, ranges). It is not a claim about game data.

GL state handling ("render audit") means the dev-build audit (`[Dev] RenderAudit=1`, `view_effects.inl`
`renderAuditBegin`/`End`). Around each mod pass it compares GL state and the game's texture-unit cache, and it
checks every effect input against the expected texture and size. Run 1 (Hunter, 2026-11-05, v1 build): 0 input
mismatches, 0 cache desyncs caused by mod passes, one state leak (replay blend, since fixed in 478b214).

## Game mechanisms the render code depends on (proven in this audit)

| Mechanism | Evidence | Status |
|---|---|---|
| Frame order: world pass, then a default material bind, then post, then UI | 0x4BAB0 calls the world pass (0xA0CE0 single, 0x4B6D0 supersampled, 0xA0CE0 offscreen at 0x4BF09). Then `call 0x73F20` with material 0x2AC180 (0x4BF5E), 0x25350 (0x4BF6A), the post copy 0x9FF90/0xA0640 and the UI. The mod's effects run on the world pass's return, so they run before the post and the HUD. | PROVEN |
| Texture binding goes through a cache | 0x6FC30(unit, object): it binds only when `DAT_49B600[unit*16]` differs from the object, and calls glActiveTexture only when the unit differs from `DAT_49B5F0`. Target is `object+0x40`, GL name is `object+0x58` (0x6FCE8-0x6FCF4). | PROVEN, BYTE-EXACT (silhouette group) |
| Material bind | 0x73F20: stores `DAT_49B800`, binds `material+0x48/0x50/0x58/0x60/0x68` on units 0-4 through 0x6FC30, sets blend through 0x73E90 (no cache), client arrays through 0x6ACD0 (cache `DAT_49B510`), cull through 0x73E30 (cache `DAT_49B820`). | PROVEN, BYTE-EXACT |
| Per-node submit binds the skin texture set | 0x200770 (single-sample submit): `node+0x1D0[index]` is the material, its `+0x100` is a texture set, and `set+0x10/+0x18/+0x20` are bound on units 0/1/2 through 0x6FC30 (0x200858-0x2008CF). | PROVEN |
| Skinned mesh layout | 0xD9350 (material sort) reads `node+0x1C8` (last material), `+0x1D0` (material table), `+0x268` (last face), `+0x2F8` (face tags). 0x69520 reads `+0x2F8` and `material+0xB0/+0xB4`. 0x5E340 (triangle emitter) reads `material+0x1C`, `+0xB0`, `+0xB4`, `+0xB8`, `+0xE8`. 0xD39B0 (bone register) reads `skin+0x10` (count) and `skin+0x18`/`+0x20` (arrays), and stores the id at `node+0x17C`. | Offsets PROVEN; per-field meaning (for example tag 0 = the body) from the 2026-10-20 review, not re-derived |
| HUD vitals | 0x13E0F0: left bar 0x13C880 gets `[char+0xD70]/[char+0x103C]` and `[char+0xD5C]/[char+0x103C]` (0x13E327-0x13E366); right bar 0x13C470 gets `[char+0xD74]` and `[char+0xD78]` (0x13E3A5-0x13E3B6). 0x17A100 writes `+0xD70` from `+0xD6C` and `+0xD58` (0x17A1DA-0x17A1F7). So the left bar's fill follows `+0xD58`. | PROVEN (which bar the player calls health is the HUD's own layout) |
| Engine log / on-screen lines | `DAT_2AB6B0` is set to 0x25D80 at 0x26D6F. 0x25D80 concatenates and appends to the log list; level 4 or 5 goes to 0x259A0, a 16-line ring (2 s + 50 ms per character); 0x25A50 draws it every frame (from 0x26150). The text draw (0x27FA0/0x28750) treats only CR, LF and space specially. | PROVEN, BYTE-EXACT (engineLog group) |

## Per setting

| Setting (ini key) | Game code or data it reads or patches | Proof | GL state | Verdict |
|---|---|---|---|---|
| Ambient occlusion / Contact shadows (`View/AmbientOcclusion`) | Hooks 0xA0CE0/0x4B6D0 return; window colour and depth copied by GL blit; view matrices `view+0x60`/`+0xE0` | Hooks BYTE-EXACT (viewEffects); matrices WITNESSED (`view::renderView/renderProjection`); frame order PROVEN | Audit: inputs bound and checked (fixed 7abc04a); held pixels kept out (85ec084) | PROVEN for game data; look is OURS |
| Depth of field (`View/DepthOfField`) | Same; centre depth read back | as above | audit clean | PROVEN; look OURS |
| Screen-space reflections (`View/ScreenSpaceReflections`) | Same; world up from view matrix elements 4..6 | WITNESSED matrices | audit clean | PROVEN for data. Floor weighting by colour (478b214) is OURS, not the game's specular data (that needs the material set, see above) |
| Edge smoothing (`View/EdgeSmoothing`) | Image only; "supersampled" from which hook ran | Hooks BYTE-EXACT | audit clean | PROVEN; look OURS |
| Supersampling adaptation (no row; the game's own Supersample) | 0x4B6D0; `DAT_49CA10` sample index, `DAT_2AFC60` sample count, `DAT_49ACD0` per-sample viewports, `DAT_49A4F0` bound framebuffer; matrix hook | Globals by Relative signatures with reviewed notes (writers 0x4B857, 0x4B783, 0x4BA01) | n/a | WITNESSED; not re-derived in this audit |
| Hide helmet and hair (`View/Helmet`) | Skinned mesh layout and tags (above); material face ranges; descriptor `material+0x68` (+0x14 indices); `renderer+0x80` configuration (+0xC uniforms, +0x14 sample location) | Mesh offsets PROVEN (0xD9350/0x69520/0x5E340); headwear functions BYTE-EXACT (headwearRender); descriptor `+0x14` and renderer `+0x80/+0xC/+0x14` UNPROVEN in this audit | replay under cueLock, restored raw (not audited per pass) | Mostly PROVEN; the descriptor and renderer-uniform reads are UNPROVEN. Not gated: they are read only inside byte-exact owners whose code is unchanged, and a wrong value fails the draw closed (the native draw runs). Flagged for the audit thread |
| Helmet view (`View/HelmetView`) | Whether headwear was seen (the headwear path) | as Hide helmet | audit clean | as above |
| See yourself through walls (`View/PlayerGhost`) | Highlight pass 0x4F1D0 hook; replays native bind 0x73F20, per-node submit `material+0x80`, part draw `part+0x90(part+0x98)` | Pass, camera pass, bind and framebuffer bind BYTE-EXACT (silhouette); `+0x80`/`+0x90` checked at runtime to equal contracted 0x201630/0x6B8B0; cache restore PROVEN (6FC30) | Audit found the blend leak (fixed 478b214); cache restored (7abc04a) | PROVEN |
| Held item detail (`View/HeldItemDetail`) | Same replay mask; held node `renderTransform`, `sector+0x64` origin; material kind from a readback of unit-0 texture | Replay PROVEN; `sceneNode::renderTransform`, `sector::origin` WITNESSED; material kind is OURS | as above | Core PROVEN. **Light** (light-node `+0x210/+0x21C/+0x224`, sector list `+0x130`, origin `+0x64`): UNPROVEN, **gated off**. **Wear and dirt** (item record: `node+0x180` id, database rows `+0x10/+0x18`, row `+0x8/+0xC`): UNPROVEN, **gated off**. Weapon blood spot source: see Wounds |
| Armour and clothing detail (`View/ArmourDetail`) | Same as held items, for worn pieces | as above | as above | as above (same gates) |
| World object detail (`View/ObjectDetail`) | Same, for nearby placed objects | as above | as above | as above (same gates) |
| Level detail (`View/LevelDetail`) | People within 4 m (`worldObject::characters`); the two nearest lamps (light-node fields) | Roster WITNESSED; lamps UNPROVEN | audit clean | Surface detail PROVEN (image only, OURS). **Lamps gated off** |
| Draw distance (`View/DrawDistance`) | Writes cull marks on sector meshes (`sector::meshes`), camera pose, sector origin | WITNESSED (`camera::pose`, `sector::meshes`, `sector::origin`) | no GL | WITNESSED; meaning not re-derived. Audit thread |
| Lights per object (`View/LightsPerObject`) | Writes `lightEntry::active` on per-node light lists (`sceneNode::lights`) and restores at the swap | WITNESSED (lights group) | no GL | WITNESSED; meaning not re-derived. Audit thread |
| Finger poses (`View/FingerPoses`, `FingerPosesOthers`) | Bone register 0xD39B0 and skin arrays | `skin+0x10/+0x18/+0x20`, `node+0x17C` PROVEN in 0xD39B0 | no GL | Data PROVEN; pose choice OURS; animation side for the audit thread |
| Hurt and eye adjustment (`View/ConditionEffects`, was "Hurt and winded") | Player health `character+0xD58` over `+0x103C` | PROVEN: the HUD's left bar fills to `+0xD70` (0x13E327), which 0x17A100 writes from `+0xD6C` and `+0xD58` (0x17A1DA-0x17A1F7), over `+0x103C`; `+0xD5C` is the bar's pale second layer (0x13E35E). This branch now reads `+0xD58` (named `postureReadiness` here; `health` in the playtest after 8389d19) | n/a | Hurt PROVEN. **Winded WRONG, removed** (see below). Eye adjustment reads the image only (OURS) |
| Smooth lighting (`View/SmoothLighting`) | glShaderSource hook: rewrites a game shader only if its text declares `uniform sampler3D VoxelGI` and calls `texture(VoxelGI,` | The match is on the game's own shader text; the rewrite must compile or the original is kept (`rewriteCompiles`) | n/a | PROVEN (by text match) |
| Wounds (`View/Wounds`) | Hits: `character+0xC20` target part, `+0xC28` point, `+0xC38` weapon (now through `field::character::impact*`); part pose; `character::lifeFlags`; bleed value; weapon `+0xF0`/`+0x120` as transforms | impact fields WITNESSED (collision collector review); weapon transforms UNPROVEN, **gated off** (cut direction then picked on the surface) | wound pass audited with the screen effects | Mostly WITNESSED. Retirement: a mark is dropped when its part is unreadable, the part's character pointer changed, or reading the character faults (wounds.inl ~531-550). "Retired = character removed" is UNPROVEN (part memory reuse also retires) |
| Wound skins (part of Wounds) | Skin mesh (above); texture set `material+0x100`, `set+0x10/+0x18/+0x20` on units 0/1/2; GL size and format queried on the copy | Set and units PROVEN (0x200770); unit roles (diffuse/spec/normal) from the shading review, not re-derived | bake restores its own snapshot; not per-pass audited | PROVEN for the data; unit roles UNPROVEN in this audit |
| Stone skin | Same texture set and units | as above | as above | as wound skins |
| Wound debug highlight (`View/WoundDebug`) | none (shader colour) | OURS | n/a | n/a |
| Draw far rooms (`View/FarRooms`) | level, room and sector fields (farRooms group) | WITNESSED (2026-11-04 hardening), hash-gated | no GL | WITNESSED; audit thread |
| VR effects (`VR/Effects`) | Runs AO and SSR per eye in the eye framebuffers | Same as AO/SSR; VR contracts whole-function | eye path not audited in game | PROVEN data; eye path not run in this audit |
| Mod messages (no row) | Engine log above | PROVEN | n/a | PROVEN |

## Corrected by this audit (from the audit thread's decoding, spot-checked here)

- **Winded was WRONG; removed.** It read `+0xD74`/`+0xD78` (the HUD's right, blue bar, 0x13E3A5/0x13E3B6) as
  stamina. That bar is spent only through 0x17A640, and all 13 callers are methods of spell and power effect
  classes (by their VMT class names): TForceBlastEffect 0x232060, TForceWaveEffect 0x235210,
  TForceTorrentEffect 0x236620, TForceBarrierEffect 0x23CC30, TPwrForceKinesis_S 0x246200, TPwrDispVanish_I
  0x255A50, TPwrDispLongDoor_I 0x256190, TPwrEnergyShock_I 0x28EF30, and one unnamed (0x2473C0). It is magic.
  No game code was found for the "low-stamina idle" the old comment named. The setting is now "Hurt and eye
  adjustment"; the shader's winded input stays 0. (Before 8389d19 this branch read `+0x10B8`/`+0x10BC`, which
  the HUD does not draw at all.)
- **Death check.** PROVEN: `stateFlags +0xDF4 = 0x800` is written by 0x156480 (0x1564D6). The damage
  function 0x159700 calls it (0x1598C3) right after clamping health to its minimum (0x15988C-0x15989D). Life
  flag `+0xDF8 | 0x100` is set at one place only, 0x17B248 in 0x17B100. That function also zeroes `+0xDF4`
  and plays motion 3, and its callers include TTendrilEmitter (0x1A2CE0) and 0x1A1550, 0x28F4F0. It is not
  death. Fixed in render code: wound healing now tests `+0xDF4 & 0x800` (wounds.inl). overlay.inl's
  crouch gate already covers 0x800 (in its 0xA00 mask), so I left its 0x100 test in place and commented it.
  Other uses of `lifeFlags & 0x100` as "dead", outside render, are for the audit thread: camera.inl
  ~113 and ~635, camera_motion.h ~480-482 (knockdown follow), combat.inl ~12, input.inl ~650 and ~1466,
  motions.inl ~22, xr_bones.inl ~765, xr_drive.inl ~545.

## Gated off by this audit (`unprovenRenderReads = false`, src/mod/wounds.inl)

1. Held, armour and object light direction and colour from light nodes (`heldLightFor`, view_effects.inl). They now use the default light from above.
2. Held and worn item wear and dirt from the item database record (`heldConditionOf`). They now read as unknown, which the shader already handles.
3. Level detail's two nearest lamps (`levelLightsFor`). Now no lamps.
4. A weapon's `+0xF0`/`+0x120` transforms for a cut's direction (wounds.inl). The direction is now picked on the surface. The weapon's identity (`impactWeapon`) is still used.

Each comes back on when its fields are proven (a contract witness plus a decoded reader) and the flag is
flipped. The blood thread's level lamps (`lampsInView`, branch tig8fi) depend on the same light-node fields.

## Not yet proven, not gated (for the audit thread)

- Headwear: `descriptor+0x14` index pointer and the `renderer+0x80 → +0xC → +0x14` sample-uniform chain
  (headwear_render.inl ~189, ~568-579). Both are read inside byte-exact owners; a wrong value fails the draw
  closed.
- Unit roles of the skin texture set (0 diffuse, 1 specular, 2 normal): from the shader review, not re-derived.
- Supersampling globals: meaning from the notes in targets.json, not re-derived in this audit.
- Draw distance, Lights per object, Far rooms: offsets witnessed; meaning not re-derived in this audit.
