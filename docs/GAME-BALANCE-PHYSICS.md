# How Exanima keeps a character upright, and why a kicker can fall

Read from the installed Exanima 0.9.5.2g executable (`Exanima.exe`). Every number in brackets is an RVA (an
address inside the executable), so each claim can be checked in a disassembler. Nothing here comes from field
names, comments or colours. **PROVEN** means the instructions were read and the claim follows from them.
**NOT PROVEN** means it was not traced, or only partly. No game was run for this document. Nothing in the mod was
changed.

Short names used below: "the character" is the fighter object the game updates each physics step. `+0x12B0`
means "the value stored 0x12B0 bytes into the character". "Per step" means once per physics step.

---

## 0. The short answer

The game does not decide that you fell by testing whether the body is physically toppling over. It uses a **lean
rule**:

* It measures the angle between straight up and a line drawn from a **support point between the two feet** to the
  **chest**. It calls the result uprightness (`+0x12B0`): 1 is straight, 0 is flat.
* If the lean stays past about **31 degrees** for about **three physics steps**, uprightness drops below 0.3 and the
  character is put on the ground (state 0x200).
* The support point is built from **both** feet's current positions, including a foot that is up in the air.

During the game's own kick (motion 13), nothing in the leg-support code knows about the kick except one branch. That
branch pins both legs as supports and turns off corrective stepping. The pose motors that drag every body part
toward its target pose are **world-space pulls**, so they hold whatever pose the targets describe. A mod that moves
the hips and leans the torso therefore moves the real chest. If the clip puts the chest more than about 31 degrees off
vertical from that support point, the game's own rule will drop the character. The rule and the pull are PROVEN.
Whether a given clip gets that far is NOT PROVEN; it has to be measured (section 7).

---

## 1. What happens in one physics step

The character update is `combatUpdate` 0x169200 (a TMotileDynamics routine). In address order:

| Step | Call site | What it does (PROVEN unless marked) |
|---|---|---|
| Chest position | `supportPosition` 0x163BB0 at 0x16BB4C | Sets `+0xBE8` to the average position of the chest part's (`+0xD48`) attachment points. Each point is a weighted mix of the chest's four simulated nodes (0x1C900 is `w0·P0+w1·P1+w2·P2+w3·P3`, 0x1C900–0x1C94A; loop 0x163C08–0x163C6D). With no chest it uses the root position `+0x2C4` (0x163C90). So **+0xBE8 is the chest's current position**. |
| Chest velocity | 0x169E74–0x169EE4 | `+0xC50` and `+0xC5C` are running averages (weights 0.4 and 0.1) of how far `+0xBE8` moved since the last update (`+0xC44` holds the previous position). |
| Balance frame | 0x16CD0C–0x16CF8D | Builds `+0xDFC` (section 5.3). |
| Pose builder | 0x1760F0 at 0x16CFF0 (only when `+0x4C8` is set) | Turns the current clip into per-part targets: `animationPose +0x550`, `targetPose +0x4F0`. These are the mod's compose sites (MIXAMO-KICK-DESIGN.md 3.2). |
| Motion clock | 0x178350 at 0x16D021, clock advance 0x16CB16–0x16CB4B | Advances the clip (section 6.2). |
| Contacts and lean | `contactFrame` 0x1603F0 at 0x16DDD0 | Computes uprightness (section 2), decides whether to step, sets the leg contacts (section 4). |
| End of a step | `supportSettle` 0x160070 at 0x16E04C | Runs only when a step was in progress at 0x16DDD9 (`+0x12EE` set) **and** ended during 0x16DEB6–0x16DFFD. If `+0x12EE` was already clear, 0x16DDE0 jumps straight past it to 0x16E054. (This corrects the earlier note "runs when +0x12EE is clear".) |
| Height and feet | `supportHeight` 0x16E1D0 at 0x16E058 | Ground test, height target, leg IK through `standingLegs` 0x15F080, `heightForce` 0x16FDF0 (section 4). |
| Movement total reset | 0x16E18B | `+0x1038 = 0` (section 3). |

The physics solve itself is in the world routine **0x80CD0**. It loops over the world's bodies and calls each one's
virtual slots 0xD8 (0x81092, 0x816F0) and 0x168 (0x811F0, 0x81D88). For the character and its parts those slots
hold:

* Slot 0xD8 = **0x151580**, the part motor (section 5.1). Pointers are at 0x31B5A0 in the TMotileDynamics VMT
  and 0x31B760 in the TMotilePart VMT. The class names come from VMT name pointers 0x31B4E0 → "TMotileDynamics"
  and 0x31B6A0 → "TMotilePart".
* Slot 0x168 = **0x15DA80**, the part contact update (pointers at 0x31B630 and 0x31B7F0).

0x15DA80 refreshes every contact point's world position `+0x3C` from the part's nodes and keeps the old one in
`+0x64` (0x15E604–0x15E646 and 0x15EC88–0x15ECCE). So **contact +0x3C is where the foot point is now**.

The node integrator `tetraIntegrate` 0x88D30 and the joint helpers `motorWeights` 0x1CAF0 are named in
MIXAMO-KICK-DESIGN.md 3.2. They were not re-traced here.

NOT PROVEN: how often 0x80CD0 runs relative to `combatUpdate`. The address order inside `combatUpdate` is read
straight off the code, but a few blocks could be reached out of order through jumps not followed here.

---

## 2. Uprightness `+0x12B0`: how it is computed (PROVEN)

All of this is in `contactFrame` 0x1603F0. It runs in every state except getting up (0x300, test at 0x160910) and
needs at least two legs (0x16042E). **It runs before the kick branch** at 0x1612C9, so it runs during the kick too.

1. **Feet centre.** `+0xC94` = the average of every leg's contact position `+0x3C` (0x16045E–0x160525).
2. **Chest point with lead.** p = chest `+0xBE8` (0x160561) plus three extra terms:
   * `+0xC5C × +0xC78` when `+0xC78 > 0.05` and not walking. `+0xC78` fades by 0.05 each step (0x1605CC–0x1605E5)
     and is set to 5 on landing (0x16F936).
   * `+0xC50 × 3` when walking (`+0x12EA`) and on the feet (`+0x12FC`) (0x1605F0–0x160623).
   * `+0xA08 × (−10 × +0x100C)` (0x160628–0x160651).
3. **Support point.** Take the line from foot 0 to foot 1 (their `+0x3C`), measured flat (height dropped,
   0x160693–0x1606A8). Find the place along that line nearest to p in the flat view, clamped to the two ends
   (0x1606DE–0x160768). Then put the height back and take that point on the real 3-D line (0x16079C). The result is
   stored in `+0xC7C`. When not walking, the point is moved halfway toward the feet centre `+0xC94` (0x1607C2–0x1607EC).
   **Both feet are used, including a foot that is in the air.**
4. **Lean.** d = p − support point (0x160804). `+0xC68` = d / |d| (0x160842–0x16088F), so `+0xC68.y` is the cosine of
   the lean angle θ from vertical.
5. **Raw value u.** u = (cos θ)^8 when not walking, (cos θ)^4 when walking (0x160A61–0x160AA8), plus 0.05 × `+0xB10`,
   capped at 1 (0x160AAB–0x160AD7).
6. **Fast vertical chest motion.** When |`+0xC60`| > 3 (the vertical part of the slow chest velocity `+0xC5C`) and a
   foot touches the ground (`+0x101C` > 0): u = 0 above 6, otherwise u × (1 − (|v|−3)/3)². The counter is then forced
   to 100 (0x160AE0–0x160B75).
7. **Counter `+0x12B4`.**
   * u > 0.99 (upright): the counter goes down by 10 (floor 0) while walking or stepping. Otherwise it goes down by 5,
     **but never below 80** (0x160B80–0x160C10).
   * u ≤ 0.99: the counter goes up by `round(10 − 10u)` (0x160C12–0x160C2F).
8. **Writing uprightness.**
   * Counter ≥ 100: `+0x12B0 = u`, counter = 100 (0x160C35–0x160C5F).
   * Otherwise uprightness recovers: `+0x12B0 += (5 − |C5C|) × 0.01` (or 0.05 when the chest is still), capped at 1,
     but only while the slow chest speed |`+0xC5C`| < 5 (0x160C64–0x160D18).

**What the numbers mean (arithmetic on the constants above):**

* Standing (power 8): u < 0.3 means cos θ < 0.3^(1/8) = 0.860, so θ > about **30.7°**. Walking (power 4): θ > about
  42°.
* A lean only counts once `round(10 − 10u) ≥ 1`, which means u ≤ 0.95, so θ above about 6.5° standing.
* Standing upright pins the counter at 80 or more. A lean past 30.7° (u < 0.3) adds 7 per step, so about **3 steps**
  later uprightness becomes u.
* The rounding assumes the default round-to-nearest CPU mode. That mode was not checked: NOT PROVEN.

**Other writers of `+0x12B0`:**

| Writer | Effect |
|---|---|
| supportHeight 0x16F98D | `+0x12B0 = 0` when no foot is near the ground and the character is not walking (section 4.3). |
| 0x16095B–0x16099A | +0.05 per step while getting up (state 0x300). |
| 0x1701FE, 0x171E22 | Resets to 1. |

**Readers:**

* supportHeight 0x16F9E4–0x16F9F4: **`+0x1020 = +0x101C × +0x12B0`**. `+0x101C` is 1 when a foot touches the ground,
  else 0. `+0x1020` is the "standing" factor that scales the motors (squared, section 5.1), `heightForce` (0x170084)
  and the step radius (0x160D35).
* The fall test 0x16A5B9 / 0x16A687 / 0x16A72A (section 3).
* Blend terms 0x1692BB, 0x169396, 0x169693.
* 0x16B58F, 0x16C1B4, 0x168045, 0xB89F5 (not followed).

---

## 3. What makes a character fall (PROVEN)

In `combatUpdate`, for any character that is not dead (state & 0x800 clear, 0x16A42E–0x16A433) and not in state
& 0x40 (0x16A581). The kick state 0x8000 passes both tests.

* **Uprightness < 0.6** (or getting up, 0x16A5B5–0x16A5D7):
  * 0x1688C0(0.5, 0.5, 2): sets per-part values along the `+0xE60` chain; not decoded.
  * `+0xDC8 = 1.25 × +0xDCC` (0x16A602–0x16A612).
  * 0x152930 (0x16A623).
  * Walking flags cleared (0x16A62C, 0x16A637).
  * Then, if not getting up:
* **Uprightness < 0.3 and `+0x1038` < 25** → **state 0x200 (on the ground)** at 0x16A6BA. If `+0xDF8` & 0x10 the game
  also zeroes `+0x590` and balance and calls 0x1794F0 (0x16A6C4–0x16A707). Otherwise it calls 0x162C10, which picks the
  get-up motion (0x16A718).
* **Uprightness < 0.3 and `+0x1038` ≥ 25** → **state 0x200 anyway** at 0x16A73E, without that set-up.

So **uprightness < 0.3 always means falling down**. `+0x1038` only decides whether the get-up set-up runs that step.

**`+0x1038`:**

* For each body part, the part motor 0x151580 finds the largest of four node distances (between `+0x120..` and
  `+0x150..`, 0x151664–0x1516E3). It adds the square root of that value to the character's `+0x1038` (0x151787–0x15179A).
* `combatUpdate` sets `+0x1038` to 0 at 0x16E18B.
* So it is a whole-body sum of per-part node movement: high while the body is still moving fast.
* NOT PROVEN: which two node copies `+0x120` and `+0x150` are (likely current and previous positions).

**Balance (`+0x10B8`).** Not re-derived here: see `GAME-CODE-VALIDATION.md` section 1c. Balance on its own never sets
state 0x200. It weakens the motors (balance⁴ in 0x151580, below) and triggers Rebalance.

Three refinements found while tracing:

* **0x179810** (called at 0x169BD7 when balance < max − 0.05) does not choose a motion. It copies every part's current
  orientation relative to its parent into the buffer `+0xB88` (0x179837–0x17990C) and the root position into `+0xB90`.
  That captures the current pose for the blend. Motion 3 itself is started by `motionPlay` at 0x169C5C, with blend
  time `(1.05 − balance) × 300` (0x169C3C–0x169C50). That block also sets state to 0 (0x169C06), so a kick (0x8000)
  is cut off when balance falls under max − 0.05.
* **Motion 0x21 (Rebalance)** at 0x169B46 is triggered by `+0xDF8` bit 0x100. Only 0x17B100 sets that bit
  (0x17B248), together with state 0. Its callers are 0x1A1594, 0x1A3082 and 0x28F534. The validation doc calls this a
  spell stun; what 0x17B100 is was not checked here: NOT PROVEN.
* **0x16AFED–0x16B066**: before the kick block, a character with `+0x590` < 0.85 plays motion 3 and gets state 0x100
  instead.

---

## 4. Feet, support and height

### 4.1 Contact flags and anchors (`contactFrame` after the lean)

Each shin's `+0x340` points at its foot contact. That contact is the first entry of the shin's contact array
(stride 0x98; counted by `+0x338`). Its fields:

| Field | Meaning |
|---|---|
| `+0x10` | flags |
| `+0x24` | support anchor (where the foot should be) |
| `+0x30` | the foot's spot in the body frame |
| `+0x3C` | the foot's current world position (0x15DA80) |
| `+0x90` | ground height under it |

* **Step decision** (0x160D20–0x161061), only while uprightness ≥ 0.2 and on the feet:
  * Radius r = (0.25 + 0.75·balance) × `+0xC90` × `+0x410` × 0.5 × (1.25 while stepping) × (1 + `+0xB10`)
    (0x1609F7–0x160A5D).
  * When the flat chest offset |d| is more than 1.5 × r × balance⁴ × `+0x1020`, a step starts (0x160DA6–0x160DD4).
  * The step target is the chest plus 15 or 30 × scale × the chest velocity (0x160EBD–0x160F5B), saved in `+0xA30`.
  * If it is far enough, `+0x12EE` is set ("stepping", 0x16100C–0x161011).
  * **Lower balance means a smaller radius, so the game steps sooner.**
* **The anchor frame** (0x1610B4–0x161238): rotation from `+0xA54` (0x1610C5). The position is the body-frame position
  `+0xA14` (0x161212), or a point moved toward the step target while stepping (0x1610FF–0x1611F3).
* **Kick branch** (0x1612C0–0x1612E5). It is taken when local bl is set, motion == 13, not on the feet (`+0x12FC` == 0),
  or state == 0x4000:
  * Every leg's contact gets flags = exactly 2 (support), and anchor `+0x24` = frame × `+0x30` (0x161308–0x161339).
  * In state 0x300 below progress 0.2, bit 2 is cleared again (0x161344–0x1613AD).
  * It then **jumps to the end** (0x16233C), skipping the normal stepping path that starts at 0x1613C0.
* **The normal path** (0x1613C0 on):
  * Per leg it reads bit 8 and `+0x109C`, clears bits 0xC (0x161436 / 0x16143E), and sets bit 2|0x100 when bit 4 and a
    small `+0x80` hold (0x161483–0x161495).
  * It copies `+0x3C` → `+0x24` (0x161498), then builds the per-leg step targets.
  * The rest of that path (0x1614BC–0x16233C) was not decoded: NOT PROVEN.

So **during motion 13 both feet are supports, anchored at their body-frame spots, and the game cannot take a
corrective step** (PROVEN). The step decision above still runs and can set `+0x12EE`, but the code that moves a foot
is skipped. `+0x12EE` also stops `supportSettle` until it clears (section 1).

### 4.2 `supportSettle` 0x160070 (end of a step)

* It picks the leg whose contact is nearest its body-frame spot, skipping legs with bits 0xC == 4 (0x1600C3–0x160172).
* For that leg (unless bit 8) it sets `+0x24 = +0x3C` and flags = (flags & 0xC0) | 2 (0x1601FC–0x16021F).
* It then moves the body position `+0xAB4` through 0x162740 (0x16027E). 0x162740 copies the new point straight in while
  stepping (0x162768–0x162781). Otherwise it copies it only when it is farther than a radius (0x16278C–0x1627E5).
* Body frame position `+0xA14` = `+0xAB4` and height `+0xA18` = root height `+0x2C8` every step (0x16D30E–0x16D336).

### 4.3 `supportHeight` 0x16E1D0 from 0x16F2A8

* **Per leg (0x16F2A8–0x16F3FC):**
  * A support while stepping: shin `+0x624` = a lift term from how far `+0x3C` is from `+0x24`, against 3 × shin
    `+0x644`, × (1.5 − [`+0xAF8`]+0x78) × (1 − `+0x100C`) (0x16F2C2–0x16F38E).
  * Otherwise `+0x624` × 0.9 (0x16F399–0x16F3BB).
  * Non-support legs: `+0x624` × 0.8 and `+0x63C` = 0 (0x16F3C5–0x16F3F5).
* **Height target `+0xAC8`** (0x16F46F–0x16F7D8): from the ground heights `+0x90` under the feet, the root `+0x304`,
  contact `+0x34`/`+0x94`, 60 or 70 × scale and `+0x100C`. When the two feet's grounds differ by 40–60 units it blends
  toward the other foot (0x16F60D–0x16F75D). It never goes above ground + `+0xB68` + shin `+0x638` (0x16F768–0x16F7A3).
  It is −100000 with no legs (0x16F7D2).
* **On the feet (0x16F7DE–0x16F9A8):**
  * A leg touches the ground when its contact `+0x40` (height of `+0x3C`) ≤ ground `+0x90` + `+0x94` + 20 (30 × scale
    while walking) and bit 0x20 is clear (0x16F84B–0x16F872).
  * If any leg touches: `+0x12FC = 1`, `+0x101C = 1` (0x16F944–0x16F955).
  * Otherwise `+0x12FC = 0`, `+0x101C = 0`, and when not walking **`+0x12B0 = 0`** (0x16F95F–0x16F98D).
  * Then `+0x1020 = +0x101C × +0x12B0` (0x16F9E4–0x16F9F4).
* Then: the feet-spread factor `+0xA6C` (0x16F9FC–0x16FAB3), `+0x12AC` (0x16FB5E), `+0xFD4` (0x16FB6E–0x16FBC1).
  0x16FC60 onward was not decoded.
* **The only motion test in `supportHeight`** is for motion 0x13 (19), at 0x16EFF2. There is none for 13 (PROVEN by
  listing every `+0x9B8` read).

### 4.4 `heightForce` 0x16FDF0 (called at 0x16E34E, 0x16E44C, 0x16FD53)

* error = target `+0xAC8` − root height `+0x2C8` + 5, 10, 15 or 20 (depending on walking, stepping and `+0x109C`),
  × 0.075 × scale. The result is stored in `+0xACC`.
* Damping uses balance² × the vertical velocity from 0x891F0 (0x16FE15, 0x170019–0x17002F).
* × (`+0x590` − 0.5) × 2, × `+0x1020`. It is added to `+0x5BC` along the vector at `[+0x418]+0x14` (0x170033–0x1700A9).
  0x151580 then applies `+0x5BC` to the nodes (0x1518DF–0x15190B).
* **It is a height spring only.** It has no horizontal term and no motion test (PROVEN, whole routine read).

### 4.5 `standingLegs` 0x15F080

* A two-bone leg solver. The target is the support anchor `+0x24` lowered by 2 × `+0xACC` (0x15F265–0x15F290), or a
  blend of `+0x3C` and `+0x24` with bit 4 (0x15F236–0x15F25D).
* It uses hip-to-target distance and thigh/shin lengths `+0x62C` (law of cosines, 0x15F35E–0x15F45B).
* It writes `+0x628` and `+0x640` (0x15F571, 0x15F650) and the thigh's extra rotation `+0x5EC` (0x15F88A). It sets
  part flag 0x4000 (0x15F59C, 0x15F67F).
* It has no motion test. It runs after the pose builder in the same step, so its output is used in the **next** step's
  pose builder.
* That the builder applies `+0x5EC` when flag 0x4000 is set comes from MIXAMO-KICK-DESIGN.md 3.2. It was not re-traced
  here.

---

## 5. What holds the body in its pose

### 5.1 The part motor 0x151580: a world-space pull (PROVEN)

For each part, for each of its four nodes i (0x151AD0–0x151C1B):

1. t = `targetPose +0x4F0` × the node's rest point `+0x58+12i` (0x151AEA, matrix × point 0x18870).
2. t − node (0x151B02).
3. The pull is scaled down by its projection on `+0x180+12i` over `+0xBD8`, and skipped at ≥ 1 (0x151B44–0x151BB5).
4. **Standing: the full horizontal part and 0.1 of the vertical part. On the ground (state 0x200): vertical only**
   (0x151BBA–0x151BF6).
5. The result is added to the node with strength (0x151A45–0x151AC2):

   k = part `+0x5C8` × char `+0x58C` × part `+0x590`² × **balance⁴** × (0.5 + 0.5 × char `+0x58C`) × **`+0x1020`²**

The target pose is in world space, so this is a pull on the node toward a place in the world, not a joint torque. A
part goes where its target says, as hard as k allows. Two loops feed each other:

* When uprightness drops, `+0x1020` drops and the pull falls with its square. At uprightness 0.6 the pull is 0.36 of
  full.
* The same happens with balance⁴.

### 5.2 `+0x590`

The earlier notes called this "footing", but what the code does with it is different:

* Knocked out: set to 0 (0x156CE0), with state 0x200 (0x156D04). It then recovers 0.001 per step up to 1
  (0x156B92–0x156BC7, plus extra while getting up 0x156B50–0x156B88).
* Dying (state 0x800, 0x167EDF): it fades 0.003 per step; under 0.25 it is set to 0.01 and the body goes slack
  (`+0x4C8` = 0) (0x167FB2–0x167FFF).
* Some falls zero it (0x16A6DD).
* Resets: 0x170810, 0x171CB0, 0x1A0870 and others.
* 0x151DB0 copies the character's `+0x590` to every body part flagged 0x2000 (0x151E71–0x151E8F; called at 0x16E17C
  every step). The **part's** `+0x590` is the one the motor squares (5.1). It is also the one in the validation doc's
  "1 − footing × balance" (0x1518A5).
* The writer at 0x160545 named in earlier notes **cannot run**. It needs zero legs, but `contactFrame` leaves early
  with fewer than two legs (0x16042E), and the counter at 0x1604E7 counts every leg.
* During motion 13 nothing writes `+0x590`.
* What the code needs from it:
  * ≥ 0.85 to start or keep the kick (0x16AFF1, otherwise motion 3 and state 0x100).
  * ≥ 0.85 for the balance frame (0x16CD21).
  * > 0.5 for `heightForce` (0x170033) and for the contact-point branch in 0x15DA80 (0x15E5B8).

Plain words: this behaves like **muscle tone or consciousness**, not ground grip. The name is NOT PROVEN.

### 5.3 The balance frame `+0xDFC`

* Built only when state & 0xAC0 == 0 and `+0x590` > 0.85 (0x16CD0C–0x16CD36). Otherwise it is the identity
  (0x16CF95, 0x16CFFB, 0x16E138).
* Inputs:
  * A direction from `+0xCFC` and the body frame `+0x9F0` (0x18B00 at 0x16CD56) → heading (0x19F00).
  * A rate-limited heading `+0xD08`: 0.02 per step walking, 0.12 × … in state 2, 0.05 otherwise (0x16CD94–0x16CE69).
  * Height part `+0xD18` minus `+0x100C` (× 0.3 in state 2), with y × 0.75 (0x16CE7F–0x16CF53).
* It is built into a rotation (0x1D660 at 0x16CF70). **Its translation is zero** (`+0xE20..+0xE28` = 0,
  0x16CF79–0x16CF8D).

So the "balance frame" turns and pitches the whole pose. **No input that measures the chest against the feet was found
among its inputs** (PROVEN for 0x16CD0C–0x16CF8D). Whether `+0xCFC` or `+0xD18` themselves depend on the lean: NOT
PROVEN.

### 5.4 Where the body is placed

* The body frame `+0x9F0` is built from yaw `+0xAD4` (0x1CD60 at 0x16D309). Its position `+0xA14` is the body
  position `+0xAB4` (x, z) plus the current root height `+0x2C8` (0x16D30E–0x16D336).
* `+0xAB4` is moved by:
  * the end-of-step settle (4.2)
  * the start of walking (0x16BE72)
  * death (0x169437)
  * resets

The game keeps balance **kinematically**: the targets put the pelvis and feet back in the body frame, the motor pulls
the parts there, and a step moves the body frame when the chest drifts too far (4.1). There is no torque-based "keep
the centre of mass over the feet" controller in any routine read here.

---

## 6. What motion 13 (Kick) changes

Only two places in the executable compare the motion with 13: 0x1612C9 and 0x16B06F. Only 0x16B09C writes state
0x8000 (PROVEN by scanning every `+0x9B8` compare and every `+0xDF4` write of 0x8000). Tests of the bit through masks
were not scanned: NOT PROVEN.

1. **Contacts** (0x1612C9): both legs are supports with body-frame anchors, and corrective stepping is skipped (4.1).
2. **State and speed** (0x16B06F–0x16B0D0): reached only when uprightness ≥ 0.6, the motion is outside 5..12 and
   `+0x590` ≥ 0.85.
   * State is set to 0x8000 (0x16B09C).
   * **`+0xDC8 = 2 × +0xDCC`** (0x16B0AA–0x16B0BA). `+0xDC8` is the clip playback rate: outside motions 5..12, and
     unless state & 0x80, the clock `+0xA20` gains `+0xDC8 × 0.6` each step (0x16CB16–0x16CB4B). The progress `+0xA28`
     comes from `+0xA20`, and the end-of-clip test is at 0x16CB88. So the **kick clip plays at twice the base rate**
     (1.2 × `+0xDCC` per step).
   * `+0xDCC` itself is raised 0.01 per step after a knock-out (0x156BEB–0x156C20) and set to 0.4 by it (0x156CEC).
     Other `+0xDC8` writers are in 0x166580 (defence exchange) and 0x17B0CA.
3. **The "re-grip" call 0x152930(char, `+0xE2C`)** (0x16B0C6–0x16B0D0) **does not use its argument**. Register edx is
   overwritten at 0x152949 before any read.
   * When `+0x1068` & 0x30, it sets `+0x458 = 0x7FFFFFFF` on each entry of the `+0x928` list (`+0x920` entries,
     0x152930–0x152970).
   * It does not touch legs, contacts or balance.
   * What `+0x928` holds and what `+0x458` means: NOT PROVEN.
4. **Nothing else.** These have **no motion-13 or 0x8000 test**: `supportHeight`, `standingLegs`, `heightForce`,
   `supportSettle`, `supportPosition`, the part motor 0x151580 and the contact update 0x15DA80 (every `+0x9B8` and
   `+0xDF4` read in them listed). The lean rule (section 2) and the fall test (section 3) apply unchanged during the
   kick.
5. Balance under max − 0.05 still interrupts the kick with motion 3 and state 0 (section 3).

---

## 7. A mod that replaces the motor targets

The Animated kick (`src/mod/pose_drive.inl`) blends each planned part's local target toward the clip at the three
compose sites. It also sets the pelvis target position so the clip's hip travel is carried **over the standing foot**
(`kickPlan`, pose_drive.inl:262–270). It drives both legs, including the standing one, from the clip
(pose_drive.inl:295–303).

What follows from the traced code:

1. **The motor carries out the new targets in world space** (5.1). The real pelvis, belly and chest go where the clip
   puts them, as hard as k allows. **PROVEN** (mechanism).
2. **Uprightness is measured from the real chest `+0xBE8`**, not from the pelvis (1, 2). Moving the hips only matters
   through where it carries the chest. Leaning the torso back or forward moves the chest directly. **PROVEN.**
3. **The support point moves up toward a raised foot.** It sits on the line between the two current foot points and is
   pulled halfway to their middle (2.3). A kicking foot raised high lifts that point by about half the foot's height
   and moves it toward the kick. That shortens the vertical part of d and tilts it. The lean then reads larger than the
   chest's lean over the standing foot alone. **PROVEN** (formula). This is also true of the game's own kick.
4. **The game's own defences are turned off or work the wrong way during the kick:**
   * No corrective step, because the motion-13 branch skips the stepping path (4.1). **PROVEN.**
   * The balance frame does not measure lean (5.3) and has zero translation. **PROVEN** for the traced inputs.
   * `heightForce` is vertical only (4.4). **PROVEN.**
   * The motor obeys the mod's targets rather than fighting them (5.1). **PROVEN.**
   * When the lean starts to count, uprightness drops, `+0x1020` drops and the motor weakens with its square. Gravity
     then takes over, which deepens the lean (5.1). **PROVEN** (formula).
5. **The game's leg solver is likely overridden.** `standingLegs` keeps each support foot on its anchor by adding
   extra rotation `+0x5EC` (4.5). If the mod's blend at full weight replaces the whole local rotation, that correction
   is lost. The standing leg then follows the clip instead of the ground. **NOT PROVEN**: it rests on the pose builder
   applying `+0x5EC` before the compose call (design doc 3.2, not re-traced here).
6. **Uprightness 0 when no foot is near the ground** (4.3). This happens if the clip lifts the standing foot more than
   20 units above its ground height, or raises the whole body (a hop), while `+0x12EA` (walking) is clear. **PROVEN**
   (rule). Whether the clips do this: NOT PROVEN. If 100 units = 1 m as the mod assumes, 20 units is 20 cm; that scale
   is NOT PROVEN from the exe.
7. **A fast vertical chest movement** also cuts uprightness at once (2.6). That means the slow-averaged vertical chest
   speed above 3 units per update, with a foot down. **PROVEN** (rule). Reached by the clips: NOT PROVEN.

**Does driving the hips and torso away from the support base tilt the body past 0.3?**

* **PROVEN:** it does exactly when the chest ends up more than about 30.7° from vertical over the game's support point
  for about 3 steps. That point lies between both current foot positions and halfway to their middle, so a raised
  kicking foot lifts it. The game takes no step and applies no horizontal correction to stop it during motion 13.
* **NOT PROVEN:** that the mod's clips reach that angle. That needs numbers from a run.
* **To prove it, log once per physics step during a kick:** `+0x12B0`, `+0x12B4`, `+0xC68` (y is cos θ), `+0xBE8`,
  both contacts' `+0x3C`, `+0xC7C`, `+0x101C`, `+0x1020`, `+0xC5C`, `+0x10B8`, `+0xDF4`. The step where `+0x12B4`
  reaches 100 with `+0xC68.y < 0.86` would be the proof.

---

## 8. Why a kicker might fall

**PROVEN (traced in the exe):**

* Falling is state 0x200, set at 0x16A6BA or 0x16A73E whenever uprightness `+0x12B0` < 0.3, during the kick too.
* Uprightness is (cos θ)^8 standing, where θ is the lean of the chest (with velocity lead) from a support point built
  from both feet's current positions (0x1603F0, 0x1605F0–0x160D18). It takes effect after a counter reaches 100, about
  3 steps for a lean over about 31°.
* Uprightness is forced to 0 when no foot is within 20 units of the ground while not walking (0x16F98D). It is cut
  when the chest's slow vertical speed is above 3 (0x160AE0–0x160B75).
* During motion 13 both legs are forced to be supports with body-frame anchors, and the corrective-step path is
  skipped (0x1612C9–0x16133E).
* The kick clip plays at twice the base rate (`+0xDC8 = 2 × +0xDCC`, 0x16B0AA; used at 0x16CB33).
* The part motor pulls each part toward its world target with strength ∝ balance⁴ × `+0x1020`²; standing it pulls
  fully sideways and only 0.1 vertically (0x151A45–0x151C13). Targets that place the chest badly are carried out, and
  a falling uprightness weakens the hold further.
* The balance frame `+0xDFC` has zero translation and no lean input among its traced inputs. `heightForce` is vertical
  only.
* Balance below max − 0.05 interrupts the kick (state 0, motion 3, 0x169BC5–0x169C5C). Balance alone never sets state
  0x200 (validation doc 1c).
* 0x152930 ignores the `+0xE2C` argument. `+0x590` is not written during the kick.

**NOT PROVEN:**

* That the mod's clips put the chest past about 31° from the support point, or lift the standing foot or the body
  more than 20 units, or move the chest fast enough vertically. All three need the log in section 7.
* That the mod's full-weight blend removes the game's leg-solver correction (`+0x5EC`) on the standing leg.
* The rest of the stepping path in `contactFrame` (0x1614BC–0x16233C), `supportHeight` after 0x16FC60, and the
  meaning of `+0xCFC`, `+0xD18`, `+0x100C`, `+0xB10`, `+0xA08`, `+0x410`.
* Whether `+0x120`/`+0x150` (used for `+0x1038`) are current and previous node positions.
* What the `+0x928` list in 0x152930 holds.
* The game's units per metre.
