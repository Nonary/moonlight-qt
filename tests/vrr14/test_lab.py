#!/usr/bin/env python3
"""Integration tests for native replay, virtual time, and concurrent resume."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

lab = Path(sys.argv[1]).resolve()
repo = Path(__file__).resolve().parents[2]


def run(*args):
    process = subprocess.run([str(lab), *map(str, args)], capture_output=True, text=True, check=True)
    return json.loads(process.stdout)


with tempfile.TemporaryDirectory(prefix="vrr14-") as directory:
    root = Path(directory)
    capture = root / "ring.vrr14"
    first = run("simulate", "--frames", 6000, "--seed", 17, "--capture", capture)
    second = run("simulate", "--frames", 6000, "--seed", 17)
    assert first == second
    assert capture.stat().st_size == 32 + 32768 * 128
    exact = run("replay", capture)
    measured = run("measure", capture)
    assert measured["mode"] == "measured-capture" and measured["intervals"] > 256
    assert measured["target_99_95_met"] is None  # Less than five minutes is not a failed goal.
    assert measured["score_kind"] == "source_cadence_diagnostic"
    assert measured["hitches"] >= 0 and measured["feedback_coverage_percent"] >= 90
    assert exact["divergences"] == 0 and exact["checkpoints"] > 1
    assert exact["arrivals"] < 6000  # This exercised overwrite and checkpoint recovery.
    missing = run("simulate", "--frames", 500, "--feedback-loss-percent", 100)
    assert missing["smoothness_percent"] is None and missing["tracking_decisions"] == 0
    fixed = run("simulate", "--frames", 500, "--fixed-display", 1)
    assert fixed["displayed"] + fixed["dropped"] == fixed["input_frames"]
    baseline = run("simulate", "--frames", 800, "--jitter-us", 0, "--wake-us", 0, "--compositor-us", 0)
    assert baseline["dropped"] == 0 and baseline["latency_p95_ms"] < 1000 / 116 + 5
    settled = run("simulate", "--frames", 45000, "--source-hz", 100, "--render-us", 1000,
                  "--jitter-us", 0, "--wake-us", 0, "--compositor-us", 0,
                  "--feedback-us", 0, "--floor-hz", 0, "--stall-us", 0)
    assert settled["dropped"] == 0
    assert settled["learned_deadline_error_ms"] <= 0.5
    assert settled["reserve_target_ms"] <= 0.25  # No mandatory 2 ms cushion or learned pacing waits.
    assert settled["latency_p95_ms"] < 2.5
    cold = run("simulate", "--frames", 6000, "--source-hz", 116, "--render-us", 4000,
               "--initial-render-us", 285000, "--jitter-us", 0, "--wake-us", 0,
               "--compositor-us", 0, "--feedback-us", 0, "--floor-hz", 0, "--stall-us", 0)
    assert cold["displayed"] > 5900
    assert cold["latency_p95_ms"] < 20
    assert cold["learned_deadline_error_ms"] < 10
    # Captured first-run failure: render cost and compositor phases must not
    # force a slow-tail delay into every frame. The old controller exceeded
    # 22 ms p95 here even with zero receiver jitter.
    costly = run("simulate", "--frames", 800, "--source-hz", 90, "--render-us", 6000,
                 "--jitter-us", 0, "--wake-us", 50, "--compositor-us", 1000,
                 "--feedback-us", 2000, "--fixed-display", 1, "--floor-hz", 0,
                 "--swapchain-images", 3)
    assert costly["dropped"] == 0 and costly["latency_p95_ms"] < 1000 / 90 + 18
    # Capturing and resimulating the same work must not charge image acquisition
    # and overlapping GPU waits a second time.
    pipeline_args = ["--frames", 800, "--source-hz", 100, "--render-us", 6000,
                     "--jitter-us", 0, "--wake-us", 0, "--compositor-us", 1000,
                     "--feedback-us", 2000, "--fixed-display", 1, "--floor-hz", 0,
                     "--swapchain-images", 2, "--stall-us", 0]
    pipeline = run("simulate", *pipeline_args, "--capture", capture)
    assert run("simulate", *pipeline_args, "--input", capture) == pipeline
    config = root / "smooth.conf"
    config.write_text("smooth_cadence=1\ncadence_slew_us=50\n")
    run("simulate", "--frames", 1200, "--config", config, "--capture", capture)
    assert run("replay", capture)["divergences"] == 0
    broken = root / "broken.vrr14"
    broken.write_bytes(capture.read_bytes()[:-1])
    assert subprocess.run([str(lab), "replay", str(broken)], capture_output=True).returncode != 0
    grid = root / "grid.json"
    grid.write_text(json.dumps({"guard_us": [50, 100], "smooth_cadence": [0, 1]}))
    commands = [sys.executable, str(repo / "scripts/vrr_sweep.py"), "--lab", str(lab), "--grid", str(grid),
                "--output", str(root / "sweep"), "--frames", "600", "--jobs", "3", "--seeds", "1,2",
                "--validation-seeds", "101"]
    subprocess.run(commands, check=True)
    jobs = {p.name: p.read_bytes() for p in (root / "sweep").glob("*.json") if p.name != "summary.json"}
    subprocess.run(commands + ["--resume"], check=True)
    assert jobs == {p.name: p.read_bytes() for p in (root / "sweep").glob("*.json") if p.name != "summary.json"}
    summary = json.loads((root / "sweep/summary.json").read_text())
    assert summary["failed_jobs"] == 0 and summary["validation"]
    # Changing job concurrency/order does not change any numerical result.
    commands[commands.index("--output") + 1] = str(root / "serial")
    commands[commands.index("--jobs") + 1] = "1"
    subprocess.run(commands, check=True)
    serial = json.loads((root / "serial/summary.json").read_text())
    assert summary["training"] == serial["training"] and summary["validation"] == serial["validation"]
print("Native replay, ring recovery, virtual display, missing feedback, concurrent determinism and resume checks passed")
