# Threat markers

Small soft arcs on a ring around the screen centre point toward anyone fighting you who is off screen or near
its edge. They brighten while that attacker's swing winds up. No text, no window: they are drawn in the game's
own frame with the wind-up arc's program (`CueResources`), in the combat cursor's colour.

Settings (Combat → Threat markers): **Threat markers** Off / Combat only (default, only while you are in combat
stance) / Always, and **Threat markers: opacity** (35% by default). ini: `[View] ThreatMarkers=off|combat|always`,
`ThreatOpacity=10..100`.

## Who is a threat (`fp::threat::isThreat`, threat_markers.h)

Alive, not you, within ten metres, aiming at you, and on guard or attacking. Each input is a game value the
code below proves; the range is ours.

| Input | Field | Proof |
|---|---|---|
| Aiming at you | `character::aimOpponent` +E38 equals the player's root | For other characters: TMotileAI's tick (0xB4570, VMT 0x309960 slot +F0) calls its state procedure in behaviour +8E8 (0xB4BFE). The combat state 0xB8440, installed there by 0xB8190 and 0xBE0E0, calls 0xB8210 (0xBA58E, 0xBA5B3). That routine sets its character's +E38 to the chosen record's +10 (0xB8288) and clears it with no target (0xB8227, 0xB8345). The game reads +E38 back as a character: defenseReactive 0x163880 hands it to 0x1636E0, which reads its +9B8, +D30 and +1018. For the player: combatAttention 0xBEAC0 (0xBED90), called only from TPlayerBehaviour's tick 0xC1CF0. |
| Alive, not down | `stateFlags` +DF4: dead `& 0x800`, down `& 0xA00` | docs/GAME-FIELDS-AUDIT.md |
| Attacking | `motion` +9B8 in 5-12 | motionPlay 179F60-7A (audit) |
| Winding up | `motionProgress` +A28 below 0.30, 0.4 for the Stab (11) | the game's commit points (audit; input_policy.h) |
| On guard | `combatStance` +12FE, or motion 3 | +12FE is written by the TAB toggle for the player only (0x1200EC, through playerRoot 0x4F0830). For other characters it is set only at spawn, with motion 3 (0xF026A/0xF0274), and cleared on death (0x156900). So an NPC's guard is also told by motion 3, "Defence stance" in the game's motion-set table. |
| Where | `sceneNode` +18, its world origin (frame.inl `sceneNodeWorldOrigin`) | contracted |

Every field is already contracted, so check_game covers it; nothing new is read.

## When it runs

`threatObserve` (threats.inl) runs on the game thread right after the game's own update of each character
(`combatUpdate`, combat.inl), outside nested updates. It copies the values above into a 32-slot table. The
reads are guarded with `__try`, as the balance shadow reads the same live objects. That hook is installed with
manual defense, so the rows need the same game-build capability.

`presentThreats` (threat_draw.inl) runs on the swap thread after the wind-up arc:

- It uses only entries seen in the last 0.3 s.
- It finds the player's own entry, and in Combat only mode requires the player's combat stance.
- It places each threat with the achieved camera (`lastCamera`, `renderedFov`, under `cameraLock`):
  - A threat in front and well inside the view (both screen offsets under 70% of the half size) gets no marker.
  - A threat in front but near the edge or off screen points along its projection.
  - A threat behind points from above, so behind is down (`fp::threat::markerAngle`).
- Markers fade in over 0.18 s and out over 0.26 s. Their opacity is the setting's, times 1.8 while winding up,
  at most 1.

Status: `threat_markers` in first-person-status.json (mode, reads, read faults, markers shown, draws).

## Ours, not the game's

The ten-metre range, the ring (21% of the smaller screen side), the arc's span and widths, the 70% inner area,
the fades and the brightening.
