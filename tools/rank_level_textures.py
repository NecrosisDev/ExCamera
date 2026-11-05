"""Rank the exported level textures by how much they would gain from improvement, from the game's own data.

    python tools/rank_level_textures.py [path/to/Exanima] [export-dir]

Reads the level files (Resource.rpk *.rfc, one entry at a time) and the export made by export_level_textures.py
(index.csv and its PNGs). Writes improvement.csv and improvement-report.html into the export folder. Read-only on
the game folder; nothing here is committed but this script.

Mesh layout, from the game's own scene writer (so the reading matches what the game wrote):
  * node chunk 0x3D03 (0x1CD320): header from 0x1CC690 -- name[16], u32 flags, u32 flags2, u32 parent id,
    u32, 12 floats (3x4 transform), then two float3 (bounds); then the vertex streams and chunk 0x3DC0.
  * vertex streams: [u32 flag][u16 bits][u16 format][u32 last index] then (last+1) elements; bits 1 position
    (format 0xF3, 3 floats), 0x10 texcoord (0xF2, 2 floats).
  * chunk 0x3DC0 (0x1CD990): u32 0, u32 vertex count - 1, u32 material count - 1; per material u32 0, 8 bytes,
    name[16]; u32 stream count - 1; per stream u32 stream bits then one u16 index per vertex (u32 when the
    vertex count reaches 0xFFFF); sub-chunk 0x3DC1: u32 0, u32 triangle count - 1, 3 u16 vertex indices per
    triangle (12 bytes per triangle when large), then per material 8 bytes: first and last triangle.
World size: 1 unit = 1 cm. The node's own 3x4 scale is applied; parent transforms are not (labelled).
"""
from __future__ import annotations

import csv
import html
import math
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_level_textures import ACTOR_FILE, Pack, material_textures  # noqa: E402

NODE, FACES, TRIS = 0x3D03, 0x3DC0, 0x3DC1


def streams_before(d: bytes, start: int, end: int):
    """The vertex streams that end exactly at `end`, searched from `start`; {bits: array}."""
    for at in range(start, end - 12):
        flag, bits, fmt, last = struct.unpack_from('<IHHI', d, at)
        if bits != 1 or fmt != 0xF3 or flag != 1:
            continue
        out, p = {}, at
        while p < end:
            flag, bits, fmt, last = struct.unpack_from('<IHHI', d, p)
            n, kind = fmt & 0xF, fmt >> 4
            if kind != 0xF or n not in (1, 2, 3, 4) or last > 10 ** 6:
                break
            out[bits] = np.frombuffer(d, np.float32, (last + 1) * n, p + 12).reshape(-1, n)
            p += 12 + (last + 1) * n * 4
        if p == end and 1 in out:
            return out
    return None


def meshes(d: bytes):
    """(material name, triangle positions (n,3,3), triangle texcoords (n,3,2)) per material of each mesh."""
    for m in re.finditer(struct.pack('<I', FACES), d):
        c = m.start()
        try:
            size = struct.unpack_from('<I', d, c + 4)[0]
            zero, vlast, mlast = struct.unpack_from('<III', d, c + 8)
            if zero or vlast > 10 ** 6 or mlast > 255 or c + 8 + size > len(d):
                continue
            at = c + 20
            names = []
            for _ in range(mlast + 1):
                names.append(d[at + 12:at + 28].split(b'\0')[0].decode('latin1'))
                at += 28
            slast = struct.unpack_from('<I', d, at)[0]
            at += 4
            wide = vlast + 1 >= 0xFFFF
            index = {}
            for _ in range(slast + 1):
                sid = struct.unpack_from('<I', d, at)[0]
                at += 4
                index[sid & 0xFFFF] = np.frombuffer(d, np.uint32 if wide else np.uint16, vlast + 1, at).astype(np.int64)
                at += (vlast + 1) * (4 if wide else 2)
            cid, csz = struct.unpack_from('<II', d, at)
            if cid != TRIS:
                continue
            z, tlast = struct.unpack_from('<II', d, at + 8)
            tri = np.frombuffer(d, np.uint32 if wide else np.uint16, (tlast + 1) * 3, at + 16).astype(np.int64).reshape(-1, 3)
            ranges_at = at + 16 + (tlast + 1) * 3 * (4 if wide else 2)
            ranges = [struct.unpack_from('<II', d, ranges_at + 8 * i) for i in range(mlast + 1)]
        except struct.error:
            continue
        # The node holding this mesh: the nearest 0x3D03 header before it whose chunk covers it.
        node = None
        for n in re.finditer(struct.pack('<I', NODE), d[max(0, c - 0x400000):c]):
            pos = max(0, c - 0x400000) + n.start()
            nsize = struct.unpack_from('<I', d, pos + 4)[0]
            if pos + 8 + nsize >= c:
                node = pos
        if node is None:
            continue
        basis = np.array(struct.unpack_from('<12f', d, node + 8 + 32), np.float32)
        scale = abs(np.linalg.det(basis[:9].reshape(3, 3))) ** (1 / 3) if np.all(np.isfinite(basis)) else 1.0
        streams = streams_before(d, node + 8 + 32 + 48 + 24, c)
        if not streams or 0x10 not in streams or 1 not in index or 0x10 not in index:
            continue
        pos, uv = streams[1][:, :3] * (scale if 0.01 < scale < 100 else 1.0), streams[0x10][:, :2]
        if index[1].max() >= len(pos) or index[0x10].max() >= len(uv) or tri.max() > vlast:
            continue
        for (first, last), name in zip(ranges, names):
            t = tri[first:last + 1]
            if len(t):
                yield name, pos[index[1][t]], uv[index[0x10][t]]


def areas(tri: np.ndarray) -> np.ndarray:
    if tri.shape[2] == 2:
        tri = np.concatenate([tri, np.zeros(tri.shape[:2] + (1,), tri.dtype)], 2)
    return np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1) / 2


def block_damage(img: np.ndarray) -> float:
    """BC block edges: mean step across 4x4 block boundaries over the mean step inside blocks (1 = none)."""
    g = img.astype(np.float32)
    if g.ndim == 3:
        g = g[:, :, :3].mean(2)
    dx = np.abs(np.diff(g, axis=1))
    cols = np.arange(dx.shape[1])
    across, inside = dx[:, cols % 4 == 3], dx[:, cols % 4 != 3]
    live = g[:, :-1] > 8
    a, i = across[live[:, cols % 4 == 3]], inside[live[:, cols % 4 != 3]]
    return float(a.mean() / max(i.mean(), 1e-3)) if a.size and i.size else 1.0


def main(argv: list[str]) -> int:
    game = Path(argv[1]) if len(argv) > 1 else Path(r'D:\SteamLibrary\steamapps\common\Exanima')
    out = Path(argv[2]) if len(argv) > 2 else Path(r'E:\ExCamera-playtest\textures\level')
    rows = {r['name'].lower(): r for r in csv.DictReader(open(out / 'index.csv', encoding='utf-8'))}
    resource = Pack(game / 'Resource.rpk')
    # Material name -> textures (per level file, as the records give them).
    density = defaultdict(list)   # texture -> [(texels per cm, world area cm2)]
    material_maps = defaultdict(set)
    seen_files = set()
    for key, (name, offset, size) in sorted(resource.entries.items()):
        if not key.endswith('.rfc') or ACTOR_FILE.search(key):
            continue
        d = resource.read(key)
        textures = defaultdict(list)
        for material, slot, texture in material_textures(d):
            textures[material.lower()].append((slot, texture.lower()))
            material_maps[material.lower()].add((slot, texture.lower()))
        for material, pos, uv in meshes(d):
            maps = textures.get(material.lower())
            if not maps:
                continue
            world, uvs = areas(pos.astype(np.float64)), areas(uv.astype(np.float64))
            keep = (world > 1e-2) & (uvs > 0)
            if not keep.any():
                continue
            seen_files.add(name)
            for slot, texture in maps:
                row = rows.get(texture)
                if not row or row.get('status') != 'exported':
                    continue
                w, h = int(row['width']), int(row['height'])
                tpc = np.sqrt(uvs[keep] * w * h / world[keep])
                density[texture].extend(zip(tpc.tolist(), world[keep].tolist()))
    results = []
    for key, row in rows.items():
        if row.get('status') != 'exported':
            continue
        w, h = int(row['width']), int(row['height'])
        samples = density.get(key, [])
        median = worst = None
        area = 0.0
        if samples:
            s = np.array(samples)
            order = np.argsort(s[:, 0])
            cum = np.cumsum(s[order, 1]) / s[:, 1].sum()
            median = float(s[order][np.searchsorted(cum, 0.5), 0])
            worst = float(s[order][np.searchsorted(cum, 0.1), 0])
            area = float(s[:, 1].sum()) / 1e4  # m2
        issues = []
        img = np.asarray(Image.open(out / row['png']))
        damage = block_damage(img) if row['format'] == 'DXT1' and 'DifTex' in row['map_type'] else None
        if median is not None and median < 2.0:
            issues.append(f'low density {median:.2f} texels/cm')
        if damage and damage > 1.6:
            issues.append(f'BC1 block edges x{damage:.1f}')
        # Within a material: maps much smaller than its colour map, or no normal/gloss where colour exists.
        for material in [m for m, maps in material_maps.items() if any(t == key for _, t in maps)]:
            maps = material_maps[material]
            colour = [t for s, t in maps if s == 0x2 and t in rows and rows[t].get('width')]
            if colour and key != colour[0] and rows[key].get('width'):
                cw = int(rows[colour[0]]['width']) * int(rows[colour[0]]['height'])
                if w * h * 4 <= cw:
                    issues.append(f'{w}x{h} under its colour map in {material}')
        results.append({'name': row['name'], 'type': row['map_type'], 'size': f'{w}x{h}', 'format': row['format'],
                        'texels_per_cm_median': round(median, 2) if median is not None else '',
                        'texels_per_cm_worst10': round(worst, 2) if worst is not None else '',
                        'area_m2': round(area, 1), 'block_edges': round(damage, 2) if damage else '',
                        'issues': '; '.join(sorted(set(issues))), 'levels': row['levels'], 'png': row['png']})
    for r in results:
        dens = r['texels_per_cm_median'] if r['texels_per_cm_median'] != '' else 99
        # What is seen most: colour, then normal, then the rest (a height map only drives relief).
        weight = 1.0 if 'DifTex' in r['type'] else 0.8 if 'NrmTex' in r['type'] else 0.5 if             ('GlsTex' in r['type'] or 'SpcTex' in r['type']) else 0.35
        r['score'] = round(weight * ((r['area_m2'] ** 0.5) / max(dens, 0.05) + (3 if r['issues'] else 0)), 2)
        tips = []
        if dens < 1.0:
            tips.append('4x upscale')
        elif dens < 2.0:
            tips.append('2x upscale')
        if r['block_edges'] != '' and r['block_edges'] > 1.6:
            tips.append('recompress BC7 (from a clean upscale)')
        if 'NrmTex' in r['type'] and dens < 2.0:
            tips.append('re-derive normal from upscaled height/colour')
        r['suggested'] = ', '.join(tips)
    results.sort(key=lambda r: -r['score'])
    fields = ['name', 'type', 'size', 'format', 'texels_per_cm_median', 'texels_per_cm_worst10', 'area_m2', 'block_edges',
              'issues', 'suggested', 'score', 'levels', 'png']
    with open(out / 'improvement.csv', 'w', newline='', encoding='utf-8') as f:
        wr = csv.DictWriter(f, fields)
        wr.writeheader()
        wr.writerows(results)
    body = []
    for i, r in enumerate(results):
        thumb = f'<img loading="lazy" src="{html.escape(r["png"])}">' if i < 30 else ''
        body.append('<tr>' + f'<td>{thumb}</td>' + ''.join(f'<td>{html.escape(str(r[k]))}</td>' for k in fields[:-1]) + '</tr>')
    (out / 'improvement-report.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Level texture improvements</title><style>'
        'body{font:12px sans-serif;background:#1d1d1d;color:#ddd}table{border-collapse:collapse}td,th{border:1px solid #444;'
        'padding:3px;vertical-align:top}th{cursor:pointer;background:#333}img{width:128px;height:128px;object-fit:contain;'
        'background:#555}</style>'
        f'<p>{len(results)} textures; {len(density)} measured on {len(seen_files)} level files. Click a header to sort. '
        'Texel density: texture texels per world centimetre over the level meshes using it (area-weighted median, '
        'and the 10% worst). Area: world surface the texture covers in the level files (each file counted once).</p>'
        '<table id=t><thead><tr><th></th>' + ''.join(f'<th>{k}</th>' for k in fields[:-1]) + '</tr></thead><tbody>' +
        ''.join(body) + '</tbody></table><script>'
        'document.querySelectorAll("th").forEach((th,i)=>th.onclick=()=>{const b=document.querySelector("tbody");'
        'const r=[...b.rows];const n=x=>{const v=parseFloat(x);return isNaN(v)?x:v};const d=th.dataset.d=th.dataset.d=="1"?"0":"1";'
        'r.sort((a,c)=>{const x=n(a.cells[i].innerText),y=n(c.cells[i].innerText);return (x>y?1:x<y?-1:0)*(d=="1"?-1:1)});'
        'r.forEach(x=>b.appendChild(x))})</script>', encoding='utf-8')
    print(f'{len(results)} ranked; {len(density)} with density from {len(seen_files)} level files')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
