#!/usr/bin/env python3
"""Convert a Mixamo binary FBX animation into a small JSON skeleton clip.

Standard library only. Reads FBX 7.x binary files (the format Mixamo
exports), keeps the 22 body bones and samples them at 30 fps.

Output space is FBX/Mixamo space: +Y up, +Z character forward,
+X character left, centimetres. Quaternions are (x, y, z, w).

    python tools/mixamo_fbx2json.py "Mma Kick.fbx" -o mma_kick.json
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import zlib
from pathlib import Path

FBX_TICKS_PER_SECOND = 46186158000
SAMPLE_FPS = 30

BODY_BONES = (
    "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
)


# --- binary FBX parsing -----------------------------------------------------

class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name, props, children):
        self.name = name
        self.props = props
        self.children = children

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def all(self, name):
        return [c for c in self.children if c.name == name]


_ARRAY_FORMATS = {"f": "f", "d": "d", "l": "q", "i": "i", "b": "?"}
_SCALAR_FORMATS = {"Y": "<h", "C": "<?", "I": "<i", "F": "<f", "D": "<d", "L": "<q"}


def _read_prop(data, pos):
    code = chr(data[pos])
    pos += 1
    if code in _SCALAR_FORMATS:
        fmt = _SCALAR_FORMATS[code]
        (value,) = struct.unpack_from(fmt, data, pos)
        return value, pos + struct.calcsize(fmt)
    if code in _ARRAY_FORMATS:
        count, encoding, length = struct.unpack_from("<III", data, pos)
        pos += 12
        raw = data[pos:pos + length]
        pos += length
        if encoding == 1:
            raw = zlib.decompress(raw)
        elif encoding != 0:
            raise ValueError(f"unknown FBX array encoding {encoding}")
        return list(struct.unpack(f"<{count}{_ARRAY_FORMATS[code]}", raw)), pos
    if code in ("S", "R"):
        (length,) = struct.unpack_from("<I", data, pos)
        pos += 4
        raw = bytes(data[pos:pos + length])
        pos += length
        return (raw.decode("utf-8", "replace") if code == "S" else raw), pos
    raise ValueError(f"unknown FBX property type {code!r} at {pos - 1}")


def _read_node(data, pos, wide):
    if wide:
        end, nprops, _plen = struct.unpack_from("<QQQ", data, pos)
        pos += 24
    else:
        end, nprops, _plen = struct.unpack_from("<III", data, pos)
        pos += 12
    name_len = data[pos]
    pos += 1
    if end == 0:
        return None, pos + name_len
    name = bytes(data[pos:pos + name_len]).decode("ascii", "replace")
    pos += name_len
    props = []
    for _ in range(nprops):
        value, pos = _read_prop(data, pos)
        props.append(value)
    children = []
    while pos < end:
        node, pos = _read_node(data, pos, wide)
        if node is None:
            break
        children.append(node)
    return Node(name, props, children), end


def parse_fbx(data: bytes) -> Node:
    if not data.startswith(b"Kaydara FBX Binary  \x00"):
        raise ValueError("not a binary FBX file")
    (version,) = struct.unpack_from("<I", data, 23)
    if version < 7000:
        raise ValueError(f"FBX version {version} is too old")
    wide = version >= 7500
    data = memoryview(data)
    pos = 27
    roots = []
    while pos < len(data):
        node, pos = _read_node(data, pos, wide)
        if node is None:
            break
        roots.append(node)
    return Node("", [version], roots)


def properties70(node):
    """Map P name -> list of values from a Properties70 block."""
    result = {}
    block = node.child("Properties70") if node else None
    if block is None:
        return result
    for p in block.all("P"):
        if p.props:
            result[p.props[0]] = p.props[4:]
    return result


# --- quaternion maths (x, y, z, w) -------------------------------------------

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def qaxis(axis, degrees):
    h = math.radians(degrees) * 0.5
    s = math.sin(h)
    v = [0.0, 0.0, 0.0]
    v[axis] = s
    return (v[0], v[1], v[2], math.cos(h))


def qeuler_xyz(deg):
    """FBX eEulerXYZ: R = Rz * Ry * Rx (X applied first)."""
    return qmul(qaxis(2, deg[2]), qmul(qaxis(1, deg[1]), qaxis(0, deg[0])))


def qrotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # t = 2 * cross(q.xyz, v)
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    )


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


# --- scene extraction -------------------------------------------------------

def _short_name(raw):
    name = raw.split("\x00", 1)[0]
    if ":" in name:
        name = name.split(":", 1)[1]
    return name


class Curve:
    def __init__(self, times, values):
        self.times = [t / FBX_TICKS_PER_SECOND for t in times]
        self.values = values

    def sample(self, t):
        ts, vs = self.times, self.values
        if not ts:
            return 0.0
        if t <= ts[0]:
            return vs[0]
        if t >= ts[-1]:
            return vs[-1]
        lo, hi = 0, len(ts) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if ts[mid] <= t:
                lo = mid
            else:
                hi = mid
        span = ts[hi] - ts[lo]
        u = (t - ts[lo]) / span if span > 0 else 0.0
        return vs[lo] + (vs[hi] - vs[lo]) * u


def extract(root: Node):
    objects = root.child("Objects")
    conns = root.child("Connections")
    if objects is None or conns is None:
        raise ValueError("FBX has no Objects/Connections")

    models, curve_nodes, curves = {}, {}, {}
    for obj in objects.children:
        if obj.name == "Model" and len(obj.props) >= 3:
            models[obj.props[0]] = obj
        elif obj.name == "AnimationCurveNode":
            curve_nodes[obj.props[0]] = obj
        elif obj.name == "AnimationCurve":
            kt = obj.child("KeyTime")
            kv = obj.child("KeyValueFloat")
            if kt is not None and kv is not None:
                curves[obj.props[0]] = Curve(kt.props[0], kv.props[0])

    parent_of = {}
    node_channel = {}   # curve node id -> (model id, 'Lcl Rotation'|'Lcl Translation')
    curve_axis = {}     # curve node id -> {axis index: Curve}
    for c in conns.all("C"):
        kind, child, parent = c.props[0], c.props[1], c.props[2]
        if kind == "OO" and child in models:
            parent_of[child] = parent
        elif kind == "OP":
            prop = c.props[3] if len(c.props) > 3 else ""
            if child in curve_nodes and parent in models:
                node_channel[child] = (parent, prop)
            elif child in curves and parent in curve_nodes:
                axis = {"d|X": 0, "d|Y": 1, "d|Z": 2}.get(prop)
                if axis is not None:
                    curve_axis.setdefault(parent, {})[axis] = curves[child]

    bones = {}
    for mid, model in models.items():
        if model.props[2] != "LimbNode":
            continue
        name = _short_name(model.props[1])
        if name not in BODY_BONES:
            continue
        p70 = properties70(model)
        order = int(p70.get("RotationOrder", [0])[0]) if "RotationOrder" in p70 else 0
        if order != 0:
            raise ValueError(f"{name}: rotation order {order} unsupported (only XYZ)")
        bones[mid] = {
            "name": name,
            "pre": [float(v) for v in p70.get("PreRotation", [0, 0, 0])[:3]],
            "t": [float(v) for v in p70.get("Lcl Translation", [0, 0, 0])[:3]],
            "r": [float(v) for v in p70.get("Lcl Rotation", [0, 0, 0])[:3]],
            "curves": {},
        }

    for cn_id, (mid, prop) in node_channel.items():
        if mid not in bones or prop not in ("Lcl Rotation", "Lcl Translation"):
            continue
        defaults = properties70(curve_nodes[cn_id])
        axes = curve_axis.get(cn_id, {})
        chans = []
        for i, key in enumerate(("d|X", "d|Y", "d|Z")):
            if i in axes:
                chans.append(axes[i])
            else:
                chans.append(float(defaults.get(key, [0.0])[0]))
        bones[mid]["curves"][prop] = chans

    missing = sorted(set(BODY_BONES) - {b["name"] for b in bones.values()})
    if missing:
        raise ValueError(f"missing Mixamo bones: {', '.join(missing)}")

    name_of = {mid: b["name"] for mid, b in bones.items()}
    for mid, bone in bones.items():
        parent = parent_of.get(mid)
        bone["parent"] = name_of.get(parent)
    return bones


def _channel(values, t, fallback):
    if values is None:
        return list(fallback)
    return [v.sample(t) if isinstance(v, Curve) else v for v in values]


def convert(path: Path) -> dict:
    root = parse_fbx(path.read_bytes())
    bones = extract(root)
    by_name = {b["name"]: b for b in bones.values()}
    ordered = [by_name[n] for n in BODY_BONES]

    end = 0.0
    for b in ordered:
        for chans in b["curves"].values():
            for c in chans:
                if isinstance(c, Curve) and c.times:
                    end = max(end, c.times[-1])
    count = int(round(end * SAMPLE_FPS)) + 1

    frames = []
    for f in range(count):
        t = f / SAMPLE_FPS
        local_q, global_q, global_p = {}, {}, {}
        for b in ordered:
            rot = _channel(b["curves"].get("Lcl Rotation"), t, b["r"])
            trans = _channel(b["curves"].get("Lcl Translation"), t, b["t"])
            lq = qnorm(qmul(qeuler_xyz(b["pre"]), qeuler_xyz(rot)))
            local_q[b["name"]] = lq
            parent = b["parent"]
            if parent is None:
                global_q[b["name"]] = lq
                global_p[b["name"]] = tuple(trans)
            else:
                pq = global_q[parent]
                off = qrotate(pq, trans)
                pp = global_p[parent]
                global_q[b["name"]] = qnorm(qmul(pq, lq))
                global_p[b["name"]] = (pp[0] + off[0], pp[1] + off[1], pp[2] + off[2])
        frames.append({
            "hips": [round(v, 4) for v in global_p["Hips"]],
            "local": [[round(v, 6) for v in local_q[n]] for n in BODY_BONES],
            "rot": [[round(v, 6) for v in global_q[n]] for n in BODY_BONES],
            "pos": [[round(v, 4) for v in global_p[n]] for n in BODY_BONES],
        })

    return {
        "source": path.name,
        "space": "fbx: +Y up, +Z forward, +X character left, cm; quat x,y,z,w",
        "fps": SAMPLE_FPS,
        "frames": count,
        "bones": [
            {
                "name": b["name"],
                "parent": b["parent"],
                "rest_translation": [round(v, 4) for v in b["t"]],
                "pre_rotation": [round(v, 4) for v in b["pre"]],
            }
            for b in ordered
        ],
        "samples": frames,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fbx", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    args = ap.parse_args(argv)
    clip = convert(args.fbx)
    text = json.dumps(clip, separators=(",", ":"))
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
