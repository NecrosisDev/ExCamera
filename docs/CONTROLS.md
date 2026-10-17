# Controls

ExCamera uses your existing Exanima movement, run, duck, attack and interaction bindings.
First and third person share action bindings, with separate aim-box and defense settings.
Press **F8** to change settings.

## Main keys

| Input | Action |
|---|---|
| **F6** | Cycle first person → third person → overhead. |
| **F7**, held for one second | Stop ExCamera and restore the game's view; F6 resumes after a clean stop. |
| **F8** | Toggle the free pointer and Settings. |
| **F9 / F10 / F11** | Lower / raise / reset the base field of view in first or third person. Changes use 5° steps; reset is 75°. Third person can have its own FOV setting. |
| **Tab** and other game UI keys | Keep their normal Exanima actions. |

## What changes with the view

| Control | First person | Third person | Overhead |
|---|---|---|---|
| Mouse | Aim within the aim box; turn your head at its edge. | Aim within the aim box; turn the shoulder view at its edge. Free aim can be disabled. | Normal game pointer. |
| Movement keys | Move relative to the horizontal direction you are looking, in exploration and combat. | Same. Looking at a nearby or elevated target does not change the movement frame. | Normal game movement. |
| Camera rotate / camera-turn bindings | Disabled during gameplay. | Disabled during gameplay. | Normal game camera controls. |
| Wheel, empty hands | No camera zoom. | No camera zoom; adjust distance in Settings. | Normal zoom. |
| Wheel, held object or eligible door/lid | Move the held target nearer or farther, within reach. | Same. | Normal game handling. |
| Right mouse, exploration | Free pointer: hold by default, with toggle and off options. | Same. | Normal game action. |
| Right mouse, ordinary combat | Optional Manual guard; no cursor walking. | Same, with an independent defense setting. | Normal game action. |

Each aim-box axis can be set to zero for direct turning. Physical head, body and weapon motion
still follow the game's simulation. Releasing movement withdraws the request; an accepted step,
braking or balance correction may still finish.

## Combat and manual guard

**Automatic defense is the default.** In Settings, choose **First person: defense** or
**Third person: defense** independently.

With **Manual (hold RMB)**, hold right mouse to permit the game's defensive assistance. You can
still aim and move. Equipment, facing, reach, recovery and physical contact determine whether a
parry succeeds; this option adds no guaranteed block or timed-parry bonus.

Guard takes priority over a new left-mouse attack. If a press overlaps guard, release left mouse
and press it again after releasing guard to start another attack. Letting go of right mouse
does not replay the suppressed attack. Existing swings obey the game's interruption rules.

Interaction and object handling keep their own controls. If Manual is unavailable for the
current game/mod combination, Settings reports that and Automatic remains effective.

The optional `AttackSideKey` in `first-person.ini` starts unbound (`0`). Hold an assigned key
when starting an ordinary attack to reverse the chosen swing side; it does not start an attack.

## Leaning, crouching and held objects

| Action | Control |
|---|---|
| Lean with the mouse | Hold the Lean binding (**Alt** by default) and move the mouse. Mouse up bends forward. Release to straighten. |
| Lean with separate keys | Select **Lean controls: left / right keys**, then assign the keys. Hold one to lean; release it or hold both to straighten. The mouse remains available for aiming. |
| Rotate a held object | Hold **Rotate held object** (**RMB** by default) and move the mouse. Mouse look pauses while rotating. |
| Exploration crouch | Use the game's duck binding. Choose toggle, hold or the game's short duck in Settings. Combat retains the normal duck behavior. |

When bindings overlap, held-object rotation takes priority. Manual guard takes RMB before a
lean binding in ordinary combat; lean takes priority over exploration free pointer. A binding
used for these actions also suppresses its matching movement, run, duck or alternate-attack
action while owned. Other game/UI binding conflicts are not automatically remapped.

After switching views, changing bindings, opening UI or returning focus, release and press held
action buttons again. This prevents an old hold from becoming a new attack, guard or lean.
The free pointer pauses gameplay look and movement; game windows retain their own controls.

## Current limits

- Keyboard and mouse are the supported controlset. Controller input retains native analog
  behavior and is not a complete first/third-person controlset.
- The game's restrictions on walking while carrying remain. Held-object rotation does not
  enable unrestricted carrying movement.
- Crouching keeps the game's physical response; headroom checks do not guarantee clearance
  for the whole character.
- This update has passed offline checks but has not been playtested in Exanima. Its EMTK
  loading path also remains untested in the game.
