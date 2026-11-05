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
| Hide helmet and hair (`View/Helmet`) — see also "Headwear reads" below | Skinned mesh layout and tags (above); material face ranges; descriptor `material+0x68` (+0x14 indices); `renderer+0x80` configuration (+0xC uniforms, +0x14 sample location) | Mesh offsets PROVEN (0xD9350/0x69520/0x5E340); headwear functions BYTE-EXACT (headwearRender); descriptor `+0x14` and renderer `+0x80/+0xC/+0x14` UNPROVEN in this audit | replay under cueLock, restored raw (not audited per pass) | Mostly PROVEN; the descriptor and renderer-uniform reads are UNPROVEN. Not gated: they are read only inside byte-exact owners whose code is unchanged, and a wrong value fails the draw closed (the native draw runs). Flagged for the audit thread |
| Helmet view (`View/HelmetView`) | Whether headwear was seen (the headwear path) | as Hide helmet | audit clean | as above |
| See yourself through walls (`View/PlayerGhost`) | Highlight pass 0x4F1D0 hook; replays native bind 0x73F20, per-node submit `material+0x80`, part draw `part+0x90(part+0x98)` | Pass, camera pass, bind and framebuffer bind BYTE-EXACT (silhouette); `+0x80`/`+0x90` checked at runtime to equal contracted 0x201630/0x6B8B0; cache restore PROVEN (6FC30) | Audit found the blend leak (fixed 478b214); cache restored (7abc04a) | PROVEN |
| Held item detail (`View/HeldItemDetail`) — **off: colour material guess** | Same replay mask; held node `renderTransform`, `sector+0x64` origin; material kind from a readback of unit-0 texture | Replay PROVEN; `sceneNode::renderTransform`, `sector::origin` WITNESSED; material kind is OURS | as above | Core PROVEN. **Light** (light-node `+0x210/+0x21C/+0x224`, sector list `+0x130`, origin `+0x64`): UNPROVEN, **gated off**. **Wear and dirt** (item record: `node+0x180` id, database rows `+0x10/+0x18`, row `+0x8/+0xC`): UNPROVEN, **gated off**. Weapon blood spot source: see Wounds |
| Armour and clothing detail (`View/ArmourDetail`) — **off: colour material guess** | Same as held items, for worn pieces | as above | as above | as above (same gates) |
| World object detail (`View/ObjectDetail`) — **off: colour material guess** | Same, for nearby placed objects | as above | as above | as above (same gates) |
| Level detail (`View/LevelDetail`) — **off: colour material guess** | People within 4 m (`worldObject::characters`); the two nearest lamps (light-node fields) | Roster WITNESSED; lamps UNPROVEN | audit clean | Surface detail PROVEN (image only, OURS). **Lamps gated off** |
| Draw distance (`View/DrawDistance`) | Clears the frustum bits on sector meshes | PROVEN (0x58EFB; see "Witnessed fields re-checked") | no GL | PROVEN |
| Lights per object (`View/LightsPerObject`) | Writes `lightEntry::active` on per-node light lists and restores at the swap | PROVEN (0x26F420; see "Witnessed fields re-checked") | no GL | PROVEN |
| Finger poses (`View/FingerPoses`, `FingerPosesOthers`) | Bone register 0xD39B0 and skin arrays | `skin+0x10/+0x18/+0x20`, `node+0x17C` PROVEN in 0xD39B0 | no GL | Data PROVEN; pose choice OURS; animation side for the audit thread |
| Hurt and eye adjustment (`View/ConditionEffects`, was "Hurt and winded") | Player health `character+0xD58` over `+0x103C` | PROVEN: the HUD's left bar fills to `+0xD70` (0x13E327), which 0x17A100 writes from `+0xD6C` and `+0xD58` (0x17A1DA-0x17A1F7), over `+0x103C`; `+0xD5C` is the bar's pale second layer (0x13E35E). This branch now reads `+0xD58` (named `postureReadiness` here; `health` in the playtest after 8389d19) | n/a | Hurt PROVEN. **Winded WRONG, removed** (see below). Eye adjustment reads the image only (OURS) |
| Smooth lighting (`View/SmoothLighting`) | glShaderSource hook: rewrites a game shader only if its text declares `uniform sampler3D VoxelGI` and calls `texture(VoxelGI,` | The match is on the game's own shader text; the rewrite must compile or the original is kept (`rewriteCompiles`) | n/a | PROVEN (by text match) |
| Wounds (`View/Wounds`) | Hits: `character+0xC20` target part, `+0xC28` point, `+0xC38` weapon (now through `field::character::impact*`); part pose; `character::lifeFlags`; bleed value; weapon `+0xF0`/`+0x120` as transforms | impact fields WITNESSED (collision collector review); weapon transforms UNPROVEN, **gated off** (cut direction then picked on the surface) | wound pass audited with the screen effects | Mostly WITNESSED. Retirement: a mark is dropped when its part is unreadable, the part's character pointer changed, or reading the character faults (wounds.inl ~531-550). "Retired = character removed" is UNPROVEN (part memory reuse also retires) |
| Wound skins (part of Wounds) | Skin mesh (above); texture set `material+0x100`, `set+0x10/+0x18/+0x20` on units 0/1/2; GL size and format queried on the copy | Set and units PROVEN (0x200770); unit roles (diffuse/spec/normal) from the shading review, not re-derived | bake restores its own snapshot; not per-pass audited | PROVEN for the data; unit roles UNPROVEN in this audit |
| Stone skin | Same texture set and units | as above | as above | as wound skins |
| Wound debug highlight (`View/WoundDebug`) | none (shader colour) | OURS | n/a | n/a |
| Draw far rooms (`View/FarRooms`) | level, room and sector fields (farRooms group) | PROVEN (0x143C20, 0x5B170; see "Witnessed fields re-checked") | no GL | PROVEN |
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
- **Death check.** PROVEN from the game's writers and tests of `stateFlags +0xDF4`:
  - It is a state value whose values combine bits. The game writes 0, 1, 2, 8, 0x20, 0xC0, 0x100, 0x200,
    0x300, 0x600, 0x800, 0xA00, 0x8000, 0x10000, 0x10004 (all `mov dword [char+0xDF4], imm` in `.pdata`
    functions).
  - It tests it both ways. By equality: `cmp …, 0xA00` 55 times, `cmp 2` 52, `cmp 0x300` 23, `cmp 1` 14. By
    mask after a load: `and 0x200` 34 times, `and 0x800` 32, `and 0xA00` 12, `and 0xAC0` 7, `and 0x40204` once.
  - 0x800 is written by 0x156480 (0x1564D6), which the damage function 0x159700 calls (0x1598C3) right after
    clamping health to its minimum (0x15988C-0x15989D), and by 0x167ED0. 0xA00 (= 0x800 | 0x200) is written
    by 0xF1220 (0xF12C4), 0x156D20 (0x156E48) and 0x1A0970 (0x1A0A95).
  - Among all written values, `& 0x800` holds for exactly 0x800 and 0xA00, and it is the test the game itself
    uses 32 times. **So dead is `(+0xDF4 & 0x800) != 0`**, the game's own test, true for exactly 0x800 and
    0xA00 among the stored values (the form agreed across threads). Equality with 0x800 alone would miss
    0xA00. The audit thread reads 0x800 as dying (still ragdolling) and 0xA00 as a settled corpse (physics
    off); that split is UNPROVEN here. Of the 0xA00 writers I only looked at 0x156D20, which calls 0x162E80
    first (it clears `+0x12F3` and calls 0x179D50); that is not enough to name the state.
  - Life flag `+0xDF8 | 0x100` is set in one place only, 0x17B248 in 0x17B100, which also zeroes `+0xDF4` and
    plays motion 3; its callers include TTendrilEmitter (0x1A2CE0), 0x1A1550 and 0x28F4F0. It is not death.
  - Fixed in render code: wound healing tests `+0xDF4 & 0x800` (wounds.inl).
  - overlay.inl's crouch gate masks the state with `0xAC0 | 0x40204 | 0xA00`. Those are masks the game itself
    applies to `+0xDF4` (`and 0xAC0` at 0x16388A and 6 more, `and 0x40204` at 0xBFF90, `and 0xA00` at 0xAB3E7
    and 11 more), and the union covers both dead values. Its extra life-flag 0x100 test was meant as "dead";
    since only the spell collapse 0x17B100 sets that bit, it is removed.
  - Other uses of `lifeFlags & 0x100` as "dead", outside render, are for the audit thread: camera.inl ~113
    and ~635, camera_motion.h ~480-482 (knockdown follow), combat.inl ~12, input.inl ~650 and ~1466,
    motions.inl ~22, xr_bones.inl ~765, xr_drive.inl ~545.

- **Knockdown.** Low health alone does not knock a character down (audit thread); the knocked-down state is
  still being traced. No render logic depends on it.

## Gated off by this audit (`unprovenRenderReads = false`, src/mod/wounds.inl)

1. Held, armour and object light direction and colour from light nodes (`heldLightFor`, view_effects.inl). They now use the default light from above.
2. Held and worn item wear and dirt from the item database record (`heldConditionOf`). They now read as unknown, which the shader already handles.
3. Level detail's two nearest lamps (`levelLightsFor`). Now no lamps.
4. ~~A weapon's `+0xF0`/`+0x120` transforms for a cut's direction~~: **PROVEN and back on** (8d7e359+). The
   value at `character+0xC38` is the first argument of 0x1590D0 (the collision consumer that stores it), a
   physics part with its pose at `+0x2A0`. The part step 0x89330 builds that pose from the four points at
   `+0xF0` (call 0x7ECD0, as in the witnessed partSettle at 0x89800), then copies `+0x120 → +0x150` and
   `+0xF0 → +0x120` (0x8956A-0x89597). So `+0xF0` is the four current points and `+0x120` the four of the step
   before; their centroid difference is the part's motion over one step.
   Independent witnesses (audit thread): heldItemAttach 0x155C20 writes the attached item's four points to
   `+0xF0` and copies them to `+0x120` and `+0x150` (0x155D72-0x155D9F); TTetraRBD's integrate method 0x88D30
   (in the TTetraRBD VMT) runs the Verlet step over `+0xF0`/`+0x120`/`+0x150`.

Each comes back on when its fields are proven (a contract witness plus a decoded reader) and the flag is
flipped. The blood thread's level lamps (`lampsInView`, branch tig8fi) depend on the same light-node fields.

## Progress on the gated items

- **Light-node fields (items 1 and 3): fields PROVEN, use still UNPROVEN, so still gated.**
  - TLightSource is the class named by the ShortString at 0x2FD048, VMT at 0x2FCF40. Its instance size is 744
    (0x2E8), matching `lightSize`.
  - Its update 0x51EB0 (blood thread's decoding, checked here):
    - It clamps `+0x220` to at least 1.19e-7 (0x51EB9-0x51ED2).
    - It converts `+0x210` (sRGB colour, 3 floats) to linear into `+0x278` with 0x269D50, the exact sRGB curve
      (0.04045, 12.92, 0.055, 1.055, 2.4; 0x269D5D-0x269DA8).
    - It scales `+0x278` by `sqrt(+0x220) · (+0x21C) · 3` or by `sqrt(+0x220 · +0x21C) · k`, depending on the
      byte at 0x2AC0D0 (0x51F67-0x51FC5; 0x19260 multiplies a vec3).
    - It copies `+0x224` (range) to `+0x2A4`, under a threshold on `+0x21C` (0x51FCA-0x5200C).
  - So `+0x21C` and `+0x220` are both brightness factors, and the code's "+0x21C is intensity" alone is
    incomplete.
  - **Correction (blood thread): the lit shaders do not get `+0x278`/`+0x2A4`.** The per-mesh light walk
    (0x1FF462-0x1FF504) packs three vec4 per light into renderer state `+0x170` (stride 0x30, 0x1FF740-0x1FF9D8).
    A is a position from a table at `+0x40` indexed by light `+0x288`, with w = `+0x17C`. B is the raw `+0x210`
    colour × 1/state `+0x3B8` × (1 − `[light+0x1C8]+0x234`) × state `+0x5B4`, with w = light `+0x230`. C comes from
    `+0x238`, plus spot-cone values for type 0x200.
  - Checked here: the upload at 0x200BA5-0x200BBD (inside the byte-verified 0x200770) calls 0x498F40 with
    location `[[r13+0xC]+8]`, count 3 × state `+0xA8` and data state `+0x170`. The pack details are the blood
    thread's and not re-read here.
  - **Plan:** read the game's own packed vec4s (state `+0x170`, count `+0xA8`) after the pack runs, once the
    shader dump shows how the lit shader uses A, B and C. Do not switch to `+0x278`/`+0x2A4`. Lighting stays
    off until then.
- **Item wear and dirt (item 2): mostly proven, still gated.**
  - Lookup, PROVEN. 0x103DB0 picks the table by the id's top bits (0x80000000 level `DAT_4EF930`, 0x40000000
    world `DAT_4EF940`, else `DAT_4EF920`). 0x104660 binary-searches `count +8` rows of 16 bytes at `+0x10` by
    `[row+0]`. 0x104B10 returns the record `data(+0x18) + row[+8]` with size `row[+0xC]`. This is exactly the
    mod's read.
  - Chunks, PROVEN. 0x103A90 walks `[+0]` tag and `[+4]` size inside the record, as the mod does.
  - Byte placement, PROVEN. The chunk writers move the same three source bytes (`+8`, `+9`, `+0xA`): for tag
    0x4000 to chunk `+0x14..+0x16` (0x12DED4-0x12DF15), for tag 0x1000 to chunk `+0x1B8` block bytes 1-2
    (0x12DFCC-0x12E025). Those are the offsets the mod reads.
  - Byte order, PROVEN for the game's item editor form. 0x296D60 copies a 100-byte block to `form+0x170` and
    shows byte 0 in its "Quality" widget, byte 1 in "Wear " and byte 2 in "Dirt " (labels at 0x3528E0,
    0x352940, 0x352988; the setters 0x295480/0x295640/0x2957C0 write `+0x170`/`+0x171`/`+0x172`).
  - **Missing:** (1) that the form's block is the record chunk at the offsets above (0x296CE0 copies
    `form+0x264` into chunk 0x4000's payload, a different form offset, not yet tied together); (2) that
    `+0x180` on the held item's scene node is that item id. 0x118F80 passes `[rbx+0x180]` to the lookup, but
    rbx's class is not yet named.
- **Skin texture unit roles: PROVEN, and now checked at every swap.**
  - The game's shader sources are decoded from its own files (`tools/extract_game_shaders.py`). Resource.rpk ->
    shading.flb share one pack format. Each body is dword-rotated by the key stream of 0x1FA4D0 (called from
    0x1FA6F0, seed 0x2BCBE389). The main lit shader `shading_f` samples `DifTex`, `SpcTex`, `NrmTex`, `GlsTex`
    and so on.
  - Units: after linking, 0x2010C0 gives the samplers consecutive units in this order, each only if the
    permutation flags at `renderer+8` have it: DifTex 0x20000, OpcTex 0x480000, EmiTex 0x80000, SpcTex
    0x40000, NrmTex 0x200000, GlsTex 0x1000000, then DspTex, DrtTex, EnvMap, VoxelGI.
  - So units 0/1/2 are diffuse/specular/normal exactly when the flags have those three and neither an opacity
    nor an emissive map. `stoneSkinOriginals` (stone_skin.inl) now checks that on the live flags and leaves
    any other material to the game's own draw. Wound skins and stone skin go through it.
- **Headwear reads (e).**
  - **Descriptor: PROVEN.** The native part draw 0x6B8B0 reads `+0x68` as the descriptor:
    - `[+0]` is the element buffer, bound to 0x8893 when nonzero;
    - `[+4]` is the index width (1 byte 0x1401, 2 short 0x1403, otherwise int 0x1405);
    - `[+8]` is the number of ranges, each at `+0xC` as mode, count and indices; the indices are the qword at
      `+0x14` (an offset into the buffer) or, without a buffer, inline from `+0x14` (0x6B910-0x6B984).
    - That is exactly what headwear_render.inl ~184-189 reads. It also checks at runtime that the mode is
      GL_TRIANGLES and the count is faces × 3.
  - **Renderer uniforms: PROVEN for the matrix location.** 0x200770 loads `renderer+0x80` (r13), its `+0xC`
    table, and uploads two 4×4 matrices at the table's `[+0]` location (0x200B73-0x200B81, count 2 from
    `+0x3C0`), as the mod does. `configuration+8` equal to the current program is the mod's runtime guard.
  - **The sample-index location `[+0x14]` is UNPROVEN** (not found in the game's submits): with several
    samples (the game's Supersample) the mount is now not attempted and the native draw stays (gated,
    headwear_render.inl).

- **Witnessed fields re-checked (f).**
  - **Supersampling globals: PROVEN.** 0x4B6D0 writes the sample index `DAT_49CA10` from its loop counter r13d
    (0x4B857) and resets it to 0 (0x4BA2E). It writes the sample count `DAT_2AFC60` (0x4B783) and resets it to 1
    (0x4BA02).
  - **Wound hit fields: PROVEN.** 0x1590D0(rcx = part A, rdx = part B, …, [rbp+0x30] = point) takes A's owner
    character `rdi = [A+0x580]` and skips parts of the same owner (`+0x420`). On that character it writes:
    `+0xC18` the hit kind (1-3), `+0xC20` part B (the part struck), `+0xC28` the 12-byte contact point (the
    stack argument), `+0xC38` part A (its own part that made contact, see +F0/+120 above). Wounds place the mark
    on `+0xC20`, as intended.
  - **Lights per object: PROVEN.** 0x26F420 walks a mesh's list from the head `+0x1DC` through entries at
    `+0x1E0` (0x18 bytes each). It continues while `[entry+0xC]` (active) is nonzero and the weight `[entry+0]`
    is above a threshold, follows `[entry+8]` (next index, negative ends), and stops after min(sector `+0x1F0`,
    6). Clearing an entry's active flag ends the walk there, as view.inl does. The queue 0x26F230 and the binder
    were not re-read.
  - **Draw distance: PROVEN.** The sector update's frustum pass ORs `0x1000001` into `mesh+0xE0` when 0x79E60
    accepts the mesh's sphere at `mesh+0x20` (0x58EEE-0x58EFB). Draw distance clears exactly those bits on the
    sector's meshes (`sector+0x1D0` list, last index `+0x1C8`; both witnessed in pickRay), as if outside the
    view. `sceneNode::bodyPart +0x160` and `bodyPart::character +0x580` are witnessed by their writers.
  - **Far rooms: PROVEN.**
    - The room sort 0x143C20 walks `level+0x1C` rooms (0x144205) in entries of 0x54 bytes at `level+0x20`. When
      an entry's state `+0x30` differs from its cached `+0x2C`, it sets the room's sector (`entry+0x34`) `+0x318`
      to 2 for state 1, 1 for state 2 and 0 otherwise (0x14423A-0x14427A).
    - Its neighbour walk reads `entry+0x18` (edge count) and `entry+0x1C` (8-byte pairs, edge index at `+4`),
      and maps an edge to a room through `level+0x28`, numbered from 1 (0x143DD8-0x143E9C).
    - The render walk in 0x5B170 reads `+0x318`: 0 clears the sector's mesh masks (not drawn, 0x5B439-0x5B470);
      2 walks the sector's portals (`+0x268`, 0x58 bytes, neighbour sector at `+0x40`) and draws through those
      into sectors with 1 (0x5B49B-0x5B56E).
    - That is exactly the layout view.inl reads and the meaning it relies on.

## Colour heuristics standing in for game material data

These are the mod's guesses about game materials from a pixel's or texture's colour. They are not game data, so
under the release rule they are off unless proven (`colourMaterialGuesses = false`, src/mod/wounds.inl):

| Where | Guess | State |
|---|---|---|
| Held, armour and object detail (`heldSurface`, view_effects.inl) | Material kind (steel, wood, leather, mail, paint, cloth) from the item texture's average colour (CPU readback of the game's unit-0 texture, `heldMaterialCode`), the pixel's saturation and warmth, and the blade-vs-haft split from a spot being warmer or greyer than the item average | **Off**: the held/worn/object mask replay and the texture readback do not run, so these three detail rows draw nothing |
| Level detail (`levelSurface`) | Wood, earth, metal, plaster or stone from the pixel's colour, brightness and facing | **Off**: level detail draws nothing |
| Reflections (`ssrShader`) | Floor reflectivity by colour class (478b214) | **Removed.** Reflections are off and greyed out until the game's own specular data feeds them (Hunter: yes; plan below) |
| Helmet view (`finishShader` visor) | "Anything within 18 cm of the eye is helmet" (a depth guess) | **Off**, greyed out, until the helmet's own geometry shapes it |
| See yourself through walls | A capsule of 25 cm radius standing in for the body, both as the fallback ghost and to ignore the body's own limbs | **Replaced**: the ghost is drawn only from the game's body draw (mask R), and a second channel (B) marks where the body itself is the nearest surface, from the same draw. No capsule. Needs an in-game check |
| Wound marks (`woundShader`) | A grey, unsaturated surface is metal (smears, no soak) | Blood thread's code; they turned their gear metal-vs-cloth guess off in bc00466 |

Still in use, and why (each is a choice about how an effect looks, not a claim about game data, except the
first, which is listed for a decision):
- **Ambient occlusion's "bright pixels are direct light" rule** (`shadeSurfaces`: darkening fades out above a
  luminance of 0.3-0.7). It stands in for knowing which light is indirect. The game's indirect light is its
  voxel GI volume (`VoxelGI`, the sampler Smooth lighting already rewrites), so the by-construction version
  is to apply occlusion to that term inside the game's shader. **UNPROVEN as data; decision needed:** keep,
  or move AO into the game's shader through the existing hook.
- Eye adaptation: the image's own brightness (our design). Hurt: proven health. Edge smoothing, depth of
  field and the fire look: image filters, our design. Wounds and blood: the blood thread's.
To bring the material-based ones back: decode the material type the game itself keeps per material or part,
then flip `panel::materialDataDecoded` (settings_model.h).

**Game material data, decoded so far (PROVEN):** the game has its own procedural material system
(`FLX_Materials_Procedural`). 0x1EAEE0(id) creates and caches (`DAT_56B030[id]`) one instance per ID 1-25 through
the jump table at 0x32FC08, by class (VMT class names):

| ID | Class | ID | Class | ID | Class |
|---|---|---|---|---|---|
| 1 | TClothBasic | 10 | TSuedeA01 | 19 | TIronGen |
| 2 | TClothFine | 11 | TSteelChain | 20 | TCopperAlloy |
| 3 | TClothSilk | 12 | TSteelGen | 21 | TCopperSatin |
| 4 | TClothVelvet | 13 | TSteelPlate2 | 22 | TWiredGripA01 |
| 5 | TClothPadded1 | 14 | TSteelPainted | 23 | TGemstone |
| 6 | TClothPadded2 | 15 | TSteelPlate2 | 24 | TResin |
| 7 | TLeatherA02 | 16 | TSteelBlade | 25 | TPlanksPainted |
| 8 | TLeatherA01 | 17 | TWoodFineA | | |
| 9 | TSuedeA01 | 18 | TWoodRoughA | | |

The item factory 0x20FB80 gives each layer of an item part its procedural material: the ID is the byte at
`[table + index*9 + 0x8C]`, 12 (TSteelGen) when 0 (0x2103B8-0x2103F0), and the instance is stored at the layer's
`+0x50` (0x2103FC). **Missing:** the link from a drawn mesh material (`node+0x1D0[i]`) back to its layer and
procedural ID. That is the next step to bring the four detail rows back with the game's own materials.

## The game's own shading, as decoded (for the reflections and AO work)

- `shading_f`: `layout(std140) uniform Material { MatDif, MatSpc, MatEmi, MatDrt, ... }`. Specular: per light
  a GGX-style term with roughness from `Gloss = 1 - MatSpc.w * texture(GlsTex).r` (or `1 - MatSpc.w`), plus
  `speculargi`. It is multiplied by `texture(SpcTex).xyz` and `MatSpc.xyz` with a Fresnel factor `spf`. The
  opaque path ends `FragColor = diffuse` with `diffuse.w = 1.0`.
- Lights: `uniform vec4 Light[Lights*3]`, three vec4 per light, matching the renderer's packed `state+0x170`
  uploaded at 0x200BBD.
- Indirect light: `ambient` builds `amb` from `texture(VoxelGI, ...)` (`xyz` light, `w` occlusion), scaled by
  `VXGIScl.w` and `Ambient`, and adds it to `diffuse` before the material colour multiplies in. That is the
  game's own GI term, where screen-space AO belongs by construction.
- Plan: (1) reflections take the game's own specular reflectance per pixel (`MatSpc.xyz * SpcTex`, and
  roughness from `Gloss`), written into the opaque pass's alpha by a rewrite of the expanded `shading_f`, with
  exact-text guards. (2) AO multiplies `amb` inside the same shader instead of the finished image. Both need
  the expanded text the game hands to glShaderSource (the dev shader dump) to anchor the rewrite exactly,
  plus the framebuffer alpha and blend checks.

## Not yet proven, not gated (for the audit thread)

- Unit roles of the skin texture set (0 diffuse, 1 specular, 2 normal): from the shader review, not re-derived.
