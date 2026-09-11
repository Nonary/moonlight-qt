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


MODES = {0: "Smoothest", 1: "Balanced", 2: "Lowest latency"}
SESSION_FIELDS = ("session_latency_mode", "session_readiness_hitch_feedback",
                  "session_latency_oscillation", "latency_test_phase",
                  "calibration_loaded", "initial_cached_samples", "history_version",
                  "stream_rate_hz", "display_refresh_hz", "playout_initial_profile")


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
            ("decoder_submit_to_output_us", "decode_submit_us", "decoder_output_us"),
            ("queue_residence_us", "pacer_arrival_us", "dequeue_us"),
            ("output_to_submission_us", "decoder_output_us", "submission_boundary_us"),
            ("ready_to_submission_us", "decode_complete_us", "submission_boundary_us"),
            ("ingress_to_present_return_us", "frame_receive_us", "present_end_us"),
        ):
            difference(row, name, start, end)
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
             "| Preset | Frames | Client processing | Queue/pacing | Rendering | Padding | Jerk >2 ms |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for r in reports:
        jerk = r["cadence"]["jerk_over_2ms_percent"]
        text = "N/A" if jerk is None else f"{jerk:.2f}%"
        label = r['preset'] + (f" (phase {r['phase']})" if r['phase'] is not None else "")
        lines.append(f"| {label} | {r['presented']} | " + " | ".join(mean(r, k) for k in
                     ("client_processing_us", "queue_pacing_us", "rendering_us", "playout_delay_us")) + f" | {text} |")
    lines += ["", "Client processing runs from decoder output to present-call return. Queue/pacing excludes explicit GPU decode waiting; queue/pacing plus rendering plus that wait partitions the interval; padding is a controller budget, not another additive component.",
              "Submission jerk measures changes between adjacent submission intervals, including host variation. It does not establish physical display smoothness."]
    missing = set(MODES.values()) - {r["preset"] for r in reports}
    if missing:
        lines += ["", "Missing measured presets: " + ", ".join(sorted(missing)) + "."]
    lines += ["", "Compare the same gameplay, display settings, and duration after reconnecting. Each preset has separate calibration; capture metadata records its starting state. These checks do not certify matched workloads."]
    for r in reports:
        lines += ["", f"## {Path(r['path']).name}", "", f"Complete capture: {r['complete_capture']}; duration: {r['duration_seconds']:.2f} s; jerk pairs: {r['cadence']['jerk_pairs']}; source stalls >25 ms: {r['cadence']['source_stalls_over_25ms']}.", ""]
        lines += ["- " + w for w in r["warnings"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, help="JSON report")
    parser.add_argument("--markdown", type=Path, help="Markdown comparison")
    args = parser.parse_args()
    try:
        reports = [analyze(path) for path in args.captures]
        result = json.dumps({"report_schema": 1, "kind": "observed-captures", "captures": reports}, indent=2)
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
