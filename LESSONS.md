# Lessons Learned

## Cursor controls

- Lesson: native **Controls > Cursor Look** (slot 8) is a persistent toggle. Automatic cursor release is a fixed policy for inventory/loot/containers, shops/arsenal, arena/company screens, and settled item descriptions; dialogue is excluded.
- Why: coupling cursor ownership to movement freezing or a global Menu Cursor switch made it impossible to reach arena controls while preserving the player's intended cursor state.
- How to apply: keep free-pointer state independent from movement and camera ownership, preserve Cursor Look through automatic panel open/close, and retain native world-pick reach checks for item and door interactions. Ignore the legacy `FreezeForWindows` INI key.