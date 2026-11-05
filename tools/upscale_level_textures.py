"""Upscale the approved level textures and write them as replacement .dds files for the mod's texture hook.

    python tools/upscale_level_textures.py --model RealESRGAN_x4plus.pth [--game D:/.../Exanima]
        [--export E:/ExCamera-playtest/textures/level] [--out E:/ExCamera-playtest/textures/level-upscaled]
        [--dry-run]   (Lanczos instead of the model: checks the pipeline only; never install its output)

Reads each texture's own pixels from Textures.rpk (through export_level_textures.Pack/decode, one entry at a time),
upscales with Real-ESRGAN x4plus (spandrel loads it; run through onnxruntime on the GPU when available, else torch),
with wrap-around padding so tiling textures stay seamless, and writes:
  * <out>/dds/<Name>.dds  -- the block format the game's own format table gives the texture's format word
    (src/texture_formats.h; BC1/BC3/BC4/BC5), full mip chain: what the hook (mod/texture_replace.inl) accepts.
  * <out>/png/<Name>.png  -- preview of the upscaled level 0.
  * <out>/compare.html    -- original vs upscaled, zoomed crops, and a 2x2 tiling of each for the seam check.
  * <out>/upscale.csv     -- per texture: group, scale, sizes, block format, seam score before/after, refusals.
Normal maps: X and Y upscaled as two channels and Z rebuilt to unit length (never derived from colour).
Gloss, height and other one-channel maps: upscaled as one channel (replicated to RGB for the model).
Images and DDS files are the game's (derived): never commit them.
"""
from __future__ import annotations

import argparse
import csv
import html
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_level_textures import Pack, decode, format_of, mip0_size  # noqa: E402

# Approved by Hunter (2026-11-05) from improvement-report.html: (group, scale, name pattern over the export).
GROUPS = [
    ('4x terrain rock', 4, r'TrnRk02[DNH]|TrnRck02GN'),
    ('4x wall height maps', 4, r'UBBPA?Wl\d\dH|UW01WallA\d\dH'),
    ('2x hex floor', 2, r'UB6FlrB01[DNG]'),
    ('2x sewer trim', 2, r'USwTR01D'),
    ('2x rocks gloss/normal', 2, r'Rocks_ALA\d\d[GN]'),
    ('2x arena inner wall gloss', 2, r'XAA04WaIn01G'),
    ('2x exposure walls', 2, r'APOAWl\d\d[A-Z]|UB6BW\d\d[A-Z]|UB6TA\d\d[A-Z]'),
]
INTERNAL_BLOCK = {0x83F0: 1, 0x83F1: 1, 0x83F3: 3, 0x8C70: 4, 0x8DBB: 4, 0x8DBD: 5}
S3TC = {0x813BC600: 0x83F0, 0x813BE601: 0x83F1, 0x823BC600: 0x83F0, 0x817BE608: 0x83F3, 0x827BE608: 0x83F3,
        0x827BA408: 0x83F3}
LATC = {0x813B4200: 0x8C70, 0x813B2008: 0x8C70, 0x817B6208: 0x8C72}
RGTC2 = {0x817B8400: 0x8DBD, 0x927B8400: 0x8DBD}


def block_for(word: int) -> int:
    """As the game's table resolves it (src/texture_formats.h): 1/3/4/5, 0 when the hook would refuse it."""
    def matches(entry):
        return (entry & 0x800CFFFF) == (word & 0x800CFFFF) if word & 0x80100000 == 0x100000 else entry == word
    for table in (S3TC, LATC, RGTC2):
        for entry, internal in table.items():
            if matches(entry):
                return INTERNAL_BLOCK.get(internal, 0)
    return 0


# ---- upscaling ----
class Upscaler:
    def __init__(self, model_path: str | None):
        self.dry = model_path is None
        if self.dry:
            return
        import spandrel
        import torch
        self.torch = torch
        self.model = spandrel.ModelLoader().load_from_file(model_path).eval()
        self.scale = self.model.scale
        self.session = None
        try:
            import onnxruntime as ort
            if 'CUDAExecutionProvider' in ort.get_available_providers() and not torch.cuda.is_available():
                onnx = Path(model_path).with_suffix('.onnx')
                if not onnx.exists():
                    dummy = torch.zeros(1, 3, 64, 64)
                    torch.onnx.export(self.model.model, dummy, str(onnx), input_names=['x'], output_names=['y'],
                                      dynamic_axes={'x': {2: 'h', 3: 'w'}, 'y': {2: 'h', 3: 'w'}}, opset_version=17)
                self.session = ort.InferenceSession(str(onnx), providers=['CUDAExecutionProvider', 'CPUExecutionProvider'])
        except Exception as e:  # torch on CPU still works, slowly
            print('onnxruntime GPU path unavailable:', e)
        self.device = 'cuda' if torch.cuda.is_available() else 'cpu'
        self.model.to(self.device)

    def run(self, rgb: np.ndarray) -> np.ndarray:
        """float32 HxWx3 in 0..1 -> 4x, float32."""
        x = rgb.transpose(2, 0, 1)[None].astype(np.float32)
        if self.session is not None:
            return self.session.run(None, {'x': x})[0][0].transpose(1, 2, 0)
        with self.torch.no_grad():
            return self.model.model(self.torch.from_numpy(x).to(self.device)).cpu().numpy()[0].transpose(1, 2, 0)

    def upscale(self, img: np.ndarray, scale: int, tile: int = 256, pad: int = 16) -> np.ndarray:
        """img float32 HxWxC (C 1..3) in 0..1, wrapped at the edges (the textures tile)."""
        h, w, c = img.shape
        if self.dry:
            wrapped = np.pad(img, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
            out = np.stack([np.asarray(Image.fromarray(wrapped[:, :, k]).resize(
                ((w + 2 * pad) * scale, (h + 2 * pad) * scale), Image.LANCZOS)) for k in range(c)], -1)
            return np.clip(out[pad * scale:(pad + h) * scale, pad * scale:(pad + w) * scale], 0, 1)
        rgb = np.repeat(img, 3, 2) if c == 1 else np.concatenate([img, np.zeros_like(img[:, :, :1])], 2) if c == 2 else img
        wrapped = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
        s = self.scale
        out = np.zeros((h * s, w * s, 3), np.float32)
        for y in range(0, h, tile):
            for x in range(0, w, tile):
                th, tw = min(tile, h - y), min(tile, w - x)
                piece = wrapped[y:y + th + 2 * pad, x:x + tw + 2 * pad]
                up = self.run(piece)
                out[y * s:(y + th) * s, x * s:(x + tw) * s] = up[pad * s:(pad + th) * s, pad * s:(pad + tw) * s]
        out = np.clip(out, 0, 1)
        if scale != s:  # x4 model, x2 wanted: area downscale of the x4 result
            f = s // scale
            out = out.reshape(h * scale, f, w * scale, f, 3).mean((1, 3))
        if c == 1:
            return out.mean(2, keepdims=True)
        if c == 2:
            return out[:, :, :2]
        return out


# ---- block compression (numpy; range fit, good enough for upscaled sources) ----
def to_blocks(img: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    c = img.shape[2]
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        img = np.pad(img, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    return img.reshape(ph // 4, 4, pw // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def encode_alpha(values: np.ndarray) -> np.ndarray:
    """values: (n,16) uint8 -> (n,8) BC4 blocks (8-value mode)."""
    lo, hi = values.min(1).astype(np.int32), values.max(1).astype(np.int32)
    hi = np.maximum(hi, lo + 1)
    a0, a1 = hi, lo  # a0 > a1: eight interpolated values
    palette = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], 1)
    idx = np.abs(values[:, :, None].astype(np.int32) - palette[:, None, :]).argmin(2).astype(np.uint64)
    bits = np.zeros(len(values), np.uint64)
    for k in range(16):
        bits |= idx[:, k] << np.uint64(3 * k)
    out = np.zeros((len(values), 8), np.uint8)
    out[:, 0], out[:, 1] = np.minimum(a0, 255), lo
    for k in range(6):
        out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def encode_colour(rgb: np.ndarray) -> np.ndarray:
    """rgb: (n,16,3) uint8 -> (n,8) BC1 blocks, four-colour mode, endpoints along the principal axis."""
    p = rgb.astype(np.float32)
    mean = p.mean(1, keepdims=True)
    d = p - mean
    cov = np.einsum('nki,nkj->nij', d, d)
    axis = np.ones((len(p), 3), np.float32)
    for _ in range(4):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum('nki,ni->nk', d, axis)
    e0 = mean[:, 0] + axis * t.max(1, keepdims=True)
    e1 = mean[:, 0] + axis * t.min(1, keepdims=True)

    def pack(c):
        c = np.clip(np.rint(c), 0, 255).astype(np.int32)
        return ((c[:, 0] * 31 + 127) // 255) << 11 | ((c[:, 1] * 63 + 127) // 255) << 5 | ((c[:, 2] * 31 + 127) // 255)

    def unpack(v):
        return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], 1)
    c0, c1 = pack(e0), pack(e1)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    same = c0 == c1
    c0 = np.where(same & (c0 < 0xFFFF), c0 + 1, c0)
    c1 = np.where(same & (c0 == 0xFFFF) & (c1 > 0), c1 - 1, c1)
    q0, q1 = unpack(c0), unpack(c1)
    palette = np.stack([q0, q1, (2 * q0 + q1) // 3, (q0 + 2 * q1) // 3], 1)
    idx = ((p[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(3).argmin(2).astype(np.uint32)
    bits = np.zeros(len(p), np.uint32)
    for k in range(16):
        bits |= idx[:, k] << np.uint32(2 * k)
    out = np.zeros((len(p), 8), np.uint8)
    out[:, 0], out[:, 1] = c0 & 255, c0 >> 8
    out[:, 2], out[:, 3] = c1 & 255, c1 >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> np.uint32(8 * k)) & 255
    return out


def encode(img: np.ndarray, block: int) -> bytes:
    """img uint8 HxWxC -> one level's blocks."""
    if block == 1:
        return encode_colour(to_blocks(img[:, :, :3])).tobytes()
    if block == 3:
        b = to_blocks(img)
        return np.concatenate([encode_alpha(b[:, :, 3]), encode_colour(b[:, :, :3])], 1).tobytes()
    if block == 4:
        return encode_alpha(to_blocks(img[:, :, :1])[:, :, 0]).tobytes()
    b = to_blocks(img[:, :, :2])
    return np.concatenate([encode_alpha(b[:, :, 0]), encode_alpha(b[:, :, 1])], 1).tobytes()


def mips(img: np.ndarray):
    level = img.astype(np.float32)
    while True:
        yield np.clip(np.rint(level), 0, 255).astype(np.uint8)
        h, w = level.shape[:2]
        if h == 1 and w == 1:
            return
        nh, nw = max(1, h // 2), max(1, w // 2)
        level = level[:nh * (h // nh), :nw * (w // nw)].reshape(nh, h // nh, nw, w // nw, -1).mean((1, 3))


def write_dds(path: Path, img: np.ndarray, block: int):
    h, w = img.shape[:2]
    levels = [encode(m, block) for m in mips(img)]
    fourcc = {1: b'DXT1', 3: b'DXT5', 4: b'ATI1', 5: b'ATI2'}[block]
    header = bytearray(128)
    struct.pack_into('<4sIIIIIII', header, 0, b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w,
                     len(levels[0]), 0, len(levels))
    struct.pack_into('<II4s', header, 76, 32, 0x4, fourcc)
    struct.pack_into('<I', header, 108, 0x1000 | 0x8 | 0x400000)
    path.write_bytes(bytes(header) + b''.join(levels))
    return len(levels)


def seam_score(img: np.ndarray) -> float:
    """Step across the wrap edges over the mean step inside (about 1 for a seamless tile)."""
    g = img.astype(np.float32).mean(2) if img.ndim == 3 else img.astype(np.float32)
    inside = (np.abs(np.diff(g, axis=1)).mean() + np.abs(np.diff(g, axis=0)).mean()) / 2
    edge = (np.abs(g[:, 0] - g[:, -1]).mean() + np.abs(g[0, :] - g[-1, :]).mean()) / 2
    return float(edge / max(inside, 1e-3))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--model')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--game', default=r'D:\SteamLibrary\steamapps\common\Exanima')
    ap.add_argument('--export', default=r'E:\ExCamera-playtest\textures\level')
    ap.add_argument('--out', default=r'E:\ExCamera-playtest\textures\level-upscaled')
    ap.add_argument('--only', help='comma-separated names (testing)')
    a = ap.parse_args(argv[1:])
    if not a.dry_run and not a.model:
        ap.error('--model is required (or --dry-run)')
    up = Upscaler(None if a.dry_run else a.model)
    textures = Pack(Path(a.game) / 'Textures.rpk')
    index = {r['name'].lower(): r for r in csv.DictReader(open(Path(a.export) / 'index.csv', encoding='utf-8'))}
    out = Path(a.out)
    (out / 'dds').mkdir(parents=True, exist_ok=True)
    (out / 'png').mkdir(exist_ok=True)
    (out / 'crops').mkdir(exist_ok=True)
    only = {n.lower() for n in a.only.split(',')} if a.only else None
    rows, cards = [], []
    import re
    for group, scale, pattern in GROUPS:
        names = sorted(r['name'] for r in index.values() if re.fullmatch(pattern, r['name']) and r.get('status') == 'exported')
        for name in names:
            if only and name.lower() not in only:
                continue
            row = index.get(name.lower())
            entry = textures.entries.get(name.lower())
            rec = {'group': group, 'name': name, 'scale': scale}
            if not row or not entry or row.get('status') != 'exported':
                rec['status'] = 'not in the export or the pack'
                rows.append(rec)
                continue
            head = textures.read(name.lower(), 32)
            _, width, height, _, word, _, _, _ = struct.unpack('<8I', head)
            block = block_for(word)
            kind = format_of(word)
            rec.update({'type': row['map_type'], 'from': f'{width}x{height}', 'format_word': f'0x{word:08X}'})
            if len(name) + 4 > 16:
                rec['status'] = 'refused: name key over 16 bytes'
            elif not block:
                rec['status'] = 'refused: the game uploads it in a format the hook does not replace'
            if 'status' in rec:
                rows.append(rec)
                continue
            data = textures.read(name.lower(), 32 + mip0_size(width, height, kind))[32:]
            original = decode(data, width, height, kind)
            normal = block == 5
            if block == 4:
                src = original[:, :, :1].astype(np.float32) / 255
            elif normal:
                src = original[:, :, :2].astype(np.float32) / 255
            elif block == 3:
                src = original[:, :, :3].astype(np.float32) / 255
            else:
                src = original[:, :, :3].astype(np.float32) / 255
            big = up.upscale(src, scale)
            if block == 3:  # alpha upscaled on its own, as one channel
                alpha = up.upscale(original[:, :, 3:4].astype(np.float32) / 255, scale)
                big = np.concatenate([big, alpha], 2)
            if normal:
                xy = big * 2 - 1
                z = np.sqrt(np.clip(1 - (xy ** 2).sum(2, keepdims=True), 0, 1))
                n = np.concatenate([xy, z], 2)
                n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
                big = np.concatenate([(n[:, :, :2] + 1) / 2, np.zeros_like(n[:, :, :1])], 2)
            img8 = np.clip(np.rint(big * 255), 0, 255).astype(np.uint8)
            levels = write_dds(out / 'dds' / f'{name}.dds', img8, block)
            preview = img8[:, :, 0] if block == 4 else img8
            if normal:
                preview = np.concatenate([img8[:, :, :2], np.clip(np.rint(z * 127.5 + 127.5), 0, 255).astype(np.uint8)], 2)
            Image.fromarray(preview).save(out / 'png' / f'{name}.png')
            # Crops: the same 128-texel patch of the original, enlarged, and of the upscale.
            oy, ox = height // 3, width // 3
            crop_o = original[oy:oy + 64, ox:ox + 64, :1 if block == 4 else 3]
            crop_u = preview[oy * scale:(oy + 64) * scale, ox * scale:(ox + 64) * scale]
            Image.fromarray(np.squeeze(crop_o)).resize((256, 256), Image.NEAREST).save(out / 'crops' / f'{name}_before.png')
            Image.fromarray(np.squeeze(crop_u)).resize((256, 256), Image.LANCZOS).save(out / 'crops' / f'{name}_after.png')
            tile = np.tile(preview if preview.ndim == 3 else preview[:, :, None], (2, 2, 1))
            th = Image.fromarray(np.squeeze(tile)).resize((512, 512 * tile.shape[0] // tile.shape[1]), Image.LANCZOS)
            th.save(out / 'crops' / f'{name}_tiled.png')
            before_seam = seam_score(original[:, :, :3] if original.shape[2] >= 3 else original)
            after_seam = seam_score(preview if preview.ndim == 3 else preview[:, :, None])
            rec.update({'to': f'{img8.shape[1]}x{img8.shape[0]}', 'block': {1: 'BC1', 3: 'BC3', 4: 'BC4', 5: 'BC5'}[block],
                        'mips': levels, 'seam_before': round(before_seam, 2), 'seam_after': round(after_seam, 2),
                        'dds_mb': round((out / 'dds' / f'{name}.dds').stat().st_size / 2 ** 20, 1),
                        'status': 'dry run (Lanczos): do not install' if up.dry else 'written'})
            rows.append(rec)
            cards.append(f'<tr><td><b>{html.escape(name)}</b><br>{html.escape(group)}<br>{rec["from"]} &rarr; {rec["to"]} '
                         f'{rec["block"]}<br>seam {rec["seam_before"]} &rarr; {rec["seam_after"]}</td>'
                         f'<td><img src="crops/{name}_before.png"></td><td><img src="crops/{name}_after.png"></td>'
                         f'<td><img src="crops/{name}_tiled.png"></td></tr>')
            print(name, rec['status'], rec.get('to'))
    fields = ['group', 'name', 'type', 'scale', 'from', 'to', 'format_word', 'block', 'mips', 'dds_mb', 'seam_before',
              'seam_after', 'status']
    with open(out / 'upscale.csv', 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fields, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)
    (out / 'compare.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Upscaled level textures</title><style>body{font:13px sans-serif;'
        'background:#1d1d1d;color:#ddd}td{padding:4px;vertical-align:top}img{width:256px}</style>'
        '<p>Left: original (64 texels, enlarged). Middle: upscaled, same patch. Right: upscaled tiled 2x2 '
        '(seams would show as lines through the middle). Seam score: step across the wrap edge over the mean step '
        'inside (about 1 is seamless).</p><table>' + ''.join(cards) + '</table>', encoding='utf-8')
    print(f'{sum(1 for r in rows if r.get("status") == "written")} written, '
          f'{sum(1 for r in rows if str(r.get("status", "")).startswith("refused"))} refused')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
