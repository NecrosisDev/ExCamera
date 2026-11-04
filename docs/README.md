# Documentation map

ExCamera ships from a single runtime source tree on `main`. Use the status labels below before relying on a document.

- **Current** — describes the shipped v0.3.0 runtime, tooling, and release process.
- **Historical** — records a past design, plan, or review. It is not a current architecture, roadmap, or evidence claim.

## Current release

| Document | Purpose |
| --- | --- |
| [README.md](../README.md) | Player overview, compatibility, build summary, controls, and release behavior. |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | Developer setup, `build.py` flags, Python & C++ unit tests, profiling, tagging, and release recovery. |
| [CHANGELOG.md](../CHANGELOG.md) | Release history through v0.3.0. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Shipped single-translation-unit build, `ScriptRuntime.dll` coexistence, and native integration boundaries. |
| [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) | v0.3.0 release validation, including in-game verification items. |
| [PLAYER_README.txt](PLAYER_README.txt) | Source for the packaged `README.txt`; player-facing install and controls. |
| [tools/README.md](../tools/README.md) | Native signature, field-contract, and XR-contract generator/checker overview. |

The shipped runtime is built solely by `build.py` from `src/first_person.cpp`, `src/mod/`, `src/launcher.cpp`, `src/native_settings.cpp`, and `src/settings_presentation.cpp`.

## Standalone runtime & `ScriptRuntime.dll` coexistence

ExCamera operates **100% standalone** with zero external dependencies, including its native Settings window integration and native procedural wounds, blood, and fire. When `ScriptRuntime.dll` (ExanimaLua) is present in the game process, ExCamera optionally coexists with it — hosting third-party mods' registered Settings tabs inside ExCamera's widened native Options form and using `ScriptRuntime` inventory exports for the optional VR hip pouch.

## Historical and archived

| Item | Status |
| --- | --- |
| [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) | Historical v0.3.0 hardening plan. Phase 5 (ExanimaLua-hosted ExCamera settings) was abandoned; Phase 6 is tracked in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). |
| Branch `archive/rebuild-prototype` | Preserves the parked clean-room CMake prototype and its acceptance notes outside `main`. Not built or shipped with releases. |
| `docs/HEADWEAR-RENDER-REVIEW.md` | Local-only historical review of an earlier headwear renderer iteration; excluded by `.gitignore`. |

For the current build, testing, and release state, start with [README.md](../README.md), [CONTRIBUTING.md](../CONTRIBUTING.md), and [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).
