# Balance shadow

The balance shadow watches every fighter's balance while you play and logs it. **It changes nothing in the game**:
it writes no game memory, and the game's own balance drives every fighter as before. It exists so that any future
balance model has a measured baseline to match or beat (the reliability plan, `GAME-BALANCE-RELIABILITY.md` in the
playtest notes).

**The goal it measures is resilience, not a fall rate.** A fighter should catch itself whenever a real body could,
and fall only when it physically can't recover. Falling because a rule fired (the game's 31-degree lean test, which
a kick could trip on its own) is counted as a fault of the balance system. There is no target number of falls.

Code: `src/balance_shadow_model.h` (the maths, unit-tested in `tests/unit/balance_shadow_tests.cpp`) and
`src/mod/balance_shadow.inl` (reading the game, files, messages). It runs right after the game's own character
update (`combatUpdate` 0x169200, already hooked for manual defense in `combat.inl`), once per fighter per physics
step.

## Turning it on and off

On by default. `[Diagnostics] BalanceShadow=0` in `first-person.ini` turns it off. F7 stops it with the rest of the
mod. `first-person-status.json` → `balance_shadow.state` says whether it runs:

* `running`
* `off_by_setting`
* `group_withheld`: a game update changed the code it reads.
* `no_update_hook`: manual defense, and so the update hook, is unavailable.

## What it reads (all checked at startup)

The game fields come from the optional group `balanceShadow`. Each has a reviewed instruction witness in
`tools/native_contracts.json`, owned by `contactFrame` 0x1603F0, `supportHeight` 0x16E1D0 or `heightForce` 0x16FDF0.
If a game update moves any of them, the shadow switches itself off; nothing else is affected.

| Field | What it is |
|---|---|
| `character::uprightness` +12B0, `leanCounter` +12B4 | the game's lean rule (falls under 0.3) |
| `character::footDown` +101C, `standingFactor` +1020, `onFeet` +12FC | the game's "a foot is down" and the standing factor the motors square |
| `character::stepping` +12EE, `footSlides` +DE4 | a corrective step under way; stepping-foot anchor slides |
| `character::chestPosition` +BE8, `tone` +590 | the chest point the lean rule uses; muscle tone |
| `contact::position` +3C, `contact::groundHeight` +90 | each leg contact's point and the ground under it |
| (existing) part nodes `+F0` / `+120` / `+150`, parts, legs, state, motion | the body itself |

## What it measures, per fighter, per physics step

* **Centre of mass:** the mean of every body-part node. The game's integrator gives every node the same mass, so
  this is the centre the physics actually moves.
* **Velocity:** from the node history, at 83 steps a second (the game's own constant).
* **Capture point:** the centre of mass plus velocity ÷ √(g / height), with g = 980.665 cm/s² (the game's own
  gravity; 1 unit = 1 cm).
* **Base of support:** the outline of the loaded feet. That's every support contact within 20 units of its ground,
  plus every node of the parts hanging from that shin. **Margin** is how far the capture point is inside that
  outline (negative = outside).
* **Shake:** the mean node acceleration with the whole body's own acceleration removed, so walking and getting hit
  don't count, only parts jittering against each other.
* **Costs:** how long the game's own update took for that fighter, and how long the shadow took.
* **Step rate per fighter**, measured each second. This checks the 83-a-second assumption.

## Problems it flags

A condition is reported once when it reaches its duration, and again only after it has cleared.

| Event | Flagged on screen? | Rule (starting values, `balance_shadow_model.h`) |
|---|---|---|
| `non_finite` | yes | a node position is NaN or infinite (bad maths) |
| `flung` | yes | a node moves faster than 30 m/s |
| `floating` | yes | upright, no foot down, not dropping faster than 0.5 m/s, for 0.5 s; jumps excluded |
| `shaking` | yes | upright and nearly still (< 0.3 m/s), shake over 4000 cm/s² for 0.25 s |
| `stuck_down` | yes | on the ground or getting up for 15 s while alive |
| `failed_get_up` | yes | getting up (0x300) went back to lying (0x200) |
| `fall_by_rule` | yes | the game dropped the fighter while the capture point was inside the feet: a rule fired, not physics |
| `fall_physical` | no, counted | the game dropped the fighter within 0.3 s of the capture point leaving the feet: a real fall |
| `recovered` | no, counted | upright on its feet, the capture point left the feet and came back inside for 0.2 s without a fall: the fighter caught a stumble. How deep and how long each caught stumble was goes into the summary. |
| `held_up` | no, counted | upright, a foot down, capture point more than 15 cm outside the feet for 0.5 s, no step and no fall: the "string" holding a body up. Expected to be common with the game's balance and to drop with a new model. |

**On screen** means the game's own message line (the engine log, level 4, through `modMessage`), for example "Balance
check: an NPC is upright with no foot on the ground (floating)." **There is no new window.** At most one message per
problem kind every 15 s; the rest are still counted and logged. If the shadow itself trips over the game's data
more than 1000 times, it stops, says so once, and the game is unaffected.

**The game's own physics error reports** ("Excessive constraint delta", "Excessive pivot delta", "Object position
overflow", "Duplicate RBD grid migration") already show on screen through the engine. The shadow also counts them
by reading the new lines of `%APPDATA%\Exanima\Exanima.log` (a file read, not game memory).

## Files

They're written beside `first-person.ini` in a `balance` folder, one set per session (`<stamp>` is the start
time):

* `balance-<stamp>-events.csv`: one row per event and per game physics report: time, kind, fighter, player,
  state, margin, height, uprightness.
* `balance-<stamp>-seconds.csv`: one row per fighter per second: step rate, state, motion, uprightness, foot down,
  margin, height, shake.
* `balance-<stamp>-summary.txt`: rewritten every 10 s and when the mod stops. It holds:
  * totals and rates per standing fighter-minute;
  * **resilience**: stumbles caught ÷ (caught + real falls);
  * the spreads (mean, p50, p95, p99, peak) of how deep and how long caught stumbles were, margin, shake, step
    rate and both costs;
  * the faults, and `balance_model=game`.

## Comparing

```
python tools/balance_compare.py <baseline-summary.txt> <candidate-summary.txt>
```

Every summary written today is a **baseline**: the game's own balance. A future model's sessions are candidates.
It passes only when all of these hold:

* zero faults;
* every flagged problem's rate, falls by rule included, is no higher than the baseline's (+0.05 per fighter-minute
  for noise);
* **resilience is no lower** (within 0.02);
* the game's physics reports per minute are no higher;
* the p95 costs grow by no more than 10%.

Caught stumbles, real falls, how deep a caught stumble goes, and `held_up` are printed for information. A better
model catches deeper stumbles, has no falls by rule, and drops `held_up` because the strings are gone.

## Tuning

The thresholds above are starting values. The first stock sessions give their real spread: the summary's p95 and
peak of shake and margin. The limits are then set just outside the stock spread, so that stock play flags almost
nothing and a real fault stands out.
