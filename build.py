"""Build the ExCamera runtime from repository root on Windows x64.
Requires Python 3.8 or newer, Visual Studio 2022 C++ Build Tools, and the Windows SDK.
No game executable or research files are distributed or needed to compile.
"""
from pathlib import Path
import argparse, os, subprocess, sys, zipfile, shutil, re, tempfile
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parent
SRC = ROOT / 'src'
OUT = ROOT / 'build'
MINHOOK = [
    'vendor/minhook/src/buffer.c',
    'vendor/minhook/src/hook.c',
    'vendor/minhook/src/trampoline.c',
    'vendor/minhook/src/hde/hde64.c',
]
CL = 'cl /nologo /MT /O2 /std:c++17 /EHsc /fp:strict /W4 /Brepro'
MINHOOK_CL = 'cl /nologo /MT /O2 /W1 /Brepro /c'
LIBS = 'user32.lib gdi32.lib opengl32.lib'
OPENXR_LOADER_CANDIDATES = (
    'bin/openxr_loader.dll',
    'x64/bin/openxr_loader.dll',
    'build/src/loader/Release/openxr_loader.dll',
    'openxr_loader.dll',
)
OPENXR_LICENSE_CANDIDATES = (
    'LICENSE',
    'LICENSE.txt',
    'LICENSES/Apache-2.0.txt',
)


def find_vcvarsall():
    v = (
        Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
        / 'Microsoft Visual Studio/Installer/vswhere.exe'
    )
    if not v.is_file():
        raise SystemExit('vswhere.exe not found; install Visual Studio 2022 C++ Build Tools')
    path = subprocess.check_output(
        [
            str(v), '-latest', '-products', '*', '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property', 'installationPath', '-utf8',
        ],
        text=True,
        encoding='utf-8-sig',
    ).strip()
    bat = Path(path) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not bat.is_file():
        raise SystemExit('MSVC x64 C++ toolchain not found')
    return bat


vcvars = find_vcvarsall


def run_msvc(commands, stage, source_dir=SRC, vcvars_path=None):
    stage = Path(stage)
    environment = os.environ.copy()
    environment.update(
        EXCAMERA_BUILD_SOURCE=str(source_dir),
        EXCAMERA_BUILD_STAGE=str(stage),
        EXCAMERA_BUILD_VCVARS=str(vcvars_path or find_vcvarsall()),
    )
    # Keep path bytes out of cmd's command string and switch code page to UTF-8
    # so non-ASCII SDK and workspace paths never fail with UnicodeEncodeError.
    lines = [
        '@echo off',
        'chcp 65001 >nul',
        'setlocal DisableDelayedExpansion',
        'call "%EXCAMERA_BUILD_VCVARS%" >nul || exit /b 1',
        'setlocal DisableDelayedExpansion',
        'cd /d "%EXCAMERA_BUILD_SOURCE%" || exit /b 1',
    ]
    prefix = '"' + str(stage) + os.sep
    for c in commands:
        lines.append(c.replace(prefix, '"%EXCAMERA_BUILD_STAGE%' + os.sep) + ' || exit /b 1')
    lines.append('exit /b 0')
    script = stage / '_msvc_build.cmd'
    script.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    if subprocess.run(
        ['cmd.exe', '/d', '/v:off', '/c', r'.\_msvc_build.cmd'], cwd=stage, env=environment
    ).returncode:
        raise SystemExit('Runtime build failed')


run = run_msvc


def read_define(source, name='FP_BUILD'):
    text = Path(source).read_text(encoding='utf-8') if isinstance(source, Path) else source
    matches = re.findall(rf'^#define {re.escape(name)} "([\w.-]+)"$', text, re.M)
    if len(matches) != 1:
        raise ValueError(f'Expected exactly one {name} definition')
    return matches[0]


def validate_manifest(source, expected_version=None):
    text = Path(source).read_text(encoding='utf-8') if isinstance(source, Path) else source
    if not re.search(r'^\[plugin\]\s*$', text, re.M):
        raise ValueError('manifest.toml is missing [plugin] section')
    names = re.findall(r'^name = "([^"\r\n]+)"$', text, re.M)
    if names != ['ExCamera']:
        raise ValueError('manifest.toml must declare name = "ExCamera"')
    versions = re.findall(r'^version = "([^"\r\n]+)"$', text, re.M)
    if len(versions) != 1 or not versions[0]:
        raise ValueError('manifest.toml has no valid version line')
    if expected_version is not None and versions[0] != expected_version:
        raise ValueError(f'manifest.toml version {versions[0]!r} does not match {expected_version!r}')
    return versions[0]


@contextmanager
def build_lock(out):
    import msvcrt

    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    # Persistent coordination file, not a sentinel. Never unlink it: another
    # process must lock the same file, not a newly recreated path.
    with (out / '.build.lock').open('a+b') as stream:
        os.set_inheritable(stream.fileno(), False)
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError(
                'Another build.py process is already running (build/.build.lock is held)'
            ) from error
        try:
            yield
        finally:
            stream.seek(0)
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError as error:
                cleanup_warning("explicit lock release failed; closing its handle: " + str(error))


def cleanup_warning(message):
    try:
        print("WARNING: " + message, file=sys.stderr)
    except OSError:
        pass


def staged_publish(out, names, build, replace=os.replace):
    out = Path(out)
    out.mkdir(exist_ok=True)
    with build_lock(out):
        _publish_locked(out, names, build, replace)


def _publish_locked(out, names, build, replace):
    stage = None
    keep = False
    try:
        stage = Path(tempfile.mkdtemp(prefix='.stage-', dir=out))
        build(stage)  # All compiler/export/ZIP validation happens before publishing anything.
        previous = stage / 'previous'
        previous.mkdir()
        for name in names:
            target = out / name
            if target.is_symlink() or (target.exists() and not target.is_file()):
                raise ValueError("Refusing non-file or symlink publication target: " + str(target))
            if not (stage / name).is_file() or not (stage / name).stat().st_size:
                raise ValueError('Missing or empty staged artifact: ' + name)
            if (out / name).exists():
                shutil.copy2(out / name, previous / name)
        attempted = []
        try:
            for name in names:  # DLL, EXE, and ZIP last; replacements individually atomic.
                attempted.append(name)  # Cover handled interruption immediately after replacement.
                replace(stage / name, out / name)
        except BaseException:
            keep = True  # A second interruption during rollback must not discard recovery files.
            failed = []
            for name in reversed(attempted):
                try:
                    if (previous / name).exists():
                        # A locked-target replace can fail before changing anything.
                        # Prove its bytes unchanged before deciding a restore is needed.
                        if (
                            not (out / name).exists()
                            or (out / name).read_bytes() != (previous / name).read_bytes()
                        ):
                            replace(previous / name, out / name)
                    else:
                        (out / name).unlink(missing_ok=True)
                except BaseException as error:
                    failed.append(name + ': ' + type(error).__name__ + ': ' + str(error))
            if failed:
                raise RuntimeError(
                    'Publication incomplete. Do not use build outputs. Recovery files: '
                    + str(stage) + '; ' + ', '.join(failed)
                )
            keep = False
            raise
    except BaseException as error:
        if keep:
            raise
        raise RuntimeError(
            'Build/publication failed; previous published outputs were retained, not rebuilt: '
            + str(error)
        ) from error
    finally:
        if not keep and stage is not None:
            try:
                shutil.rmtree(stage)
            except OSError as error:
                cleanup_warning(
                    "staging cleanup failed; files retained at " + str(stage) + ": " + str(error)
                )


def resolve_openxr_sdk(sdk, require_loader=False):
    if sdk is None:
        return None
    sdk = Path(sdk).resolve()
    if not (sdk / 'include/openxr/openxr.h').is_file():
        raise FileNotFoundError('OpenXR headers missing: ' + str(sdk))
    loader = next((sdk / rel for rel in OPENXR_LOADER_CANDIDATES if (sdk / rel).is_file()), None)
    if require_loader and loader is None:
        raise FileNotFoundError(
            f'openxr_loader.dll not found under {sdk} (searched {", ".join(OPENXR_LOADER_CANDIDATES)}); '
            'xr_stereo.inl requires openxr_loader.dll beside ExanimaFirstPerson.dll'
        )
    license_file = next((sdk / rel for rel in OPENXR_LICENSE_CANDIDATES if (sdk / rel).is_file()), None)
    return {'root': sdk, 'loader': loader, 'license': license_file}


def xr_flags(sdk):
    """Compile flags for the optional OpenXR integration; None keeps it out of the build."""
    info = resolve_openxr_sdk(sdk, require_loader=False)
    if info is None:
        return ''
    return f'/DFP_OPENXR_EMULATION /DXR_USE_PLATFORM_WIN32 /DXR_USE_GRAPHICS_API_OPENGL /I"{info["root"] / "include"}" '


def build_runtime(stage, ver, package, dev=False, openxr_sdk=None, wx=False):
    xr_info = resolve_openxr_sdk(openxr_sdk, require_loader=package)
    obj = stage / 'obj'
    obj.mkdir(exist_ok=True)
    cl = f'{CL}{" /WX" if wx else ""}{" /DFP_DEV_TOOLS=1" if dev else ""} /Fo:"{obj}\\\\" '
    xr = xr_flags(openxr_sdk)
    minhook = ' '.join(f'"{obj / Path(f).with_suffix(".obj").name}"' for f in MINHOOK)
    shim = obj / 'supersampling_shim.obj'
    settings_objects = ' '.join(
        f'"{obj / (name + ".obj")}"' for name in ("native_settings", "settings_presentation")
    )
    commands = [
        f'{cl}/c native_settings.cpp settings_presentation.cpp',
        f'{MINHOOK_CL} {" ".join(MINHOOK)} /Fo:"{obj}\\\\"',
        f'ml64 /nologo /c /Fo"{shim}" supersampling_shim.asm',
        (
            f'{cl}{xr}/LD first_person.cpp {"xr_host.cpp " if xr else ""}{minhook} "{shim}" {settings_objects} {LIBS} '
            f'/Fe:"{stage / "ExanimaFirstPerson.dll"}" /link /INCREMENTAL:NO /Brepro'
        ),
        (
            f'{cl}launcher.cpp user32.lib /Fe:"{stage / "ExCamera.exe"}" '
            f'/link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /Brepro'
        ),
        f'dumpbin /nologo /exports "{stage / "ExanimaFirstPerson.dll"}" > "{obj / "exports.txt"}"',
    ]
    for symbol in ('enable', 'disable', 'allowOtherHooks', 'enableWithStatus'):
        commands.append(f'findstr /r /c:" {symbol}$" "{obj / "exports.txt"}" >nul')
    run_msvc(commands, stage)
    # EMTK skips a mod folder without manifest.toml; its version is the build's.
    raw_manifest = (ROOT / 'manifest.toml').read_text(encoding='utf-8')
    validate_manifest(raw_manifest)
    manifest = re.sub(r'^version = "[^"\r\n]*"$', f'version = "{ver}"', raw_manifest, count=1, flags=re.M)
    validate_manifest(manifest, expected_version=ver)
    (stage / 'manifest.toml').write_text(manifest, encoding='utf-8', newline='\n')
    if xr_info:
        if xr_info['loader'] is not None:
            shutil.copy2(xr_info['loader'], stage / 'openxr_loader.dll')
        if xr_info['license'] is not None:
            shutil.copy2(xr_info['license'], stage / 'LICENSE-OpenXR.txt')
    if package:
        archive_name = f'ExCamera-{ver}-openxr.zip' if xr_info else f'ExCamera-{ver}-minimal.zip'
        path = stage / archive_name
        entries = {
            'ExanimaFirstPerson.dll': (stage / 'ExanimaFirstPerson.dll').read_bytes(),
            'ExCamera.exe': (stage / 'ExCamera.exe').read_bytes(),
            'manifest.toml': (stage / 'manifest.toml').read_bytes(),
            'README.txt': (ROOT / 'docs/PLAYER_README.txt').read_bytes(),
            'LICENSE.txt': (ROOT / 'LICENSE').read_bytes(),
            'LICENSE-MinHook.txt': (SRC / 'vendor/minhook/LICENSE.txt').read_bytes(),
        }
        if xr_info:
            entries['openxr_loader.dll'] = (stage / 'openxr_loader.dll').read_bytes()
            if xr_info['license'] is not None:
                entries['LICENSE-OpenXR.txt'] = (stage / 'LICENSE-OpenXR.txt').read_bytes()
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name, data in entries.items():
                z.writestr(name, data)
        with zipfile.ZipFile(path) as z:
            if (
                len(z.namelist()) != len(entries)
                or set(z.namelist()) != set(entries)
                or z.testzip() is not None
            ):
                raise ValueError('Staged runtime ZIP failed validation')
            if any(not z.getinfo(name).file_size for name in entries):
                raise ValueError('Staged runtime ZIP contains an empty file')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--package', action='store_true', help='Build and create runtime release ZIP')
    p.add_argument('--dev', action='store_true', help='Include dev automation (never for release packages)')
    p.add_argument(
        '--openxr-sdk',
        type=Path,
        help='Compile the XR integration with this OpenXR SDK (stages openxr_loader.dll; packages as *-openxr.zip)',
    )
    p.add_argument(
        '--wx',
        '--warnings-as-errors',
        dest='wx',
        action='store_true',
        help='Treat C++ compiler warnings as errors (/WX)',
    )
    args = p.parse_args()
    if args.dev and args.package:
        p.error('--dev builds are never packaged')
    xr_info = resolve_openxr_sdk(args.openxr_sdk, require_loader=args.package)
    ver = read_define(SRC / 'build_identity.h', 'FP_BUILD')
    names = ['ExanimaFirstPerson.dll', 'ExCamera.exe', 'manifest.toml']
    if xr_info:
        if xr_info['loader'] is not None:
            names.append('openxr_loader.dll')
        if xr_info['license'] is not None:
            names.append('LICENSE-OpenXR.txt')
    if args.package:
        names.append(f'ExCamera-{ver}-openxr.zip' if xr_info else f'ExCamera-{ver}-minimal.zip')
    staged_publish(
        OUT,
        names,
        lambda stage: build_runtime(stage, ver, args.package, args.dev, args.openxr_sdk, args.wx),
    )
    print('BUILT', ver, OUT / 'ExanimaFirstPerson.dll', OUT / 'ExCamera.exe')
    if args.package:
        print('PACKAGE', OUT / names[-1])
    else:
        print('No ZIP was refreshed; any existing package is from a previous build.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
