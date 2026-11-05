# Exanima's own blood systems (read-only survey, blood thread)

Exanima 0.9.5.2g (Steam build 25803178). All addresses are RVAs in Exanima.exe. This survey reads the game's
code only. It changes no behaviour, and it reproduces no shader text: shader lines are described, not quoted.
Hunter's direction (2026-11-05): extend these systems rather than draw beside them.

## The hit handler that feeds all of them

All of these systems start from one place, the hit's blood handler. It is the function round 0x15C680-0x15C8F5,
whose error string is "Error processing blood effects." (referenced at 0x15C8D1). For a hit on `victim`
([rbp-0x68]) with blow value `v` ([rbp-0x98]) at point [rbx+0x10], it does the following, in order:

1. **Damage.** It calls the damage routine 0x159700 at 0x15C735, with v × k.
2. **Gate.** It continues only if the blood effects global at .data 0x4F0860 is set (0x15C73B) and
   victim+0x10A8 & 1 (0x15C74D; the victim can bleed). It also needs v above a threshold (0x15C771).
3. **Sprays.** It spawns sprays: TBloodSprays.Spawn 0x1000E0 on the global spray object [0x4F0860]. The call is
   at 0x15C7B9, with a fixed amount when v exceeds a second threshold; otherwise it is at 0x15C7E8, with
   amount v × k.
4. **Body blood.** It calls 0x158D70(victim, point, v, the striking item) at 0x15C808. This queues the
   vertex-alpha jobs described below.
5. **Weapon blood.** Here item = [rbp-8]+0x988, the striking item. The item's slot item+0x67C decides:
   - 0: TWeaponBlood.Add(item node [item+0x18]) 0x100640. Its result (index + 1) is stored at item+0x67C.
   - Then TWeaponBlood.AddBlood(index, v × 0.5) 0x1007C0 is called, at 0x15C88C.
   - -2: another path instead, 0x158AD0(node, point, v) at 0x15C8C6, which calls 0xD1A10 / 0xD32C0. That path
     is not decoded.

Our wound hooks (bodyDamage at 0x159700 and physicalImpact) sit on the same chain. What we do in addition
duplicates what this handler already does.

## Systems

| System | State: where, layout | Written by | Read by | How long it lives | Hook points to extend it |
|---|---|---|---|---|---|
| **Weapon blood** (TWeaponBlood, VMT 0x30F870, instance 0x18 bytes; record type TBloodyWeapon) | Singleton at .data 0x4F0870 (made by 0x100530, stored at 0xC2E5A / 0x1813F3). +8 count, +0x10 array of 0x18-byte records: +0 the weapon's node, +8 **amount 0..1** (starts 0, 0x100743), +0xC and +0x10 the node's extent terms ((node+0x14 + node+0x34) × k, √(80 / that)), +0x14 0. The item keeps its record index + 1 at item+0x67C. | Add 0x100640, which skips a node whose +0x204 & 0x400000 is set. AddBlood 0x1007C0: amount += v × 0.5, clamped to 0..1; it is the only writer of +8 in the module. Remove 0x100770 (from 0x14EC98, when the item goes). Clear 0x100630 (count 0, at the per-level reset 0x12417B). | The stain pass ≈0x10081F-0x100CE3 ("Error rendering weapon blood stains.", 0x100CBB). For each record with amount > 0.01 (0x1009B5) and node+0xE0 & 0x1000000, it sets a uniform −(+0xC) × (k − √amount × (+0x10)) (0x100A44). Then it draws the node's first material again: 0x6B8B0 at 0x100AD9 / 0x100C1F, and per shadow sample 0x6B7F0 at 0x100BC8. | **No drying or drain was found in this module**: only AddBlood writes the amount. The record lives until the item is removed or the level resets. | Read [0x4F0870] → records → amount per weapon node: this is the game's own "how bloody is this blade". Drive our drips and cast-off from it, and drain it with our drips if Hunter wants drying (one float). The stain draws are already hooked (gameBloodStains group, f3be18b) and kept as drawn. |
| **Body blood, vertex alpha** | Per mesh: the vertex colour attribute's alpha byte (vCol, 4 × ubyte; shading_v scales it to 0..1 as vrCol). | 0x62DA0(node, point, dir, amount), which refuses an instanced node (0x62E07, "Cannot apply blood to an instanced object"). It lowers alpha toward a falloff value from the hit point, writing only when the new value is lower (0x630B2-0x630CC). It runs from the job at 0x158A90, called at 0x158AB3, with record {+0 node, +8 point, +0x14 direction, +0x20 amount}. 0x158D70 builds the jobs, one per mesh node of the victim (victim+0x11E0 → +0x18 → +0x120[i]), and queues them at 0x158D01 (0x1A890 on the queue at .data 0x499990). | The skin shader shadesss_f and shading_f, when DirtMode is 4: a dirt amount from the DrtTex pattern (r, at twice the texcoord) against max(vrCol.w, 0.02) (shadesss_f lines 141-144). That amount lowers Gloss (line 144). It also multiplies the diffuse and the subsurface term by a dark-red blood tint, deeper as the amount grows (lines 163-170). DirtMode 4 is the game's blood mode, so **the game already shows blood on skin**: the pattern grows where alpha was lowered. | Not seen to recover: the write only lowers. Whether anything raises the alpha back (washing, healing) is not found yet. | Read the alpha per vertex to place our pools and runs where the game's blood is. Feed our own amounts by queueing the game's job (0x158A90 record, through 0x1A890), only once that is proven safe. Our skin bake should **layer on** this (its copies keep the vertex colours, so the game's dirt still shows) rather than replace it. |
| **Blood sprays** (TBloodSprays, VMT 0x30F6D0, instance 0x238 bytes) | A global instance [0x4F0860] (also the gate tested at 0x15C73B). A per-object instance at obj+0x160 is made at 0x10034D (0xFFFD0) for something carrying sprays. Array at +0x138 of 0x70-byte records, count +0x130; a record is free when its +0x34 < 1. | Spawn 0x1000E0(this, point, dir, amount, …), from the hit handler at 0x15C7B9 / 0x15C7E8. Reset 0x100090. | The particle renderer: not traced yet. | Not traced. A particle record's life is held at +0x34. | Spawn from our own events (an artery spurt, a drip) through 0x1000E0, so the game's own particles fly. |
| **Surface blood** (floors, walls) | No decal or splat-texture system found in this region: no class, string or texture pass for floor blood. The sprays are particles. | n/a | n/a | n/a | Our floor blood layer stays ours. It could take its splats from the spray records once the particle update and landing are traced. |
| **Wounds** (cuts, gashes) | No wound decal, cut texture or texture swap on a hit was found. The game's visible injury is the vertex-alpha blood and dirt above, and the state values (health, bleed). | n/a | n/a | n/a | Our gashes, punctures and bruises (the skin bake) remain additions. They draw over the game's dirt, which stays. |

## Not yet decoded (next reads)

- 0x158AD0 → 0xD1A10 / 0xD32C0: the weapon blood path for item+0x67C == −2.
- Whether any code raises the vertex alpha back (washing or healing), and whether the hit handler itself runs for
  every hit or only for some blows.
- The TBloodSprays update and render, and where a spray lands.
- The meaning of v ([rbp-0x98]) in the handler. It feeds the damage routine, so it is the blow's damage value
  before the k scale; the exact k at 0x15C71C and 0x15C729 is to be read.
