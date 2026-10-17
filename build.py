"""Builds ExCamera and runs its tests.

    python build.py             build + tests, into build/
    python build.py --package   also make build/ExCamera-<version>.zip, laid out as a release

Needs the Visual Studio C++ x64 build tools (found with vswhere) and Python 3.
"""
from pathlib import Path
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
SRC = ROOT / 'src'
OUT = ROOT / 'build'
MINHOOK = ['vendor/minhook/src/buffer.c', 'vendor/minhook/src/hook.c', 'vendor/minhook/src/trampoline.c',
           'vendor/minhook/src/hde/hde64.c']
CL = 'cl /nologo /MT /O2 /std:c++17 /EHsc /fp:strict /W4 /Brepro'
# MinHook includes the documented entry-ownership guard and is built once at /W1: its upstream
# warnings would bury ours.
MINHOOK_CL = 'cl /nologo /MT /O2 /W1 /Brepro /c'
LIBS = 'user32.lib gdi32.lib opengl32.lib'
# True: the test builds first_person.cpp itself, so it links MinHook and the system libraries too.
TESTS = {'test_camera_motion': False, 'test_yaw_stabilization': False, 'test_view_boundaries': False,
         'test_game_layout': False, 'test_status_json': False, 'test_held_object': False,
         'test_module_export': False,
         'test_fov_input': True, 'test_controls': True, 'test_runtime_lifecycle': True, 'test_chaining': True,
         'test_pose_ownership': True, 'test_held_lifecycle': True,
         'test_input_handoff': True, 'test_exploration_movement': True, 'test_movement': True,
         'test_combat_controls': True, 'test_binding_ownership': True, 'test_tp_aim_freshness': True,
         'test_manual_admission': True, 'test_input_ownership': False,
         'test_activation_status': False, 'test_raw_registration': False,
         'test_activation_lifecycle': True, 'test_cue_resources': False,
         'test_cue_lifecycle': True, 'test_tp_hide_origin': True,
         'test_native_contracts': False}
GAME_VERSION = '0.9.5.2g'


def vcvars():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    path = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    bat = Path(path) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not bat.is_file():
        sys.exit('The Visual Studio C++ x64 build tools were not found.')
    return bat


def run(commands):
    """Runs the commands in one Visual Studio environment; stops at the first failure (a crashed test
    exits with a negative status, so the check is 'not zero', not 'errorlevel 1')."""
    script = ['@echo off', f'call "{vcvars()}" >nul', f'cd /d "{SRC}"']
    for c in commands:
        script += [c, 'if %errorlevel% neq 0 exit /b 1']
    cmd = OUT / 'build.cmd'
    cmd.write_text('\n'.join(script) + '\n', encoding='utf-8')
    if subprocess.run(['cmd.exe', '/d', '/c', str(cmd)]).returncode:
        sys.exit('Build or tests failed.')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(version):
    """The release layout: the DLL and ExCamera.exe, default settings, the readme and licences, and the
    folder EMTK's legacy native-plugin loader reads (<Exanima>\\mods\\exanima-first-person)."""
    stage = OUT / 'package'
    if stage.exists():
        shutil.rmtree(stage)
    mod = stage / 'mods/exanima-first-person'
    mod.mkdir(parents=True)
    ini = '[Camera]\nFOV=75\n[Input]\nSensitivityPercent=100\nInvertY=0\nRawMouse=0\nAttackSideKey=0\n[View]\nHelmet=show\nAttackCue=1\n[Compat]\nOtherHooks=ask\n'
    for folder in (stage, mod):
        shutil.copyfile(OUT / 'ExanimaFirstPerson.dll', folder / 'ExanimaFirstPerson.dll')
        (folder / 'first-person.ini').write_text(ini, encoding='utf-8')
    shutil.copyfile(OUT / 'ExCamera.exe', stage / 'ExCamera.exe')
    shutil.copyfile(ROOT / 'packaging/README.txt', stage / 'README.txt')
    shutil.copyfile(ROOT / 'LICENSE', stage / 'LICENSE.txt')
    shutil.copyfile(SRC / 'vendor/minhook/LICENSE.txt', stage / 'LICENSE-MinHook.txt')
    (mod / 'config.toml').write_text(
        '[plugin]\nid = "exanima-first-person"\nname = "ExCamera"\nversion = "' + version + '"\n'
        'supported_versions = ["' + GAME_VERSION + '"]\nurl = ""\nenabled = true\nexecutable = "ExanimaFirstPerson.dll"\n\n'
        '[plugin.author]\nname = "NecrosisDev"\n', encoding='utf-8')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage / 'SHA256SUMS.txt').write_text(''.join(sha(p) + '  ' + p.relative_to(stage).as_posix() + '\n' for p in files), encoding='utf-8')
    archive = OUT / f'ExCamera-{version}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())
    print('PACKAGE=' + str(archive))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--package', action='store_true')
    args = parser.parse_args()
    identity = (SRC / 'build_identity.h').read_text(encoding='utf-8')
    version = re.search(r'^#define FP_BUILD "([\w.-]+)"$', identity, re.MULTILINE)[1]
    OUT.mkdir(exist_ok=True)
    obj = OUT / 'obj'
    obj.mkdir(exist_ok=True)
    cl = f'{CL} /Fo:"{obj}\\\\" '
    minhook = ' '.join(f'"{obj / Path(c).with_suffix(".obj").name}"' for c in MINHOOK)
    commands = [
        f'{MINHOOK_CL} {" ".join(MINHOOK)} /Fo:"{obj}\\\\"',
        f'{cl}/LD first_person.cpp {minhook} {LIBS} /Fe:"{OUT / "ExanimaFirstPerson.dll"}" /link /INCREMENTAL:NO /Brepro',
        f'{cl}launcher.cpp user32.lib /Fe:"{OUT / "ExCamera.exe"}" /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /Brepro',
        # EMTK keeps enable/disable; ExCamera.exe uses the request-bound activation protocol.
        f'dumpbin /nologo /exports "{OUT / "ExanimaFirstPerson.dll"}" > "{obj / "exports.txt"}"',
        f'findstr /r /c:" enable$" "{obj / "exports.txt"}" >nul',
        f'findstr /r /c:" disable$" "{obj / "exports.txt"}" >nul',
        f'findstr /r /c:" allowOtherHooks$" "{obj / "exports.txt"}" >nul',
        f'findstr /r /c:" enableWithStatus$" "{obj / "exports.txt"}" >nul',
        f'"{sys.executable}" "{ROOT / "tools/test_signatures.py"}" > "{OUT / "test_signatures.log"}" 2>&1',
        f'"{sys.executable}" "{ROOT / "tools/test_native_contracts.py"}" > "{OUT / "test_native_contracts_python.log"}" 2>&1',
    ]
    for test, game in TESTS.items():
        deps = f' {minhook} {LIBS}' if game else ''
        exe = obj / (test + '.exe')
        commands += [f'{cl}{test}.cpp{deps} /Fe:"{exe}" /link /INCREMENTAL:NO /Brepro', f'"{exe}" > "{OUT / (test + ".log")}"']
    run(commands)
    print('BUILT=' + str(OUT / 'ExanimaFirstPerson.dll') + ' (' + version + '); tests passed: '
          + ', '.join(TESTS) + ', test_signatures, test_native_contracts_python')
    if args.package:
        package(version)


if __name__ == '__main__':
    main()
