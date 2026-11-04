"""Compile and run MSVC C++ unit tests for shipped ExCamera headers.

Uses an isolated build/_cpp_tests staging folder so it never contends with build/.build.lock.
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import build  # noqa: E402

TESTS_DIR = ROOT / 'tests' / 'unit'
STAGE_DIR = ROOT / 'build' / '_cpp_tests'
CL_FLAGS = '/nologo /MT /O2 /std:c++17 /EHsc /W4 /WX'


def extra_sources_for(test_source: Path):
    text = test_source.read_text(encoding='utf-8')
    extras = []
    if 'settings_presentation.h' in text:
        extras.append(build.SRC / 'settings_presentation.cpp')
    return extras


def main():
    sources = sorted(TESTS_DIR.glob('*_tests.cpp'))
    if not sources:
        raise SystemExit(f'No C++ unit tests found in {TESTS_DIR}')

    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    obj_dir = STAGE_DIR / 'obj'
    obj_dir.mkdir(parents=True, exist_ok=True)

    vcvars = build.find_vcvarsall()
    hde_obj = obj_dir / 'hde64.obj'
    commands = [
        f'{build.MINHOOK_CL} "{build.SRC / "vendor/minhook/src/hde/hde64.c"}" /Fo:"{obj_dir}\\\\"'
    ]
    binaries = []
    includes = f'/I"{build.SRC}" /I"{ROOT / "tests"}"'
    for src in sources:
        stem = src.stem
        target_obj_dir = obj_dir / stem
        target_obj_dir.mkdir(parents=True, exist_ok=True)
        exe_path = STAGE_DIR / f'{stem}.exe'
        inputs = [src, *extra_sources_for(src)]
        quoted_inputs = ' '.join(f'"{p}"' for p in inputs)
        commands.append(
            f'cl {CL_FLAGS} {includes} {quoted_inputs} "{hde_obj}" /Fo:"{target_obj_dir}\\\\" /Fe:"{exe_path}" /link /INCREMENTAL:NO'
        )
        binaries.append(exe_path)

    build.run_msvc(commands, STAGE_DIR, source_dir=ROOT, vcvars_path=vcvars)

    failures = 0
    for exe in binaries:
        print(f'=== {exe.name} ===', flush=True)
        result = subprocess.run([str(exe)], cwd=STAGE_DIR)
        if result.returncode != 0:
            failures += 1

    if failures:
        raise SystemExit(f'{failures} of {len(binaries)} C++ test binaries failed')
    print(f'All {len(binaries)} C++ unit test binaries passed.')


if __name__ == '__main__':
    main()
