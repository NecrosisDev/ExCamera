"""Export the textures Exanima's level files use, as PNG, for review and per-texture upscaling choices.

    python tools/export_level_textures.py [path/to/Exanima] [out-dir]

Default out-dir: E:\\ExCamera-playtest\\textures\\level. The images are the game's: keep them out of the repository.
Read-only on the game folder; every pack entry is read on its own (seek + read), never a whole pack.

What counts as a level texture (docs/render-assumptions.md, "Level textures"):
  * Material records in the level files (Resource.rpk *.rfc, tag 0x0000BA01, size, then the material name). The
    game builds a TMaterial (VMT 0x2FFE10) per record through its material manager (0x6E370 -> 0x6E030 ->
    0x6CC60). Each texture entry in a record is [u16 0][u16 slot bits][u32 0x38][u32 0][5 floats][u32 0x30][u32 0]
    then the file name ("Name.rfi"). The slot bits equal the renderer's sampler flags >> 16 that 0x2010C0 tests
    (DifTex 0x20000, SpcTex 0x40000, EmiTex 0x80000, NrmTex 0x200000, OpcTex 0x400000, DspTex 0x800000,
    GlsTex 0x1000000, DrtTex 0x8000000); 0x10 (the "R" reflection maps) is not among them.
  * Terrain materials in the tile tables (Resource.rpk *.ftb, "Mat <name>" then texture names without extension);
    their map type comes from the name's last letter only (not proven from code).
  * Names resolve in Textures.rpk (the same pack format the game's reader 0x6F400 checks: tag 0xAFBF0C01, table
    size, 32-byte entries of 16-byte name, offset, size).

Texture files (.rfi): 32-byte header (u32 tag 0x1D2D3DC6, width, height, u32, u32 format, u32, u32, u32 data size),
then the mip chain. The format word's top byte 0x8x is block-compressed (DXT5 when the low byte has 0x08 or the
word is a normal-map form 0x92xxxxxx/0x827BA408, else DXT1), 0x01 uncompressed; this reading of the format word
is from the files themselves (every compressed file's data size matches its mip chain), not from the game's
loader, and is labelled so in the index.
"""
from __future__ import annotations

import csv
import html
import os
import re
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path

import numpy as np

PACK_TAG = 0xAFBF0C01
RFI_TAG = 0x1D2D3DC6
MATERIAL_TAG = b'\x01\xba\x00\x00'
SLOTS = {0x2: 'DifTex', 0x4: 'SpcTex', 0x8: 'EmiTex', 0x10: 'Reflection(R, unproven)', 0x20: 'NrmTex',
         0x40: 'OpcTex', 0x80: 'DspTex', 0x100: 'GlsTex', 0x800: 'DrtTex'}
SUFFIX = {'d': 'DifTex', 'n': 'NrmTex', 's': 'SpcTex', 'g': 'GlsTex', 'e': 'EmiTex', 'o': 'OpcTex', 'h': 'DspTex',
          'r': 'Reflection(R, unproven)', 'm': 'mask(M, unproven)', 'b': 'B(unproven)'}
ACTOR_FILE = re.compile(r'(base|morph)\.rfc$|^dummyhead\.rfc$|^menuscene\.rfc$')


class Pack:
    def __init__(self, path: Path):
        self.path = path
        self.file = open(path, 'rb')
        tag, size = struct.unpack('<II', self.file.read(8))
        if tag != PACK_TAG:
            raise ValueError(f'{path.name}: not a pack')
        table = self.file.read(size)
        self.entries = {}
        for at in range(0, size, 32):
            name = table[at:at + 16].split(b'\0')[0].decode('latin1')
            offset, length = struct.unpack_from('<II', table, at + 16)
            self.entries[name.lower()] = (name, 8 + size + offset, length)

    def read(self, key: str, length: int | None = None) -> bytes:
        name, offset, size = self.entries[key]
        self.file.seek(offset)
        return self.file.read(size if length is None else min(length, size))


def material_textures(data: bytes):
    """(material name, slot bits, texture file name) for each texture entry in a level file's material records."""
    records = []
    for m in re.finditer(re.escape(MATERIAL_TAG), data):
        size = struct.unpack_from('<I', data, m.start() + 4)[0]
        name = data[m.start() + 8:m.start() + 40].split(b'\0')[0].decode('latin1', 'replace')
        records.append((m.start(), m.start() + 8 + size, name))
    for m in re.finditer(rb'([A-Za-z0-9_\-]{1,40})\.rfi\x00', data):
        at = m.start()
        if at < 0x28 or struct.unpack_from('<I', data, at - 0x24)[0] != 0x38 or \
                struct.unpack_from('<I', data, at - 8)[0] != 0x30:
            continue
        slot = struct.unpack_from('<H', data, at - 0x26)[0]
        owner = next((n for s, e, n in records if s < at < e), '')
        yield owner, slot, m.group(1).decode('latin1')


def terrain_textures(data: bytes):
    """(material name, texture name) from a tile table: 'Mat <name>' then its texture names."""
    material = ''
    for m in re.finditer(rb'[\x20-\x7e]{4,40}', data):
        text = m.group().decode('latin1')
        if text.startswith('Mat '):
            material = text
        elif material and re.fullmatch(r'[A-Za-z0-9_]+', text):
            yield material, text


def mip0_size(width: int, height: int, kind: str) -> int:
    blocks = max(1, (width + 3) // 4) * max(1, (height + 3) // 4)
    return {'DXT1': blocks * 8, 'BC4': blocks * 8, 'DXT5': blocks * 16, 'BC5': blocks * 16, 'RGBA8': width * height * 4, 'RGB8': width * height * 3,
            'R8': width * height}[kind]


# The format word's second byte names the channels (0xC6 RGB, 0x42 one channel, 0x84 two, 0xE6/0xA4 with alpha);
# its top bit marks block compression: RGB -> DXT1 (BC1), one channel -> BC4, two -> BC5 (a normal map's X and Y),
# with alpha -> DXT5 (BC3). Read from the files (each decodes to a coherent image), not from the game's loader.
def format_of(word: int) -> str | None:
    channels = (word >> 8) & 0xFF
    if word >> 31:
        return {0xC6: 'DXT1', 0x42: 'BC4', 0x84: 'BC5', 0xE6: 'DXT5', 0xA4: 'DXT5'}.get(channels)
    if (word >> 24) in (0x01, 0x11):
        return {0xC6: 'RGB8', 0xE6: 'RGBA8', 0x42: 'R8', 0x20: 'R8'}.get(channels)
    return None


def colours565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_colour_blocks(blocks: np.ndarray, dxt1: bool) -> np.ndarray:
    """blocks: (n, 8) uint8 colour halves. Returns (n, 16, 4) RGBA."""
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)
    p0, p1 = colours565(c0), colours565(c1)
    four = (c0 > c1) | (not dxt1)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], 1)  # n,4,3
    alpha = np.full((len(blocks), 4), 255, np.int32)
    if dxt1:
        alpha[:, 3] = np.where(four, 255, 0)
    bits = blocks[:, 4:8].astype(np.uint32)
    word = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    idx = ((word[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3).astype(np.int64)
    rgb = np.take_along_axis(palette, idx[:, :, None].repeat(3, 2), 1)
    a = np.take_along_axis(alpha, idx, 1)
    return np.concatenate([rgb, a[:, :, None]], 2)


def decode_alpha_blocks(blocks: np.ndarray) -> np.ndarray:
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    table = np.zeros((len(blocks), 8), np.int32)
    table[:, 0], table[:, 1] = a0, a1
    eight = a0 > a1
    for i in range(1, 7):
        table[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        table[:, i + 1] = np.where(eight, table[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    table[:, 6] = np.where(eight, table[:, 6], 0)
    table[:, 7] = np.where(eight, table[:, 7], 255)
    raw = blocks[:, 2:8].astype(np.uint64)
    word = sum(raw[:, k] << np.uint64(8 * k) for k in range(6))
    idx = ((word[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
    return np.take_along_axis(table, idx, 1)


def decode(data: bytes, width: int, height: int, kind: str) -> np.ndarray:
    if kind in ('DXT1', 'DXT5', 'BC4', 'BC5'):
        bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
        size = 8 if kind in ('DXT1', 'BC4') else 16
        raw = np.frombuffer(data, np.uint8, bw * bh * size).reshape(-1, size)
        if kind == 'BC4':
            v = decode_alpha_blocks(raw).astype(np.uint8)
            return v.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)[:height, :width, None].copy()
        if kind == 'BC5':
            # X and Y as stored; Z rebuilt (unit length) so the map reads as a normal map.
            x = decode_alpha_blocks(raw[:, :8]).astype(np.float32)
            y = decode_alpha_blocks(raw[:, 8:]).astype(np.float32)
            nx, ny = x / 127.5 - 1.0, y / 127.5 - 1.0
            z = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 0.0, 1.0)) * 127.5 + 127.5
            px = np.stack([x, y, z, np.full_like(x, 255.0)], -1).astype(np.uint8)
        elif kind == 'DXT1':
            px = decode_colour_blocks(raw, True)
        else:
            px = decode_colour_blocks(raw[:, 8:], False)
            px[:, :, 3] = decode_alpha_blocks(raw[:, :8])
        img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
        return img[:height, :width].astype(np.uint8)
    channels = {'RGBA8': 4, 'RGB8': 3, 'R8': 1}[kind]
    img = np.frombuffer(data, np.uint8, width * height * channels).reshape(height, width, channels)
    return img.copy()


def write_png(path: Path, img: np.ndarray):
    height, width = img.shape[:2]
    channels = 1 if img.ndim == 2 else img.shape[2]
    colour = {1: 0, 3: 2, 4: 6}[channels]
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(height))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, colour, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main(argv: list[str]) -> int:
    game = Path(argv[1]) if len(argv) > 1 else Path(r'D:\SteamLibrary\steamapps\common\Exanima')
    out = Path(argv[2]) if len(argv) > 2 else Path(r'E:\ExCamera-playtest\textures\level')
    resource, textures = Pack(game / 'Resource.rpk'), Pack(game / 'Textures.rpk')
    uses = defaultdict(lambda: {'types': set(), 'levels': set(), 'materials': set(), 'source': set(), 'display': ''})
    for key, (name, offset, size) in sorted(resource.entries.items()):
        if key.endswith('.rfc') and not ACTOR_FILE.search(key):
            for material, slot, texture in material_textures(resource.read(key)):
                u = uses[texture.lower()]
                u['display'] = u['display'] or texture  # the level file's own spelling
                u['types'].add(SLOTS.get(slot, f'slot 0x{slot:X}'))
                u['levels'].add(name)
                u['materials'].add(material)
                u['source'].add('level material record (slot bits)')
        elif key.endswith('.ftb'):
            for material, texture in terrain_textures(resource.read(key)):
                if texture.lower() not in textures.entries:
                    continue
                u = uses[texture.lower()]
                u['display'] = u['display'] or texture
                u['types'].add(SUFFIX.get(texture[-1].lower(), 'unknown') + ' (from name)')
                u['levels'].add(name)
                u['materials'].add(material)
                u['source'].add('terrain tile table (name suffix)')
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for key in sorted(uses):
        u = uses[key]
        if key not in textures.entries:
            rows.append({'name': u['display'] or key, 'levels': ' '.join(sorted(u['levels'])),
                         'materials': ' | '.join(sorted(m for m in u['materials'] if m)),
                         'status': 'referenced by the level files but not shipped in any pack'})
            continue
        packName, offset, size = textures.entries[key]
        name = u['display'] or packName
        head = textures.read(key, 32)
        tag, width, height, _, word, _, _, data_size = struct.unpack('<8I', head)
        kind = format_of(word) if tag == RFI_TAG else None
        mapType = sorted(u['types'])[0].split(' ')[0].split('(')[0]
        row = {'name': name, 'map_type': '; '.join(sorted(u['types'])), 'width': width, 'height': height,
               'format': kind or f'unknown 0x{word:08X}', 'format_word': f'0x{word:08X}',
               'levels': ' '.join(sorted(u['levels'])), 'materials': ' | '.join(sorted(m for m in u['materials'] if m)),
               'evidence': '; '.join(sorted(u['source'])), 'pack': 'Textures.rpk', 'offset': offset, 'size': size,
               'png': '', 'status': ''}
        if tag != RFI_TAG or not kind or 32 + mip0_size(width, height, kind) > size:
            row['status'] = 'not decoded'
        else:
            data = textures.read(key, 32 + mip0_size(width, height, kind))[32:]
            folder = out / mapType
            folder.mkdir(exist_ok=True)
            png = folder / f'{name}.png'
            img = decode(data, width, height, kind)
            write_png(png, img[:, :, 0] if img.shape[2] == 1 else img)
            row['png'] = str(png.relative_to(out)).replace('\\', '/')
            row['status'] = 'exported'
        rows.append(row)
    fields = ['name', 'map_type', 'width', 'height', 'format', 'format_word', 'levels', 'materials', 'evidence', 'pack',
              'offset', 'size', 'png', 'status']
    with open(out / 'index.csv', 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fields, extrasaction='ignore')
        w.writeheader()
        for row in rows:
            w.writerow(row)
    cards = []
    for row in rows:
        if row.get('status') != 'exported':
            continue
        cards.append(f'<figure><img loading="lazy" src="{html.escape(row["png"])}"><figcaption><b>{html.escape(row["name"])}</b><br>'
                     f'{html.escape(row["map_type"])}<br>{row["width"]}x{row["height"]} {row["format"]}<br>'
                     f'<small>{html.escape(row["levels"])}</small></figcaption></figure>')
    (out / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Exanima level textures</title><style>'
        'body{font:13px sans-serif;background:#222;color:#ddd}figure{display:inline-block;width:220px;margin:6px;'
        'vertical-align:top}img{width:216px;height:216px;object-fit:contain;background:#555}small{color:#999}</style>'
        f'<p>{len(cards)} textures. See index.csv for pack offsets and which level files use each.</p>' + ''.join(cards),
        encoding='utf-8')
    exported = [r for r in rows if r.get('status') == 'exported']
    print(f'{len(exported)} exported, {len(rows) - len(exported)} not, to {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
