#!/usr/bin/env python3
"""Audit observed presentation against the FIRST recorded plan, before retiming.

This is a read-only capture audit. It does not infer optical smoothness, score
missing feedback as success, or simulate the effect of a different scheduler.
"""
import argparse
import json
import math
from pathlib import Path
import statistics
from vrr_trace import records


def summarize(values):
    if not values:
        return None
    values = sorted(values)
    return {"mean_ms": statistics.mean(values) / 1e6,
            "p95_ms": values[math.ceil(len(values) * .95) - 1] / 1e6,
            "p99_ms": values[math.ceil(len(values) * .99) - 1] / 1e6}


def audit(events):
    frames = {}
    first_at = last_at = None
    for event in events:
        at = event["observed_ns"]
        first_at = at if first_at is None else min(first_at, at)
        last_at = at if last_at is None else max(last_at, at)
        row = frames.setdefault(event["frame_id"], {})
        # prepared_plan events do not overwrite the original target.
        row.setdefault(event["event"], event)
    series = {key: [] for key in ("queue_wait", "preparation", "gpu_completion_wait",
              "post_ready_wait", "decode_to_presentation", "initial_target_error",
              "final_target_error", "target_shift")}
    seen = known = measured = early = late = final_late = moved = 0
    for frame_id, row in sorted(frames.items()):
        submitted = row.get("submit")
        if not submitted or not submitted["success"]:
            continue
        seen += 1
        feedback = row.get("feedback")
        if not feedback:
            continue
        if feedback["outcome"] == 1:  # Confirmed discards are separate delivery failures.
            known += 1
            continue
        if (feedback["outcome"] != 0 or feedback["quality"] == 0 or
                not 0 <= feedback["uncertainty_ns"] <= 500000 or
                not submitted["observed_ns"] <= feedback["presentation_ns"] <= feedback["observed_ns"]):
            continue
        known += 1
        if not all(key in row for key in ("arrival", "plan", "render")):
            continue
        a, p, r = (row[key] for key in ("arrival", "plan", "render"))
        shown = feedback["presentation_ns"]
        original = p.get("original_deadline_ns") or p["predicted_scanout_ns"]
        execution = submitted.get("execution_target_ns") or submitted["predicted_scanout_ns"]
        initial_error = shown - original
        final_error = shown - execution
        shift = execution - original
        measured += 1
        late += initial_error > 3000000
        early += initial_error < -3000000
        final_late += final_error > 3000000
        moved += shift > 3000000
        for key, value in (("initial_target_error", initial_error), ("final_target_error", final_error),
                           ("target_shift", shift), ("decode_to_presentation", shown - a["decoded_ns"]),
                           ("queue_wait", max(0, r["started_ns"] - a["decoded_ns"]) +
                            max(0, submitted["observed_ns"] - r["observed_ns"])),
                           ("preparation", r["observed_ns"] - r["started_ns"]),
                           ("post_ready_wait", max(0, submitted["observed_ns"] - r["observed_ns"]))):
            series[key].append(value)
        stages = row.get("prepare_stages")
        if stages and stages["gpu_ready_ns"] >= stages["commands_submitted_ns"] > 0:
            series["gpu_completion_wait"].append(stages["gpu_ready_ns"] - stages["commands_submitted_ns"])
    return {"captured_seconds": (last_at - first_at) / 1e9 if first_at is not None else 0,
            "submitted_frames": seen, "resolved_feedback_frames": known,
            "paired_frames": measured, "late_over_3ms_initial_target": late,
            "early_over_3ms_initial_target": early, "late_over_3ms_final_target": final_late,
            "targets_shifted_later_over_3ms": moved,
            "note": "Retained correlated frames only; not a five-minute goal or perceptual score.",
            "timings": {key: summarize(value) for key, value in series.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(audit(records(args.capture)), indent=2) + "\n"
    if args.output:
        args.output.write_text(report)
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
