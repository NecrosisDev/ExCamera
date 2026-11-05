#!/usr/bin/env python3
"""Bake the Animated kick's full-body tracks and their C++ header.

    python tools/kick_bake.py bake <dir of mixamo_fbx2json.py JSON files>  # -> tools/kick_clips.json
    python tools/kick_bake.py header                                     # tools/kick_clips.json -> src/kick_clip.h
    python tools/kick_bake.py check                                      # header matches the baked tracks

Per frame, in the clip's heading frame (+X character left, +Y up, +Z forward), joint-to-joint chords that
exist on both skeletons, so no knowledge of the game's part rest frames is needed (src/kick_retarget.h):
  pelvis up (mid-hip -> Spine2) and across (left hip - right hip), chest up (Spine2 -> Head) and across
  (left shoulder joint - right shoulder joint), unit directions of each shoulder, upper arm, forearm, thigh,
  shin and foot, and the mid-hip's offset from the supporting foot in cm (the clip's hip travel, kept as authored).
Every track is stored as a right-leg kick (a left-leg clip is mirrored). The motion is kept as authored.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRACKS = ROOT / 'tools' / 'kick_clips.json'
HEADER = ROOT / 'src' / 'kick_clip.h'

# Sets: 0 unarmed or one-handed, 1 a two-handed weapon (Hunter, 2026-11-05). Order is the rotation order.
CLIPS = (
    ('standingMelee', 'standing_melee_attack_kick_ver_2', 0),
    ('swordShield', 'sword_and_shield_kick', 0),
    ('mma', 'mma_kick', 0),
    ('greatSword', 'great_sword_kick', 1),
    ('greatSword1', 'great_sword_kick_1', 1),
)
# Slot order of src/kick_retarget.h (Slot).
SLOTS = ('pelvisUp', 'pelvisAcross', 'chestUp', 'chestAcross',
         'leftShoulder', 'leftUpperArm', 'leftForearm', 'rightShoulder', 'rightUpperArm', 'rightForearm',
         'leftThigh', 'leftShin', 'leftFoot', 'rightThigh', 'rightShin', 'rightFoot', 'hipOffset')
ACROSS = {'pelvisAcross', 'chestAcross'}


def _sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def _mid(a, b):
    return [(a[i] + b[i]) / 2 for i in range(3)]


def _len(v):
    return math.sqrt(sum(c * c for c in v))


def _unit(v):
    n = _len(v)
    if n < 1e-6:
        raise ValueError('degenerate chord')
    return [c / n for c in v]


def _mirror(name, v):
    """The mirror image (X negated) with left and right swapped: an across vector (left - right) flips."""
    return [v[0], -v[1], -v[2]] if name in ACROSS else [-v[0], v[1], v[2]]


def _frame(s, index):
    p = lambda bone: s['pos'][index[bone]]
    hip = _mid(p('LeftUpLeg'), p('RightUpLeg'))
    v = {
        'pelvisUp': _sub(p('Spine2'), hip), 'pelvisAcross': _sub(p('LeftUpLeg'), p('RightUpLeg')),
        'chestUp': _sub(p('Head'), p('Spine2')), 'chestAcross': _sub(p('LeftArm'), p('RightArm')),
        'hipOffset': _sub(hip, p('LeftFoot')),  # from the supporting (left) foot of a right-leg kick
    }
    for side in ('Left', 'Right'):
        low = side.lower()
        v[low + 'Shoulder'] = _sub(p(side + 'Arm'), p(side + 'Shoulder'))
        v[low + 'UpperArm'] = _sub(p(side + 'ForeArm'), p(side + 'Arm'))
        v[low + 'Forearm'] = _sub(p(side + 'Hand'), p(side + 'ForeArm'))
        v[low + 'Thigh'] = _sub(p(side + 'Leg'), p(side + 'UpLeg'))
        v[low + 'Shin'] = _sub(p(side + 'Foot'), p(side + 'Leg'))
        v[low + 'Foot'] = _sub(p(side + 'ToeBase'), p(side + 'Foot'))
    return {k: (x if k == 'hipOffset' else _unit(x)) for k, x in v.items()}


def bake(clip: dict, name: str, kick_set: int) -> dict:
    names = [b['name'] for b in clip['bones']]
    index = {n: i for i, n in enumerate(names)}
    samples = clip['samples']

    def height(side):
        return max(s['pos'][index[side + 'Foot']][1] for s in samples)

    left = height('Left') > height('Right')
    frames = []
    for s in samples:
        if left:  # mirror the positions first, so the supporting foot is the stored left one
            m = {'pos': [[-q[0], q[1], q[2]] for q in s['pos']]}
            swapped = {n: index[('Right' + n[4:]) if n.startswith('Left') else ('Left' + n[5:]) if n.startswith('Right') else n] for n in names}
            f = _frame(m, swapped)
        else:
            f = _frame(s, index)
        frames.append([[round(c, 4) for c in f[k]] for k in SLOTS])
    kick = SLOTS.index('rightFoot')
    support = SLOTS.index('hipOffset')
    feet = [s['pos'][index[('Left' if left else 'Right') + 'Foot']][1] for s in samples]
    peak = max(range(len(feet)), key=lambda f: feet[f])
    s0 = samples[0]
    p0 = lambda bone: s0['pos'][index[bone]]
    side = 'Left' if left else 'Right'
    leg = _len(_sub(p0(side + 'Leg'), p0(side + 'UpLeg'))) + _len(_sub(p0(side + 'Foot'), p0(side + 'Leg')))
    lead = frames[0][support]  # the supporting foot ahead of the hips at the start: the kick is from the rear leg
    del kick
    return {
        'name': name,
        'source': clip.get('source', ''),
        'set': kick_set,
        'leg': 'left' if left else 'right',
        'fps': clip['fps'],
        'peak': peak,
        'leg_length_cm': round(leg, 2),
        'support_leads': lead[2] < 0,
        'frames': frames,
    }


def _num(v):
    text = repr(float(v))
    return (text if ('.' in text or 'e' in text) else text + '.0') + 'f'


def header(tracks: dict) -> str:
    clips = tracks['clips']
    lines = [
        '#pragma once',
        '// Generated by tools/kick_bake.py from tools/kick_clips.json; do not edit.',
        '// Per frame, clip heading frame (+X left, +Y up, +Z forward), right-leg kick, in Slot order (kick_retarget.h):',
        '// ' + ', '.join(SLOTS) + '.',
        '// Set 0: unarmed or one-handed; 1: two-handed.',
        '#include "kick_retarget.h"',
        '',
        'namespace fp::kick {',
        f'static_assert(slotCount == {len(SLOTS)}, "kick_bake.py SLOTS and kick_retarget.h Slot disagree");',
    ]
    for c in clips:
        lines.append(f'// {c["source"]} ({c["leg"]} leg), set {c["set"]}, peak frame {c["peak"]}.')
        lines.append(f'inline constexpr Frame {c["name"]}Frames[] = {{')
        for f in c['frames']:
            lines.append('    {{' + ', '.join('{' + ', '.join(_num(x) for x in d) + '}' for d in f) + '}},')
        lines.append('};')
    lines.append('inline constexpr Clip clips[] = {')
    for c in clips:
        lines.append(f'    {{"{c["name"]}", {c["name"]}Frames, {len(c["frames"])}, {_num(c["fps"])}, {int(c["peak"])}, {_num(c["leg_length_cm"])}, '
                     f'{int(c["set"])}, {"true" if c["support_leads"] else "false"}}},')
    lines += ['};', f'inline constexpr int clipCount = {len(clips)};', '}', '']
    return '\n'.join(lines)


def check_header():
    tracks = json.loads(TRACKS.read_text(encoding='utf-8'))
    clips = tracks['clips']
    if not clips or {c['set'] for c in clips} != {0, 1}:
        raise ValueError('kick tracks need clips in both sets')
    for c in clips:
        if len(c['frames']) < 8 or not 0 < c['peak'] < len(c['frames']) - 1 or not 50 < c['leg_length_cm'] < 150:
            raise ValueError('kick track %s is too short, peaks at an end or has an odd leg' % c['name'])
        for f in c['frames']:
            if len(f) != len(SLOTS):
                raise ValueError('kick track %s has the wrong slot count' % c['name'])
            for k, d in zip(SLOTS, f):
                if k != 'hipOffset' and abs(_len(d) - 1) > 1e-3:
                    raise ValueError('kick track %s %s is not unit length' % (c['name'], k))
    if HEADER.read_text(encoding='utf-8') != header(tracks):
        raise ValueError('Generated kick header is stale; run tools/kick_bake.py header')
    return len(clips)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('bake')
    b.add_argument('dir', type=Path)
    sub.add_parser('header')
    sub.add_parser('check')
    args = ap.parse_args(argv)
    if args.cmd == 'bake':
        clips = []
        for name, stem, kick_set in CLIPS:
            c = bake(json.loads((args.dir / (stem + '.json')).read_text(encoding='utf-8')), name, kick_set)
            clips.append(c)
            print(f'{name}: {len(c["frames"])} frames, {c["leg"]} leg, peak {c["peak"]}, leg {c["leg_length_cm"]} cm, '
                  f'support leads {c["support_leads"]}, set {kick_set}')
        TRACKS.write_text(json.dumps({'clips': clips}, separators=(',', ':')) + '\n', encoding='utf-8', newline='\n')
    elif args.cmd == 'header':
        HEADER.write_text(header(json.loads(TRACKS.read_text(encoding='utf-8'))), encoding='utf-8', newline='\n')
        print(HEADER)
    else:
        print(f'kick tracks: {check_header()} clips, header consistent')
    return 0


if __name__ == '__main__':
    sys.exit(main())
