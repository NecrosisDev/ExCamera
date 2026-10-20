"""Control opt-in ExCamera captures and compare reports. Python 3.8+, standard library only.

Never launches, installs, injects into or changes game memory. Start/stop change only
profiling.ini; the already-loaded profiling DLL services it on its existing worker.
"""
import argparse
import configparser
import ctypes
import json
import math
import os
from pathlib import Path
import tempfile
import time
import uuid


def default_directory():
    if os.name != 'nt':
        raise ValueError('Use --directory off Windows')
    class GUID(ctypes.Structure):
        _fields_ = [('data', ctypes.c_ubyte * 16)]
    folder = GUID((ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID('fdd39ad0-238f-46af-adb4-6c85480369c7').bytes_le))
    result = ctypes.c_void_p()
    hr = ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(result))
    if hr != 0:
        raise OSError('Windows Documents folder unavailable')
    try:
        return Path(ctypes.wstring_at(result)) / 'ExCamera'
    finally:
        ctypes.windll.ole32.CoTaskMemFree(result)


def atomic_text(path, text, encoding='utf-8'):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding=encoding, newline='') as stream:
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def alive(pid):
    if os.name != 'nt':
        return pid == os.getpid()  # portable fixture only
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259
    finally:
        kernel.CloseHandle(handle)


def status(directory):
    result = json.loads((directory / 'profiles/status.json').read_text(encoding='utf-8'))
    if result.get('schema') != 1 or not isinstance(result.get('pid'), int):
        raise ValueError('Unsupported profiler status')
    result['process_alive'] = alive(result['pid'])
    control = configparser.ConfigParser(interpolation=None)
    path = directory / 'profiling.ini'
    if path.exists():
        control.read(path, encoding='utf-16')
        result['requested'] = control.get('Profile', 'Request', fallback='')
        result['pending'] = (control.getboolean('Profile', 'Enabled', fallback=False) and
                             result['requested'] != result.get('request'))
    return result


def start(directory, label, seconds=60, every=64):
    if not 5 <= seconds <= 300 or not 1 <= every <= 4096:
        raise ValueError('Seconds must be 5..300; sample-every must be 1..4096')
    if not 1 <= len(label) <= 80 or any(ord(c) < 32 or c in '[]' for c in label):
        raise ValueError('Label must be 1..80 printable characters without brackets')
    loaded = status(directory)
    if not loaded['process_alive'] or '-prof' not in loaded.get('build', ''):
        raise ValueError('No live profiling build reported; load one normally and wait for activation')
    request = uuid.uuid4().hex
    # PID binding plus expiry prevent a stale enabled request arming another game session.
    text = ('[Profile]\r\nEnabled=1\r\nRequest={}\r\nLabel={}\r\nSeconds={}\r\n'
            'SampleEvery={}\r\nTargetPID={}\r\nNotAfter={}\r\n').format(
                request, label, seconds, every, loaded['pid'], int(time.time()) + 30)
    atomic_text(directory / 'profiling.ini', text, 'utf-16')
    return request


def stop(directory):
    atomic_text(directory / 'profiling.ini', '[Profile]\r\nEnabled=0\r\n', 'utf-16')


def load_report(path):
    report = json.loads(Path(path).read_text(encoding='utf-8'))
    if report.get('schema') != 1:
        raise ValueError('Unsupported report schema')
    if not isinstance(report.get('frequency'), int) or report['frequency'] <= 0:
        raise ValueError('Invalid timer frequency')
    if not isinstance(report.get('sample_every'), int) or not 1 <= report['sample_every'] <= 4096:
        raise ValueError('Invalid sampling interval')
    if not isinstance(report.get('threads'), list):
        raise ValueError('Missing threads')
    for row in report['threads']:
        for key in ('presents', 'frame_intervals', 'frame_ticks', 'max_frame_ticks', 'frame_overflow', 'depth_drops', 'clock_errors'):
            if not isinstance(row.get(key), int) or row[key] < 0:
                raise ValueError('Invalid thread counter: ' + key)
        hist = row.get('frame_histogram', [])
        if len(hist) != 1024 or any(type(x) is not int or x < 0 for x in hist) or sum(hist) != row['frame_intervals']:
            raise ValueError('Invalid frame histogram')
        if len(row.get('modes', [])) != 4 or sum(row['modes']) != row['presents']:
            raise ValueError('Invalid view counts')
        seen = set()
        for hook in row.get('hooks', []):
            if hook['name'] in seen:
                raise ValueError('Duplicate hook')
            seen.add(hook['name'])
            for key in ('calls', 'samples', 'inclusive_ticks', 'self_ticks', 'max_ticks', 'max_self_ticks'):
                if not isinstance(hook.get(key), int) or hook[key] < 0:
                    raise ValueError('Invalid hook counter: ' + key)
            if hook['samples'] > hook['calls'] or hook['self_ticks'] > hook['inclusive_ticks']:
                raise ValueError('Inconsistent hook counters')
    return report


def percentile(row, fraction):
    if not row['frame_intervals']:
        return None
    target, count = math.ceil(row['frame_intervals'] * fraction), 0
    for index, amount in enumerate(row['frame_histogram']):
        count += amount
        if count >= target:
            return (index + 1) * .25 if index < 1023 else float('inf')


def primary(report):
    return max(report['threads'], key=lambda r: r['presents'], default=None)


def estimate(report, hook, frames, field='self_ticks'):
    # Every nested invocation has the root's same independent inclusion probability.
    # Weight observed ticks by N, rather than treating a sparse sample as exact time.
    if not frames or not hook or not hook['samples']:
        return None
    return hook[field] * report['sample_every'] * 1000 / report['frequency'] / frames


def number(value):
    if value is None:
        return 'n/a'
    return '>=255.75' if math.isinf(value) else '{:.4f}'.format(value)


def compare(before, after, top=25):
    rows = [primary(r) for r in (before, after)]
    if any(row is None or not row['presents'] for row in rows):
        raise ValueError('Both captures need owned game presentations')
    output = ['QPC wall time; estimated hook ms per owned presentation. Lower is faster.',
              'Self subtracts instrumented children only. Native/wait rows are separate.',
              'Do not add overlapping inclusive times or sum threads into frame time.',
              'Sampled maxima are observed maxima, not worst-case bounds.']
    warnings = []
    if before['sample_every'] != after['sample_every']:
        warnings.append('Sampling rates differ; repeat with the same sample-every.')
    if before.get('settings_start') != after.get('settings_start'):
        warnings.append('Recorded camera settings differ.')
    if before.get('source_base') != after.get('source_base'):
        warnings.append('Different source bases.')
    if (before.get('variant'), after.get('variant')) != ('previous_release', 'optimized'):
        warnings.append('Expected previous_release then optimized; check report order/build labels.')
    for report, row in zip((before, after), rows):
        label = '{} [{}] {}'.format(report['build'], report.get('label', ''), report.get('request', ''))
        output += ['', label]
        intervals = row['frame_intervals']
        mean = row['frame_ticks'] * 1000 / report['frequency'] / intervals if intervals else None
        output += ['  presents={} render-thread={} mean={}ms FPS={} p50~{}ms p95~{}ms p99~{}ms max={}ms'.format(
            row['presents'], row['id'], number(mean), number(1000 / mean if mean else None),
            number(percentile(row, .5)), number(percentile(row, .95)), number(percentile(row, .99)),
            number(row['max_frame_ticks'] * 1000 / report['frequency']))]
        output += ['  view counts FP/TP/blend/native-or-parked: ' + str(row['modes'])]
        if report.get('settings_changed'):
            warnings.append(report['build'] + ': settings changed during capture.')
        if report.get('reason') != 'complete':
            warnings.append(report['build'] + ': capture ended early (' + str(report.get('reason')) + ').')
        if report.get('thread_drops') or any(t['depth_drops'] or t['clock_errors'] for t in report['threads']):
            warnings.append(report['build'] + ': recorder drops/clock errors; inspect JSON and repeat.')
        if sum(t['presents'] > 0 for t in report['threads']) != 1:
            warnings.append(report['build'] + ': multiple presentation threads; table uses busiest thread only.')
        if row['presents'] < 300:
            warnings.append(report['build'] + ': short capture (<300 presentations).')
    mixes = [[n / row['presents'] for n in row['modes']] for row in rows]
    if any(abs(a - b) > .05 for a, b in zip(*mixes)):
        warnings.append('View-mode mix differs by more than five percentage points.')
    warnings.append('Match scene, route, graphics, supersampling, cap/VSync and all settings manually; recorded settings are partial.')
    maps = [{h['name']: h for h in row['hooks'] if h['kind'] != 'marker'} for row in rows]
    names = set(maps[0]) | set(maps[1])
    names = sorted(names, key=lambda name: max(estimate(r, m.get(name), t['presents']) or 0
                    for r, m, t in zip((before, after), maps, rows)), reverse=True)
    output += ['', 'Render thread: name [kind] | self before -> after (delta) | inclusive before -> after | calls/frame before -> after | samples before/after']
    for name in names[:top]:
        hooks = [m.get(name) for m in maps]
        values = [estimate(r, h, t['presents']) for r, h, t in zip((before, after), hooks, rows)]
        inclusive = [estimate(r, h, t['presents'], 'inclusive_ticks') for r, h, t in zip((before, after), hooks, rows)]
        delta = values[1] - values[0] if all(v is not None for v in values) else None
        samples = [h['samples'] if h else 0 for h in hooks]
        output.append('{} [{}] | {} -> {} ({}) | {} -> {} | {} -> {} | {}/{}{}'.format(
            name, next(h['kind'] for h in hooks if h), number(values[0]), number(values[1]), number(delta),
            number(inclusive[0]), number(inclusive[1]),
            *[number(h['calls'] / t['presents']) if h else 'n/a' for h, t in zip(hooks, rows)],
            *samples, ' SPARSE' if min(samples) < 32 else ''))
    for report, main in zip((before, after), rows):
        for row in report['threads']:
            if row is main:
                continue
            output += ['', '{} other thread {} (may overlap render work):'.format(report['build'], row['id'])]
            hooks = sorted(row['hooks'], key=lambda h: h['self_ticks'], reverse=True)
            for hook in hooks[:min(top, 8)]:
                output.append('  {} [{}] ~{} ms/presentation self; calls={} samples={}{}'.format(
                    hook['name'], hook['kind'], number(estimate(report, hook, main['presents'])), hook['calls'],
                    hook['samples'], ' SPARSE' if hook['samples'] < 32 else ''))
    return '\n'.join(['WARNING: ' + w for w in warnings] + [''] + output) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, help='ExCamera settings directory; default: Windows Documents/ExCamera')
    sub = parser.add_subparsers(dest='command', required=True)
    capture = sub.add_parser('start')
    capture.add_argument('--label', required=True)
    capture.add_argument('--seconds', type=int, default=60)
    capture.add_argument('--sample-every', type=int, default=64)
    sub.add_parser('stop')
    sub.add_parser('status')
    comparison = sub.add_parser('compare')
    comparison.add_argument('before', type=Path)
    comparison.add_argument('after', type=Path)
    comparison.add_argument('--top', type=int, default=25)
    comparison.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.command == 'compare':
        if not 1 <= args.top <= 100:
            parser.error('--top must be 1..100')
        result = compare(load_report(args.before), load_report(args.after), args.top)
        if args.output:
            atomic_text(args.output, result)
        print(result, end='')
        return
    directory = args.directory if args.directory is not None else default_directory()
    if args.command == 'start':
        print('Requested ' + start(directory, args.label, args.seconds, args.sample_every))
        print('Request expires in 30 seconds; status must show this ID as capturing/complete.')
    elif args.command == 'stop':
        stop(directory)
        print('Stop requested; status/report updates after admitted callbacks drain.')
    else:
        print(json.dumps(status(directory), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, configparser.Error) as error:
        raise SystemExit(str(error))
