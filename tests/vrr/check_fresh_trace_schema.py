#!/usr/bin/env python3
"""Check the fresh-capture replay boundary using an exported worker fixture."""

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("replay", type=Path)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    replay = str(args.replay.resolve())
    decoder = Path(__file__).resolve().parents[2] / "scripts/decode-vrr-trace.py"

    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        summary = directory / "baseline.json"
        result = subprocess.run([replay, str(args.trace.resolve()), "--require-exact-baseline",
                                 "--output", str(summary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"fresh baseline failed: {result.stderr}")
        data = json.loads(summary.read_text())
        assert data["capture"]["schema"] == 6
        assert data["capture"]["recorded_sequence_integrity_valid"]
        assert data["fidelity"]["baseline_exact"]

        decoded = directory / "capture.csv"
        subprocess.run([sys.executable, str(decoder), str(args.trace.resolve()), str(decoded)], check=True)
        lines = decoded.read_text().splitlines()
        header = next(csv.reader([lines[0]]))
        recorded = next(csv.reader([lines[1]]))
        retired = {
            "additional_queued_frame", "missed_ticks", "param_render_lead_ceiling_us",
            "param_pacing_latency_queue_mode_extra", "param_latch_base_guard_exit",
            "param_playout_responsive_buffer", "param_playout_serial_service_gate",
            "param_playout_recent_pressure_release", "param_playout_hold_renew_below_target",
            "param_playout_input_clock_relative_history", "param_playout_input_rate_drop_snap",
            "param_playout_input_bounded_recovery_credit", "param_playout_absorbed_jitter_release",
            "input_release_allowed", "input_percentile_release_factor_ppm",
            "input_percentile_hold_until_us", "input_percentile_proposed_misses",
        }
        assert retired.isdisjoint(header), "retired policy fields remain in the trace"
        session_summary = directory / "session.json"
        session = subprocess.run([replay, str(args.trace.resolve()), "--output", str(session_summary)],
                                 capture_output=True, text=True)
        if session.returncode:
            raise AssertionError(f"session-policy replay failed: {session.stderr}")
        current = json.loads(session_summary.read_text())["simulation"]["resolved_parameters"]["controller"]
        for column, value in zip(header, recorded):
            if column.startswith("param_"):
                name = column[len("param_"):]
                assert int(value) == int(current[name]), f"session policy differs for {name}"

        old = directory / "old.csv"
        old.write_text("\n".join([lines[0]] + ["5," + row[2:] if row.startswith("6,") else row
                                                 for row in lines[1:]]) + "\n")
        rejected = subprocess.run([replay, str(old)], capture_output=True, text=True)
        assert rejected.returncode and "capture a fresh schema 6 trace" in rejected.stderr

        retired_column = header.index("param_playout_input_goal_window_us")
        retired = directory / "retired-policy.csv"
        with retired.open("w", newline="") as output:
            writer = csv.writer(output, lineterminator="\n")
            for index, line in enumerate(lines):
                if line.startswith("#"):
                    output.write(line + "\n")
                else:
                    row = next(csv.reader([line]))
                    if index:
                        row[retired_column] = "0"
                    writer.writerow(row)
        rejected = subprocess.run([replay, str(retired)], capture_output=True, text=True)
        assert rejected.returncode and "retired VRR policy is unsupported" in rejected.stderr

        column = header.index("param_playout_input_goal_window_us")
        missing = directory / "missing.csv"
        with missing.open("w", newline="") as output:
            writer = csv.writer(output, lineterminator="\n")
            for line in lines:
                if line.startswith("#"):
                    output.write(line + "\n")
                else:
                    row = next(csv.reader([line]))
                    del row[column]
                    writer.writerow(row)
        rejected = subprocess.run([replay, str(missing)], capture_output=True, text=True)
        assert rejected.returncode and "missing captured controller parameter" in rejected.stderr

        column = header.index("input_percentile_target_us")
        missing = directory / "missing-diagnostic.csv"
        with missing.open("w", newline="") as output:
            writer = csv.writer(output, lineterminator="\n")
            for line in lines:
                if line.startswith("#"):
                    output.write(line + "\n")
                else:
                    row = next(csv.reader([line]))
                    del row[column]
                    writer.writerow(row)
        rejected = subprocess.run([replay, str(missing)], capture_output=True, text=True)
        assert rejected.returncode and "missing current policy diagnostic" in rejected.stderr
    print("fresh trace exact replay and schema rejection passed")


if __name__ == "__main__":
    main()
