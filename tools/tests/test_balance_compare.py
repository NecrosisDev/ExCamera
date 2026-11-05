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
rate.fall_predicted_per_fighter_minute=0.3
rate.fall_unpredicted_per_fighter_minute=0.1
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

    def test_same_session_passes_and_falls_need_a_band(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            rows = balance_compare.compare(balance_compare.read(base), balance_compare.read(base), None)
            verdicts = {name: verdict for name, _, _, verdict, _ in rows}
            self.assertNotIn('FAIL', verdicts.values())
            self.assertEqual(verdicts['falls /min'], 'SET')
            self.assertEqual(balance_compare.main([base, base, '--fall-band', '0', '1']), 0)

    def test_worse_problem_rate_or_any_fault_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            base = self.write(folder, 'a.txt', BASE)
            worse = self.write(folder, 'b.txt', BASE.replace('rate.floating_per_fighter_minute=0.2',
                                                             'rate.floating_per_fighter_minute=0.5'))
            faulted = self.write(folder, 'c.txt', BASE.replace('fault=0', 'fault=1'))
            self.assertEqual(balance_compare.main([base, worse, '--fall-band', '0', '1']), 1)
            self.assertEqual(balance_compare.main([base, faulted, '--fall-band', '0', '1']), 1)


if __name__ == '__main__':
    unittest.main()
