#!/usr/bin/env python3
"""Score an opt-in Moonlight VRR cadence trace.

Capture a trace with:

    MOONLIGHT_VRR_TRACE=/tmp/moonlight-vrr.csv moonlight

The simulator intentionally uses the exact source interval and recovery
spacing selected by the pacer for every recorded frame. That lets a tuning
run answer concrete questions (queue p99, input-to-present p99, recovery
duration, and cadence error) instead of relying on aggregate log snippets.
It does not try to reinvent the full pacer offline; the C++ controller tests
cover deterministic policy transitions while this tool scores real runs.
"""

import argparse
import csv
import json
import math
import statistics
import sys


NUMERIC_FIELDS = (
    "sequence",
    "source_pts90k",
    "decode_done_us",
    "target_us",
    "present_us",
    "queue_age_us",
    "net_render_us",
    "queued_behind",
    "source_interval_us",
    "flip_floor_us",
    "recovery_spacing_us",
    "target_queue_us",
    "raw_demand_us",
    "effective_demand_us",
    "confidence_permille",
    "headroom_permille",
    "phase_advance_us",
    "phase_delay_us",
    "near_buffered",
    "stale_schedule",
    "rush_present",
    "vsync_latched",
)

OPTIONAL_NUMERIC_FIELDS = (
    "prepare_us",
    "protection_age_us",
    "preparation_floor_us",
    "protection_reserve_us",
    "backlog_relief_us",
    "lead_margin_us",
)


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summary(values):
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "mean_ms": statistics.fmean(values) / 1000.0,
        "min_ms": min(values) / 1000.0,
        "p05_ms": percentile(values, 0.05) / 1000.0,
        "p50_ms": percentile(values, 0.50) / 1000.0,
        "p95_ms": percentile(values, 0.95) / 1000.0,
        "p99_ms": percentile(values, 0.99) / 1000.0,
        "max_ms": max(values) / 1000.0,
    }


def parse_trace(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as trace:
        reader = csv.DictReader(trace)
        missing = set(NUMERIC_FIELDS) - set(reader.fieldnames or ())
        if missing:
            raise ValueError("missing trace columns: " + ", ".join(sorted(missing)))
        optional_fields = tuple(
            field for field in OPTIONAL_NUMERIC_FIELDS
            if field in (reader.fieldnames or ())
        )
        parsed_fields = NUMERIC_FIELDS + optional_fields
        for line_number, row in enumerate(reader, start=2):
            try:
                rows.append({field: int(row[field]) for field in parsed_fields})
            except (TypeError, ValueError) as error:
                raise ValueError(f"invalid numeric value on line {line_number}: {error}") from error
    if not rows:
        raise ValueError("trace contains no presented frames")
    return rows


def score(rows):
    queue_age = [row["queue_age_us"] for row in rows]
    queue_target_error = [
        row["queue_age_us"] - row["target_queue_us"]
        for row in rows
    ]
    preparation = [row["prepare_us"] for row in rows if "prepare_us" in row]
    protection_age = [
        row["protection_age_us"]
        for row in rows
        if "protection_age_us" in row
    ]
    preparation_floor = [
        row["preparation_floor_us"]
        for row in rows
        if "preparation_floor_us" in row
    ]
    protection_reserve = [
        row["protection_reserve_us"]
        for row in rows
        if "protection_reserve_us" in row
    ]
    protection_target_error = [
        row["protection_age_us"] - row["protection_reserve_us"]
        for row in rows
        if "protection_age_us" in row and "protection_reserve_us" in row
    ]
    backlog_relief = [
        row["backlog_relief_us"]
        for row in rows
        if "backlog_relief_us" in row
    ]
    lead_margin = [
        row["lead_margin_us"]
        for row in rows
        if "lead_margin_us" in row
    ]
    end_to_end = [
        row["present_us"] - row["decode_done_us"]
        for row in rows
        if row["present_us"] >= row["decode_done_us"] > 0
    ]
    present_pairs = [
        (rows[index], rows[index]["present_us"] - rows[index - 1]["present_us"])
        for index in range(1, len(rows))
        if rows[index]["present_us"] >= rows[index - 1]["present_us"] > 0
    ]
    present_intervals = [interval for _, interval in present_pairs]
    cadence_error = [
        abs(interval - row["source_interval_us"])
        for row, interval in present_pairs
        if row["source_interval_us"] > 0
    ]
    steady_cadence_error = [
        abs(interval - row["source_interval_us"])
        for row, interval in present_pairs
        if row["source_interval_us"] > 0 and
        row["queued_behind"] == 0 and
        not row["stale_schedule"] and
        not row["rush_present"]
    ]
    recovery_cadence_error = [
        abs(interval - row["source_interval_us"])
        for row, interval in present_pairs
        if row["source_interval_us"] > 0 and
        (row["queued_behind"] > 0 or
         row["stale_schedule"] or
         row["rush_present"])
    ]
    recovery_frames = [row for row in rows if row["rush_present"]]
    one_frame_recovery_us = []
    for row in rows:
        source = row["source_interval_us"]
        spacing = row["recovery_spacing_us"]
        if source > spacing > 0:
            frames = math.ceil(source / (source - spacing))
            one_frame_recovery_us.append(frames * source)

    inferred_source_gaps = 0
    for previous, current in zip(rows, rows[1:]):
        source = current["source_interval_us"]
        ticks = current["source_pts90k"] - previous["source_pts90k"]
        if source <= 0 or ticks <= 0:
            continue
        delta_us = ticks * 1_000_000 / 90_000
        if delta_us > source * 1.5:
            inferred_source_gaps += max(0, round(delta_us / source) - 1)

    first_present = rows[0]["present_us"]
    last_present = rows[-1]["present_us"]
    duration_us = max(0, last_present - first_present)
    return {
        "frames": len(rows),
        "duration_s": duration_us / 1_000_000.0,
        "queue_age": summary(queue_age),
        "queue_target_error": summary(queue_target_error),
        "renderer_preparation": summary(preparation),
        "protection_age": summary(protection_age),
        "preparation_floor": summary(preparation_floor),
        "protection_reserve": summary(protection_reserve),
        "protection_target_error": summary(protection_target_error),
        "backlog_relief": summary(backlog_relief),
        "lead_margin": summary(lead_margin),
        "decode_to_present": summary(end_to_end),
        "present_interval": summary(present_intervals),
        "absolute_cadence_error": summary(cadence_error),
        "steady_cadence_error": summary(steady_cadence_error),
        "recovery_cadence_error": summary(recovery_cadence_error),
        "smooth_recovery_model": summary(one_frame_recovery_us),
        "recovery_frames": len(recovery_frames),
        "stale_frames": sum(row["stale_schedule"] for row in rows),
        "latched_frames": sum(row["vsync_latched"] for row in rows),
        "inferred_source_frame_gaps": inferred_source_gaps,
    }


def print_human(result):
    print(f"frames: {result['frames']}  duration: {result['duration_s']:.3f}s")
    for label, values in (
        ("queue age", result["queue_age"]),
        ("queue target error (+ is excess latency)", result["queue_target_error"]),
        ("renderer preparation", result["renderer_preparation"]),
        ("actual protection age", result["protection_age"]),
        ("preparation floor", result["preparation_floor"]),
        ("protection reserve", result["protection_reserve"]),
        ("protection target error (+ is extra reserve)", result["protection_target_error"]),
        ("backlog relief", result["backlog_relief"]),
        ("render lead margin", result["lead_margin"]),
        ("decode to present", result["decode_to_present"]),
        ("present interval", result["present_interval"]),
        ("absolute cadence error", result["absolute_cadence_error"]),
        ("steady cadence error", result["steady_cadence_error"]),
        ("recovery cadence error", result["recovery_cadence_error"]),
        ("one-frame smooth recovery model", result["smooth_recovery_model"]),
    ):
        if values["count"]:
            if "error" in label:
                print(
                    f"{label}: p05 {values['p05_ms']:.3f} ms  "
                    f"p50 {values['p50_ms']:.3f} ms  "
                    f"p95 {values['p95_ms']:.3f} ms"
                )
            else:
                print(
                    f"{label}: p50 {values['p50_ms']:.3f} ms  "
                    f"p95 {values['p95_ms']:.3f} ms  "
                    f"p99 {values['p99_ms']:.3f} ms  "
                    f"max {values['max_ms']:.3f} ms"
                )
    print(
        "recovery frames: {recovery_frames}  stale frames: {stale_frames}  "
        "latched frames: {latched_frames}  inferred source gaps: "
        "{inferred_source_frame_gaps}".format(**result)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="CSV path supplied to MOONLIGHT_VRR_TRACE")
    parser.add_argument("--json", action="store_true", help="emit machine-readable metrics")
    args = parser.parse_args()
    try:
        result = score(parse_trace(args.trace))
    except (OSError, ValueError) as error:
        print(f"vrr-latency-sim: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_human(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
