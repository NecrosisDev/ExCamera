# Native source specifications

The JSON files are the source of truth for the generated native headers. Do not
hand-edit `src/game_signatures.h` or `src/game_contracts.h`.

## Checks without the game

Python 3.8 or newer, standard library only:

    python tools/check_generated.py

The consistency check covers declared offsets, witness ownership, target coverage,
optional groups, signature metadata, enum ordering, slot checks, and exact generated
header text. It detects source/header drift; it cannot establish that new native
claims are correct without the executable and review.

## Native validation and generation

Use Python 3.10 or newer for native generation, with `pefile` and `capstone` installed. Keep the game
executable outside the repository and release archives.

    python tools/signatures.py generate /path/to/Exanima.exe
    python tools/native_contracts.py generate /path/to/Exanima.exe
    python tools/signatures.py check /path/to/Exanima.exe
    python tools/native_contracts.py check /path/to/Exanima.exe
    python tools/check_generated.py

Generation requires the SHA-256 source archive recorded in both specifications.
It verifies the reviewed evidence before writing headers. Signature generation
checks all patterns individually, not just whether one fallback resolves a target.
The runtime checker can check other game builds without a hash allowlist.

`targets.json` retains the complete reviewed signature set. This includes interior
anchors with signed addends, register-blind alternatives, independent references,
and held-function slot checks. Automatic rediscovery must not silently replace or
drop these. `target_order` retains the runtime enum order independently of signature
row order.

`native_contracts.json` records native owner ranges, exact reviewed instruction
windows and member operands, derivations, references, and complete event sequences.
A field with `witness` is guarded by that named event window; it is not an extra
runtime row. Negative header/frame offsets use `adjust`; the element stride uses
an immediate operand. These forms are checked against actual disassembly during
generation. Event windows intentionally retain their reviewed bytes, including
fixed branch displacements, rather than automatically weakening their masks.

For native changes, review and update the JSON evidence and provenance, regenerate
against its identified executable, inspect the generated diff, and run both native
checks. Verification fixtures are maintained outside this repository. Passing byte checks establishes only the
recorded instruction/target contracts, not object lifetimes, every caller, or
in-game behavior on an untested build.
