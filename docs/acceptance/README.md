# Rebuild acceptance records

**Status: parked research.** These records describe live acceptance performed against the prototype rebuild. They do not describe or validate the shipped v0.3.0 release, and they do not establish release support for the rebuild.

| Record | Scope |
| --- | --- |
| [camera-adapter.md](camera-adapter.md) | Camera modes, presentation transitions, and paused-state behavior. |
| [input-adapter.md](input-adapter.md) | Mouse look, F6 cycling, pause-safe ownership, and focus recovery. |
| [settings-adapter.md](settings-adapter.md) | FOV controls and persistence across a complete game restart. |
| [lifecycle-adapter.md](lifecycle-adapter.md) | Identity changes, story-to-arena transition, and defeat-safe teardown. |
| [standalone-lifecycle.md](standalone-lifecycle.md) | Attach, status, stop, and re-attach behavior with no remaining hooks. |
| [collision-adapter.md](collision-adapter.md) | Central static-level camera boom, nearest-hit distances, and native cross-check. |
| [frustum-adapter.md](frustum-adapter.md) | Pivot, near-plane corner, and transition-origin coverage. |
| [collision-performance.md](collision-performance.md) | Collision interval measurements and the performance gates that remained open. |

Interaction, ground coverage, doorway and dynamic-object behavior, full performance validation, remaining lifecycle edges, coexistence, and play-soak acceptance were not completed. Raw live logs and local captures are not published with these records.
