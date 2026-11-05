"""Repackage the OpenXR build as a private VR tester ZIP (standard library only).

    python build.py --openxr-sdk <sdk> --package --wx
    python tools/vr_tester.py [--commit <sha>] [--build-dir build]

Reads build/ExCamera-<FP_BUILD>-openxr.zip and writes build/ExCamera-<FP_BUILD>-vr-tester.zip: the same
files plus VR-TESTER-README.txt (docs/VR-TESTER-README.txt with the version and commit filled in). The
tester ZIP is never a release asset; .github/workflows/vr-tester.yml uploads it as a workflow artifact.
"""
import argparse
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = ROOT / 'docs/VR-TESTER-README.txt'
REQUIRED = {'ExanimaFirstPerson.dll', 'ExCamera.exe', 'openxr_loader.dll', 'manifest.toml'}


def build_version():
    text = (ROOT / 'src/build_identity.h').read_text(encoding='utf-8')
    matches = re.findall(r'^#define FP_BUILD "([\w.-]+)"$', text, re.M)
    if len(matches) != 1:
        raise ValueError('Expected exactly one FP_BUILD definition')
    return matches[0]


def tester_readme(version, commit):
    text = README.read_text(encoding='utf-8')
    if '{version}' not in text or '{commit}' not in text:
        raise ValueError(f'{README.name} lost its {{version}} or {{commit}} placeholder')
    text = text.replace('{version}', version).replace('{commit}', commit or 'local')
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')  # Notepad-friendly line ends


def package(build_dir, version, commit=None):
    build_dir = Path(build_dir)
    if not re.fullmatch(r'[\w.-]+', version, flags=re.ASCII):
        raise ValueError('Unsafe package version')
    if commit is not None and not re.fullmatch(r'[0-9a-fA-F]{7,40}', commit):
        raise ValueError('The commit must be a hexadecimal git object name')
    source = build_dir / f'ExCamera-{version}-openxr.zip'
    target = build_dir / f'ExCamera-{version}-vr-tester.zip'
    if not source.is_file():
        raise FileNotFoundError(f'{source} not found; run build.py --openxr-sdk <sdk> --package first')
    with zipfile.ZipFile(source) as z:
        if z.testzip() is not None:
            raise ValueError(f'{source.name} failed its integrity check')
        names = set(z.namelist())
        missing = sorted(REQUIRED - names)
        if missing:
            raise ValueError(f'{source.name} is missing {", ".join(missing)}')
        if 'VR-TESTER-README.txt' in names:
            raise ValueError(f'{source.name} already carries a tester README')
        entries = {name: z.read(name) for name in z.namelist()}
    for name in REQUIRED:
        if not entries[name]:
            raise ValueError(f'Empty file in {source.name}: {name}')
    manifest = entries['manifest.toml'].decode('utf-8')
    if re.findall(r'^version = "([^"\r\n]+)"$', manifest, re.M) != [version]:
        raise ValueError(f'{source.name} manifest.toml does not say version {version}')
    entries['VR-TESTER-README.txt'] = tester_readme(version, commit[:12] if commit else None)
    partial = target.with_suffix('.zip.partial')
    with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    with zipfile.ZipFile(partial) as z:
        if set(z.namelist()) != set(entries) or z.testzip() is not None:
            partial.unlink()
            raise ValueError('The tester ZIP failed validation')
    partial.replace(target)
    return target


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--build-dir', default=str(ROOT / 'build'))
    p.add_argument('--commit', help='git commit the build came from, shown in the README')
    args = p.parse_args(argv)
    try:
        target = package(args.build_dir, build_version(), args.commit)
    except (OSError, ValueError) as e:
        print(f'vr_tester: {e}', file=sys.stderr)
        return 1
    print(f'Ready: {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
