# Game exertion / fatigue research (Exanima 0.9.5.2g)

**PARTIAL — stopped early.** This research was cancelled (another thread owns it). Only what was disassembled before
stopping is recorded. Addresses are RVAs. Nothing here answers whether the game has an exertion system: **NOT PROVEN**
either way.

## What was disassembled

### combatUpdate 0x169200, 0x16B914–0x16BB45 (one block, read in full)

- 0x16B918–0x16B939: if byte +0x1303 != 0, `+D58 += const` (double at 0x16B92D's RIP target; value **not read**).
  0x16B945–0x16B96A: +0x1303 is cleared when `+0x11B8 - [global] + 0xBB8 < [global]` (two globals, not decoded; 0xBB8
  = 3000, units NOT PROVEN). What sets +0x1303: **NOT PROVEN** (not searched).
- 0x16B974–0x16B994: if `+DF4 == 2` (active defense) or `+9B8 == 4` (motion 4), `+1010 = [global at 0x16B98E target]`
  (value not read) and the regeneration step is skipped (jump to 0x16BA34).
- 0x16B99F–0x16B9EC: otherwise `+1010 += [float global] × [double const]`, then clamped down to a float constant
  when above it (constants not read). So +0x1010 (the health regen rate) is reset while defending / in motion 4 and
  ramps back up otherwise. Whether that reset value is lower than the ramp target (i.e. "defending stops regen"):
  **NOT PROVEN** (constants not read).
- 0x16B9F4–0x16BA2B: if `+D5C > const`, `+D58 += sqrt(+D5C) × +1010`; 0x16BA34–0x16BA55 cap +D58 at +D5C;
  0x16BA5C–0x16BA7D cap +D5C at +103C. (Agrees with GAME-FIELDS-AUDIT.md.)
- 0x16BA84–0x16BB3B (+0xD60, the HUD's third element): when `[global] - const > +D60`, `+D64 += const` (capped at a
  const, 0x16BAAF–0x16BAED), `+D60 += +D64` (0x16BB08); when +D60 reaches a global it is set to that global and
  `+D68 = -1` (0x16BB31–0x16BB3B). So +D60 is a value that accelerates (+D64 is its rising rate) toward a ceiling
  each step. What it represents: **NOT PROVEN** (writers 0x1700F0 at 0x1702E6, 0x186BC0 at 0x186BED, 0x159A60 at
  0x15B861/0x15BAD2/0x15BAE2 and reader 0x162F80 at 0x16334B not decoded).

### Field inventory (fieldx scan of `[reg+0xOFF]`, not decoded)

- Writers of +0xD58 still undecoded: 0xB6A50 (0xB6A9E), 0x116700 (0x1167BB), 0x123D30 (0x124019), 0x1568E0
  (0x156A4F), 0x156D20 (0x156F13), 0x168B40 (0x168B67), 0x168BC0 (0x168CA1), 0x186BC0 (0x186BE1), 0x1E2B90
  (0x1E2F7A), 0x1E3080 (0x1E3236), 0x25B150 (0x25B280), 0x25B2A0 (0x25B590). The scan matches any struct with a
  field at that offset, so these are not proven to be character writes.
- Writers of +0x1010: 0x159700 (0x159713), 0x159A60 (0x159E3A), 0x168410 (0x16869F), 0x169200 (0x16B994, 0x16B9C3,
  0x16B9EC). Only the 0x169200 sites were read.
- +0xD60 users: 0x13E0F0 (0x13E436), 0x159A60, 0x162F80, 0x169200, 0x1700F0, 0x186BC0.

## Not started (NOT PROVEN)

Questions 2–6 of the brief: breath/pant sounds in Sound.rpk, the 'Head Pain' motion (45), the low-health warning
0x158A10 / 0x13D700, speed/strength scaling by health (0x163EF0, 0x1CAF0, attack routines), and the HUD draw of
+D60 through 0x13CC90.
