"""Read-only compatibility report for an installed Exanima.exe.

    python tools/check_game.py                 find Exanima.exe in the Steam libraries
    python tools/check_game.py <Exanima.exe>   check that executable

Run this after a game update, before playing with ExCamera. It identifies the build by SHA-256
against the hashes in src/native_build.h, then runs the three native checks the mod relies on:
code signatures (tools/signatures.py), field/reference contracts (tools/native_contracts.py) and
the whole-function VR contracts (tools/xr_contracts.py). The executable is only read, never run.

A reviewed build passes everything. On another build, a FAIL row is a target or field the mod would
refuse (the DLL then stays off, or withholds that optional feature); features listed under
"hash-gated" are switched off on any build not listed in src/native_build.h because they use struct
offsets no checker covers. Needs: pip install pefile capstone.
"""
from pathlib import Path
import contextlib
import hashlib
import io
import os
import re
import sys

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

ROOT = TOOLS_DIR.parent
BUILD_HEADER = ROOT / 'src/native_build.h'
HASH_RE = re.compile(r'"([0-9a-f]{64})",\s*//\s*(.+)')
# Optional features that require a hash-listed build (src/native_build.h `verifiedBuild`).
HASH_GATED = ['attack variants (Manual defense keeps working)']


def known_builds(text=None):
    """{sha256: label} from the executableHashes table in src/native_build.h."""
    if text is None:
        text = BUILD_HEADER.read_text(encoding='utf-8')
    return {h: label.strip() for h, label in HASH_RE.findall(text)}


def steam_libraries():
    roots = []
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(env):
            roots.append(Path(os.environ[env]) / 'Steam')
    libraries = list(roots)
    for root in roots:
        vdf = root / 'steamapps' / 'libraryfolders.vdf'
        try:
            text = vdf.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        libraries += [Path(p.replace('\\\\', '\\')) for p in re.findall(r'"path"\s+"([^"]+)"', text)]
    return libraries


def find_exe():
    for library in steam_libraries():
        exe = library / 'steamapps' / 'common' / 'Exanima' / 'Exanima.exe'
        if exe.is_file():
            return exe
    return None


def run(label, fn, *args):
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            code = fn(*args)
    except SystemExit as stop:
        code = stop.code if isinstance(stop.code, int) else 1
        if stop.code and not isinstance(stop.code, int):
            print(stop.code, file=out)
    except (ValueError, KeyError, IndexError, OSError) as error:
        code = 1
        print(error, file=out)
    status = 'ok' if code == 0 else 'PROBLEM'
    print('[%s] %s' % (status, label))
    for line in out.getvalue().splitlines():
        print('    ' + line)
    return code == 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] in (['-h'], ['--help']):
        print(__doc__)
        return 0
    exe = Path(argv[0]) if argv else find_exe()
    if exe is None or not exe.is_file():
        print('Exanima.exe not found; pass its path: python tools/check_game.py <Exanima.exe>')
        return 2
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    label = known_builds().get(digest)
    print('Executable: %s' % exe)
    print('SHA-256:    %s' % digest)
    print('Build:      %s' % (('reviewed, ' + label) if label else 'NOT a reviewed build'))
    try:
        import signatures
        import native_contracts
        import xr_contracts
    except ImportError as error:
        print('Cannot import the checkers: %s' % error)
        return 2
    ok = run('code signatures', signatures.check, exe)
    ok = run('field and reference contracts', native_contracts.check, exe) and ok
    if not run('VR whole-function contracts', xr_contracts.check, exe):
        print('    VR is withheld on this build; flat-screen play is unaffected.')
    if not label:
        print('Hash-gated features off on this build: ' + ', '.join(HASH_GATED))
    print('Result: %s' % ('compatible' if ok else 'NOT compatible, see PROBLEM rows above'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
