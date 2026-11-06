#!/usr/bin/env python3
"""Bake the Animated jump's full-body tracks and their C++ header.

    python tools/jump_bake.py bake      # tools/clip_sources/*.json -> tools/jump_clips.json
    python tools/jump_bake.py header    # tools/jump_clips.json -> src/jump_clip.h
    python tools/jump_bake.py check     # header matches the baked tracks

The same chords as the kick (tools/kick_bake.py, src/kick_retarget.h Slot order), in the clip's own axes (+X left,
+Y up, +Z forward), stored as authored (never mirrored here). The root's translation and the forward travel are
dropped: every slot is a direction between joints, and hipOffset is zero (the jump never drives the hips' position;
the game's physics alone lifts and moves the body). Per clip the phase frames are found from the data:
  takeoff   the lowest Hips frame before the peak (the last frame before the hips rise; 0 when they rise at once),
  peak      the highest Hips frame,
  touchdown the first frame after the peak where as many feet are down as at frame 0 (a foot is down when its lowest
            joint, ankle or toe, is within GROUND_CM of the clip's floor, the lowest such point in the clip),
  end       the first frame from touchdown on after which the Hips stay within SETTLE_CM of frame 0's.
Per frame, the drop: the mid-hip's height above the lowest foot point (cm), as authored. The standing tuck default
(src/jump_warp.h maxTuck, the same sums) is the largest StandingJumpTuck in 0.01 steps for which the lowest foot
stays within the game's 20 cm in-air margin (0x16F7FF) at every flight frame, with the hips lifted by a ballistic
15 cm hop (fp::jumpPeakCm) mapped onto the flight frames as the warp maps them. A geometric estimate from the clip:
the character's leg is taken as the clip's, and the margin's own +94 tolerance is left out (so it errs low).
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import kick_bake  # noqa: E402

ROOT = TOOLS.parent
SOURCES = ROOT / 'tools' / 'clip_sources'
TRACKS = ROOT / 'tools' / 'jump_clips.json'
HEADER = ROOT / 'src' / 'jump_clip.h'

# (name in C++, source stem). Order: index 0 standing, 1 travelling (src/jump_warp.h standingClip/travellingClip).
CLIPS = (
    ('jumping', 'jumping'),
    ('runningJump', 'running_jump'),
)
SLOTS = kick_bake.SLOTS
LEG_SLOTS = ('leftThigh', 'leftShin', 'leftFoot', 'rightThigh', 'rightShin', 'rightFoot')
GROUND_CM = 1.0      # ours: a foot point this close to the clip's floor is down
SETTLE_CM = 1.0      # ours: the hips this close to frame 0's are settled
PEAK_CM = 15.0       # fp::jumpPeakCm (camera_motion.h), ours: the standing hop
MARGIN_CM = 20.0     # supportHeight's standing in-air margin (0x16F7FF)
TUCK_STEP = 0.01


def _sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def _len(v):
    return math.sqrt(sum(c * c for c in v))


def _norm(v):
    n = _len(v)
    return [c / n for c in v] if n > 1e-6 else [0.0, -1.0, 0.0]


def _positions(sample: dict, names: list) -> list:
    """Bone positions in bone order, from either converter layout (a 'pos' list or a 'position_cm' map)."""
    if 'pos' in sample:
        return sample['pos']
    return [sample['position_cm'][n] for n in names]


def phases(hips: list, lowest: list, floor: float) -> dict:
    """takeoff, peak, touchdown, end and the landing dip from per-frame Hips y and [left, right] lowest foot y."""
    n = len(hips)
    peak = max(range(n), key=lambda f: (hips[f], -f))
    takeoff = min(range(peak + 1), key=lambda f: (hips[f], f))
    down = lambda f: sum(1 for y in lowest[f] if y - floor <= GROUND_CM)
    touchdown = next((f for f in range(peak + 1, n) if down(f) >= down(0)), n - 1)
    end = next((f for f in range(touchdown, n) if all(abs(hips[g] - hips[0]) <= SETTLE_CM for g in range(f, n))), n - 1)
    dip = min(range(touchdown, end + 1), key=lambda f: (hips[f], f))
    return {'takeoff': takeoff, 'peak': peak, 'touchdown': touchdown, 'end': end, 'landing_dip': dip}


def bake(clip: dict, name: str) -> dict:
    names = [b['name'] for b in clip['bones']]
    index = {b: i for i, b in enumerate(names)}
    frames, hips, lowest, drops = [], [], [], []
    for s in clip['samples']:
        pos = _positions(s, names)
        p = lambda bone: pos[index[bone]]
        f = kick_bake._frame({'pos': pos}, index)
        f['hipOffset'] = [0.0, 0.0, 0.0]  # not driven: physics alone moves the body
        frames.append([[round(c, 4) for c in f[k]] for k in SLOTS])
        hips.append(p('Hips')[1])
        sides = [min(p(side + 'Foot')[1], p(side + 'ToeBase')[1]) for side in ('Left', 'Right')]
        lowest.append(sides)
        mid = (p('LeftUpLeg')[1] + p('RightUpLeg')[1]) / 2
        drops.append(round(mid - min(sides), 2))
    floor = min(min(x) for x in lowest)
    s0 = _positions(clip['samples'][0], names)
    p0 = lambda bone: s0[index[bone]]
    bone = lambda a, b: sum(_len(_sub(p0(side + b), p0(side + a))) for side in ('Left', 'Right')) / 2
    out = {
        'name': name,
        'source': clip.get('source', ''),
        'fps': clip['fps'],
        **phases(hips, lowest, floor),
        'hips_cm': [round(y, 2) for y in hips],
        'hip_half_width_cm': round(_len(_sub(p0('LeftUpLeg'), p0('RightUpLeg'))) / 2, 2),
        'thigh_cm': round(bone('UpLeg', 'Leg'), 2),
        'shin_cm': round(bone('Leg', 'Foot'), 2),
        'foot_cm': round(bone('Foot', 'ToeBase'), 2),
        'left_leads': p0('LeftFoot')[2] > p0('RightFoot')[2],
        'drops': drops,
        'frames': frames,
    }
    out['leg_length_cm'] = round(out['thigh_cm'] + out['shin_cm'], 2)
    return out


# The tuck sums, as src/jump_warp.h has them (tucked, dropBelowHips, flightRise, footClearance, maxTuck).
def tucked(f0, f, s):
    out = [list(v) for v in f]
    for k in LEG_SLOTS:
        i = SLOTS.index(k)
        out[i] = _norm([f0[i][c] + s * (f[i][c] - f0[i][c]) for c in range(3)])
    return out


def drop_below_hips(c, f):
    across = f[SLOTS.index('pelvisAcross')]
    low = math.inf
    for sign, side in ((1, 'left'), (-1, 'right')):
        hip = sign * c['hip_half_width_cm'] * across[1]
        ankle = hip + c['thigh_cm'] * f[SLOTS.index(side + 'Thigh')][1] + c['shin_cm'] * f[SLOTS.index(side + 'Shin')][1]
        toe = ankle + c['foot_cm'] * f[SLOTS.index(side + 'Foot')][1]
        low = min(low, ankle, toe)
    return -low


def flight_rise(c, frame, peak_cm):
    t, p, d = c['takeoff'], c['peak'], c['touchdown']
    if t <= frame <= p and p > t:
        u = (frame - t) / (p - t)
        return peak_cm * (1 - (1 - u) ** 2)
    if p < frame <= d:
        u = (frame - p) / (d - p)
        return peak_cm * (1 - u * u)
    return 0.0


def foot_clearance(c, frame, s, peak_cm):
    f0 = c['frames'][0]
    return flight_rise(c, frame, peak_cm) + drop_below_hips(c, f0) - drop_below_hips(c, tucked(f0, c['frames'][frame], s))


def max_tuck(c, peak_cm=PEAK_CM, margin_cm=MARGIN_CM):
    best = 0.0
    for k in range(int(round(1 / TUCK_STEP)) + 1):
        s = k * TUCK_STEP
        if any(foot_clearance(c, f, s, peak_cm) > margin_cm for f in range(c['takeoff'], c['touchdown'] + 1)):
            break
        best = s
    return round(best, 2)


def _num(v):
    return kick_bake._num(v)


def header(tracks: dict) -> str:
    clips = tracks['clips']
    lines = [
        '#pragma once',
        '// Generated by tools/jump_bake.py from tools/jump_clips.json; do not edit.',
        '// Per frame, clip axes (+X left, +Y up, +Z forward), as authored, in Slot order (kick_retarget.h); hipOffset',
        '// is zero (the jump never drives the hips). Drops: the mid-hip above the lowest foot point, cm, as authored.',
        '#include "jump_warp.h"',
        '',
        'namespace fp::jump {',
        f'static_assert(kick::slotCount == {len(SLOTS)}, "jump_bake.py SLOTS and kick_retarget.h Slot disagree");',
    ]
    for c in clips:
        lines.append(f'// {c["source"]}: takeoff {c["takeoff"]}, peak {c["peak"]}, touchdown {c["touchdown"]}, end {c["end"]} '
                     f'(landing dip {c["landing_dip"]}).')
        lines.append(f'inline constexpr kick::Frame {c["name"]}Frames[] = {{')
        for f in c['frames']:
            lines.append('    {{' + ', '.join('{' + ', '.join(_num(x) for x in d) + '}' for d in f) + '}},')
        lines.append('};')
        lines.append(f'inline constexpr float {c["name"]}Drops[] = {{' + ', '.join(_num(x) for x in c['drops']) + '};')
    lines.append('inline constexpr Clip clips[] = {')
    for c in clips:
        lines.append(f'    {{"{c["name"]}", {c["name"]}Frames, {c["name"]}Drops, {len(c["frames"])}, {_num(c["fps"])}, {c["takeoff"]}, '
                     f'{c["peak"]}, {c["touchdown"]}, {c["end"]}, {_num(c["leg_length_cm"])}, {_num(c["hip_half_width_cm"])}, '
                     f'{_num(c["thigh_cm"])}, {_num(c["shin_cm"])}, {_num(c["foot_cm"])}, {"true" if c["left_leads"] else "false"}}},')
    lines.append('};')
    lines.append(f'inline constexpr int clipCount = {len(clips)};')
    lines.append(f'// The largest StandingJumpTuck keeping the lowest foot within {MARGIN_CM:g} cm of the ground at every flight frame of')
    lines.append(f'// a {PEAK_CM:g} cm hop (jump_bake.py max_tuck; maxTuck here): a geometric estimate from the clip.')
    lines.append(f'inline constexpr float standingTuckDefault = {_num(tracks["standing_tuck_default"])};')
    lines += ['}', '']
    return '\n'.join(lines)


def check_header():
    tracks = json.loads(TRACKS.read_text(encoding='utf-8'))
    clips = tracks['clips']
    if [c['name'] for c in clips] != [n for n, _ in CLIPS]:
        raise ValueError('jump tracks are not the standing and travelling clips in order')
    for c in clips:
        n = len(c['frames'])
        if not (n >= 8 and 0 <= c['takeoff'] < c['peak'] < c['touchdown'] <= c['end'] < n and len(c['drops']) == n
                and 50 < c['leg_length_cm'] < 150):
            raise ValueError('jump track %s has phases out of order or an odd leg' % c['name'])
        if max(range(n), key=lambda f: (c['hips_cm'][f], -f)) != c['peak']:
            raise ValueError('jump track %s peak does not match its hips' % c['name'])
        for f in c['frames']:
            if len(f) != len(SLOTS):
                raise ValueError('jump track %s has the wrong slot count' % c['name'])
            for k, d in zip(SLOTS, f):
                if (k == 'hipOffset' and any(d)) or (k != 'hipOffset' and abs(_len(d) - 1) > 1e-3):
                    raise ValueError('jump track %s %s is not a unit chord (or a zero offset)' % (c['name'], k))
    if abs(max_tuck(clips[0]) - tracks['standing_tuck_default']) > 1e-6:
        raise ValueError('jump standing tuck default is stale; run tools/jump_bake.py bake')
    if HEADER.read_text(encoding='utf-8') != header(tracks):
        raise ValueError('Generated jump header is stale; run tools/jump_bake.py header')
    return len(clips)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('bake')
    b.add_argument('dir', type=Path, nargs='?', default=SOURCES)
    sub.add_parser('header')
    sub.add_parser('check')
    args = ap.parse_args(argv)
    if args.cmd == 'bake':
        clips = []
        for name, stem in CLIPS:
            c = bake(json.loads((args.dir / (stem + '.json')).read_text(encoding='utf-8')), name)
            clips.append(c)
            print(f'{name}: {len(c["frames"])} frames @ {c["fps"]} fps, takeoff {c["takeoff"]}, peak {c["peak"]} '
                  f'(hips {c["hips_cm"][c["peak"]]}), touchdown {c["touchdown"]}, end {c["end"]}, landing dip {c["landing_dip"]}, '
                  f'leg {c["leg_length_cm"]} cm, left leads {c["left_leads"]}')
        tuck = max_tuck(clips[0])
        worst = max(foot_clearance(clips[0], f, tuck, PEAK_CM) for f in range(clips[0]['takeoff'], clips[0]['touchdown'] + 1))
        full = max(foot_clearance(clips[0], f, 1.0, PEAK_CM) for f in range(clips[0]['takeoff'], clips[0]['touchdown'] + 1))
        print(f'standing tuck default {tuck}: worst flight clearance {worst:.1f} cm (as authored, 1.0: {full:.1f} cm)')
        TRACKS.write_text(json.dumps({'standing_tuck_default': tuck, 'clips': clips}, separators=(',', ':')) + '\n',
                          encoding='utf-8', newline='\n')
    elif args.cmd == 'header':
        HEADER.write_text(header(json.loads(TRACKS.read_text(encoding='utf-8'))), encoding='utf-8', newline='\n')
        print(HEADER)
    else:
        print(f'jump tracks: {check_header()} clips, header consistent')
    return 0


if __name__ == '__main__':
    sys.exit(main())
