import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("camera_metrics", ROOT / "tools" / "camera_metrics.py")
metrics = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(metrics)


class CameraMetricsTests(unittest.TestCase):
    def test_parse_and_calculate_rates(self):
        text = ("PID 42: mode=third level_available=1 query_failed=0 collision_queries=20 "
                "collision_failures=0 camera_samples=10 collision_mesh_traces=80 "
                "collision_ns=40000 clearance=240 applied_distance=240 pivot=1,2,3 back=0,0,1 abi=1\n")
        before = metrics.parse_status(text)
        text = text.replace("collision_queries=20", "collision_queries=40")
        text = text.replace("camera_samples=10", "camera_samples=30")
        text = text.replace("collision_mesh_traces=80", "collision_mesh_traces=160")
        text = text.replace("collision_ns=40000", "collision_ns=240000")
        after = metrics.parse_status(text)
        result = metrics.metric_delta(before, after, 2.0)
        self.assertEqual(result["delta_camera_samples"], 20)
        self.assertEqual(result["delta_collision_queries"], 20)
        self.assertEqual(result["delta_collision_mesh_traces"], 80)
        self.assertEqual(result["camera_samples_per_second"], 10)
        self.assertEqual(result["collision_us_per_query"], 10)
        self.assertEqual(result["collision_us_per_sample"], 10)
        self.assertEqual(result["queries_per_sample"], 1)
        self.assertEqual(result["mesh_traces_per_query"], 4)

    def test_idle_interval_has_null_rates_not_division_errors(self):
        text = ("PID 42: mode=first level_available=0 query_failed=0 collision_queries=0 "
                "collision_failures=0 camera_samples=0 collision_mesh_traces=0 "
                "collision_ns=0 clearance=0 applied_distance=0 abi=1\n")
        snapshot = metrics.parse_status(text)
        result = metrics.metric_delta(snapshot, snapshot, 1.0)
        self.assertIsNone(result["collision_us_per_sample"])
        self.assertIsNone(result["collision_us_per_query"])
        self.assertIsNone(result["queries_per_sample"])

    def test_regressions_and_missing_metrics_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing metrics"):
            metrics.parse_status("PID 42: mode=third")
        with self.assertRaisesRegex(ValueError, "interval"):
            metrics.metric_delta({}, {}, 0)
        before = {"pid": 42, "values": dict.fromkeys(metrics.COUNTERS, 10)}
        after = {"pid": 42, "values": dict.fromkeys(metrics.COUNTERS, 9)}
        with self.assertRaisesRegex(ValueError, "regressed"):
            metrics.metric_delta(before, after, 1)


if __name__ == "__main__":
    unittest.main()
