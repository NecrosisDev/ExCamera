"""Standard-library release checks and commit notes; never builds or publishes."""
import argparse
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TAG_RE = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")
EXPECTED_FILES = {'ExCamera.exe', 'ExanimaFirstPerson.dll', 'manifest.toml', 'LICENSE.txt', 'LICENSE-MinHook.txt'}


def version_from_tag(tag):
    match = TAG_RE.fullmatch(tag)
    if not match or any(part.isdigit() and len(part) > 1 and part.startswith('0')
                        for part in (match.group(4) or '').split('.')):
        raise ValueError('Expected vX.Y.Z or vX.Y.Z-prerelease (SemVer, no build metadata)')
    return tag[1:]


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True, encoding='utf-8').strip()


def release_context():
    tag = os.environ.get('GITHUB_REF_NAME', '')
    version = version_from_tag(tag)
    if os.environ.get('GITHUB_REF') != 'refs/tags/' + tag:
        raise ValueError('Release requires a tag push')
    if git('rev-parse', 'HEAD') != git('rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}'):
        raise ValueError('Checked-out commit does not match the release tag')
    return tag, version


def prepare():
    tag, version = release_context()
    header = ROOT / 'src/build_identity.h'
    source = header.read_text(encoding='utf-8')
    source, count = re.subn(r'^#define FP_BUILD "[^"\r\n]+"$',
                            '#define FP_BUILD "' + version + '"', source, flags=re.M)
    if count != 1:
        raise ValueError('Expected exactly one FP_BUILD definition')
    header.write_text(source, encoding='utf-8')
    # This only modifies the ephemeral CI checkout, never a source commit/tag.
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write(f'version={version}\ntag={tag}\ncommit={git("rev-parse", "HEAD")}\nasset=build/ExCamera-{version}-minimal.zip\n')
    print(f'Prepared {tag}; FP_REVISION is unchanged')


def verify_package(version):
    if not re.fullmatch(r'[\w.-]+', version, flags=re.ASCII):
        raise ValueError('Unsafe package version')
    packages = list((ROOT / 'build').glob('ExCamera-*-minimal.zip'))
    expected = ROOT / 'build' / f'ExCamera-{version}-minimal.zip'
    if packages != [expected]:
        raise ValueError(f'Expected only {expected.name}, found {[p.name for p in packages]}')
    with zipfile.ZipFile(expected) as archive:
        if len(archive.namelist()) != len(EXPECTED_FILES) or set(archive.namelist()) != EXPECTED_FILES:
            raise ValueError('Release ZIP contains missing or unexpected files')
        if archive.testzip() is not None:
            raise ValueError('Release ZIP failed its integrity check')
        for name in EXPECTED_FILES:
            if not archive.getinfo(name).file_size:
                raise ValueError(f'Empty release file: {name}')
    print(f'Ready: {expected}')


def header_version():
    source = (ROOT / 'src/build_identity.h').read_text(encoding='utf-8')
    versions = re.findall(r'^#define FP_BUILD "([^"\r\n]+)"$', source, re.M)
    if len(versions) != 1:
        raise ValueError('Expected exactly one FP_BUILD definition')
    return versions[0]


def markdown_text(text):
    text = re.sub(r'([\\`*_{}\[\]()#+.!|~-])', r'\\\1', text)
    return text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('@', '&#64;')


def notes(path):
    tag, _ = release_context()
    head_commit = git('rev-parse', 'HEAD')
    repository = os.environ.get('GITHUB_REPOSITORY', '')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Expected GitHub owner/repository')
    candidates = []
    for candidate in git('tag', '--merged', 'HEAD').splitlines():
        try:
            version_from_tag(candidate)
        except ValueError:
            continue
        if candidate != tag:
            candidates.append(candidate)
    # Use git describe's reachable-tag selection, not a lexical version sort.
    # Its default candidate search is bounded; do not promise a global closest tag.
    previous = git('describe', '--tags', '--abbrev=0', *[arg for candidate in candidates
                   for arg in ('--match', candidate)], 'HEAD') if candidates else None
    tag_ref = 'refs/tags/' + tag
    revision_range = 'refs/tags/' + previous + '..' + tag_ref if previous else tag_ref
    records = git('log', '--reverse', '--format=%H%x09%s', revision_range, '--').splitlines()
    url = 'https://github.com/' + repository
    lines = [f'# ExCamera {tag}', '',
             'Windows x64 runtime. Extract the ZIP and keep the launcher, DLL, and license files together.', '',
             '## Commit changelog', '',
             f'Commit subjects since {previous} (including direct commits and merges):' if previous else
             'Commit subjects from the initial history (including direct commits and merges):', '']
    for record in records:
        commit, subject = record.split('\t', 1)
        lines.append(f'- {markdown_text(subject)} ([{commit[:12]}]({url}/commit/{commit}))')
    if not records:
        lines.append('- No new commits since the previous tag.')
    lines += ['', 'These are commit subjects, not independently verified feature or compatibility claims.', '']
    if previous:
        lines.append(f'[Full comparison]({url}/compare/tags/{quote(previous, safe="")}...tags/{quote(tag, safe="")})')
    else:
        lines.append(f'[Full history]({url}/commits/{head_commit})')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'Wrote {len(records)} commit entries to {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'verify', 'notes'))
    parser.add_argument('--local', action='store_true', help='Verify the checked-in FP_BUILD package instead of a release tag')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/release-notes.md')
    args = parser.parse_args()
    if args.local and args.command != 'verify':
        parser.error('--local only applies to verify')
    if args.command == 'prepare':
        prepare()
    elif args.command == 'verify':
        version = header_version() if args.local else release_context()[1]
        if header_version() != version:
            raise ValueError('FP_BUILD does not match release tag')
        verify_package(version)
    else:
        notes(args.output)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, subprocess.CalledProcessError, OSError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error))
