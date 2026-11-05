# Game-code validation checklist

Audited build: the merged test build `claude/playtest-0.3.0` at 8b75b15 (what Hunter plays).
Game: Exanima 0.9.5.2g, Steam, `D:\SteamLibrary\steamapps\common\Exanima\Exanima.exe` (SHA-256 af9033c7…).
Date: 2026-11-05.

## TL;DR (the short version)

- **The game's own code was decoded for this, not guessed.** Exanima keeps the developer's class names inside the exe
  (for example `TPlayerBehaviour`, `TMotileDynamics`, `TForceBlastEffect`). Each claim below names the game routine
  that proves it and the class it belongs to.
- **Left bar is PROVEN.** Current health is +D58 (yellow). Restorable health is +D5C (pale). Max health is +103C (the
  rest is drawn red, which is the health you've lost for good).
- **The blue bar is MAGIC, PROVEN.** Only spells spend it. The mod's "Winded" screen effect reads it as stamina, which
  is **WRONG**.
- **The mod's "dead" check is WRONG.** The flag it reads (+DF8 bit 0x100) is a short stun from a spell hit that lasts
  about half a second. The game marks death as state +DF4 = 0x800. Eight places in the mod use the wrong flag.
- **The wound hook's arguments are mislabelled (WRONG).** What the mod calls "balance" is recoverable health damage.
  What it calls "health" is permanent damage, which lowers the restorable ceiling.
- **Balance is real and PROVEN.** It's a hidden value at +10B8 with its maximum at +10BC. Hits and force spells knock
  it down and it refills a little every physics step. The kick's balance gate uses the same threshold the game does.
- **Everything else is UNPROVEN until decoded.** Some of it is camera and render plumbing where the game routine is
  identified but not re-decoded. Some is game-rule claims nobody has checked (  attack timing, the kick clip's identity, cut/thrust/crush). Recommended removals are listed under Decisions.

## Decisions for Hunter (all in one place)

1. **Remove "Winded" from Hurt and winded, or turn it into a magic effect.** Recommended: remove it. The game has
   no stamina bar for it to read.
2. **Fix the "dead" check everywhere to use the game's death test, `(+DF4 & 0x800) != 0`.** That covers both
   dying (0x800) and corpse (0xA00), and it's the same test the game itself makes 32 times. Recommended: yes. It's a
   straight correction and the feature thread owns it.
3. **Plumbing vs game rules.** Camera, render and scene fields are located in named game routines, but their meaning
   hasn't been re-decoded line by line. Recommended: keep the plumbing (removing it removes the mod) and remove or
   disable only the **game-rule** claims marked UNPROVEN until each one is decoded. Say if you'd rather pull
   everything unproven.
4. **The knocked-down camera is now mostly proven.** Its state check matches the game's on-the-ground, getting-up
   and dead states. Only its use of the spell-stun bit as "dead" is wrong (fix #2 covers it). Recommended: keep it.
5. **The game's own vocabulary.** The game's loading tips say "Impact mainly deals stamina damage" (exe text at
   0x316F00), so the developer seems to call recoverable health loss "stamina damage". The mod should use your terms
   (current / restorable / lost health) and never call the blue bar stamina. No action is needed unless you want the
   game's term used.

## Fix status (as of 2026-11-05 09:30 UTC)

| Finding | Status |
|---|---|
| Winded reads the magic bar | Removed on render (abkkik). Hurt now reads +D58. |
| Weapon +F0/+120 gate | Lifted on render (272a386). Render's own witness is part step 89330 (8956A–89597). |
| lifeFlags 0x100 used as dead | Fixed on render (8d7e359) and VR xr_bones.inl (4dc1497, now `stateFlags & 0x800`). VR xr_drive.inl keeps 0x100 on purpose as "stunned, no teleport", which is correct. Still to fix: feature (camera, combat, input, motions, game_structs.h:41 comment), render overlay.inl:903, blood wounds.inl:540. |
| Damage-hook labels, cut/crush guess, limb list | Fixed or removed on blood (ae0f0a1), waiting for the feature thread to merge. |
| Render dead check / crouch overlay / headwear | Render 73315b9: dead = (DF4 & 0x800) != 0, overlay crouch 0x100 test removed. Headwear index data (6B8B0) and head matrix upload (200770 at 200B73–200B81) proven by render; the unproven "which sample" uniform is switched off. |
| Swing checks using gripFlags | Feature: only `grip & 0x80` needs care (see the grip row). `wide = gripFlags != 0` turned out to be the game's own test; no change needed. |
| Knocked-down mask | Feature to switch to `(DF4 & 0xA00) != 0`. |

## How proof works here

- **PROVEN**: the game routine was disassembled and does what the mod assumes. The routine address and what it does
  are given.
- **WRONG**: the routine was disassembled and does something different from what the mod assumes.
- **UNPROVEN**: not decoded to its meaning yet. `tools/check_game.py` may still prove the *address* (crash
  safety), but that says nothing about *meaning*. Old notes, bar colours, variable names and earlier threads don't
  count.
- Class names come from the Free Pascal class tables in the exe (VMTs). A routine listed in a class's VMT is that
  class's method. Otherwise the doc names the nearest class that calls it. The decoder is a read-only script over
  the exe and doesn't run the game.

## 1. Character stats and states (the game-rule values)

| Value (mod name) | Game location | What the game does with it (decoded) | Verdict | Used by the mod for |
|---|---|---|---|---|
| Current health (`health`) | +D58 | HUD 13E0F0 draws it yellow through its smoothed copy +D70 (17A100). bodyDamage 159700 lowers it. Below 0.05 it calls 158A10. Below 0.01 it's set to 0 and the die routine 156480 runs. Bleed 168410 drains it. | **PROVEN** | Kick gate (≥ 0.01), Hurt screen effect (health/max) |
| Restorable health (`healthRestorable`) | +D5C | HUD draws health to this point in pale (13C880 colours 0x2BA430–460). Permanent damage lowers it (159700, 0.2× its argument). Bleed above 1 lowers it (168410). Current health is rescaled when it drops. | **PROVEN** | Status / notes |
| Max health (`healthMax`) | +103C | HUD divides both health values by it. The part past restorable is drawn red (13C880 colours 0x2BA470–4A0). | **PROVEN** | Hurt effect, wound sizing |
| Blue bar (`stamina`, `staminaCeiling`) | +D74 / +D78 | Spent only by 17A640, which only spell classes call: TForceBlastEffect, TForceWaveEffect, TForceTorrentEffect, TForceBarrierEffect, TPwrForceKinesis_S, TPwrDispVanish_I, TPwrDispLongDoor_I, TPwrEnergyShock_I. Refilled by 17A6B0. The AI spell picker B7E20 compares +D74×200 with spell cost. +D78 is read only by the HUD, the refill, init and copy routines. | **PROVEN = MAGIC**. "Stamina" label **WRONG** | **Winded** screen effect (view_effects.inl:5712–5736): **WRONG**. Its "low-stamina idle" claim has no game code. |
| Balance (`balance`, `balanceMax`) | +10B8 / +10BC | 169A4C adds 0.005 per physics step. Impacts multiply it down (1587E0, from 159A60/157FF0). Force spells lower it (231710, 2393A0, 23C5C0, 23D680, 242A00). Below max − 0.05, TMotileDynamics plays the recover motion (169B99 → 179810). Death timing uses (1.2 − balance)×300 (156504). | **PROVEN** (hidden balance, not on the HUD) | Kick gate "balance ≥ max − 0.05" (combat.inl:141, input.inl:1515), the same threshold the game uses |
| "Dead" bit (`lifeFlags & 0x100`) | +DF8 bit 0x100 | Set only by 17B100 (from spell effects TTendrilEmitter 1A2CE0, 1A1550 and 28F4F0). 17B100 deals damage, sets a 500–700 ms timer (+11B4), and plays motion 3. TMotileDynamics clears it when the timer runs out (169B8A) and meanwhile plays motion 0x21 and sets balance to max − 0.5. | **WRONG**: it's a short spell-hit stun, not death | camera.inl:113 (opponent alive), combat.inl:12, input.inl:695, input.inl:1513, motions.inl:25 and :88, overlay.inl:891, camera.inl:406/665 and camera_motion.h:474–482 (knockdown follow), xr_bones.inl ~765, xr_drive.inl ~545. Render has already fixed its copy in wounds.inl. |
| Death | +DF4 == 0x800 (dying) or 0xA00 (corpse) | The die routine 156480 writes 0x800 (1564D6). It's called from bodyDamage and bleed when health < 0.01, and 167ED0 also writes it. While the state is 0x800, 167ED0 runs every step and fades +590 by 0.003. Once that drops under 0.25 it switches the body's physics off (+4C8 = 0) and 156D20 sets 0xA00. 1568E0 then clears combat stance for the corpse. Low health alone doesn't knock you down: between 0.01 and 0.05, 158A10 only plays a warning for the player every 60 s (13D700). | **PROVEN**. Test it as `(DF4 & 0x800) != 0`, the game's own form | Not used by the mod yet (it should be) |
| State (`stateFlags`) | +DF4 | The game uses it both as values and as bit masks: it tests &0xAC0, &0xA00, &0x200, &0x40 and &0x10203. Decoded so far: 0x200 = **on the ground** (16A6BA sets it when uprightness +12B0 < 0.3 and +1038 < 25; 156A90 also sets it and clears gait). 0x300 = **getting up** (162C10 picks get-up motion 0xE or 0xF by facing, and 169A20 refills balance faster). 0x800 = **dying**, 0xA00 = **corpse**. 0xAC0 is the game's own "don't interrupt" mask: low balance resets the state to 0 unless state&0xAC0 or state == 2 (169BE6). | 0x200 / 0x300 / 0x800 / 0xA00 **PROVEN**. The mod's mask 0xAC0 is the game's own mask (**PROVEN**). Bit 0x40000 in 0x40204 is never written by the game (dead code). Bit 0x4 and "== 0x20 means right mouse is the game's" are **UNPROVEN**. | Knockdown camera, "upright" gate, kick gates, attack cue |
| Bleeding rate | +10DC | 168410 (TMotileDynamics step) adds 0.03×inflow(+10D4), caps at 20, drains current health by 4e-5×rate and, above 1, restorable by (0.002×rate)², decays 0.1/step without inflow, and kills at health < 0. | **PROVEN**. Comment "drains health and balance" is **WRONG**: it drains current and restorable health. | Wounds / blood |
| Crouch (`duckAmount`) | +100C | Written by TPlayerBehaviour (C25B9). TMotileDynamics lowers it by 0.02 per step (1698E3). | **PROVEN** location and decay; "the game's crouch" **UNPROVEN** (no key handler traced) | Toggle crouch (input.inl:1437, combat.inl:253) |
| Combat stance (`combatStance`) | +12FE (byte) | The key handler 120AA0 compares the pressed key with the game's binding slot 0x2B7390 (default VK 9 = TAB) and calls the combat toggle 11FED0 with the opposite of the current state (120C6A–120C80). 11FED0 stores the new state and writes player +12FE (1200EC). TPlayerBehaviour's tick branches on it (C2028), and the HUD 13E9C0 draws a red marker when it's set. | **PROVEN** = TAB combat mode | Almost every combat/camera decision |
| Current motion (`motion`) | +9B8 | motionPlay 179EE0 stores it (17A034) and for motions 5–12 clears active defense and calls cleanup 162E80 (179F60–179F7A). **The attack families, decoded from the player's attack chooser AE9D0 (called from TPlayerBehaviour's tick C1CF0):** on a new press, C1CF0 marks a second press within 300 ms (+8E4 + 0x12C > now → behaviour +9C bit 8, C2462–C247E) and classifies the side: the angle between the aim heading +1B0 and the body heading +AD4, minus 15° (no weapon class) or 20° (any class, C24C3), negative → +1BC = 2 (C251E). Alt (binding slot 6, default VK 0x12, polled into keyWord bit 0x80 at AA1DA) with any weapon class sets +9C bit 0x10 (C283E–C285A). Then AE9D0 picks: bit 0x10 → thrust starter AEDC0 → **motion 11**; bit 8 → rapid-press starter AEEC0 → **motion 12**; +1BC == 2 → opposite-side starter AEF50 → 157700 → **motions 8–10** (8 + variant); else default-side starter AEFD0 → 157240 → **motions 5–7** (5 + variant). The variant (0–2) comes from the side classifier 156FB0. | **PROVEN**: 5–7 default side, 8–10 opposite side (aim crossed the body past 15°/20°), 11 thrust (Alt + press), 12 second press within 300 ms. The mod's "overhead" name for 12 is **UNPROVEN** as a name (the game gives it 1.5× damage at 159C68 and the loading tip praises overhead blows, which fits) | Attack cue, swing gates |
| Motion progress (`motionProgress`) | +A28 | The pose builder 1760F0 (and 174E00) sets it to (elapsed − blend-in) / clip length (1761D7–1761F7), where the blend-in is the ms passed to motionPlay (stored in +A24 at 17A063). So it runs from negative during the blend-in to 1 at the clip's end. The game's own 0.3 threshold: defense update 166580 treats an attack past 0.3 as committed (1669BA, 1669CF; 0.25 when swing bit 0x10000 is set), and the impact code gives a defender a parry only while their own progress is under 0.3 (15B990, 15BCAC). | Formula **PROVEN**, "negative in the start dwell" **PROVEN** (the blend-in), 0.30 **PROVEN** as the game's commit point. The wind-up arc's 0.40 for motion 11 (input_policy.h:456) is **UNPROVEN**: no game code pairs 0.4 with motion 0xB | Wind-up arc timing, kick clip retarget |
| Swing bits (`swingBits`) | +1018 | Bit 2 is set by the defender-side block routine 158EF0 (from 1587E0 when state == 2) and by impact 157FF0 (15859D). Bit 4 is set by physicalImpact 159A60 in three contact outcomes (15C212, 15C242, 15C26A). Bit 0x20 is **never set** by the game (only masked off). The starters clear the word (1576A3, 157B7C) and set 0x80000 / 0x10000. | 0x2 and 0x4 **PROVEN** as "the swing made contact"; "bounce" vs "block" labels **UNPROVEN**; 0x20 is dead | Wind-up arc ends on contact |
| Kick clip | motion 0xD | No game code starts 0xD with a constant. The game does handle it specially in two places. In the dynamics step (16B06F), when +590 ≥ 0.85, it sets state 0x8000, doubles +DC8 and re-grips through 152930. In the leg-support routine (1612C9), it skips the normal support check the same way state 0x4000 does. Nothing decoded names it a kick. | "Kick clip" **UNPROVEN**. "Unused" is **WRONG** (the game has code for it) | Kick (Animated and Classic) |
| Jump | motion 0x12 + rigid push 89240 | 89240 is the rigid-body push that force spells and cloth also use. Motion 0x12 also has no direct starter. | Push **PROVEN**. 0x12 = "jump clip" **UNPROVEN** | Jump |
| Selected opponent (`aimOpponent`) | +E38 | Written by combatAttention BEAC0 (TPlayerBehaviour) and cleared by AI/behaviour routines. | Location **PROVEN**, meaning **PLAUSIBLE** | Camera framing, aim |
| Walking (`walking`) | +12EA | Set by explore BFEA0 / player tick, cleared by motionPlay and the spell stun. | **UNPROVEN** meaning | Spine-follow, gait |
| Grip flags / weapon class (`gripFlags`) | +1068 | Grip selector 1523F0 (and its twin 152660) sets it to the low 16 bits of the primary weapon's item record (weapon +18 → +198 → word 0), then keeps & 0xF0. From that class it picks the motion set by the game's own named table at 0x2ED0F0 (name then code, read at 258CE5): **0x10 → "One Handed"** (motion set 1), or **0x90 with two weapons → "2W Light"** (set 4). **0x20 → "Two Handed"** (set 0x10), **"2H Light"** (0x20, item code bit 0 set) or **"Greatsword"** (0x30, item code 0x521). **0x30 → "Polearm"** (set 0x40). No recognised class gives 0x80, or 0xC0 when body +AF8+58 & 0x100. A shield gives sets 0x100/0x200/0x300 ("1H & Shield" / "Shield C1" / "Shield C2") and a special held item gives "1H & Torch" (3); in those two paths +1068 keeps the item code. The grip table lookup 155250 indexes G2BAC50 by (grip & 0x70) >> 4, using 1 for 0x90 and 5 when its second argument is 1. | **PROVEN** with the game's own names. Kick `classify` (one-handed / dual / two-handed / polearm / unarmed / shield) **matches**. `wide = gripFlags != 0` (input.inl:1173, 1834) is **PROVEN**: it's the game's own side-threshold test (C24C3: 15° when +1068 == 0, 20° otherwise). (An earlier version of this doc called it wrong; that was a mistake.) `grip & 0x80` = "alternate" (input.inl:1746) is true for no-weapon (0x80/0xC0) as well as for two weapons (0x90), so it's **only PROVEN for 0x90**. | Kick weapon table, wide swing threshold, swing hands |
| Impact record | +C18 / +C20 / +C28 / +C38 | Written in physicalContact 1590D0 (a TMotileDynamics/TMotilePart method). Read by impact 159A60. | Location **PROVEN**, field meanings **PLAUSIBLE** | Blade drag, VR contacts, wound weapon lookup |

### Knocked-down verdict (every value the mod's mask matches)

The mod's mask 0xAC0 | 0x40204 | 0xA00 matches seven values the game stores in +DF4:

| DF4 value | What it is (decoded) | Writers | Down? |
|---|---|---|---|
| 0x200 | **On the ground** after a fall. The dynamics step sets it when uprightness +12B0 < 0.3 and +1038 < 25. 156A90 also sets it and clears the gait. | 16A6BA (169200), 156AA8 (156A90), 156D04 (156B20) | **Yes, PROVEN** |
| 0x300 | **Getting up.** Picks get-up motion 0xE or 0xF by facing, and balance refills faster (169A20). | 162D69 (162C10), 16A998 (169200) | **Yes, PROVEN** |
| 0x600 | **Laid on the ground by a scripted game sequence.** 126050 plays motion 0x1D on the player, lays them down through 156A90 for 3 s, then sets 0x600. The dynamics step special-cases it at 16C050. | 126555 (126050) | **Yes, PROVEN** (on the ground; which story/arena event triggers it is unproven) |
| 0x800 | **Dying**: a ragdoll that hasn't settled yet | 1564D6 (die routine 156480), 167EDF (167ED0) | **Yes (dead), PROVEN** |
| 0xA00 | **Corpse**: physics switched off | 156E48 (156D20), F12C4 (F1220), 1A0A95 (1A0970) | **Yes (dead), PROVEN** |
| 0xC0 | **Standing spell-hit stagger.** 17B050 stops walking and sets a 600 ms timer (+EA4). The dynamics step resets it to 0 when the timer runs out (16A581–16A5A6). | 17B0E1 (17B050, from spell-hit routine 17B100) | **No, PROVEN.** The mask wrongly treats it as down. |
| 0x10004 | **Standing action.** Set while motion 0x1F is past 60 % (16B4C3). Bit 4 marks a one-shot callback (136BF0) as already queued, and before 60 % the value is 0x10000. | 16B504 (169200) | **No, PROVEN.** Matched only through bit 0x4 of 0x40204. |


The other values the game stores in +DF4 (all **standing**, none caught by the game's own 0xA00 / 0x800 tests):

| DF4 value | What it is (decoded) | Writers |
|---|---|---|
| 0 | Normal: nothing special running. Many resets, including low balance (169C06) and the stagger timer (16A5A6). | 169200 and others |
| 1 | A finishing/settle step after a balance-dependent blend (16B3D8 branch on +1198 == 3, then 15F000 with 0x300 if the state was above 0xFF). | 16B470 (169200) |
| 2 | **Active defense** (parry/block posture). Written by the defense attack update when it takes over. | 166604 (166580, TMotileDynamics) |
| 8 | **Reactive defense motion** running: set while +12F3 is set, or motion 4 (the defense motion 163880 picks at 163B58) is playing. | 16B168, 16B1B1 (169200) |
| 0x20 | **Performing a power / command motion.** 179E40 plays the requested motion and sets 0x20; it's called by TForceBlastEffect, TForceWaveEffect, 2360E0 and 23CB20. 169200 also sets it for motions 0x22–0x28 (16B2D6). | 179EC0 (179E40), 16B2D6 (169200) |
| 0x100 | **Stumble recover**: when the stance value +590 drops under 0.85 the game plays recover motion 3 and sets 0x100. | 16B05C (169200) |
| 0x8000 | Motion 0xD special case (see the kick row). | 16B09C (169200) |
| 0x10000 / 0x10004 | Motion 0x1F running / past 60 % (see the knocked-down table). | 16B514 / 16B504 (169200) |

So the mod's `stateFlags == 0x20` refusal (combat.inl:14, game_structs.h "right mouse is the game's") is a **real
state** (a power motion is in progress) and refusing a kick during it is right, but the label is **WRONG**: it has
nothing to do with the right mouse button. The upright test `& 0x40204` (input.inl:1829) matches 0x200 (down), 0x4
(a bit of 0x10004, a standing action) and 0x40000 (never written); it should be `& 0xA00` like the knockdown test.

**Verdict:** the knocked-down camera is safe to turn back on if it tests `(DF4 & 0xA00) != 0`. That's exactly the
on-the-ground, getting-up, scripted-down, dying and corpse states. The game makes the same test itself at 16990A,
169AB7 and 168426. The current mask also fires on a 0.6 s spell stagger and on motion 0x1F, and its 0x40000 bit is
never written.

## 2. The damage routine (wounds, hit reaction, damage flash)

bodyDamage 159700(character, A in xmm1, B in xmm2, K in r9b) returns whether the character died. **PROVEN:**

- A is recoverable damage: current health −= 0.1×A (0.05×A while +DF8 bit 0x800000 is set).
- B is permanent damage: restorable −= 0.2×B, and current health is scaled by the drop. When +12A4 bit 8 is set,
  B comes off current health instead.
- K means "may kill". Only when it's set does health < 0.01 lead to the die routine.

| Mod item | Verdict |
|---|---|
| wounds.inl:1092 `bodyDamageDetour(character, balance, health, canFall)` | **WRONG** names: "balance" = A (recoverable health), "health" = B (permanent), "canFall" = may-kill |
| `CharacterHitRecord {health, balance}` | **WRONG** labels, as above |
| Wound kind: cut if health > 0.4×balance, crush otherwise (wounds.inl:264–270) | **UNPROVEN**: it compares two health channels, not cut vs crush. The game has named damage types (loading tips at 0x316D60–0x316F00: slash, piercing, crushing, impact), so the real split should be found in the impact code 159A60 |
| Hit reaction (camera kick) strength from both amounts (wounds.inl:303) | Inputs **PROVEN** as damage amounts. Labels wrong, behaviour OK |
| Damage flash from "health" (wounds.inl:287) | **PROVEN** as permanent-damage amount (label wrong) |
| Part indices 1/4/11/14 = thighs/upper arms, bone table (wounds.inl:39–52) | **UNPROVEN** (taken from the Injuries addon) |
| +468 part index, +420 part→character, +10DC bleed | +10DC **PROVEN**. Others **UNPROVEN** in this pass |
| Metal vs cloth heuristics | **UNPROVEN**. Lead: the exe has material classes (`TClothBasic`, `TClothPadded1`, `TCopperAlloy`, …) that could prove it |
| "Lamp" = scene flags & 0xFFF == 5 | **UNPROVEN** |

**Blood thread update (ae0f0a1, spot-checked here):** the arguments are renamed recoverable / permanent / mayKill,
and wound severity = 0.35 + 0.65 × (0.1A + 0.2B) / max health (**PROVEN** arithmetic). Thrust is **PROVEN** from the
game: physicalImpact 159A60 checks the striker's motion against 0xB at 159B28 and, for that motion, measures speed
along the weapon's axis (+C38 → +2AC, ×2, 159B59–159B9A) instead of total speed. The cut/crush guess, the limb
spurt list (parts 1/4/11/14) and fracture bruises are **removed**. Lamp highlights, skin bake and severed limbs are
**off** behind one unproven flag. Metal vs cloth stays on, labelled as a screen-colour heuristic (**UNPROVEN**, kept
by choice). Still open: the slash/pierce/crush split (lead: 159A60 calls 1531E0 on the weapon).

**Where the two damage amounts come from (partly decoded):** physicalImpact 159A60 calls bodyDamage once, at 15C735.
Just before the call, with x = the hit's damage value worked out earlier in 159A60 and p = the struck part's
multiplier +6C0:
permanent B = 0.75 × p × x/(1+x) (saturating), and recoverable A = 0.75 × (p × (2 × (x − x/(1+x)) + earlier
recoverable term) + 0.5 × another impact term). So heavy hits push extra damage into the recoverable channel once
the permanent part saturates. Impact energy before that: motion 0xC multiplies it by 1.5 (159C68). Thrusts (motion
0xB) scale by (0.5 + weapon +28 + weapon +18) × 0.3. Other attacks use pow(E, 1 − 0.5 × weapon +28) × (weapon +28 +
0.3) × 1.1, where "weapon" is the weapon data from 1531E0 (part +6E0, walking up to the root part). If a global
callback G4F0900 is installed, the game hands it the hit record instead of applying damage (15C679–15C6FD). 

**Slash / crush / pierce: PROVEN.** The item stats screen (10C0E0–10C1B8) labels the item record's per-grip bytes +37,
+38 and +39 as "Slash", "Crush" and "Pierce" (strings at 0x311780 / 0x3117A8 / 0x3117C8, each shown as byte × 0.05).
The weapon-data builder 165AD0 (called from TMotileDynamics, TCharacterView and TLoadoutForm) turns those three
bytes into the part's weapon data (+6E0): **+2C = slash, +30 = crush, +34 = pierce**, each 0.5 × (byte/100)^0.7
(165BEA–165CDD). It picks one of two 6-byte grip rows by the part's +780 byte. physicalImpact 159A60 reads that same
weapon data (from 1531E0) and scales all three by one hit factor into its slash / crush / pierce locals (15A794–15A7D1).
Those then go through the armour code and are summed (15C2DB) before bodyDamage. So a wound's kind can be taken from
the striking weapon's +2C / +30 / +34 for its current grip. That's the game's own weapon split, before armour.
Which type survived the armour isn't visible outside 159A60's locals; that would need a hook inside it.

**Weapon +F0 / +120 (render vs blood): PROVEN.** These are the rigid body's four points and their previous-step
copy. heldItemAttach 155C20 writes the attached item's points to +F0 and copies them into +120 and +150 (155D72–155D9F).
TTetraRBD's own integrate method 88D30 (listed in the TTetraRBD class table) runs its Verlet step over +F0 / +120 /
+150. A held weapon is that same rigid body, so render's gate on these reads is over-cautious and can be lifted.

## 3. Every F8 setting

"Camera-only" means the setting changes only the mod's own camera maths. It makes no claim about game rules. It
depends on the camera hooks in section 4, whose game classes are identified there.

| Setting (INI) | What game value or routine it depends on | Verdict |
|---|---|---|
| Base field of view (Camera.FOV), Third person: field of view | camera +1C8 fov (navSampler, a TNavCameraControl method) | Camera-only. Routine **PROVEN** as camera sampler |
| Look sensitivity, Raw mouse input, Invert vertical look, Fast turn damping | cameraMouse 11EF40 bypass, raw input | Camera-only |
| Head stabilisation, Head steadiness roll/pitch/yaw, Head bob steadiness, Eye setback, Minimum near clip | head body part +11E0 and its pose +2A0 (aimPoint, TPlayerBehaviour). Near plane camera +1CC | Camera-only. Field located, "head part" **PLAUSIBLE** |
| View heading, Auto-center camera on cursor, Aim oval width/height (FP/TP), Aim oval edge softness | cursorAim BF680, aimPoint AE2E0 (TPlayerBehaviour) | Camera/aim-only |
| Spine follows aim | pose builder 1760F0, +12EA walking | "Walking" meaning **UNPROVEN** |
| Third person: distance / shoulder / height / tight spaces / camera lag / in the way / follow look angle / Swap shoulder | level rays 1430F0/1431C0, camera fade 90C00 | Camera-only |
| Third person: combat framing, Third person: aiming mode | combatStance +12FE (**PROVEN**, TAB), aimOpponent +E38 | Stance **PROVEN** |
| Third person: knocked down, First person: knocked down | stateFlags mask 0xAC0 \| 0x40204 \| 0xA00 and lifeFlags 0x100 | See the knocked-down verdict table below. The mask catches every down state but also two standing states (0xC0, 0x10004). Use `(DF4 & 0xA00) != 0` instead, the game's own on-the-ground test. The lifeFlags 0x100 part is **WRONG** (spell stun). |
| F6 cycle | camera mode switching, reads lifeFlags 0x100 as dead | **WRONG** dead check |
| Hit reaction | bodyDamage arguments | **PROVEN** inputs (labels wrong) |
| Grab range, Rotate held object | drag manager, heldQuery 117AA0, dragRotate 118920 | Plumbing, **UNPROVEN** meaning not re-decoded |
| Toggle crouch | duckAmount +100C | Decay **PROVEN**, "crouch" **UNPROVEN** |
| Kick (key), Kick style, VR: kick | motion 0xD, gates: health ≥ 0.01 (**PROVEN**), balance ≥ max − 0.05 (**PROVEN**), dead bit (**WRONG**), stance/state (**UNPROVEN**), kickGripClass 155250 (**UNPROVEN**) | Mixed |
| Jump (key), VR: jump | rigid push 89240 (**PROVEN** push), motion 0x12 (**UNPROVEN**) | Mixed |
| First/Third person: defense (Manual), Swing selection | defense hooks 163880/166580/156FB0/162E80 and combatUpdate 169200 (TMotileDynamics methods, **PROVEN** identity), attack starters 157240/157700. Motion range 3–12 and grip table **UNPROVEN** meaning | Hook identity **PROVEN**, rules **UNPROVEN** |
| Attack wind-up arc, size / opacity / thrusts, Crosshair during a swing, Crosshair out of combat, Aim marker, Aim indicator | motion +9B8, progress +A28, swingBits +1018 & 0x26 | Progress formula, the 0.30 commit point and the contact bits **PROVEN**. Thrust = motion 0xB **PROVEN** (impact code). The 0.40 variant for motion 11 **UNPROVEN** |
| Blade drag cuts, Stabs sink in, Blade draw gain | impact record +C18–C40 (physicalContact 1590D0), armour rows | Record location **PROVEN**, armour-row meaning **UNPROVEN** |
| Hurt and winded | Hurt: health/max (**PROVEN**). Winded: blue bar (**WRONG**) | Render branch has removed Winded and Hurt reads +D58 |
| Wounds, Wound debug highlight | section 2 | Mixed (see section 2) |
| Hide helmet and hair, Helmet view | headwear render hooks 4E610 (TRenderer), 6B8B0, 6AB40 | Render plumbing; render thread is auditing |
| See yourself through walls | silhouettePass 4F1D0 (TRenderer / T3DViewport) | Render plumbing |
| Ambient occlusion, Depth of field, Screen-space reflections, Edge smoothing, Smooth lighting | sampled views 4BAB0 (a T3DViewport/TCharacterView method), view matrices | Mod-drawn effects on the game's image, no game-rule claims. Render thread is auditing |
| Held item / Armour and clothing / World object / Level detail, Lights per object, Draw distance, Draw far rooms | mesh queue 26F230, shadow schedule 4F4A0, light lists, roomClassify 143C20 (hash-gated) | Render plumbing; render thread is auditing |
| Finger poses, Finger poses: others | The mod makes its own bone nodes (`_VRBONE` names) with the game's node constructor 4FD20, hangs them on the hand node with 50170, registers them with the skin bone register D39B0, and rewrites the `_SKIN` aggregate's vertex weights. Finger joints come from the mod's own hand model fitted to the mesh, not from game bone names. | Skin layout **PROVEN** by D39B0 itself: skin +8 back-pointer, +10 bone count, +18 node array, +20 inverse binds, +28 per-bone floats, and the 1-based id at node +17C (D39C0–D3AEF). Skin build D9940 is reached from TCharacterView's equipment refresh. The vertex formats 0xF30001 / 0xA41000 and the `_SKIN` name are **UNPROVEN** (no field contract; every read is bounds-checked and a mismatch skips the feature). No game-rule claim. |
| Head / eye tracking, Recenter tracking | none (UDP input) | Mod-only |
| Lean, Lean controls, Lean left/right key | none: hidden legacy rows, ARCHITECTURE.md says lean has no runtime input or pose path | No game claim; nothing to prove |
| All 25 VR rows | xr_* whole-function byte contracts | Not playable in this build without the OpenXR build. Not audited in this pass |

Render settings are covered row by row in `docs/render-assumptions.md` on branch `claude/project-thread-abkkik`
(PROVEN / WITNESSED / UNPROVEN per setting). Its open item: the headwear descriptor +0x14 and the renderer
+0x80 → +0xC → +0x14 chain are still UNPROVEN, but they run inside byte-verified game routines, and a wrong value
falls back to the game's own draw, so they're safe to keep while they're decoded.

## 4. Every hooked or called game routine (139)

Identity of each routine, decoded from the game's class tables. "Method of X" means the game's own class X lists
this routine. "Called from" means the nearest class that calls it. Crash safety (the bytes still match) is
`tools/check_game.py`. This table adds *what the routine belongs to*.

| Hook / game routine | Address | Game class that owns or calls it |
|---|---|---|
| navSampler | 94720 | method of TNavCameraControl |
| headRenderSample | 8B6E0 | method of TMotilePart, TRBDDoor, TRBPushSurface, TRBRotator � |
| playerBehavior | C1CF0 | method of TPlayerBehaviour |
| aimPoint | AE2E0 | method of TPlayerBehaviour |
| cursorAim | BF680 | called from TPlayerBehaviour |
| combatAttention | BEAC0 | called from TPlayerBehaviour |
| explore | BFEA0 | called from TPlayerBehaviour |
| combatStep | C13F0 | called from TPlayerBehaviour |
| poseBuilder | 1760F0 | called from TMotileAI, TMotileDynamics, TPlayerBehaviour, TPwrDispLongDoor_I, TPwrDispVanish_I |
| partSettle | 89800 | engine helper, no class caller within 3 calls |
| cursorPosition | 22710 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| pollInput | AA080 | called from TPlayerBehaviour |
| travelTarget | 19390 | engine helper, no class caller within 3 calls |
| walkGait | 163EF0 | called from TMotileDynamics |
| sectorUpdate | 58CD0 | engine helper, no class caller within 3 calls |
| coverPass | 144610 | engine helper, no class caller within 3 calls |
| coverDepth | 1444A0 | engine helper, no class caller within 3 calls |
| dragPlace | 1183C0 | engine helper, no class caller within 3 calls |
| dragRotate | 118920 | engine helper, no class caller within 3 calls |
| propagateNodePose | 50EB0 | called from TCharacterView, TFireEffectRow, TForceBarricadeEffect, TMotilePart, TNavCameraControl, TPwrForceShell_C, TPwrForceStrikes_C, TRBDDoor |
| heldQuery | 117AA0 | engine helper, no class caller within 3 calls |
| pickRay | 8F720 | called from TPlayerBehaviour |
| cursorWarp | 22780 | called from TGUIItemContainer, TPlayerBehaviour |
| grabPick | 8FB30 | engine helper, no class caller within 3 calls |
| doorHandler | 11B400 | called from TPwrForceKinesis_S |
| planeHit | 8F530 | called from TPlayerBehaviour |
| dragDrop | 119C30 | called from TArsenalView |
| queueWork | 1A890 | called from TAISearchActivity, TCharacterView, TGUIItemContainer, TLoadoutForm, TMotileAI, TMotileDynamics, TMotilePart, TPwrCommandDead_C |
| applyHeldRelease | 116EF0 | engine helper, no class caller within 3 calls |
| guiMousePress | 2FE40 | engine helper, no class caller within 3 calls |
| guiMousePoll | 2F7E0 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| hingeHandler | 11BC50 | called from TPwrForceKinesis_S |
| rebuildProjection | 9F2E0 | called from T3DViewport, TCharacterView |
| rebuildInverse | 1CEB0 | engine helper, no class caller within 3 calls |
| levelRay | 1430F0 | called from TPwrForceKinesis_S |
| levelRay2 | 1431C0 | called from TPwrForceKinesis_S |
| headingFromDirection | 19F00 | called from TAISearchActivity, TAnyPlayerBehave, TBarricadeEntity, TBlastEntity, TCharViewBehaviour, TCombatant, TForceBarrierEffect, TForceBlastEffect |
| aimFilterPush | BF1C0 | called from TForceBarrierEffect, TForceTorrentEffect, TPlayerBehaviour |
| aimFilterUpdate | BF380 | called from TForceBarrierEffect, TForceTorrentEffect, TPlayerBehaviour |
| cameraMouse | 11EF40 | engine helper, no class caller within 3 calls |
| copyCameraSnapshots | 11EB50 | engine helper, no class caller within 3 calls |
| cameraOrbit | 91F00 | called from TPlayerBehaviour |
| cameraZoom | 92630 | engine helper, no class caller within 3 calls |
| defenseReactive | 163880 | called from TMotileDynamics |
| defenseAttackUpdate | 166580 | called from TMotileDynamics |
| defenseMotionSelect | 156FB0 | called from TMotileDynamics, TMotilePart |
| defenseCleanup | 162E80 | called from TAISearchActivity, TCharacterView, TForceBarricadeEffect, TForceBarrierEffect, TForceBlastEffect, TForceBubbleEffect, TForceTorrentEffect, TForceWaveEffect |
| constraintHandler | 11BA30 | engine helper, no class caller within 3 calls |
| analogScale | 19240 | engine helper, no class caller within 3 calls |
| analogAdd | 193D0 | engine helper, no class caller within 3 calls |
| heldPlace | 118000 | engine helper, no class caller within 3 calls |
| heldSweep | 1170F0 | engine helper, no class caller within 3 calls |
| cameraFade | 90C00 | engine helper, no class caller within 3 calls |
| fadeBounds | 7A270 | called from TMotileDynamics, TNavCameraControl, TPlayerBehaviour, TPwrForceKinesis_S |
| combatPosture | B7560 | engine helper, no class caller within 3 calls |
| combatUpdate | 169200 | method of TMotileDynamics |
| aimWorldToLocal | 18960 | engine helper, no class caller within 3 calls |
| aimScreenRay | 8F460 | called from TPlayerBehaviour, TPwrForceKinesis_S |
| aimMeshRoot | 4FCB0 | engine helper, no class caller within 3 calls |
| guiCheckboxHandler | 33020 | engine helper, no class caller within 3 calls |
| renderViewport | 4BAB0 | method of T3DViewport, TCharacterView |
| terrainPass | 1B8760 | engine helper, no class caller within 3 calls |
| multiplyMatrix | 18C70 | engine helper, no class caller within 3 calls |
| guiMaintenance | 32180 | engine helper, no class caller within 3 calls |
| guiFormElements | 2B200 | method of T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView � |
| guiFormClose | 2C410 | method of T3DViewport, TAbstractToolTip, TArsenalContFrame, TClassSelect � |
| guiElementsCopy | 32860 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| guiElementsFree | 2AA90 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| guiSettingsTab | A7FC0 | engine helper, no class caller within 3 calls |
| guiControlsTab | A7FF0 | engine helper, no class caller within 3 calls |
| guiFormInvalidate | 2E760 | called from TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm, TCharLoot |
| guiOptionsOpen | A94D0 | called from TMainMenu |
| guiFormShow | 2C8F0 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| guiFormGeometry | 2BCC0 | called from T3DViewport, TAbstractToolTip, TArsenalContFrame, TArsenalView, TCacheShaderScreen, TCharBaseForm, TCharCreation, TCharEditForm |
| guiOptionsApply | A6770 | engine helper, no class caller within 3 calls |
| guiOptionsCancel | A8390 | engine helper, no class caller within 3 calls |
| guiOptionsDefaults | A8AF0 | engine helper, no class caller within 3 calls |
| meshQueue | 26F230 | engine helper, no class caller within 3 calls |
| heldUpdate | 11F610 | engine helper, no class caller within 3 calls |
| inventoryAdmission | 119AB0 | engine helper, no class caller within 3 calls |
| resourceQueue | 99BD0 | engine helper, no class caller within 3 calls |
| inventoryLoader | 11CF50 | engine helper, no class caller within 3 calls |
| inventoryComplete | 11D1D0 | engine helper, no class caller within 3 calls |
| inventoryCommit | 119F10 | engine helper, no class caller within 3 calls |
| inventorySourceFinish | 137800 | method of TCharacterView |
| attackFamilyA | 157240 | called from TMotileDynamics, TMotilePart, TPlayerBehaviour |
| attackFamilyB | 157700 | called from TMotileDynamics, TMotilePart, TPlayerBehaviour |
| guiRequestRedraw | 26140 | engine helper, no class caller within 3 calls |
| heldBodyIntegrate | 88D30 | method of TTetraRBD |
| heldBodyDestroy | 88060 | method of TRBPushSurface, TTetraRBD |
| applyHeldReleaseWithoutPose | 116D70 | engine helper, no class caller within 3 calls |
| headwearColourPass | 4E610 | called from TRenderer |
| headwearDraw | 6B8B0 | engine helper, no class caller within 3 calls |
| headwearDrawBound | 6B7F0 | engine helper, no class caller within 3 calls |
| headwearBind | 6AB40 | called from T3DViewport, TBarricadeGeometry, TBloodSprays, TBubbleEffect, TBubbleGeometry, TCharacterView, TDispDissolve, TEffectGeometry |
| headwearSubmit | 201630 | engine helper, no class caller within 3 calls |
| headwearSubmitSingleReturn | 26F56C | engine helper, no class caller within 3 calls |
| headwearSubmitSamplesReturn | 26F618 | engine helper, no class caller within 3 calls |
| headwearDrawReturn | 26F5A4 | engine helper, no class caller within 3 calls |
| headwearDrawBoundReturn | 20053D | engine helper, no class caller within 3 calls |
| sampledViews | A0CE0 | called from T3DViewport, TCharacterView |
| superSampledViews | 4B6D0 | called from T3DViewport, TCharacterView |
| sampledViewsWorldReturn | 4BF39 | method of T3DViewport, TCharacterView |
| heldItemRelease | 11B340 | called from TPwrForceKinesis_S |
| xrCompose | 18B60 | engine helper, no class caller within 3 calls |
| xrGrabKind | 118F80 | engine helper, no class caller within 3 calls |
| xrGrabStart | 11A690 | engine helper, no class caller within 3 calls |
| xrPhysicalCommit | 1592D0 | method of TMotileDynamics |
| xrPhysicalContact | 1590D0 | method of TMotileDynamics, TMotilePart |
| xrPhysicalImpact | 159A60 | called from TMotileDynamics |
| motionPlay | 179EE0 | called from TAISearchActivity, TCharacterView, TForceBarricadeEffect, TForceBarrierEffect, TForceBlastEffect, TForceBubbleEffect, TForceTorrentEffect, TForceWaveEffect |
| attachChild | 50170 | called from TCharacterView, TLoadoutForm, TPwrForceShell_C, TPwrForceStrikes_C, TVanishEffect |
| boneRegister | D39B0 | engine helper, no class caller within 3 calls |
| silhouettePass | 4F1D0 | called from T3DViewport, TCharacterView, TRenderer |
| silhouettePassReturn | 4FAD4 | called from T3DViewport, TCharacterView, TRenderer |
| cameraPass | 4F750 | called from T3DViewport, TCharacterView, TRenderer |
| materialBind | 73F20 | called from T3DViewport, TCharacterView, TClassSelect, TGameTips, TRenderer, TSplashScreen, TVXGIManager |
| sampledViewsOffscreenReturn | 4BF0E | method of T3DViewport, TCharacterView |
| silhouetteSingleSubmit | 200770 | engine helper, no class caller within 3 calls |
| rigidImpulse | 89240 | called from TForceBarricadeEffect, TForceBarrierEffect, TForceBubbleEffect, TMotileDynamics, TVCCloth |
| characterMove | 170810 | called from TMotileAI |
| nodeCtor | 4FD20 | called from T3DNode, TBarricadeGeometry, TBubbleEffect, TBubbleGeometry, TCamera, TEffectGeometry, TForceBarrierGeometry, TForceBlastGeometry |
| skinBuild | D9940 | engine helper, no class caller within 3 calls |
| shadowSchedule | 4F4A0 | called from T3DViewport, TCharacterView, TRenderer |
| bodyDamage | 159700 | called from TMotileDynamics, TMotilePart |
| woundImpact | 159A60 | called from TMotileDynamics |
| roomClassify | 143C20 | engine helper, no class caller within 3 calls |
| nodeMove | 5C030 | engine helper, no class caller within 3 calls |
| nodeVecSub | 19170 | engine helper, no class caller within 3 calls |
| nodeVecAddTo | 19100 | engine helper, no class caller within 3 calls |
| nodeMoveAttach | 50170 | called from TCharacterView, TLoadoutForm, TPwrForceShell_C, TPwrForceStrikes_C, TVanishEffect |
| nodeRegisterDynamic | 55FA0 | called from TCharacterView, TPwrForceStrikes_C, TStaticTerrain, TVanishEffect |
| loadRpkStream | 6F710 | called from TCharDecomp2, TClothBasic, TClothFine, TClothPadded1, TClothPadded2, TClothSilk, TClothVelvet, TCopperAlloy |
| xrStereoWorldDraw | 4F750 | called from T3DViewport, TCharacterView, TRenderer |
| xrStereoFrustum | 9F500 | engine helper, no class caller within 3 calls |
| xrStereoRebuildPlanes | 78050 | engine helper, no class caller within 3 calls |
| xrStereoFlushBindings | 1FF3C0 | engine helper, no class caller within 3 calls |
| kickGripClass | 155250 | engine helper, no class caller within 3 calls |
| engineLog | 25D80 | engine helper, no class caller within 3 calls |

## 5. Every game field the mod reads or writes (272)

The witness routine is where `tools/native_contracts.json` proves the field's *location*. The class column shows
which part of the game that routine belongs to. **Meaning is only PROVEN for the fields covered in sections 1 and 2.**
Every other row is located but its meaning is UNPROVEN in this audit. Most are camera, scene, render and GUI
plumbing, where a wrong meaning breaks the mod visibly rather than quietly changing game rules.

| Field | Witness routine | Address | Game class |
|---|---|---|---|
| character::sceneNode | cursorAim | BF680 | TPlayerBehaviour |
| character::physicsLive | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::weaponCount | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::weapons | gripSelect | 152100 | TCharacterView |
| character::swingWeapon | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::motion | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::motionProgress | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::travelTarget | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::movementFacing | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::position | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::bodyHeading | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::gaze | combatAttention | BEAC0 | TPlayerBehaviour |
| character::gazeDistance | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::aimDirection | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::facing | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::behaviour | cameraMouse | 11EF40 | engine (no class caller within 3 calls) |
| character::chestPart | defenseAttackUpdate | 166580 | TMotileDynamics |
| character::stateFlags | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::lifeFlags | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::contextualTarget | explore | BFEA0 | TPlayerBehaviour |
| character::handParts | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| character::heldItems | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| character::defenseIncomingMotion | defenseCleanup | 162E80 | TAISearchActivity, TCharacterView, TForceBarricadeEffect |
| character::defenseActive | defenseCleanup | 162E80 | TAISearchActivity, TCharacterView, TForceBarricadeEffect |
| character::duckAmount | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::swingBits | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::gripFlags | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::gripEntry | gripSelect | 152100 | TCharacterView |
| character::balance | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::balanceMax | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::gaitFlags | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::headPart | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| character::walking | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::actionCallback | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| character::combatStance | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::character | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::hoverState | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::inactive | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::swingState | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::aimHeading | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::aimPoint | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| behaviour::pressFlags | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::view | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::pendingMove | combatStep | C13F0 | TPlayerBehaviour |
| behaviour::lastPressTime | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| behaviour::aimFilter | cursorAim | BF680 | TPlayerBehaviour |
| behaviour::moveIntent | explore | BFEA0 | TPlayerBehaviour |
| behaviour::interactionLatch | interactionPoll | 121AF0 | engine (no class caller within 3 calls) |
| behaviour::pressBlocked | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| bodyPart::flags | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::meshNode | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| bodyPart::poseCoefficients | partSettle | 89800 | engine (no class caller within 3 calls) |
| bodyPart::points | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| bodyPart::history | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| bodyPart::history2 | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| bodyPart::pose | partSettle | 89800 | engine (no class caller within 3 calls) |
| bodyPart::rootPart | hingeHandler | 11BC50 | TPwrForceKinesis_S |
| bodyPart::parentPart | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::character | heldItemAttach | 155C20 | engine (no class caller within 3 calls) |
| bodyPart::extraRotation | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::slackCap | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| camera::pose | copyCameraSnapshots | 11EB50 | engine (no class caller within 3 calls) |
| camera::pose2 | cameraOrbit | 91F00 | TPlayerBehaviour |
| camera::dirty | navSampler | 94720 | virtual TNavCameraControl |
| camera::fov | rebuildProjection | 9F2E0 | T3DViewport, TCharacterView |
| camera::nearPlane | rebuildProjection | 9F2E0 | T3DViewport, TCharacterView |
| camera::controller | cameraControllerInit | 91DE0 | engine (no class caller within 3 calls) |
| camera::inverse | cameraPoseCommit | 94D80 | virtual TNavCameraControl |
| viewport::view | pickRay | 8F720 | TPlayerBehaviour |
| viewport::pickCamera | copyCameraSnapshots | 11EB50 | engine (no class caller within 3 calls) |
| viewport::sceneManager | pickRay | 8F720 | TPlayerBehaviour |
| sceneManager::flags | heldLevelVisibility | 2462F0 | TPwrForceKinesis_S |
| sceneManager::levelGrid | heldLevelVisibility | 2462F0 | TPwrForceKinesis_S |
| view::camera | rebuildProjection | 9F2E0 | T3DViewport, TCharacterView |
| sceneNode::renderTransform | headRenderSample | 8B6E0 | virtual TMotilePart/TRBDDoor/TRBPushSurface |
| sceneNode::transform | pickRay | 8F720 | TPlayerBehaviour |
| sceneNode::localTransform | dragRotate | 118920 | engine (no class caller within 3 calls) |
| sceneNode::transformOrigin | hingeHandler | 11BC50 | TPwrForceKinesis_S |
| sceneNode::passMask | pickRay | 8F720 | TPlayerBehaviour |
| sceneNode::lastChild | propagateNodePose | 50EB0 | TCharacterView, TFireEffectRow, TForceBarricadeEffect |
| sceneNode::children | propagateNodePose | 50EB0 | TCharacterView, TFireEffectRow, TForceBarricadeEffect |
| sceneNode::name | sceneNodePositionOverflow | AC2A0 | TAISearchActivity, TMotileAI, TPlayerBehaviour |
| sceneNode::parent | hingeHandler | 11BC50 | TPwrForceKinesis_S |
| sceneNode::sector | cursorAim | BF680 | TPlayerBehaviour |
| sceneNode::bodyPart | dragDrop | 119C30 | TArsenalView |
| sceneNode::rayRoutine | pickRay | 8F720 | TPlayerBehaviour |
| sceneNode::rayContext | pickRay | 8F720 | TPlayerBehaviour |
| sector::origin | pickRay | 8F720 | TPlayerBehaviour |
| sector::lastMesh | pickRay | 8F720 | TPlayerBehaviour |
| sector::meshes | pickRay | 8F720 | TPlayerBehaviour |
| pick::origin | pickRay | 8F720 | TPlayerBehaviour |
| pick::direction | pickRay | 8F720 | TPlayerBehaviour |
| pick::distance | pickRay | 8F720 | TPlayerBehaviour |
| drag::flags | dragDrop | 119C30 | TArsenalView |
| drag::invalidDrop | heldItemRelease | 11B340 | TPwrForceKinesis_S |
| heldRelease::body | applyHeldRelease | 116EF0 | engine (no class caller within 3 calls) |
| gui::pointerX | guiMousePress | 2FE40 | engine (no class caller within 3 calls) |
| gui::pressX | guiMousePress | 2FE40 | engine (no class caller within 3 calls) |
| gui::hoverView | guiMousePress | 2FE40 | engine (no class caller within 3 calls) |
| gui::buttons | guiMousePress | 2FE40 | engine (no class caller within 3 calls) |
| drag::object | dragDrop | 119C30 | TArsenalView |
| drag::targetPoints | dragDrop | 119C30 | TArsenalView |
| drag::pivot | doorHandler | 11B400 | TPwrForceKinesis_S |
| gui::capture | playerBehavior | C1CF0 | virtual TPlayerBehaviour |
| gui::dragFormat | clearGuiDrag | 105220 | engine (no class caller within 3 calls) |
| guiTask::formCount | findGuiForm | 29030 | engine (no class caller within 3 calls) |
| guiTask::forms | findGuiForm | 29030 | engine (no class caller within 3 calls) |
| form::kind | findGuiForm | 29030 | engine (no class caller within 3 calls) |
| form::visible | findGuiForm | 29030 | engine (no class caller within 3 calls) |
| form::closing | closeGuiForms | 31F10 | engine (no class caller within 3 calls) |
| worldObject::characters | worldCharacterLoop | 18BE20 | TEndSessionMenu, TMainMenu |
| list::count | worldCharacterLoop | 18BE20 | TEndSessionMenu, TMainMenu |
| list::items | worldCharacterLoop | 18BE20 | TEndSessionMenu, TMainMenu |
| drag::viewport | doorHandler | 11B400 | TPwrForceKinesis_S |
| drag::planeHeight | doorHandler | 11B400 | TPwrForceKinesis_S |
| drag::hingePlane | hingeHandler | 11BC50 | TPwrForceKinesis_S |
| drag::alternatePlane | hingeHandler | 11BC50 | TPwrForceKinesis_S |
| sceneNode::queryFlags | heldSweep | 1170F0 | engine (no class caller within 3 calls) |
| character::health | combatPosture | B7560 | engine (no class caller within 3 calls) |
| character::aimOpponent | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::planeBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::unassistedBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::unassistedInitBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::radiusBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::distanceBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::selectedBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::behaviorBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| aimFrame::outputBack | aimPoint | AE2E0 | virtual TPlayerBehaviour |
| viewport::width | aimUnproject | 4C520 | TPlayerBehaviour |
| viewport::height | aimUnproject | 4C520 | TPlayerBehaviour |
| camera::farPlane | rebuildProjection | 9F2E0 | T3DViewport, TCharacterView |
| camera::target | terrainPass | 1B8760 | engine (no class caller within 3 calls) |
| pick::scratchBank | pickRay | 8F720 | TPlayerBehaviour |
| meshRayQuery::node | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::minDistance | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::distance | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::origin | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::direction | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::inverseDirection | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::traversalStack | meshRay | 26A910 | engine (no class caller within 3 calls) |
| meshRayQuery::scratchBank | meshRay | 26A910 | engine (no class caller within 3 calls) |
| nativeForm::x | guiFormCreate | 2A8A0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::y | guiFormCreate | 2A8A0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::width | guiFormCreate | 2A8A0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::height | guiFormCreate | 2A8A0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::layoutFlags | guiFormLayoutFlags | 291D0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::frameFlags | guiFormCreate | 2A8A0 | TClassSelect, TCompactSkillUI, TEndSessionMenu |
| nativeForm::title | guiSettingsConstructor | A94D0 | TMainMenu |
| nativeElement::stride | guiElementAppend | 32860 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::x | guiElementLayout | 33C90 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::y | guiElementLayout | 33C90 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::width | guiElementLayout | 33C90 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::height | guiElementLayout | 33C90 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::style | guiElementLayout | 33C90 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| nativeElement::group | guiActivate | 32940 | engine (no class caller within 3 calls) |
| nativeElement::text | guiButton | 32AE0 | engine (no class caller within 3 calls) |
| nativeElement::appearance | guiButton | 32AE0 | engine (no class caller within 3 calls) |
| nativeElement::flags | guiActivate | 32940 | engine (no class caller within 3 calls) |
| nativeElement::owner | guiActivate | 32940 | engine (no class caller within 3 calls) |
| nativeElement::action | guiActivate | 32940 | engine (no class caller within 3 calls) |
| nativeElement::handler | guiButton | 32AE0 | engine (no class caller within 3 calls) |
| nativeElement::help | guiButton | 32AE0 | engine (no class caller within 3 calls) |
| nativeString::referenceHeader | guiLiteral | 80F0 | T3DNode, T3DViewport, TAISearchActivity |
| nativeString::lengthHeader | guiButton | 32AE0 | engine (no class caller within 3 calls) |
| nativeForm::elementLast | guiFormElements | 2B200 | virtual T3DViewport/TAbstractToolTip/TArsenalContFrame |
| nativeForm::elements | guiFormElements | 2B200 | virtual T3DViewport/TAbstractToolTip/TArsenalContFrame |
| nativeForm::stateFlags | guiFormElements | 2B200 | virtual T3DViewport/TAbstractToolTip/TArsenalContFrame |
| nativeForm::skin | nativeSettingsCreate | A94D0 | TMainMenu |
| nativeForm::behaviorFlags | nativeSettingsCreate | A94D0 | TMainMenu |
| optionsForm::graphicsPane | guiSettingsConstructor | A94D0 | TMainMenu |
| optionsForm::controlsPane | guiSettingsConstructor | A94D0 | TMainMenu |
| optionsForm::controllerPane | guiSettingsConstructor | A94D0 | TMainMenu |
| nativeForm::scrollExtent | guiSettingsConstructor | A94D0 | TMainMenu |
| optionsForm::applied | guiOptionsCancel | A8390 | engine (no class caller within 3 calls) |
| nativeForm::contentLeft | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::contentTop | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::contentBottom | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::horizontalScroll | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::verticalScroll | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::contentRight | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| nativeForm::contentWidth | guiViewportToContent | 2CE40 | engine (no class caller within 3 calls) |
| drag::acceptedKind | heldUpdate | 11F610 | engine (no class caller within 3 calls) |
| drag::acceptedQuery | heldUpdate | 11F610 | engine (no class caller within 3 calls) |
| drag::acceptedPoints | heldUpdate | 11F610 | engine (no class caller within 3 calls) |
| view::pickFarDepth | pickDepthUnproject | 8F460 | TPlayerBehaviour, TPwrForceKinesis_S |
| sector::scene | releaseBodyDynamics | 116BE0 | engine (no class caller within 3 calls) |
| scene::physicsContext | releaseBodyDynamics | 116BE0 | engine (no class caller within 3 calls) |
| physicsContext::frequencyHz | physicsContextInit | 832B0 | engine (no class caller within 3 calls) |
| gui::dragSource | heldRecovery | 119CB0 | engine (no class caller within 3 calls) |
| gui::dragPayload | inventoryLoader | 11CF50 | engine (no class caller within 3 calls) |
| drag::specialSource | inventoryDeferredCleanup | 119970 | engine (no class caller within 3 calls) |
| inventorySource::special | inventorySourceFinish | 137800 | virtual TCharacterView |
| inventorySource::merchant | inventorySourceFinish | 137800 | virtual TCharacterView |
| inventorySource::finishSlot | guiDragCompletionRoute | 300E0 | T3DViewport, TAbstractToolTip, TArsenalContFrame |
| inventoryPayload::source | inventoryPayloadCreate | 133FC0 | engine (no class caller within 3 calls) |
| inventoryPayload::item | inventoryPayloadCreate | 133FC0 | engine (no class caller within 3 calls) |
| inventoryPayload::flags | inventoryPayloadCreate | 133FC0 | engine (no class caller within 3 calls) |
| inventoryJob::item | inventoryJobCreate | 11D480 | engine (no class caller within 3 calls) |
| inventoryJob::flags | inventoryJobCreate | 11D480 | engine (no class caller within 3 calls) |
| inventoryJob::sector | inventoryJobCreate | 11D480 | engine (no class caller within 3 calls) |
| inventoryJob::object | inventoryLoader | 11CF50 | engine (no class caller within 3 calls) |
| character::metadata | variantActorAllocate | BFF0 | virtual T3DNode/T3DView/T3DViewport |
| nativeThread::id | variantThreadStart | 21B80 | engine (no class caller within 3 calls) |
| scene::lifecycleFlags | variantSceneRetireRequest | 5AE90 | T3DViewport, TCharacterView, TItemRenderer |
| grip::callback | variantGripCallback | 154780 | TMotileDynamics, TPlayerBehaviour |
| grip::stride | variantGripSelect | 155250 | engine (no class caller within 3 calls) |
| physicsContext::scene | physicsContextInit | 832B0 | engine (no class caller within 3 calls) |
| physicsContext::running | variantPhysicsSchedule | 8BCE0 | engine (no class caller within 3 calls) |
| bodyPart::metadata | variantActorAllocate | BFF0 | virtual T3DNode/T3DView/T3DViewport |
| bodyPart::motionFlags | heldBodyIntegrate | 88D30 | virtual TTetraRBD |
| bodyPart::constraintCount | heldConstraintWalk | 89BF0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::childCount | heldReleaseSettle | 116BE0 | engine (no class caller within 3 calls) |
| bodyPart::releaseSettling | heldReleaseSettle | 116BE0 | engine (no class caller within 3 calls) |
| view::offscreenSampling | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| bodyPart::animationPose | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::targetPose | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| view::pickPassMask | xrGrabPick | 8FB30 | engine (no class caller within 3 calls) |
| sector::passMask | xrGrabPick | 8FB30 | engine (no class caller within 3 calls) |
| sceneNode::flags | xrGrabPick | 8FB30 | engine (no class caller within 3 calls) |
| sceneNode::interactionFlags | xrGrabStart | 11A690 | engine (no class caller within 3 calls) |
| character::partCount | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::parts | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::legCount | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::legs | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::bodyFrame | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| character::balanceFrame | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::role | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::contactCount | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| bodyPart::contacts | poseBuilder | 1760F0 | TMotileAI, TMotileDynamics, TPlayerBehaviour |
| contact::flags | walkingLegs | 15F950 | TMotileDynamics |
| character::shieldPart | weaponDefense | 162F80 | TMotileDynamics |
| character::specialHeldPart | weaponPostureSelect | 1523F0 | TMotileDynamics, TSkillSelector |
| bodyPart::restTetra | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| bodyPart::jointCount | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| bodyPart::joints | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| bodyPart::weaponData | equippedBind | 155290 | engine (no class caller within 3 calls) |
| gripJoint::connectedAnchor | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| gripJoint::connectedPart | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| gripJoint::flags | gripConstraintBind | 153210 | engine (no class caller within 3 calls) |
| character::impactVelocity | physicalImpact | 159A60 | TMotileDynamics |
| character::activePartCount | physicalImpact | 159A60 | TMotileDynamics |
| character::activeParts | physicalImpact | 159A60 | TMotileDynamics |
| character::impactKind | physicalContact | 1590D0 | virtual TMotileDynamics/TMotilePart |
| character::impactTarget | physicalContact | 1590D0 | virtual TMotileDynamics/TMotilePart |
| character::impactPoint | physicalContact | 1590D0 | virtual TMotileDynamics/TMotilePart |
| character::impactWeapon | physicalContact | 1590D0 | virtual TMotileDynamics/TMotilePart |
| bodyPart::velocityTetra | tetraIntegrate | 88D30 | virtual TTetraRBD |
| view::renderView | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| view::renderProjection | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| character::healthRestorable | healthFraction | 185A00 | TEndSessionMenu, TMainMenu |
| character::healthMax | healthFraction | 185A00 | TEndSessionMenu, TMainMenu |
| view::viewTexture | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| texture::glName | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| view::outputHeight | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| view::outputWidth | sampledViews | A0CE0 | T3DViewport, TCharacterView |
| character::transform | characterMove | 170810 | TMotileAI |
| sceneNode::boundsCentre | sectorUpdateBody | 58CD0 | engine (no class caller within 3 calls) |
| sceneNode::boundsRadius | portalSphereTest | 58AC0 | engine (no class caller within 3 calls) |
| sceneNode::lightHead | meshLightRecount | 26F420 | engine (no class caller within 3 calls) |
| sceneNode::lights | meshLightRecount | 26F420 | engine (no class caller within 3 calls) |
| sector::lightCount | meshLightRecount | 26F420 | engine (no class caller within 3 calls) |
| lightEntry::active | meshLightRecount | 26F420 | engine (no class caller within 3 calls) |
| lightEntry::next | meshLightRecount | 26F420 | engine (no class caller within 3 calls) |
| viewport::scene | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| scene::lightCount | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| scene::lights | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| light::shadow | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| light::kind | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| shadowMap::dirty | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| shadowMap::needed | shadowSchedule | 4F4A0 | T3DViewport, TCharacterView, TRenderer |
| character::stamina | hudBars | 13E0F0 | engine (no class caller within 3 calls) |
| character::staminaCeiling | hudBars | 13E0F0 | engine (no class caller within 3 calls) |

## 6. Not covered yet (honest list)

- Motion clip identities (0xD kick, 0x12 jump, 3 recover, 0x11, 0x21), and whether motion 12 is an "overhead".
- Material classes for metal vs cloth, lamp detection, and the blood skin offsets (blood thread).
- The `_SKIN` vertex formats behind Finger poses (bounds-checked, feature skips on mismatch).
- Render-side details (render thread is producing docs/render-assumptions.md and will be folded in once pushed).
- VR (xr_*), which isn't in the flat-screen test build.

How to repeat any check: the decoder is a read-only Python script using capstone and pefile over the installed
exe. It indexes every call and field access, and names the owning class from the exe's Free Pascal class tables.
