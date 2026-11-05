"""Compare two balance shadow summaries: the game's own balance (baseline) against a candidate.

Run: python tools/balance_compare.py <baseline-summary.txt> <candidate-summary.txt>

Each summary is a balance-<stamp>-summary.txt that the balance shadow writes beside first-person.ini
(Documents\\ExCamera\\balance\\, or a portable copy's own balance folder). Rates are per standing
fighter-minute, so sessions of different length compare. The rules are docs/BALANCE-SHADOW.md, "Comparing":
every problem rate (falls by rule included) and the game's own physics reports must be no worse than the
baseline, resilience (stumbles caught against real falls) must be no lower, and the cost must stay within its
budget. There is no target fall rate: a fighter should fall only when it physically can't recover. Exit code 1
on any FAIL.
Standard library only.
"""
import argparse
import sys

# Problem rates: the candidate must not exceed the baseline by more than this (counts are small and noisy).
PROBLEMS = ('non_finite', 'flung', 'floating', 'shaking', 'stuck_down', 'failed_get_up', 'fall_by_rule')
RESILIENCE_SLACK = 0.02  # the share of stumbles caught may not drop by more than this
TOLERANCE = 0.05  # per fighter-minute
COST_BUDGET = 1.10  # p95 cost may grow by 10%


def read(path):
    values = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            key, value = line.split('=', 1)
            try:
                values[key] = float(value)
            except ValueError:
                values[key] = value
    return values


def compare(base, cand):
    rows = []

    def row(name, b, c, ok, note=''):
        rows.append((name, b, c, 'PASS' if ok else 'FAIL', note))

    for key in ('fault', 'read_faults'):
        c = cand.get(key, 0)
        row(key, base.get(key, 0), c, c == 0, 'must be 0')
    for name in PROBLEMS:
        key = 'rate.%s_per_fighter_minute' % name
        b, c = base.get(key, 0.0), cand.get(key, 0.0)
        row(name + ' /min', b, c, c <= b + TOLERANCE, '<= baseline + %.2f' % TOLERANCE)
    bm = max(base.get('standing_fighter_minutes', 0.0), 1e-9)
    cm = max(cand.get('standing_fighter_minutes', 0.0), 1e-9)
    b, c = base.get('game_physics_reports', 0) / bm, cand.get('game_physics_reports', 0) / cm
    row('game physics reports /min', b, c, c <= b + TOLERANCE, '<= baseline')
    for key in ('game_update_us.p95', 'shadow_cost_us.p95'):
        b, c = base.get(key, 0.0), cand.get(key, 0.0)
        row(key, b, c, c <= max(b * COST_BUDGET, b + 1.0), '<= baseline x %.2f' % COST_BUDGET)
    b, c = base.get('resilience', 1.0), cand.get('resilience', 1.0)
    row('resilience', b, c, c >= b - RESILIENCE_SLACK, 'stumbles caught / (caught + real falls) >= baseline')
    for key, note in (('rate.recovered_per_fighter_minute', 'stumbles caught'),
                      ('rate.fall_physical_per_fighter_minute', 'real falls (capture point past the feet)'),
                      ('recovery_depth_cm.p95', 'how far past the feet a caught stumble went')):
        rows.append((key, base.get(key, 0.0), cand.get(key, 0.0), 'INFO', note))
    b, c = base.get('rate.held_up_per_fighter_minute', 0.0), cand.get('rate.held_up_per_fighter_minute', 0.0)
    rows.append(('held up (string) /min', b, c, 'INFO', 'expected to drop once the strings go'))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('baseline')
    parser.add_argument('candidate')
    args = parser.parse_args(argv)
    base, cand = read(args.baseline), read(args.candidate)
    rows = compare(base, cand)
    print('%-28s %12s %12s  %-5s %s' % ('count', 'baseline', 'candidate', '', 'rule'))
    for name, b, c, verdict, note in rows:
        print('%-28s %12.3f %12.3f  %-5s %s' % (name, float(b), float(c), verdict, note))
    print('baseline model: %s, %.1f fighter-minutes; candidate model: %s, %.1f fighter-minutes' % (
        base.get('balance_model', '?'), base.get('standing_fighter_minutes', 0.0),
        cand.get('balance_model', '?'), cand.get('standing_fighter_minutes', 0.0)))
    return 1 if any(verdict == 'FAIL' for _, _, _, verdict, _ in rows) else 0


if __name__ == '__main__':
    sys.exit(main())
