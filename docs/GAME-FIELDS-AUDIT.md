# Game fields audit (kick, jump, vitals, weapon classes, Animated kick)

Exanima 0.9.5.2g (SHA-256 af9033c7…, the reviewed build). Addresses are RVAs. **PROVEN** means the code that
WRITES the value and the code that READS it were disassembled and are cited here; names follow what that code does,
not bar colours, old comments, ExanimaLua or earlier notes. **NOT PROVEN** values are not used by the build, or are
listed below as still used and waiting for a decision (section 4).

Scope: the fields the kick and jump gates, the Animated kick, the condition (screen) effects and the weapon classes
use. The other features' fields (camera, picking, held items, wounds, blood, VR...) are checked at start-up by
instruction-window witnesses (`tools/native_contracts.json`) but have **not** been decoded to this standard yet
(section 5).

## 1. Vitals

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `magic` +0xD74 (float 0..1) | init 0x173230 (0x1737E6); spent through **0x17A640** (adds the cost to the pending drain +0xD80; when the cost exceeds what is left it calls the character's callback with 0x80 and 0x179DE0); 0x17A6B0 drains the pending amount (0x17A7F4–0x17A800) and refills toward +0xD78 (0x17A934 on) | HUD 0x13E0F0 → 0x13C470 fill (`movss xmm3,[rax+0xD74]` at 0x13E3B6) | nothing now (the condition effects' winded look read it as "stamina"; removed) | **PROVEN magic.** The only callers of 0x17A640 are spell classes (their VMTs): TForceBlastEffect 0x232060 (string "Error charging blast effect."), TForceWaveEffect 0x235210, TForceTorrentEffect 0x236620 ("PowerUsed"), TForceBarrierEffect 0x23CC30 ("PowerUsed"), TPwrForceKinesis_S 0x246200, TPwrDispVanish_I 0x255A50, TPwrDispLongDoor_I 0x256190, TPwrEnergyShock_I 0x28EF30; 0x2473C0 (class not resolved). No attack or movement code calls it. Matches the audit thread's list. |
| `magicCeiling` +0xD78 (float 0..1) | init 0x173230 (0x1737E0); 0x17A6B0 lowers it by +0xD88 × +0xD84 (0x17A710–0x17A72C) | HUD 0x13E3A5 → 0x13C470 second mark; 0x17A6B0 refill limit | nothing | **PROVEN** (how far magic refills) |
| `balance` +0x10B8 / `balanceMax` +0x10BC | `combatUpdate` 0x169200: regenerates (0x169A4C–0x169A86), capped at +0x10BC (0x169C76–0x169C97); on death set to max − k (0x169B5F); reset by the character move 0x170810 (0x1708A7), 0x1710B0, 0x171CB0, 0x1A0870 | 0x169200: when below max − margin the game requests motion 3 (+0xEB0 = 3, 0x169BC9) and plays it (0x169C5C); player behaviour 0xC286E compares it with max × threshold; AI 0x231710, 0x2393A0, 0x23C5C0, 0x23D680, 0x242A00 | manual defense readiness (combat.inl, input.inl: the same comparison the game makes at 0xC286E); VR teleport puts it back after the move resets it | **PROVEN** as a meter that refills every step and, when low, makes the game play its recover motion 3. "Balance" is our name for that behaviour. Never stamina. |
| +0xD58 | init 0x173230 (0x174724); `combatUpdate` regeneration `+= sqrt(+0xD5C) × [+0x1010]` (0x16BA2B) capped at +0xD5C (0x16BA55); bleeding 0x168410 `-= +0x10DC × k` (0x1684C4); `bodyDamage` 0x159700: `-= 0.1 × arg2` (0.05 if +0xDF8 bit 0x800000; 0x159726–0x159768), then if +0x12A4 bit 8 `-= 0.2 × arg3` (0x15977B), else scaled by new/old +0xD5C (0x1597E6–0x1597FA), zeroed below 0.01 (0x15983F); `physicalImpact` 0x159A60 (0x15BBB9, 0x15BBE4); death path 0x17B100 (0x17B228) then lifeFlags 0x100 | 0x17A100 interpolates `+0xD70 = lerp(+0xD6C, +0xD58)` (0x17A1DA–0x17A1F7); HUD 0x13E327 draws `+0xD70 / +0x103C` as the first (yellow) segment of the left bar (13C880 first argument, segment 0 → first fraction, 0x13C91C–0x13C9AC); `combatPosture` B7560 gates on > 0.01 | manual defense mirrors the B7560 gate (> 0.01); condition effects' hurt look (**disabled, see 4**) | **NOT PROVEN (on hold).** My evidence: it recovers toward +0xD5C, bodyDamage's arg2 lowers it alone, bleeding lowers it, and the HUD draws its interpolated copy as the first segment. No write from attack or movement code was found. The audit thread is settling the name. |
| +0xD5C | init = +0x103C (0x173230, just before 0x174724); `bodyDamage` `-= 0.2 × arg3` (0x1597C5) and by 0.1 × +0x103C on the r9 flag (0x159884); bleeding `-= (+0x10DC × k)²` (0x16850B); healing item 0x1A0C50 `+= k × +0x103C` capped at +0x103C (0x1A0CC8–0x1A0CE1) | caps +0xD58's regeneration (0x16BA40–0x16BA55); HUD `+0xD5C / +0x103C` = end of the second (pale) segment (0x13E35E–0x13E366 → 13C880 second argument, 0x13C930–0x13C939) | nothing now | **NOT PROVEN (on hold).** My evidence: the ceiling +0xD58 refills to; only damage arg3 and bleeding lower it; healing raises it to +0x103C. |
| +0x103C | init/levels 0x173230, 0x1700F0 | divisor of both HUD fractions; cap for +0xD5C (healing 0x1A0CD0, 0x1695E3) | condition effects (disabled) | **NOT PROVEN (on hold)**, same reason |
| arg2 / arg3 of `bodyDamage` | callers 0x157FF0 (0x158284–0x158290: arg2 = xmm9 × k after adding xmm9 to +0x1040, arg3 = a constant) and `physicalImpact` 0x159A60 (0x15C714–0x15C729: two locals × k) | | | **NOT PROVEN** which physical quantity each is (cut vs blunt, or other). Agrees with the blood thread's reading of the arithmetic (+0xD5C −= 0.2·arg3, +0xD58 −= 0.1·arg2). |

No separate stamina or exertion value: the HUD draws only these two bars, the blue one is magic, and no writer of
+0xD58 found is in attack or movement code. **Exertion: NOT PROVEN to exist**, so the winded look is off.

## 2. Kick and jump gates

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `motion` +0x9B8 | `motionPlay` 0x179EE0 stores the motion; attack starts play 5 + [+0xD2C] (`swingStart` 0x157240 at 0x15760B), 8 + n (`oppositeStart` 0x157700 at 0x157AFE), 0xB (`overheadStart` 0x157DC0), 0xC (`thrustStart` 0x157BF0); motion 3 from balance loss (0x169C5C) and death (0x17B26A) | `combatUpdate` 0x16B06F: while motion 0xD, state = 0x8000 (0x16B09C); `contactFrame` 0x1612C9 special-cases 0xD | busy check (attacks 5–12, kick 0xD, jump 0x12); Animated kick runs while 0xD; stagger counter (motion 3) | **PROVEN** for 0xD (dedicated game handling), 3, and the attack starts. The upper bound of +0xD2C (swing variants) is not decoded; 5–12 covers 5+0..2, 8+0..2, 0xB, 0xC. Jump 0x12: **NOT PROVEN** (no game code found that plays it); used only as "our own jump is busy". |
| `stateFlags` +0xDF4 | 17 sites set 0 (e.g. 0x169C06 after recovering), 8 (0x16B168 while +0x12F3 defense is active; 0x16B1B1 in motion 4), 0x100 (0x16B05C after motion 3), 0x8000 (0x16B09C during motion 0xD), 0x200/0x300/0x800/0xA00/... (0x156AA8, 0x16A998, 0x1564D6, 0x156E48, ...) | dispatch in `playerBehavior` 0xC1D37; 0x169A20 (0x300), 0x169BE0 (& 0xAC0) | the gate plays only in state **0**, the base value the game returns to | **PROVEN** that 0 is the value the game resets to and that it sets other values for its own reactions (defense, recover, the kick). The meaning of each other value is **NOT PROVEN** and is not used. (The earlier "8 = upright on one foot" was wrong: 8 is set while defending or in motion 4.) |
| `lifeFlags` +0xDF8 bit 0x100 | death path 0x17B100 sets it (0x17B248) right after lowering +0xD58 | `combatUpdate` 0x169B18: plays motion 0x21 (0x169B46); `playerBehavior` 0xC1DD1 refuses live input | the gate refuses when set | **PROVEN** (dead) |
| `combatStance` +0x12FE | 0xF0274 (= 1), 0x1200EC, 0x1568E0 (= 0) | `playerBehavior` 0xC2028 chooses explore vs combat step | nothing now (the stance gate was removed) | not needed here |

The kick and jump no longer read health, magic, balance or stance.

## 3. Weapon classes and the Animated kick

| field + offset | WRITTEN by | READ by | ExCamera uses it for | status |
|---|---|---|---|---|
| `gripFlags` +0x1068 | equip posture selector 0x1523F0 (0x15241F: item flag word & 0xFFFF; 0x15256E 0x90 for two weapons; 0x152601/0x15260D 0xC0/0x80 unarmed; 0x152618 & 0xF0), `gripRefresh` 0x152100 (0x1521A2) | `gripLookup` 0x155250 ((flags & 0x70) >> 4 → grip entry), the posture switch 0x152531 (0x10 one hand, 0x20 two hands, 0x30 posture 0x40), many attack routines | weapon class → clip set (kick_retarget.h) | **PROVEN** (signature group `kickClasses` pins 0x155250 and 0x152531) |
| `weaponCount` +0x920, `shieldPart` +0xBF8, `specialHeldPart` +0xC00 | equip code (0x1552CC stores weapons) | 0x1523F0: shield branch 0x15245D, special 0x1524DD, weapon count 0x1523FD/0x152560 | weapon class | **PROVEN** as the posture selector's inputs; what a "special held part" is: **NOT PROVEN** (given lower-body mode) |
| weapon names per class | item database `objdb.rdb` weapon chunks (tag 0x4000), first word | | docs only | data, decoded (MIXAMO-KICK-DESIGN.md section 9) |
| `motionProgress` +0xA28 | pose builder 0x1761F7 = (time − [+0xA24]) / last key time | pose builder keyframe search 0x176203 on | Animated kick timing | **PROVEN** |
| `facing` +0xCFC | `playerBehavior` copies the gaze +0xCB0 (0xC2811, 0xC29DC); 11 other writers | AI and behaviour | Animated kick heading (only its level direction) | **PROVEN** that the player's is copied from where it aims |
| body parts (+0x910/+0x918 parts, +0x9D8/+0x9E0 legs, part +0x2A0 pose, +0x428 parent, +0x5B0 role 0x10/0x13, +0x338/+0x340 contacts, contact +0x10 flags) | not decoded in this audit | VR driver and pose builder (MIXAMO-KICK-DESIGN.md 3.1–3.2) | Animated kick (the part graph and the support foot) | **NOT PROVEN** to this standard (witnessed, used by the VR driver; writers not traced) |
| compose sites, `animationPose` +0x550, `targetPose` +0x4F0, `balanceFrame` +0xDFC | pose builder 0x1760F0 (0x176C69, 0x176CB2, 0x17738C) | motors (`weightedAnchor` 0x1C900, `motorWeights` 0x1CAF0) | Animated kick target replacement | **PROVEN** for the compose path (MIXAMO-KICK-DESIGN.md 3.2) |

## 4. Still used while NOT PROVEN — decisions needed

* **Condition effects (hurt look)** read +0xD58 / +0x103C. Disabled in this build until the audit thread settles
  those fields (the setting stays, the hurt look stays dark); the damage flash is unaffected.
* **Manual defense readiness** compares +0xD58 > 0.01 exactly as the game's B7560 does; that copies the game's own
  gate rather than interpreting the field, so it stays.
* **Animated kick** depends on the body-part fields in section 3 (NOT PROVEN to this standard). It is opt-in
  (Kick style: Animated). Hunter decides: keep it selectable while those fields are decoded, or turn it off.
* **Jump motion 0x12**: only our own jump uses it (no clip is played).

## 5. Not audited here

The camera, picking, held-item, wound, blood, render and VR fields are witnessed at start-up by instruction
windows but have not been decoded to the write/read standard above. They need the same pass, feature by feature.
