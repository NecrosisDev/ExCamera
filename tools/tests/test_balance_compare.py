"""Standard-library tests for tools/balance_compare.py."""
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import balance_compare  # noqa: E402

BASE = """# summary
balance_model=game
standing_fighter_minutes=10
rate.floating_per_fighter_minute=0.2
rate.fall_physical_per_fighter_minute=0.3
rate.fall_by_rule_per_fighter_minute=0.1
rate.recovered_per_fighter_minute=2.7
resilience=0.9
rate.held_up_per_fighter_minute=2
game_physics_reports=1
game_update_us.p95=40
shadow_cost_us.p95=10
fault=0
read_faults=0
"""


class BalanceCompareTests(unittest.TestCase):
    def write(self, folder, name, text):
        path = Path(folder) / name
        path.write_text(text, encoding='utf-8')
        return str(path)

    def test_same_session_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            rows = balance_compare.compare(balance_compare.read(base), balance_compare.read(base))
            self.assertNotIn('FAIL', [verdict for _, _, _, verdict, _ in rows])
            self.assertEqual(balance_compare.main([base, base]), 0)

    def test_less_resilience_or_more_falls_by_rule_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            weaker = self.write(folder, 'b.txt', BASE.replace('resilience=0.9', 'resilience=0.8'))
            ruled = self.write(folder, 'c.txt', BASE.replace('rate.fall_by_rule_per_fighter_minute=0.1',
                                                             'rate.fall_by_rule_per_fighter_minute=0.3'))
            self.assertEqual(balance_compare.main([base, weaker]), 1)
            self.assertEqual(balance_compare.main([base, ruled]), 1)

    def test_kick_breakdown_lines_change_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            tagged = self.write(folder, 'b.txt', BASE + 'event.fall_by_rule.kick=1\nevent.recovered.kick=3\n'
                                'kick_rows_dropped=0\n')
            plain = balance_compare.compare(balance_compare.read(base), balance_compare.read(base))
            with_kick = balance_compare.compare(balance_compare.read(base), balance_compare.read(tagged))
            self.assertEqual(plain, with_kick)
            self.assertEqual(balance_compare.main([base, tagged]), 0)

    def test_worse_problem_rate_or_any_fault_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            worse = self.write(folder, 'b.txt', BASE.replace('rate.floating_per_fighter_minute=0.2',
                                                             'rate.floating_per_fighter_minute=0.5'))
            faulted = self.write(folder, 'c.txt', BASE.replace('fault=0', 'fault=1'))
            self.assertEqual(balance_compare.main([base, worse]), 1)
            self.assertEqual(balance_compare.main([base, faulted]), 1)


if __name__ == '__main__':
    unittest.main()
