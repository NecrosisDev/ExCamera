"""Check replacement .dds files the way the mod's hook will, then copy the ones it accepts into a build.

    python tools/install_texture_replacements.py <dds-folder> <build>/textures [--game D:/.../Exanima] [--check-only]

For each <Name>.dds: the texture exists in Textures.rpk; its name key (name + ".rfi") fits the 16 bytes the
game's key holds; the block format is the one the game's own format table gives the texture's format word
(src/texture_formats.h, as mod/texture_replace.inl resolves it at runtime); width and height are 4..16384; the mip
count is the full chain to 1x1; the file holds every level. Refuses to copy while Exanima is running. Never
touches anything else in the build.
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_level_textures import Pack  # noqa: E402
from upscale_level_textures import block_for  # noqa: E402

FOURCC = {b'DXT1': 1, b'DXT5': 3, b'ATI1': 4, b'BC4U': 4, b'ATI2': 5, b'BC5U': 5}


def chain(width: int, height: int, block: int):
    size, levels = 0, 0
    while True:
        size += max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * (8 if block in (1, 4) else 16)
        levels += 1
        if width <= 1 and height <= 1:
            return size, levels
        width, height = max(1, width // 2), max(1, height // 2)


def check(path: Path, textures: Pack) -> str | None:
    name = path.stem
    if name.lower() not in textures.entries:
        return 'not a texture in Textures.rpk'
    if len(name) + 4 > 16:
        return 'name key over 16 bytes'
    word = struct.unpack('<8I', textures.read(name.lower(), 32))[4]
    wanted = block_for(word)
    if not wanted:
        return f'the game uploads it (format 0x{word:08X}) in a format the hook does not replace'
    data = path.read_bytes()
    if len(data) < 128 or data[:4] != b'DDS ':
        return 'not a DDS file'
    height, width = struct.unpack_from('<II', data, 12)
    mips = struct.unpack_from('<I', data, 28)[0]
    got = FOURCC.get(data[84:88], 0)
    if got != wanted:
        return f'block format {data[84:88]!r}, the game uploads it as BC{wanted}'
    if not (4 <= width <= 16384 and 4 <= height <= 16384):
        return 'size out of range'
    size, levels = chain(width, height, wanted)
    if mips != levels:
        return f'{mips} mip levels, a full chain is {levels}'
    if 128 + size > len(data):
        return 'file shorter than its levels'
    return None


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith('--')]
    check_only = '--check-only' in argv
    game = Path(next((argv[i + 1] for i, a in enumerate(argv) if a == '--game'), r'D:\SteamLibrary\steamapps\common\Exanima'))
    if '--game' in argv:
        args.remove(str(game))
    source, target = Path(args[0]), Path(args[1])
    textures = Pack(game / 'Textures.rpk')
    ok, refused = [], []
    for path in sorted(source.glob('*.dds')):
        why = check(path, textures)
        (refused if why else ok).append((path, why))
    for path, why in refused:
        print(f'REFUSED {path.name}: {why}')
    print(f'{len(ok)} accepted, {len(refused)} refused')
    if check_only:
        return 0
    running = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq Exanima.exe', '/NH'], capture_output=True, text=True).stdout
    if 'Exanima.exe' in running:
        print('Exanima is running: nothing copied')
        return 2
    target.mkdir(parents=True, exist_ok=True)
    for path, _ in ok:
        shutil.copy2(path, target / path.name)
    print(f'{len(ok)} copied to {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
