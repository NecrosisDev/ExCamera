"""Build the ExCamera runtime from repository root on Windows x64.
Requires Python 3, Visual Studio 2022 C++ Build Tools, and the Windows SDK.
No game executable or research files are distributed or needed to compile.
"""
from pathlib import Path
import argparse, os, subprocess, sys, hashlib, zipfile, shutil, re
ROOT=Path(__file__).resolve().parent
SRC=ROOT/'src'
OUT=ROOT/'build'
MINHOOK=['vendor/minhook/src/buffer.c','vendor/minhook/src/hook.c',
         'vendor/minhook/src/trampoline.c','vendor/minhook/src/hde/hde64.c']
CL='cl /nologo /MT /O2 /std:c++17 /EHsc /fp:strict /W4 /Brepro'
MINHOOK_CL='cl /nologo /MT /O2 /W1 /Brepro /c'
LIBS='user32.lib gdi32.lib opengl32.lib'

def vcvars():
    v=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    path=subprocess.check_output([str(v),'-latest','-products','*','-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
    bat=Path(path)/'VC/Auxiliary/Build/vcvars64.bat'
    if not bat.is_file():raise SystemExit('MSVC x64 C++ toolchain not found')
    return bat

def run(commands):
    lines=['@echo off',f'call "{vcvars()}" >nul',f'cd /d "{SRC}"']
    for c in commands:lines.extend([c,'if %errorlevel% neq 0 exit /b 1'])
    script=OUT/'build.cmd'
    script.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    if subprocess.run(['cmd.exe','/d','/c',str(script)]).returncode:
        raise SystemExit('Runtime build failed')

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--package',action='store_true',help='Build and create minimal release ZIP')
    args=p.parse_args()
    ver=re.search(r'^#define FP_BUILD "([\w.-]+)"$',(SRC/'build_identity.h').read_text(encoding='utf-8'),re.M).group(1)
    OUT.mkdir(exist_ok=True)
    obj=OUT/'obj';obj.mkdir(exist_ok=True)
    cl=f'{CL} /Fo:"{obj}\\\\" '
    minhook=' '.join(f'"{obj/Path(f).with_suffix(".obj").name}"' for f in MINHOOK)
    shim=obj/'supersampling_shim.obj'
    settings_objects=' '.join(f'"{obj/(name+".obj")}"' for name in ("native_settings","settings_presentation"))
    commands=[
        f'{cl}/c native_settings.cpp settings_presentation.cpp',
        f'{MINHOOK_CL} {" ".join(MINHOOK)} /Fo:"{obj}\\\\"',
        f'ml64 /nologo /c /Fo"{shim}" supersampling_shim.asm',
        f'{cl}/LD first_person.cpp {minhook} "{shim}" {settings_objects} {LIBS} /Fe:"{OUT/"ExanimaFirstPerson.dll"}" /link /INCREMENTAL:NO /Brepro',
        f'{cl}launcher.cpp user32.lib /Fe:"{OUT/"ExCamera.exe"}" /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /Brepro',
        f'dumpbin /nologo /exports "{OUT/"ExanimaFirstPerson.dll"}" > "{obj/"exports.txt"}"',
    ]
    for symbol in ('enable','disable','allowOtherHooks','enableWithStatus'):
        commands.append(f'findstr /r /c:" {symbol}$" "{obj/"exports.txt"}" >nul')
    run(commands)
    print('BUILT',ver,OUT/'ExanimaFirstPerson.dll',OUT/'ExCamera.exe')
    if args.package:
        path=OUT/f'ExCamera-{ver}-minimal.zip'
        entries={
          'ExanimaFirstPerson.dll':(OUT/'ExanimaFirstPerson.dll').read_bytes(),
          'ExCamera.exe':(OUT/'ExCamera.exe').read_bytes(),
          'LICENSE.txt':(ROOT/'LICENSE').read_bytes(),
          'LICENSE-MinHook.txt':(SRC/'vendor/minhook/LICENSE.txt').read_bytes(),
        }
        with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            for name,data in entries.items():z.writestr(name,data)
        print('PACKAGE',path)

if __name__=='__main__':main()
