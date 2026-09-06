#!/usr/bin/env python3
"""Inspect a bounded VRR14 flight recorder; optionally export named JSON events."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

FIELDS = {
    1: ("config", []),
    2: ("arrival", ["rtp_90khz", "received_ns", "assembled_ns", "decoded_ns", "source_ns", "playout_ns"]),
    3: ("plan", ["prepare_ns", "submit_ns", "predicted_scanout_ns", "earliest_ns", "latest_ns", "uncertainty_ns",
                 "buffer_ns", "render_budget_ns", "compositor_lead_ns", "mode", "below_range", "original_deadline_ns"]),
    4: ("prepared_plan", ["prepare_ns", "submit_ns", "predicted_scanout_ns", "earliest_ns", "latest_ns", "uncertainty_ns",
                          "buffer_ns", "render_budget_ns", "compositor_lead_ns", "mode", "below_range", "original_deadline_ns"]),
    5: ("render", ["started_ns", "duration_ns", "success", "dispatch_delay_ns", "probe_decoded_ns",
                   "probe_residual_ns", "probe_applied_ns", "probe_typical_ns", "probe_period_ns", "probe_recovery",
                   "metric_version", "cpu_completed_ns"]),
    6: ("wake", ["deadline_ns", "error_ns"]),
    7: ("submit", ["predicted_scanout_ns", "returned_ns", "success", "deadline_ns", "uncertainty_ns", "execution_target_ns", "gpu_ready_ns"]),
    8: ("feedback", ["refresh_sequence", "presentation_ns", "uncertainty_ns", "refresh_prediction_ns", "output_id",
                     "native_flags", "quality", "outcome", "used_by_controller"]),
    9: ("drop", ["reason", "decoded_ns", "rtp_90khz", "received_ns", "assembled_ns", "queue_depth"]),
    10: ("reset", []), 11: ("stop", []), 12: ("checkpoint_begin", ["word_count"]),
    13: ("checkpoint_data", [f"word_{i}" for i in range(12)]), 14: ("checkpoint_end", []),
    15: ("prepare_stages", ["started_ns", "acquired_ns", "commands_submitted_ns", "gpu_not_ready_ns", "gpu_ready_ns"]),
    16: ("build", [f"sha256_word_{i}" for i in range(4)]),
    17: ("overlay_work", ["overlay_type", "revision", "queue_ns", "raster_ns", "renderer_dispatch_ns"]),
    18: ("recovery", ["flags", "source_period_ns", "excluded_frames_remaining"]),
    19: ("reserve", ["usual_ns", "temporary_boost_ns", "requested_ns", "observations", "evidence", "validation_frames", "reliable", "misses", "live_duration_ns", "metric_version", "miss_tolerance_ns"]),
}


def records(path):
    with Path(path).open("rb") as capture:
        data = capture.read(32 + 32768 * 128 + 1)
    if len(data) < 32 or data[:8] != b"MLVRR14\0":
        raise ValueError("invalid capture header")
    version, count, overwritten = struct.unpack_from("<QQQ", data, 8)
    if version != 1 or count > 32768 or len(data) != 32 + count * 128:
        raise ValueError("invalid capture size/version")
    for index in range(count):
        serial, event, frame, at, *values = struct.unpack_from("<QQQq12q", data, 32 + index * 128)
        if serial != overwritten + index + 1 or event not in FIELDS:
            raise ValueError("invalid event or sequence")
        name, fields = FIELDS[event]
        result = dict(serial=serial, event=name, frame_id=frame, observed_ns=at, **dict(zip(fields, values)))
        if event == 16:
            result["binary_sha256"] = struct.pack("<4q", *values[:4]).hex()
        if event in (3, 4):
            result["mode_name"] = {0: "acquiring", 1: "tracking", 2: "stale"}.get(result["mode"], "invalid")
        if event == 8:
            result["quality_name"] = {0: "unavailable", 1: "compositor", 2: "hardware"}.get(result["quality"], "invalid")
            result["outcome_name"] = {0: "presented", 1: "discarded", 2: "unavailable", 3: "reset"}.get(result["outcome"], "invalid")
        if event == 9:
            result["reason_name"] = {0: "capacity", 1: "stale", 2: "prepare_failed", 3: "present_failed", 4: "shutdown"}.get(result["reason"], "invalid")
        if event == 18:
            result["reasons"] = [name for bit, name in enumerate(("new_epoch", "source_discontinuity", "arrival_stall",
                "burst_excluded", "rate_changed", "phase_restarted", "clock_restarted", "uncertain_completion",
                "frames_skipped")) if result["flags"] & (1 << bit)]
        yield result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture")
    parser.add_argument("--events", action="store_true", help="write named JSON lines to stdout, including checkpoints")
    args = parser.parse_args()
    counts, drops, outcomes, qualities = Counter(), Counter(), Counter(), Counter()
    first = last = None
    for record in records(args.capture):
        first = record["serial"] if first is None else first
        last = record["serial"]
        counts[record["event"]] += 1
        if record["event"] == "drop": drops[record["reason_name"]] += 1
        if record["event"] == "feedback":
            outcomes[record["outcome_name"]] += 1
            qualities[record["quality_name"]] += 1
        if args.events: print(json.dumps(record, sort_keys=True))
    if not args.events:
        print(json.dumps(dict(first_serial=first, last_serial=last, overwritten_records=(first - 1 if first else 0),
                              events=counts, drop_reasons=drops, feedback_outcomes=outcomes, feedback_quality=qualities),
                         sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
