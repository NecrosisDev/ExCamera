"""Decode the game's own shader sources from an installed Exanima, for render reviews.

Exanima keeps its GLSL in Resource.rpk -> shading.flb. Both are the same pack format: a 4-byte tag, a uint32
table size, then 32-byte entries (16-byte name, uint32 offset, uint32 size, 8 bytes unused) and the data after
the table. Each shader body is a run of dwords rotated left by a key stream (the game's 0x1FA4D0, called from
0x1FA6F0 with seed 0x2BCBE389): k = k * 0x10A860C1, rotate by (k >> 13) & 31, or 4 when that is a multiple of 8.
The output keeps the game's template markers ({?...?}, {$...}); the engine expands them before glShaderSource.

    python tools/extract_game_shaders.py [path/to/Exanima] [out-dir]

The sources are the game's: keep the output local, never commit or package it.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def entries(pack: bytes) -> dict[str, bytes]:
    table = struct.unpack_from('<I', pack, 4)[0]
    base = 8 + table
    out = {}
    for at in range(8, 8 + table, 32):
        name = pack[at:at + 16].split(b'\0')[0].decode('latin1')
        offset, size = struct.unpack_from('<II', pack, at + 16)
        out[name] = pack[base + offset:base + offset + size]
    return out


def decode(body: bytes) -> bytes:
    key, out = 0x2BCBE389, bytearray()
    for i in range(len(body) // 4):
        key = (key * 0x10A860C1) & 0xFFFFFFFF
        turn = (key >> 13) & 31
        if turn & 7 == 0:
            turn |= 4
        word = struct.unpack_from('<I', body, i * 4)[0]
        out += struct.pack('<I', ((word << turn) | (word >> (32 - turn))) & 0xFFFFFFFF)
    return bytes(out).rstrip(b'\0')


def main(argv: list[str]) -> int:
    game = Path(argv[1]) if len(argv) > 1 else Path(r'D:\SteamLibrary\steamapps\common\Exanima')
    out = Path(argv[2]) if len(argv) > 2 else Path('game-shaders')
    library = entries(entries((game / 'Resource.rpk').read_bytes())['shading.flb'])
    out.mkdir(parents=True, exist_ok=True)
    for name, body in library.items():
        (out / f'{name}.glsl').write_bytes(decode(body))
    print(f'{len(library)} shaders written to {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
