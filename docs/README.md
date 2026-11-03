# Documentation map

ExCamera has one shipped release line and several local research efforts. Use the status labels below before relying on a document.

- **Current** — describes the shipped v0.3.0 code and release process.
- **Parked** — preserved research that is not built, tested, or shipped with the current release. Its acceptance records do not apply to v0.3.0.
- **Historical** — records a past design, plan, or review. It is not a current architecture, roadmap, or evidence claim.

## Current release

| Document | Purpose |
| --- | --- |
| [README.md](../README.md) | Player overview, compatibility, build, controls, and release behavior. |
| [INSTALL.txt](../INSTALL.txt) | Source installation, tagging, and release recovery. |
| [CHANGELOG.md](../CHANGELOG.md) | Release history through v0.3.0 (revision 60). |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Shipped single-translation-unit build and native integration boundaries. |
| [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) | v0.3.0 release validation, including unchecked in-game work. |
| [PLAYER_README.txt](PLAYER_README.txt) | Source for the packaged `README.txt`; player-facing install and controls. |

The current release is built by `build.py` from `src/first_person.cpp` and `src/mod/`. It does not use CMake or the parked rebuild sources.

## Parked rebuild

The clean rebuild under `src/excamera/`, `src/runtime/`, `src/launcher/`, `src/platform/`, `include/`, `knowledge/`, and `tests/` is parked. It is not part of the v0.3.0 release, `build.py`, or CI.

| Document | Purpose |
| --- | --- |
| [REBUILD_PLAN.md](REBUILD_PLAN.md) | Design, scope, phases, and release gates for the parked rebuild. |
| [rebuild/REBUILD_README.md](rebuild/REBUILD_README.md) | Rebuild status at park time, build commands, and source layout. |
| [rebuild/REBUILD_ARCHITECTURE.md](rebuild/REBUILD_ARCHITECTURE.md) | Rebuild architecture and testing boundaries. |
| [acceptance/README.md](acceptance/README.md) | Index of rebuild acceptance records and their limits. |

Do not treat a rebuild acceptance record as evidence for the current release. The release's observed coverage is in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).

## Historical and abandoned

| Document | Status |
| --- | --- |
| [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) | Historical v0.3.0 hardening plan. Phase 5 was abandoned; Phase 6 is tracked by the current release checklist. |
| ExanimaLua Integration | Fully decoupled in v0.3.0. ExCamera operates 100% standalone with its own native settings and procedural damage/wound systems. |
| `HEADWEAR-RENDER-REVIEW.md` | Historical review of the v0.2.2-prof8 revision 56 headwear renderer. Superseded by later v0.3.0 changes; local-only and not published. |

## Local-only notes

These files are kept on disk for development but are excluded from the published source tree by `.gitignore`:

- `DECISIONS.md` — implementation rationale that is too detailed for the public docs.
- `PROFILING-README.txt` — profiling package comparison notes.
- `COMMIT_MSG.txt` — draft commit text.
- `docs/HEADWEAR-RENDER-REVIEW.md` — historical headwear review.

For the player-facing build and release state, start with the [README](../README.md) and [release checklist](RELEASE_CHECKLIST.md).
