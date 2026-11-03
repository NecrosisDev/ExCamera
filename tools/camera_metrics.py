#!/usr/bin/env python3
"""Measure ExCamera diagnostics without changing game state beyond requested controls."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
COUNTERS = (
    "camera_samples", "collision_queries", "collision_mesh_traces",
    "collision_failures", "collision_ns",
)


def parse_status(text: str) -> dict[str, object]:
    match = re.search(r"PID (\d+): (.*)", text.strip())
    if not match:
        raise ValueError(f"Not an ExCamera status line: {text!r}")
    values: dict[str, object] = {}
    for key, value in re.findall(r"(\w+)=([^\s]+)", match.group(2)):
        try:
            values[key] = int(value)
        except ValueError:
            try:
                values[key] = float(value)
            except ValueError:
                values[key] = value
    missing = [key for key in COUNTERS if key not in values]
    if missing:
        raise ValueError(f"Status is missing metrics: {', '.join(missing)}")
    return {"pid": int(match.group(1)), "values": values}


def metric_delta(before: dict[str, object], after: dict[str, object], seconds: float) -> dict[str, object]:
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("Measurement interval must be positive")
    if before["pid"] != after["pid"]:
        raise ValueError("The game process changed during measurement")
    first, second = before["values"], after["values"]
    delta = {key: int(second[key]) - int(first[key]) for key in COUNTERS}
    if any(value < 0 for value in delta.values()):
        raise ValueError("A diagnostic counter regressed; restart the measurement")
    samples, queries, traces = (delta[key] for key in COUNTERS[:3])
    nanoseconds = delta["collision_ns"]
    result: dict[str, object] = {
        "pid": after["pid"],
        "mode": second.get("mode"),
        "level_available": second.get("level_available"),
        "query_failed": second.get("query_failed"),
        "interval_seconds": seconds,
        **{f"delta_{key}": value for key, value in delta.items()},
        "camera_samples_per_second": samples / seconds,
        "collision_queries_per_second": queries / seconds,
        "collision_mesh_traces_per_second": traces / seconds,
        "collision_us_per_sample": nanoseconds / samples / 1000 if samples else None,
        "collision_us_per_query": nanoseconds / queries / 1000 if queries else None,
        "queries_per_sample": queries / samples if samples else None,
        "mesh_traces_per_query": traces / queries if queries else None,
        "clearance": second.get("clearance"),
        "applied_distance": second.get("applied_distance"),
        "ground_available": second.get("ground_available"),
        "support_height": second.get("support_height"),
        "minimum_camera_y": second.get("minimum_camera_y"),
        "camera_y": second.get("camera_y"),
    }
    return result


def seconds(value: str) -> float:
    try:
        result = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Must be a number") from error
    if not math.isfinite(result) or not 0 < result <= 60:
        raise argparse.ArgumentTypeError("Must be between 0 and 60 seconds")
    return result


def distance(value: str) -> float:
    try:
        result = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Must be a number") from error
    if not math.isfinite(result) or not 0 <= result <= 1200:
        raise argparse.ArgumentTypeError("Must be between 0 and 1200 centimetres")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", type=Path, default=ROOT / "build" / "rebuild-package" / "ExCamera.exe")
    parser.add_argument("--seconds", type=seconds, default=2.0)
    parser.add_argument("--mode", choices=("first", "third", "overhead"))
    parser.add_argument("--look", nargs=2, type=float, metavar=("YAW", "PITCH"))
    parser.add_argument("--distance", type=distance)
    args = parser.parse_args()
    launcher = args.launcher.resolve()

    def call(*arguments: str) -> str:
        result = subprocess.run([str(launcher), *arguments], capture_output=True, text=True, timeout=40)
        if result.returncode:
            raise RuntimeError(result.stdout.strip() or result.stderr.strip())
        return result.stdout

    def snapshot() -> dict[str, object]:
        return parse_status(call("--camera-status"))

    if args.mode:
        call("--mode", args.mode)
    if args.look:
        if not all(math.isfinite(value) for value in args.look):
            parser.error("Look angles must be finite")
        call("--look", *(str(value) for value in args.look))
    if args.distance is not None:
        call("--distance", str(args.distance))
    before = snapshot()
    started = time.perf_counter()
    time.sleep(args.seconds)
    after = snapshot()
    elapsed = time.perf_counter() - started
    print(json.dumps(metric_delta(before, after, elapsed), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
