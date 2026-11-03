# Exanima knowledge base

This directory is the reviewed source of truth for native build, function, layout, and capability contracts. It is not a collection of guessed offsets. The current research build contains 34 function records and five capability records.

## Evidence boundary

Imported REA/Ghidra records begin with only `observed_static`. Their `abi`, `thread`, and `memory` sections remain `unknown`, and `generation.runtime_eligible` is `false`. A successful import does not make a function safe to call or hook.

Evidence states are independent:

- `observed_static`: a tool observed the binary or derived representation.
- `reviewed_static`: a reviewer checked the relevant instructions and semantics.
- `byte_validated`: recorded instruction witnesses match the selected executable.
- `observed_runtime`: a live run observed the claim.
- `validated_runtime`: a repeatable in-game scenario passed.
- `hypothesis` and `rejected`: explicit non-supporting states retained for research.

Runtime generation requires at least `reviewed_static` and `byte_validated`. Release support additionally requires the applicable runtime acceptance record.

Raw dossiers and full decompilations stay in the ignored local `.tools/` research area. Publishable records retain evidence IDs, derived addresses, relationships, limitations, and review state, but no absolute research paths or wholesale decompiled source.

## Commands

Use the isolated tools environment from the repository root:

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r tools\requirements-kb.txt
```

The `--overwrite` commands below recreate the research import set and reset the target records to `observed_static`. Use them only when deliberately rebuilding the import set; preserve reviewed ABI, byte witnesses, and acceptance metadata first.

```powershell
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-core\overview.json --dossiers .tools\excamera-core --targets knowledge\research\core-functions.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-camera\overview.json --dossiers .tools\excamera-camera --targets knowledge\research\camera-functions.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-collision\overview.json --dossiers .tools\excamera-collision --targets knowledge\research\collision-functions.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-collision-callers\overview.json --dossiers .tools\excamera-collision-callers --targets knowledge\research\collision-callers.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-ground\overview.json --dossiers .tools\excamera-ground --targets knowledge\research\ground-functions.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-ground-callback\overview.json --dossiers .tools\excamera-ground-callback --targets knowledge\research\ground-callbacks.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-ground-character\overview.json --dossiers .tools\excamera-ground-character --targets knowledge\research\ground-character.json --overwrite
.venv\Scripts\python tools\kb.py import-rea --overview .tools\excamera-support\overview.json --dossiers .tools\excamera-support --targets knowledge\research\ground-support.json --overwrite
```

Validate and regenerate derived files, then run all Python knowledge and camera-metrics tests:

```powershell
.venv\Scripts\python tools\kb.py validate
.venv\Scripts\python tools\kb.py generate
.venv\Scripts\python tools\kb.py check
.venv\Scripts\python -m unittest discover -s tools\tests -p "test_*.py" -v
```

`generate` writes `knowledge/generated/index.md` and `knowledge/generated/contracts.json`. `check` fails if these files are stale. Generation does not emit runtime headers while every record is research-only.

## Adding or promoting a contract

1. Import observations without assigning semantics.
2. Review assembly, callers, callees, object ownership, and decompiler limitations.
3. Edit the function record with the reviewed ABI/thread/memory claims and evidence state. Re-importing with `--overwrite` resets function review metadata to `observed_static`; preserve the review and reapply it only after revalidating the new evidence.
4. Add byte witnesses and validate them against the exact executable.
5. Add and run an acceptance scenario before claiming runtime validation.
6. Run `kb.py generate` and the tests; review the generated diff.

The authoritative rebuild and release gates are in `docs/REBUILD_PLAN.md`.
