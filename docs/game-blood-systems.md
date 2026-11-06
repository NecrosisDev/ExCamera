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

## What ExCamera does with them

- **Weapon blood (in use).** Each frame, every TWeaponBlood record with an amount above 0.01 gets an entry of
  ours. The singleton is read through the reviewed signature weaponBloodList; group gameWeaponBlood is admitted
  only with AddBlood 0x1007C0 matching its bytes, which witnesses the 0x18-byte records with the amount at +8.
  - **Drips:** the blade drips at √amount × 3 a second (the stain pass shows a run that grows with √amount: bldpos.w = −e[+0xC] × (1 − √e[+8] × e[+0x10])), more when it is steep. Each drop lowers the game's own
    amount by [Wounds] BladeDrainPerDrop (default 0.01; 0 keeps the game's never-drying blood), and the game's
    stains fade with it.
  - **Cast-off:** a swing's cast-off takes the same per drop flung. It only happens in the holder's attack after
    the commit point; the holder comes from the carrying character's weapons list.
  - **Where drops leave:** the lowest of the spots our hits recorded on that blade, which run down the blade with
    gravity; otherwise the blade's origin.
  - **Replaced:** our earlier mass budget is gone.

- **Body blood, for pools (in use).** The game's own skin blood is read from the character's _SKIN mesh, every
  half second per character, read-only (CPU memory, no GL), on the render thread
  (wounds_bake.inl gameSkinBloodRead).
  - **Colour data:** attribute 0x100's data, or the node's +0x378. It is read only when attribute 0 is the
    positions (format & 0xFFFF == 1) with the same count, so a colour's index is a position's, as 0x62DA0 pairs
    them.
  - **Which vertices:** up to 8 of the bloodiest (alpha below 200), posed as the skin draws them: each weight's
    bone transform × its inverse bind × the source position.
  - **Where pools go:** a bleeding wound's pool marks are laid under one of those points, chosen by bloodiness;
    otherwise under the wound. Of the raw and sector readings, the one nearest the wound within 3 m is used.
  - **Counts:** game_skin_blood_reads and game_skin_blood_points.

## The weapon stain pass: when it runs, and why it flickered under our camera

- **When it runs** (traced by the render thread). 0x1005B6 registers the stain callback 0x100820 into the
  camera's list at +0x18. The camera pass 0x4F750 runs that list at 0x4F877: after the opaque lists, before the
  highlight pass 0x4FACF where our mask replay sits, and per sample under Supersample. Our view effects run after
  the whole view (0xA0CE0 / 0x4B6D0), from a blit that already holds the frame's stains, so none of our copies,
  history or composites can make it alternate.
- **What it sets** (its GL entry points, named from the loader: 0x498690 glDepthMask, 0x498E90 glUniform1f,
  0x498F40 glUniform4fv, 0x498FB0 glUniformMatrix4fv, 0x498610 glViewport):
  - glDepthMask(0) at 0x10084C, and 1 again at 0x100CE3. No depth func, polygon offset or cull of its own: it
    inherits the camera pass's state.
  - It beats the weapon's own depth only by nudging the projection. It saves sample 0's P[10] (0x1008EE), writes
    P[10] + 1e-5 (the double at 0x30FBA0, 0x100925) into every sample's matrix (0x100938), recomposes the
    view-projection with 0x18C70 (0x100972), and restores it at 0x100C50-0x100CA7. That moves the stain 1e-5
    nearer in NDC. (The camera pass uses the same trick for its later lists: +0.002 and +2e-5 at 0x4F8F5 /
    0x4F94E.)
  - Per weapon it composes nudged view-projection × the node's +0xA0 on the CPU in floats (0x18C70 at
    0x100AAC / 0x100B9B), uploads it, and draws the weapon's first material again (0x6B8B0 / 0x6B7F0).
- **Why it flickered with our camera.** The nudge is fixed in NDC. The difference between the stain's
  CPU-composed matrix and the weapon draw's own path grows as the weapon gets nearer the eye, roughly as world
  coordinate size × float epsilon / distance. With the blade about 30 cm from the eye in first person, that
  difference is about the size of the 1e-5 nudge, so the depth test passes on some frames and fails on others.
  At the stock camera's distance it is far smaller. The arithmetic is an estimate; the nudge, the path and the
  missing offset are read from the code.
- **Our own part in it (proven).** Under Supersample the stain's recompose goes through our hook on 0x18C70
  (0x100972: output 0x49AF10 + i, view 0x49AE10 + i, projection 0x49AD10 + i, the sample tables
  supersampleMatrixSlot accepts). correctSampleDepth (supersampling_depth.h) rescales any authored P[10] addition
  by the near-plane ratio our camera's near makes, with a floor of 16 / 2^23. So the stain's 1e-5 nudge shrinks to
  about 1.9e-6 NDC under our near plane. The weapon's own draw goes through the same corrected tables, so the two
  matrices still agree, but the stain's margin is about 5× smaller than the game gives it.
- **What else was checked.**
  - Fade (node +0x238) is set once from the mesh definition's +0x94 (0x6D29D).
  - The draw gate bit +0xE0 & 0x1000000 is assigned only at creation (0x58F32, 0xE3D23).
  - Neither changes with the view.
- **Fix** (headwear_render.inl gameBloodStainDraw). Around each stain draw only, a polygon offset toward the eye
  (factor −1, units −256, about 1.5e-5 of a 24-bit depth range), with the game's own polygon-offset state put
  back after. A dev audit line logs the depth func and the offsets in effect at the stain draw.

## Calling the game's own spray spawn and skin-blood queue: proof so far

**Sprays (TBloodSprays.Spawn 0x1000E0). Verdict: unknown, so not called.**
- **Arguments** (proven from its only two call sites, 0x15C7B9 and 0x15C7E8, both in the hit handler):
  this = [0x4F0860], rdx = &point (vec3), r8 = &direction (vec3), xmm3 = amount, 5th argument (stack) = the
  victim character.
- **Callers:** no other code calls 0x1000E0, so it only ever runs on the thread that runs the hit handler.
- **What the object is:** a particle object on a scene node. Its VMT 0x30F6D0 holds only base-class methods
  (0xF3xxx-0xF5xxx, 0x51xxx). Its records are 0x70 bytes at +0x138, count +0x130.
  - The base's set-up 0xF3410 sizes the array and gives each record a phase at +0x30, −1 at +0x34 (free), and
    a pointer to the node's matrix (+0x18's +0x40) at +0x38.
  - 0xF33B0 attaches it to a node: node+0x160 = the object, node+8 |= 0x10000, node+0xC |= 0x20, and the first
    material's +0xF0 |= 0x20. So the sprays are drawn as part of that node, not by a pass of their own.
- **Set-up 0xF3B20** (also a base method, VMT slot +0x148). Each record's matrix pointer at +0x38 is pointed at
  the carrying object's +0x2D0, or the node's +0x40 when there is none, and positioned with 0x18B60 against the
  node's +0x70. The records ride on a node's frame, which points at particles that move with what emitted them.
- **Not traced:** the per-frame update (which base method advances the records: gravity, life at +0x34) and
  whether a spray lands or only fades. Until those and the thread are known, ExCamera doesn't spawn sprays.

**Sprays, traced further. Verdict: not safe to call from our thread.**
- **What Spawn writes:** 0x1000E0 takes a free 0x70-byte record (or grows the array at +0x138 with 0xB8E0, which
  may move it) and fills it:
  - +0x34: the number of particles still to emit, int(amount × 0.3) (0x1001BA-0x1001C7).
  - +0x24: the point, held in the victim's frame (+0x38 = victim + 0x2A0).
  - +0xC: the direction, with a basis built from it at +0x18; +0x64 = direction × 2.
  - +0x40 / +0x4C: the world point, cached.
- **The emit step** (0xF5608-0xF577A, in the particle base class) walks the records. Each record whose +0x34 is
  non-zero has its world point recomputed from the frame (0x18870), then emits one particle through the object's
  virtual +0x178. The particle's velocity is the direction scaled by −(+0x30), and the count is decremented
  (0xF56E3). The rate accumulator is +0x128 += +0x124 (0xF5788).
- **So a spray** is an emitter riding the victim's body, putting out a short burst of the base system's particles.
- **Not traced:** how those particles fall, land or die (the base particle simulation). This survey found no floor
  decal from them.
- **Why not from our thread:** Spawn can reallocate the array the emit step walks. The game calls Spawn only from
  the hit handler, whose thread (hit_thread 5524 in the 4817283 run) is not ours (20640). Calling it from our
  render thread would race the update. It would be safe only if queued onto the game's own thread, for instance
  through the deferred-call queue (whose lock is proven), as a job of ours that calls Spawn. That needs the
  particle update's thread confirmed to be the queue drain's (it is ours, 20640), which is not shown.

**Camera pass lists after the stain callback (0x4F750), and which take the depth nudges.**
- **Order:** list +0x18 (0x4F877: the weapon stain 0x100820, and 0x1B8760), glDepthMask(1) (0x4F89B), list +0x68
  (0x4F8A1: 0x1B42A0, registered at 0x1B2B82), list +0x28 (0x4F8AD: the displacement shells 0x2555B0 / 0x255730,
  "Displacement Shell null sub-object.", textures thmdsp_*).
- **Then the nudges** (0x4F8CC-0x4F99D): +0.002 into the sample projections, +2e-5 into the cropped ones.
- **Then** the node list +0xC8 (0x4F9A8, 0x4E8C0) and list +0x48 (0x4F9C5; no registration into it was found
  among 0x18040's callers).
- **Consequence:** the nudges apply to +0xC8 and +0x48, not to the shells. Earlier notes said otherwise.

**Skin-blood queue (0x158D70 → 0x1A890 on .data 0x499990). Verdict: unknown for our threads, so not called.**
- **Not a blood-only queue:** 0x499990 is the game's general deferred-call queue, used by dozens of producers
  (0xAA795, 0xB4B6D, 0x1348ED … 0x158D01).
- **Layout:** a ring of 128 entries of 16 bytes (code, argument) from +8, head at +0, tail at +4.
- **Enqueue 0x1A890:** it takes the queue's lock object at +0x808 when there is one (0x1A410 before, 0x1A430
  after), and silently drops the entry when the ring is full (0x1A8CD).
- **Drain:** 0x1AC10 on that queue at 0x26603, inside the game's frame loop (0x26580-0x265DB runs the timers just
  before it), so the jobs (0x158A90 → 0x62DA0) run on the frame loop's thread.
- **The lock is always there.** It is created at start-up (0x26D13, 0x1A2F0) and stored at 0x26D18 into
  [0x49A198], the queue's +0x808. 0x1A410 / 0x1A430 wrap EnterCriticalSection / LeaveCriticalSection on its +8. The
  drain 0x1AC10 takes the same lock around each pop and calls the job outside it (0x1AC38-0x1AC89). Enqueueing from
  any thread is therefore safe.
- **Three drain sites:** 0x26603 (the frame loop), 0x8BD9B (a wait loop) and 0x180996 (a level set-up path).
  Whichever thread drains runs the job.
- **Still unchecked:** 0x158D70 also allocates (0xED70) and reads the victim before queueing. Calling it is the
  same as the game's own call only on the thread that runs the hit handler. Our hooks' thread (nav = swap =
  sector, one id in the playtest) has not yet been compared with the drain's or the hit handler's thread id.
- **Safe from the hit handler's thread.** From ours it depends on two things not yet shown: the +0x808 lock being
  present at run time, and our render-thread hooks running on the frame loop's thread. A run-time check of both
  (the lock pointer, and GetCurrentThreadId in our hook against the drain's) would settle it. Not called until then.

## Not yet decoded (next reads)

- 0x158AD0 → 0xD1A10 / 0xD32C0: the weapon blood path for item+0x67C == −2.
- Whether any code raises the vertex alpha back (washing or healing), and whether the hit handler itself runs for
  every hit or only for some blows.
- The TBloodSprays update and render, and where a spray lands.
- The meaning of v ([rbp-0x98]) in the handler. It feeds the damage routine, so it is the blow's damage value
  before the k scale; the exact k at 0x15C71C and 0x15C729 is to be read.
