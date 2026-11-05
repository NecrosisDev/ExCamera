# Game fields audit (kick, jump, vitals, weapon classes, Animated kick)

Exanima 0.9.5.2g (SHA-256 af9033c7…, the reviewed build). Addresses are RVAs. **PROVEN** = the code that WRITES
the value and the code that READS it were disassembled and are cited; names follow what that code does, never bar
colours, old comments, ExanimaLua or earlier notes. **NOT PROVEN** values are off in the build or listed in section 5.
Kept in step with the master checklist `docs/game-code-validation.md` (branch claude/project-thread-ed8e4j), copy
at `E:\ExCamera-playtest\GAME-CODE-VALIDATION.md`; class names come from Free Pascal VMTs (a routine's address
stored in .data, the ShortString class name 0xB8–0x140 bytes before it).

## 1. Vitals

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `health` +0xD58 | init 0x173230 (0x174724); TMotileDynamics `combatUpdate` 0x169200 regeneration `+= sqrt(+D5C) × [+1010]` (0x16BA2B), capped at +D5C (0x16BA55); bleeding 0x168410 `-= +10DC × k` (0x1684C4); `bodyDamage` 0x159700 `-= 0.1 × A` (0.05 × A if +DF8 bit 0x800000; 0x159726–0x159768), scaled by new/old +D5C after `+D5C -= 0.2 × B` (0x1597E6–0x1597FA), zeroed below 0.01 then the die routine 0x156480 (0x15983F, 0x1598C3); `physicalImpact` 0x159A60 (0x15BBB9); spell-hit 0x17B100 (0x17B228) | 0x17A100 `+D70 = lerp(+D6C, +D58)` (0x17A1DA–0x17A1F7); HUD 0x13E0F0 draws `+D70 / +103C` as the first (yellow) segment through 0x13C880 (first fraction, 0x13C91C–0x13C9AC); `combatPosture` 0xB7560 gates on > 0.01 | manual defense mirrors the 0xB7560 gate; hurt look (render's c198ac4 reads it in the merged build) | **PROVEN** current health (agrees with the audit thread) |
| `healthRestorable` +0xD5C | init = +103C (0x173230, before 0x174724); `bodyDamage` `-= 0.2 × B` (0x1597C5) and `-= 0.1 × +103C` on its flag (0x159884); bleeding `-= (+10DC × k)²` (0x16850B); healing item 0x1A0C50 `+= k × +103C` capped at +103C (0x1A0CC8–0x1A0CE1) | caps health's regeneration (0x16BA40–0x16BA55); HUD `+D5C / +103C` = end of the pale segment (0x13E35E–0x13E366 → 0x13C880 second fraction, 0x13C930) | nothing | **PROVEN** |
| `healthMax` +0x103C | 0x173230, 0x1700F0 (0x170326) | divisor of both HUD fractions; cap for +D5C | hurt look, wound sizing | **PROVEN** |
| `magic` +0xD74 / `magicCeiling` +0xD78 (0..1) | init 0x173230 (0x1737E6, 0x1737E0); spent through 0x17A640 (adds the cost to the pending drain +D80; if the cost exceeds what is left, callback 0x80 and 0x179DE0); 0x17A6B0 drains the pending amount (0x17A7F4–0x17A800), refills toward +D78 and lowers +D78 by +D88 × +D84 (0x17A710–0x17A72C) | HUD 0x13E3B6 / 0x13E3A5 → 0x13C470 (the blue bar); AI picker 0xB7E20 | nothing | **PROVEN magic.** Only callers of 0x17A640 (by VMT): TForceBlastEffect 0x232060 ("Error charging blast effect."), TForceWaveEffect 0x235210, TForceTorrentEffect 0x236620 ("PowerUsed"), TForceBarrierEffect 0x23CC30 ("PowerUsed"), TPwrForceKinesis_S 0x246200, TPwrDispVanish_I 0x255A50, TPwrDispLongDoor_I 0x256190, TPwrEnergyShock_I 0x28EF30; 0x2473C0 not resolved. No attack or movement code. Matches the audit thread. |
| `balance` +0x10B8 / `balanceMax` +0x10BC | 0x169200 regeneration 0.005 a step (0x169A4C–0x169A86), cap (0x169C76–0x169C97), death (0x169B5F); impacts 0x1587E0; force spells 0x231710, 0x2393A0, 0x23C5C0, 0x23D680, 0x242A00; resets 0x170810 (0x1708A7), 0x1710B0, 0x171CB0, 0x1A0870 | 0x169200: below max − 0.05 the game requests motion 3 and plays it (0x169B99 → 0x179810, 0x169C5C) unless the state is one of its reactions (& 0xAC0); player behaviour 0xC286E | kick/jump gate (≥ max − 0.05); manual defense readiness (combat.inl, input.inl, the same comparison); VR teleport restores it after the move resets it | **PROVEN** (hidden, not on the HUD) |
| `bodyDamage` arguments A, B | callers 0x157FF0 (0x158284–0x158290) and `physicalImpact` 0x159A60 (0x15C714–0x15C729) | | | arithmetic **PROVEN** (agrees with the blood and audit threads); which physical quantity each is: **NOT PROVEN**, not used |

No stamina or exertion value exists: the HUD has these two bars, the blue one is magic, and no writer of +D58 is in
attack or movement code. Low health alone does not knock the character down (audit thread). The winded look is gone.

## 2. State, death and the kick and jump gates

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `stateFlags` +0xDF4 (a value) | 0 (17 sites, e.g. 0x169C06 after recovering), 1 (0x16B470), 2 (0x166604), 8 (0x16B168 while +12F3 defense is active; 0x16B1B1 in motion 4), 0x20 (0x16B2D6, 0x179EC0), 0xC0 (0x17B0E1), 0x100 (0x16B05C after motion 3), 0x200 (0x16A6BA from 0x300, 0x16A742, 0x156AA8, 0x156D04), 0x300 (0x16A998 after motion 0xE/0xF, 0x162D69), 0x600 (0x126555), 0x800 (die 0x156480 at 0x1564D6, 0x167EDF), 0xA00 (0x156D20 at 0x156E48, called from 0x167ED0; 0xF12C4; 0x1A0A95), 0x8000 (0x16B09C while motion 0xD), 0x10000/0x10004 (0x16B514/0x16B504) | `playerBehavior` 0xC1D37; the game tests `& 0x800` (32 sites), `& 0xAC0` (0x169BE6), `& 0xA00` (0x168426) | `fp::deadState` (& 0x800), `fp::downState` (& 0xA00: knocked-down view and third-person follow), `fp::reactionState` (& 0xAC0: steady head, jump push); crouch uses the game's own masks 0xAC0 (0x16388A), 0x40204 (0xBFF96), 0xA00 (0xAB3E7); the attack cue's upright test is not down (& 0xA00); kick/jump only in 0 | **PROVEN** (checklist 55d523c and the final batch): standing values 0 standing, 1 settle step (0x16B470), 2 active defense (0x166604), 8 reactive defense motion (0x16B168, 0x16B1B1), 0x20 power/command motion in progress (0x179E40 from TForceBlastEffect, TForceWaveEffect, 0x2360E0, 0x23CB20; 0x169200 for motions 0x22–0x28 at 0x16B2D6), 0x100 stumble recover (0x16B05C: stance +0x590 < 0.85 plays motion 3), 0x8000 motion 0xD, 0x10000/0x10004 motion 0x1F; dead = `& 0x800` (0x800, 0xA00); down = `& 0xA00`, the game's own test (0x16990A, 0x169AB7, 0x168426): 0x200 on the ground (0x16A6BA, 0x156AA8, 0x156D04), 0x300 getting up (0x162D69, 0x16A998; 0x162C10 picks get-up motion 0xE/0xF by facing), 0x600 scripted lay-down (0x126555), and dead; the game's don't-interrupt test `& 0xAC0` (0x169BE6) adds the standing stagger 0xC0 (0x17B0E1); 0 = standing base; 8 defense / motion 4; 0x8000 during motion 0xD; 0x10004 a standing action in motion 0x1F (0x16B504). |
| `lifeFlags` +0xDF8 bit 0x100 | 0x17B100 only (0x17B248), reached from spell effects TTendrilEmitter 0x1A2CE0, 0x1A1550 and 0x28F4F0 | 0x169B18: plays motion 0x21, balance = max − 0.5; cleared when the +11B4 timer passes (0x169B74–0x169B8A) | nothing (every "dead" use replaced) | **PROVEN: a spell-hit stun, not death** |
| `motion` +0x9B8 | `motionPlay` 0x179EE0; attack starts 5 + [+D2C] (0x157240 at 0x15760B), 8 + n (0x157700 at 0x157AFE), 0xB (0x157DC0; the thrust, audit thread), 0xC (0x157BF0) (the owner names in xr_contracts.json are older labels); motionPlay clears defense +0x12F3 and calls 0x162E80 only for 5–12 (0x179F60–0x179F7A); 3 on balance loss (0x169C5C) and the stun (0x17B26A) | 0x16B06F (motion 0xD → state 0x8000); `contactFrame` 0x1612C9 (0xD has its own leg-contact pass) | busy check (attacks 5–12, 0xD, 0x12); Animated kick runs while 0xD; stagger counter (motion 3) | **PROVEN** (TPlayerBehaviour tick 0xC1CF0, chooser 0xAE9D0): a press under 300 ms after the last sets +0x9C \|= 8 (0xC2462–0xC247E); side = angle(aim +0x1B0, body +0xAD4) minus 15° (+0x1068 == 0) or 20° (otherwise; 0xC24C3), negative sets +0x1BC = 2 (0xC251E); Alt (binding slot 6, 0x2B7340, keyWord bit 0x80 at 0xAA1DA) with a weapon class (+0x1068 & 0x30) sets +0x9C \|= 0x10 (0xC285A). Motion: +0x9C & 0x10 → 0xAEDC0 → 11 (0xB, the thrust); & 8 → 0xAEEC0 → 12 (×1.5 damage at 0x159C68; the game gives it no name); +0x1BC == 2 → 0xAEF50 → 0x157700 → 8–10; otherwise 0xAEFD0 → 0x157240 → 5–7; the variant 0–2 from 0x156FB0. So "5–7 ordinary, 8–10 opposite, 11 thrust, 12 rapid press" and "Alt makes a thrust" are proven. |
| motion 13 / 0xD | no input of the game starts it (no constant request; +0xEB0 is written with 3, 4, 0, 0x11, 0x13, 0x19 and registers at 0xBEE27/0xBEE43, 0x163B42, 0x179D34) | 0x16B06F (state 0x8000, +0xDC8 = 2 × +0xDCC) and 0x1612C9 (no leg-support check) | Classic and Animated kick play it | **PROVEN "Kick"**: the game's motion-set name table (Resource.rpk at 0x19582826, 49 names × 24 bytes): 3 Defence stance, 5–7 Attack Mid/Down/Up, 8–10 Attack Combo 1–3, 11 Stab, 12 Overhead Swing, 13 Kick, 14/15 Get up / Get Up 2, 16/17 Fall Forward/Backward, 18 Jump, 31 Switch Weapons, 33 Rebalance |
| motion 18 / 0x12 | no starter found | | our jump plays no clip; 0x12 is only "our jump is busy" | **PROVEN "Jump"** (name table); not played |
| rigid push 0x89240 | | force spells, cloth | the jump's push | **PROVEN** (audit thread) |
| `combatStance` +0x12FE | the TAB combat toggle (key slot 0x2B7390) writes it (audit thread); 0xF0274, 0x1200EC; cleared 0x156900 in state 0xA00 | `playerBehavior` 0xC2028 (explore vs combat step); HUD 0x13E9C0 marker | not by the kick or jump any more | **PROVEN** (audit thread) |

The kick and jump read: the pointer state (ours), `motion` (busy), `stateFlags` (dead; base 0), `balance`. Never
health, magic or stance. Refusal texts: "…not while you are attacking, jumping or already kicking", "…not while the
game has you recovering, defending or in another reaction", "…wait until you've got your balance back".

## 3. Weapon classes and the Animated kick

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `gripFlags` +0x1068 | equip posture selector 0x1523F0 / twin 0x152660: = the weapon item's flag word & 0xFFFF (0x15241F), 0 with no weapon (0x152427), 0x90 for two one-handed weapons (0x15256E), 0x80 or 0xC0 unarmed by character data +0x58 bit 0x100 (0x15260D / 0x152601), finally & 0xF0 (0x152618); `gripRefresh` 0x152100 (0x1521A2); 0x155030 sets 0x10 when (flags & 0x30) == 0 (0x1550D4) | `gripLookup` 0x155250: grip entry (flags & 0x70) >> 4, 0x90 → 1, second slot → 5; posture switch 0x152531: 0x10 → posture 1 (or 4 dual), 0x20 → 0x10 / 0x20 (bit 0) / 0x30 (0x521), 0x30 → 0x40; shield (+BF8) and special-held (+C00) branches first | weapon class → clip set (kick_retarget.h) | **PROVEN**, with the game's own names (table 0x2ED0F0, read at 0x258CE5; checklist 62c3d5b): 0x10 One Handed (motion set 1), 0x90 2W Light (4), 0x20 Two Handed (0x10) / 2H Light (item bit 0, 0x20) / Greatsword (item 0x521, 0x30), 0x30 Polearm (0x40), 0x80/0xC0 no recognised class; shield path (+BF8) 0x100/0x200/0x300 = 1H & Shield / Shield C1 / Shield C2; special held (+C00) = 1H & Torch (3). Pinned by `kickClasses` (0x155250, 0x152531). `classify` in kick_retarget.h matches. The mod's own tests copy the game's: `wide = gripFlags != 0` is `playerBehavior`'s side threshold at 0xC24C3 (15° when +0x1068 == 0, else 20°); `alternate = gripFlags & 0x80` is the opposite family's slot test at 0x157AB3. |
| `weaponCount` +0x920, `shieldPart` +0xBF8, `specialHeldPart` +0xC00 | equip (0x1552CC stores weapons) | 0x1523F0 (0x1523FD, 0x15245D, 0x1524DD) | weapon class | **PROVEN** as the selector's inputs; what a special held part is: NOT PROVEN (lower-body mode) |
| `motionProgress` +0xA28 | pose builder 0x1760F0 (0x1761D7–0x1761F7) and 0x174E00: (elapsed − blend-in +A24) / clip length; blend-in stored at 0x17A063; `motionPlay` resets it | pose builder keyframe search (0x176203 on); defense update 0x166580 commits at 0.30 (0.25 with swing bit 0x10000; 0x1669BA/0x1669CF); parry window 0x159A60 (0x15B990/0x15BCAC) | Animated kick timing; attack wind-up commit point 0.30 | **PROVEN** (the game's own commit points: 0.30, and 0.4 for motion 11 the Stab in the defense update at 0x1674FA–0x16750B and 0x16778D–0x16779A) |
| `swingBits` +0x1018 | 0x2: 0x158EF0, 0x157FF0 (0x15859D); 0x4: 0x159A60 (0x15C212, 0x15C242, 0x15C26A); 0x20 never set | | the wind-up arc ends when 0x2 or 0x4 comes on | **PROVEN** for 0x6 (contact bits; no "bounce"/"block" names) |
| `facing` +0xCFC | `playerBehavior` copies the gaze +0xCB0 (0xC2811, 0xC29DC) | AI, behaviour | Animated kick heading (level direction only) | **PROVEN** for the player |
| compose sites, `animationPose` +0x550, `targetPose` +0x4F0, `balanceFrame` +0xDFC | pose builder 0x1760F0 (0x176C69, 0x176CB2, 0x17738C) | motors (0x1C900, 0x1CAF0) | Animated kick target replacement | **PROVEN** (MIXAMO-KICK-DESIGN.md 3.2) |
| body parts (+0x910/+0x918, +0x9D8/+0x9E0, part +0x2A0, +0x428, +0x5B0 role 0x10/0x13, +0x338/+0x340 contacts, contact +0x10) | not decoded here | VR driver; `contactFrame` 0x1612F0 loops the legs at +0x9D8/+0x9E0 | Animated kick part graph and support foot | **NOT PROVEN** to this standard |

## 4. Changed in the build because of this audit

* Every "dead" check reads `(DF4 & 0x800) != 0` (camera opponent, knockdown camera, manual defense, crouch, wounds,
  VR hand pose, kick/jump, jump push). `lifeFlags & 0x100` is used nowhere.
* Kick/jump: base state 0 and balance ≥ max − 0.05; no health, magic or stance gate.
* The blue bar is magic; the condition effects' winded look is removed.
* "Knocked down: Show third" and the third-person knockdown follow use the game's own down test (& 0xA00); the
  old mask (0xAC0|0x40204|0xA00) fired on the standing stagger 0xC0 and 0x10004 and named a bit (0x40000) the game
  never writes. Crouch, the steady head and the jump push stop on the game's own & 0xAC0.
* Every "unused kick clip" statement is gone (code, settings tips, changelog).

## 5. Open

* **Motion 13 as the kick's carrier.** Proven: it is the game's own "Kick" clip, which its inputs never start, so
  playing it is legitimate. While it plays the game sets state 0x8000, sets +0xDC8 to 2 × +0xDCC (as while
  defending) and re-grips via 0x152930(+0xE2C) when stance +0x590 ≥ 0.85, and `contactFrame` 0x1603F0 skips its
  leg-support check (0x1612C9, as for state 0x4000). That skipped check means the standing leg is not held by the
  game's support logic during the kick, which is the likely cause of the toppling seen with a 40 ms blend (200 ms now).
  What +0xDC8 scales is not decoded. The Animated kick still depends on the body-part fields below.
* **Body-part fields** for the Animated kick (section 3).
* **Finger poses / skin** (render and VR code): layout PROVEN by the register routine 0xD39B0 (0xD39C0–0xD3AEF): skin
  +8 back-pointer, +0x10 count, +0x18 node array, +0x20 inverse binds, +0x28 floats, node id at +0x17C; skin build
  0xD9940. The _SKIN name and the vertex formats are NOT PROVEN; they are bounds-checked and skip on mismatch.
* Still unproven, nothing named by them: any "bounce"/"block" meaning of the swing bits 0x2/0x4.
* Not audited here: camera, picking, held-item, wound, blood, render and VR fields (the master checklist).
