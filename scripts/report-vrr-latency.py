#!/usr/bin/env python3
"""Report observed VRR latency boundaries and cadence, one capture per row.

Reads .vrrtrace or expanded CSV. This is not a counterfactual replay and does
not infer display latency from CPU submission or compare unlike presets as A/B.
"""
from __future__ import annotations

import argparse
from array import array
from collections import Counter, defaultdict
from contextlib import ExitStack
import csv
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import tempfile
import zlib


MODES = {0: "Smooth", 1: "Balanced Target", 2: "Low Latency"}
SESSION_FIELDS = ("session_latency_mode", "session_readiness_hitch_feedback",
                  "session_latency_oscillation", "latency_test_phase",
                  "calibration_loaded", "initial_cached_samples", "history_version",
                  "stream_rate_hz", "display_refresh_hz", "playout_initial_profile")
BUFFER_ACTIONS = ("learning", "sequence break", "late-work growth", "growth capped",
                  "current error hold", "history hold", "clean-time hold", "releasing",
                  "minimum", "work not absorbable", "growth cooldown", "no fresh late work", "limit changed")
CLIENT_COSTS = ("handoff", "queue_residence", "decode_sync", "controller", "render_wait",
                "preparation", "target_wait", "spacing_wait", "present_call", "other_worker")


def client_costs(row):
    """A non-overlapping partition on one denominator, or unavailable.

    GPU readiness and acquisition are inside preparation. Wake overshoot is
    inside the corresponding wait. Policy padding is not an execution stage.
    """
    keys = ("decoder_output_us", "pacer_arrival_us", "dequeue_us", "decision_us",
            "decision_end_us", "render_wait_entry_us", "render_wait_final_us",
            "prepare_start_us", "prepare_end_us", "target_wait_entry_us",
            "target_wait_final_us", "present_start_us", "present_end_us")
    times = [number(row, k) for k in keys]
    if any(t is None or t <= 0 for t in times) or times != sorted(times):
        return None
    output, arrival, dequeue, decision, decision_end, render_entry, render_end, prep, prep_end, target, target_end, present, end = times
    decode = number(row, "decode_sync_wait_us")
    if decode is None or not 0 <= decode <= decision - dequeue:
        return None
    correction_start, correction_end = (number(row, k) for k in
                                         ("correction_wait_start_us", "correction_wait_end_us"))
    if correction_start is None or correction_end is None:
        return None
    if correction_start == correction_end == 0:
        spacing = 0
    elif target_end <= correction_start <= correction_end <= present:
        spacing = correction_end - correction_start
    else:
        return None
    costs = dict(zip(CLIENT_COSTS[:-1], (arrival - output, dequeue - arrival, decode,
        decision_end - decision, render_end - render_entry, prep_end - prep,
        target_end - target, spacing, end - present)))
    costs["other_worker"] = end - output - sum(costs.values())
    return costs if costs["other_worker"] >= 0 else None


def number(row, key):
    value = row.get(key)
    return int(value) if value not in (None, "") else None


def distribution(values):
    if not values:
        return {"count": 0, "mean": None, "p50": None, "p95": None, "p99": None}
    ordered = sorted(values)
    result = {"count": len(values), "mean": sum(values) / len(values),
              "min": ordered[0], "max": ordered[-1]}
    for name, fraction in (("p50", .5), ("p95", .95), ("p99", .99)):
        result[name] = ordered[math.ceil(len(ordered) * fraction) - 1]
    return result


def summarize(source, path, phase_filter=None, parent=None):
    header_bytes = source.readline()
    if not header_bytes:
        raise ValueError("empty trace")
    columns = next(csv.reader([header_bytes.decode().strip()]))
    if len(set(columns)) != len(columns) or "arrival_sequence" not in columns:
        raise ValueError("missing or duplicate trace columns")
    digest = hashlib.sha256(header_bytes)
    metrics = defaultdict(lambda: array("q"))
    cost_metrics = defaultdict(lambda: array("q"))
    buffer_actions = Counter()
    buffer_events = []
    recent_frame_costs = {}
    buffer_update_rows = 0
    previous_policy = None
    invalid = Counter()
    outcomes = Counter()
    identities = set()
    profiles = set()
    modes = set()
    sequences = set()
    source_points = {}
    phases = set()
    backends = set()
    footer = None
    duplicate_sequences = rows = selected_rows = presented = history_rows = release_allowed = 0
    first_arrival = last_arrival = None
    previous = previous_interval = None
    source_stalls = source_pairs = 0
    session = {}

    def difference(row, name, start, end):
        a, b = number(row, start), number(row, end)
        if a is None or b is None or a <= 0 or b < a:
            invalid[name] += 1
            return None
        metrics[name].append(b - a)
        return b - a

    for raw in source:
        if raw.startswith(b"#vrr_trace_footer,"):
            if footer is not None:
                raise ValueError("duplicate footer")
            footer = dict(item.split("=", 1) for item in raw.decode().strip().split(",")[1:])
            continue
        if footer is not None:
            if raw.strip():
                raise ValueError("rows after clean-close footer")
            continue
        digest.update(raw)
        fields = next(csv.reader([raw.decode().strip()]))
        if len(fields) != len(columns):
            raise ValueError("row does not match header")
        row = dict(zip(columns, fields))
        rows += 1
        seq = number(row, "arrival_sequence")
        if seq is None or seq <= 0:
            raise ValueError("invalid arrival sequence")
        duplicate_sequences += seq in sequences
        sequences.add(seq)
        phase = number(row, "latency_test_phase")
        if number(row, "session_latency_oscillation") == 1 and phase is not None:
            phases.add(phase)
        if phase_filter is not None and phase != phase_filter:
            continue
        selected_rows += 1
        source_points[seq] = (number(row, "frame"), number(row, "rtp_timestamp"), number(row, "rtp_valid"))
        identity = {k: v for k, v in row.items() if k.startswith("param_") or k in SESSION_FIELDS}
        identities.add(hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest())
        if not session:
            session = {k: row.get(k) for k in SESSION_FIELDS if k != "playout_initial_profile"}
        if row.get("playout_initial_profile"):
            profiles.add(hashlib.sha256(row["playout_initial_profile"].encode()).hexdigest())
        mode = number(row, "session_latency_mode")
        if mode is None:
            cap = number(row, "param_playout_delay_cap_source_period_per_mille")
            mode = {500: 2, 1000: 1, 2000: 0}.get(cap)
        modes.add(MODES.get(mode, "Unknown"))
        if number(row, "native_backend_valid") == 1:
            backends.add(number(row, "native_backend"))
        arrival = number(row, "pacer_arrival_us")
        if arrival:
            first_arrival = arrival if first_arrival is None else min(first_arrival, arrival)
            last_arrival = arrival if last_arrival is None else max(last_arrival, arrival)
        outcomes[row.get("disposition", "unknown")] += 1
        if number(row, "history_state_valid") == 1:
            history_rows += 1
            release_allowed += number(row, "history_can_release") == 1
        if number(row, "decision_valid") == 1:
            for key in ("requested_playout_delay_us", "buffer_cap_us", "buffer_queue_limit_us",
                        "buffer_preset_cap_us", "cadence_smoothing_us", "presentation_floor_push_us",
                        "render_lead_us", "gpu_readiness_lead_us", "render_wake_lead_us",
                        "target_wake_lead_us", "guard_us", "render_wait_overshoot_us",
                        "target_wait_overshoot_us", "controller_call_us"):
                value = number(row, key)
                if value is not None:
                    metrics[key].append(value)
            offset = number(row, "playout_offset_us")
            if previous_policy is not None and offset is not None and not number(row, "rebased"):
                metrics["source_mapping_change_us"].append(offset - previous_policy)
            previous_policy = offset
            target, original, floor = (number(row, k) for k in
                                       ("target_us", "original_target_us", "presentation_floor_push_us"))
            if None not in (target, original, floor) and target >= original + floor:
                metrics["readiness_recovery_push_us"].append(target - original - floor)
        frame_costs = client_costs(row) if number(row, "presented") == 1 else None
        frame_id = number(row, "frame")
        if frame_costs is not None:
            recent_frame_costs[frame_id] = frame_costs
            metrics["partitioned_client_processing_us"].append(sum(frame_costs.values()))
            for key, value in frame_costs.items():
                cost_metrics[key].append(value)
            # Catch-up charges the preceding presented frame, not this frame.
            while len(recent_frame_costs) > 2:
                del recent_frame_costs[next(iter(recent_frame_costs))]
        elif number(row, "presented") == 1:
            invalid["client_cost_partition"] += 1
        if number(row, "buffer_update_valid") == 1:
            before, after, clipped, action, attributed, update_frame, update_at = (number(row, k) for k in
                ("buffer_request_before_us", "buffer_request_after_us", "buffer_clipped_increase_us",
                 "buffer_action", "buffer_attributed_frame", "buffer_update_frame", "buffer_update_at_us"))
            if (None in (before, after, clipped, action, attributed, update_frame, update_at) or
                    min(before, after, clipped) < 0 or not 0 <= action < len(BUFFER_ACTIONS) or
                    update_frame != frame_id or update_at <= 0):
                invalid["buffer_update"] += 1
            else:
                buffer_update_rows += 1
                buffer_actions[BUFFER_ACTIONS[action]] += 1
                if before != after or clipped:
                    buffer_events.append({
                        "frame": frame_id, "at_us": update_at, "attributed_frame": attributed,
                        "action": BUFFER_ACTIONS[action], "request_before_us": before,
                        "request_after_us": after, "change_us": after - before,
                        "clipped_step_us": clipped,
                        "interval_error_us": number(row, "buffer_interval_error_us"),
                        "readiness_lateness_us": number(row, "buffer_lateness_us"),
                        "initial_calibration_complete": number(row, "buffer_calibration_complete"),
                        "calibration_intervals": number(row, "buffer_calibration_samples"),
                        "calibration_coverage_us": number(row, "buffer_calibration_coverage_us"),
                        "observed_client_costs_us": recent_frame_costs.get(attributed),
                    })
        if number(row, "presented") != 1:
            # Terminal rows may be emitted out of arrival order. Frame identity
            # below, rather than their position here, breaks cadence across drops.
            continue
        presented += 1
        for key in ("playout_delay_us", "decode_sync_wait_us", "prepare_us",
                    "present_call_us", "arrival_queue_depth_after", "completion_queue_depth"):
            value = number(row, key)
            if value is not None and value >= 0:
                metrics[key].append(value)
        if number(row, "prepare_timing_valid") == 1:
            for key in ("prepare_acquire_us", "prepare_render_us", "prepare_decode_sync_us", "prepare_flush_us"):
                value = number(row, key)
                if value is not None and value >= 0:
                    metrics[key].append(value)
        for name, start, end in (
            ("assembly_us", "frame_receive_us", "frame_reassembled_us"),
            ("decoder_queue_us", "frame_reassembled_us", "decode_submit_us"),
            ("decoder_submit_to_output_us", "decode_submit_us", "decoder_output_us"),
            ("queue_residence_us", "pacer_arrival_us", "dequeue_us"),
            ("output_to_submission_us", "decoder_output_us", "submission_boundary_us"),
            ("ready_to_submission_us", "decode_complete_us", "submission_boundary_us"),
            ("ingress_to_present_return_us", "frame_receive_us", "present_end_us"),
        ):
            difference(row, name, start, end)
        if (number(row, "gpu_ready_timing_valid") == 1 and
                number(row, "gpu_ready_wait_result_valid") == 1 and number(row, "gpu_ready_wait_result") == 0):
            difference(row, "gpu_render_ready_wait_us", "gpu_ready_wait_start_us", "gpu_ready_time_us")
        processing = difference(row, "client_processing_us", "decoder_output_us", "present_end_us")
        prep, call = number(row, "prepare_us"), number(row, "present_call_us")
        if processing is not None and prep is not None and call is not None and 0 <= prep + call <= processing:
            metrics["rendering_us"].append(prep + call)
            # Match the current overlay: explicit GPU decode waiting remains
            # in full client time and its own diagnostic metric, not queue time.
            decode_wait = max(0, number(row, "decode_sync_wait_us") or 0)
            metrics["queue_pacing_us"].append(processing - prep - call -
                                               min(decode_wait, processing - prep - call))
        else:
            invalid["queue_pacing_us"] += 1
            if processing is not None:
                # Keep the three overlay-equivalent totals on one denominator.
                metrics["client_processing_us"].pop()
                invalid["client_processing_us"] += 1
        frame, rtp, at = (number(row, k) for k in ("frame", "rtp_timestamp", "submission_boundary_us"))
        current = (frame, rtp, at) if None not in (frame, rtp, at) and at > 0 and number(row, "rtp_valid") == 1 else None
        adjacent = current and previous and ((frame - previous[0]) & 0xffffffff) == 1 and at > previous[2]
        if adjacent:
            source_ticks = (rtp - previous[1]) & 0xffffffff
            if 0 < source_ticks < 90000:
                interval = at - previous[2]
                source_us = source_ticks * 1000000 / 90000
                if source_us <= 25000:
                    metrics["sender_spacing_error_us"].append(round(abs(interval - source_us)))
                if previous_interval is not None:
                    metrics["submission_jerk_us"].append(abs(interval - previous_interval))
                previous_interval = interval
            else:
                previous_interval = None
        else:
            previous_interval = None
        previous = current

    # Count host stalls across all arrivals, including locally dropped frames.
    # Producer terminal rows may appear before older worker rows in the file.
    prior_source = None
    for seq, (frame, rtp, valid) in sorted(source_points.items()):
        if valid == 1 and frame is not None and rtp is not None:
            if prior_source and seq == prior_source[0] + 1 and ((frame - prior_source[1]) & 0xffffffff) == 1:
                ticks = (rtp - prior_source[2]) & 0xffffffff
                if 0 < ticks < 90000:
                    source_pairs += 1
                    source_stalls += ticks > 2250
            prior_source = (seq, frame, rtp)
        else:
            prior_source = None

    complete = bool(footer) and all(footer.get(k) == v for k, v in (
        ("clean_shutdown", "1"), ("rows_dropped", "0"), ("write_failed", "0"), ("size_capped", "0")))
    expected_hash = (footer or {}).get("decoded_sha256")
    hash_valid = expected_hash == digest.hexdigest()
    sequence_valid = bool(sequences) and min(sequences) == 1 and max(sequences) == rows and not duplicate_sequences
    accounting = bool(footer) and footer.get("rows_enqueued") == str(rows) and footer.get("arrival_sequence_allocated") == str(rows)
    if parent is not None:
        # A phase is a subset, not a newly fabricated complete trace. Its
        # integrity comes from the original file validated before splitting.
        complete = parent["complete_capture"]
        footer = parent["integrity"]["footer"]
        hash_valid = parent["integrity"]["decoded_hash_valid"]
        sequence_valid = parent["integrity"]["sequence_valid"]
        accounting = parent["integrity"]["accounting_valid"]
    warnings = []
    if not (complete and hash_valid and sequence_valid and accounting):
        warnings.append("Incomplete or inconsistent capture; do not use for a complete-session comparison.")
    if len(identities) != 1:
        warnings.append("Session/policy changed within capture; do not treat as one preset.")
    if "decoder_output_us" not in columns:
        warnings.append("Legacy capture lacks decoder output: overlay client-processing and queue/pacing latency are unavailable. Readiness latency is a different boundary.")
    if "session_latency_mode" not in columns:
        warnings.append("Preset inferred from captured cap; explicit preset metadata unavailable.")
    if "buffer_update_valid" not in columns:
        warnings.append("Legacy capture lacks buffer-decision reasons; measured costs do not establish why the controller grew or held reserve.")
    warnings.append("CPU submission cadence is not physical display smoothness; no end-to-end latency is measured.")
    jerk = metrics["submission_jerk_us"]
    sender = metrics["sender_spacing_error_us"]
    return {
        "path": str(path.resolve()), "preset": next(iter(modes)) if len(modes) == 1 else "Mixed",
        "session": session, "policy_fingerprints": sorted(identities), "initial_profile_hashes": sorted(profiles),
        "native_backends": sorted(backends), "rows": selected_rows, "capture_rows": parent["capture_rows"] if parent else rows, "presented": presented,
        "integrity_scope": "whole_capture" if parent else "capture",
        "latency_test_phases": sorted(phases), "phase": phase_filter,
        "duration_seconds": (last_arrival - first_arrival) / 1e6 if first_arrival is not None else 0,
        "outcomes": dict(outcomes), "complete_capture": complete and hash_valid and sequence_valid and accounting,
        "integrity": {"footer": footer, "decoded_hash_valid": hash_valid, "sequence_valid": sequence_valid,
                      "accounting_valid": accounting, "duplicate_sequences": duplicate_sequences},
        "metrics": {k: distribution(v) for k, v in sorted(metrics.items())}, "unavailable_or_invalid": dict(invalid),
        "client_costs_us": {k: distribution(v) for k, v in cost_metrics.items()},
        "buffer_updates": {"rows": buffer_update_rows, "actions": dict(buffer_actions), "events": buffer_events},
        "cadence": {"jerk_pairs": len(jerk), "jerk_over_2ms": sum(v > 2000 for v in jerk),
                    "jerk_over_2ms_percent": 100 * sum(v > 2000 for v in jerk) / len(jerk) if jerk else None,
                    "sender_pairs": len(sender), "sender_errors_over_3ms": sum(v > 3000 for v in sender),
                    "source_pairs": source_pairs, "source_stalls_over_25ms": source_stalls},
        "history": {"valid_rows": history_rows, "release_allowed_rows": release_allowed}, "warnings": warnings,
    }


def analyze(path):
    # A disk-backed temporary stream keeps decompression off the captured app
    # and bounds memory independently of capture length.
    with path.open("rb") as source, tempfile.TemporaryFile() as expanded:
        before = path.stat()
        raw_digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            raw_digest.update(chunk)
        raw_hash = raw_digest.hexdigest()
        source.seek(0)
        if source.read(7) == b"MLVRR1\n":
            source.seek(0)
            spec = importlib.util.spec_from_file_location("decode_vrr_trace", Path(__file__).with_name("decode-vrr-trace.py"))
            decoder = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(decoder)
            decoder.decode(source, expanded)
            expanded.seek(0)
            decoded = expanded
        else:
            source.seek(0)
            decoded = source
        result = summarize(decoded, path)
        if result["latency_test_phases"]:
            # Partition once rather than rereading a long capture for each
            # minute. Temporary phase streams carry no invented trace footer.
            result["segments"] = []
            with ExitStack() as stack:
                decoded.seek(0)
                header = decoded.readline()
                phase_column = next(csv.reader([header.decode().strip()])).index("latency_test_phase")
                streams = {}
                for raw in decoded:
                    if raw.startswith(b"#") or not raw.strip():
                        continue
                    phase = int(next(csv.reader([raw.decode().strip()]))[phase_column])
                    if phase not in streams:
                        streams[phase] = stack.enter_context(tempfile.TemporaryFile())
                        streams[phase].write(header)
                    streams[phase].write(raw)
                for phase, stream in sorted(streams.items()):
                    stream.seek(0)
                    segment = summarize(stream, path, phase_filter=phase, parent=result)
                    segment["warnings"].append("Oscillation phase: integrity refers to the whole capture; live history carries across switches, not independent calibration.")
                    result["segments"].append(segment)
        after = path.stat()
        if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
            raise ValueError("capture changed during analysis; wait for stream exit")
        result.update(sha256=raw_hash, size=before.st_size, mtime_ns=before.st_mtime_ns)
        for segment in result.get("segments", []):
            segment.update(sha256=raw_hash, size=before.st_size, mtime_ns=before.st_mtime_ns)
        return result


def markdown(reports):
    reports = [segment for r in reports for segment in r.get("segments", [r])]
    def mean(report, key):
        value = report["metrics"].get(key, {}).get("mean")
        return "N/A" if value is None else f"{value / 1000:.3f}"
    lines = ["# Observed VRR latency", "", "One row per actual capture or oscillation phase; latency averages in milliseconds. No simulated presets.", "",
             "| Preset | Frames | Client total (diagnostic) | Queue/pacing | GPU decode wait | Rendering | Reserve | Jerk >2 ms |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for r in reports:
        jerk = r["cadence"]["jerk_over_2ms_percent"]
        text = "N/A" if jerk is None else f"{jerk:.2f}%"
        label = r['preset'] + (f" (phase {r['phase']})" if r['phase'] is not None else "")
        lines.append(f"| {label} | {r['presented']} | " + " | ".join(mean(r, k) for k in
                     ("client_processing_us", "queue_pacing_us", "decode_sync_wait_us", "rendering_us", "playout_delay_us")) + f" | {text} |")
    lines += ["", "Client processing runs from decoder output to present-call return. Queue/pacing excludes explicit GPU decode waiting; queue/pacing plus rendering plus that wait partitions the interval; padding is a controller budget, not another additive component.",
              "Submission jerk measures changes between adjacent submission intervals, including host variation. It does not establish physical display smoothness."]
    missing = set(MODES.values()) - {r["preset"] for r in reports}
    if missing:
        lines += ["", "Missing measured presets: " + ", ".join(sorted(missing)) + "."]
    lines += ["", "Compare the same gameplay, display settings, and duration after reconnecting. Each preset has separate calibration; capture metadata records its starting state. These checks do not certify matched workloads."]
    for r in reports:
        lines += ["", f"## {Path(r['path']).name}", "", f"Complete capture: {r['complete_capture']}; duration: {r['duration_seconds']:.2f} s; jerk pairs: {r['cadence']['jerk_pairs']}; source stalls >25 ms: {r['cadence']['source_stalls_over_25ms']}.", ""]
        lines += ["- " + w for w in r["warnings"]]
        lines += ["", "Execution cost (ms). Only frames with complete, ordered stage timestamps are included. Stage means sum to the partitioned total below; percentile columns do not add.", "",
                  "| Stage | Samples | Mean | p95 | p99 |", "|---|---:|---:|---:|---:|"]
        for key in CLIENT_COSTS:
            d = r["client_costs_us"].get(key, {})
            values = ["N/A" if d.get(k) is None else f"{d[k] / 1000:.3f}" for k in ("mean", "p95", "p99")]
            lines.append(f"| {key.replace('_', ' ')} | {d.get('count', 0)} | " + " | ".join(values) + " |")
        d = r["metrics"].get("partitioned_client_processing_us", {})
        values = ["N/A" if d.get(k) is None else f"{d[k] / 1000:.3f}" for k in ("mean", "p95", "p99")]
        lines.append(f"| Partitioned total | {d.get('count', 0)} | " + " | ".join(values) + " |")
        lines += ["", "Upstream costs, preparation details, and policy offsets (ms). These overlap the stage table or lie outside it; do not add them to client time.", "",
                  "| Measurement | Mean | p95 | p99 |", "|---|---:|---:|---:|"]
        for key in ("assembly_us", "decoder_queue_us", "decoder_submit_to_output_us",
                    "prepare_decode_sync_us", "prepare_acquire_us", "prepare_render_us", "prepare_flush_us",
                    "gpu_render_ready_wait_us", "playout_delay_us", "requested_playout_delay_us", "buffer_cap_us",
                    "buffer_preset_cap_us", "buffer_queue_limit_us", "cadence_smoothing_us",
                    "source_mapping_change_us", "readiness_recovery_push_us", "presentation_floor_push_us",
                    "render_lead_us", "gpu_readiness_lead_us", "render_wait_overshoot_us", "target_wait_overshoot_us"):
            d = r["metrics"].get(key, {})
            values = ["N/A" if d.get(k) is None else f"{d[k] / 1000:.3f}" for k in ("mean", "p95", "p99")]
            lines.append(f"| {key} | " + " | ".join(values) + " |")
        updates = r["buffer_updates"]
        actions = "; ".join(f"{key}: {value}" for key, value in updates["actions"].items())
        lines += ["", f"Buffer decisions: {updates['rows']} recorded." + (f" {actions}." if actions else ""),
                  "Requests affect subsequent frames. A clipped step is rejected growth, not applied latency. Attribution identifies late readiness; stage timings alone do not isolate a hardware or network cause."]
        if updates["events"]:
            lines += ["", "Latest 20 request changes or capped attempts (all events and associated frame costs are in JSON):", "",
                      "| Frame | Charged frame | Reason | Request before/after ms | Change ms | Rejected ms |",
                      "|---:|---:|---|---:|---:|---:|"]
            for event in updates["events"][-20:]:
                lines.append(f"| {event['frame']} | {event['attributed_frame']} | {event['action']} | "
                             f"{event['request_before_us']/1000:.3f}/{event['request_after_us']/1000:.3f} | "
                             f"{event['change_us']/1000:+.3f} | {event['clipped_step_us']/1000:.3f} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, help="JSON report")
    parser.add_argument("--markdown", type=Path, help="Markdown comparison")
    args = parser.parse_args()
    try:
        reports = [analyze(path) for path in args.captures]
        result = json.dumps({"report_schema": 2, "kind": "observed-captures", "captures": reports}, indent=2)
        if args.output:
            args.output.write_text(result + "\n")
        if args.markdown:
            args.markdown.write_text(markdown(reports))
        if not args.output and not args.markdown:
            print(markdown(reports), end="")
    except (OSError, ValueError, csv.Error, zlib.error) as error:
        parser.exit(1, f"report-vrr-latency: {error}\n")


if __name__ == "__main__":
    main()
