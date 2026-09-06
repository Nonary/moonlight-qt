#!/usr/bin/env python3
"""Offline smoothness diagnostics and supervised next-window risk calibration.

No pacing changes. CPU submission timing is a proxy, never display or optical
ground truth. All microsecond thresholds are diagnostic bands, not visibility
thresholds. Only independently labeled recordings can train a visibility model.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import sys
import zlib


VERSION = 1
WINDOW_US = 1_000_000
BANDS_US = (125, 250, 500, 1000, 2000, 4000, 8000)
RISK_BINS = (0.0025, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, math.inf)
FIELDS = (
    "arrival_sequence", "frame", "rtp_timestamp", "rtp_valid", "presented",
    "dropped", "submission_boundary_us", "source_period_us", "decision_us",
    "decode_complete_us", "gpu_ready_observed_us", "prepare_us", "prepare_end_us", "target_us",
    "rebased", "external_rebase_applied", "gap_fills_before",
)


def trace_lines(path):
    """Stream decompressed lines; chunk boundaries need not be line boundaries."""
    with Path(path).open("rb") as stream:
        if stream.read(7) != b"MLVRR1\n":
            stream.seek(0)
            yield from stream
            return
        pending = b""
        while size := stream.read(4):
            if len(size) != 4:
                raise ValueError("incomplete compressed chunk length")
            length = struct.unpack("<I", size)[0]
            if length > 16 * 1024 * 1024:
                raise ValueError("implausibly large compressed chunk")
            payload = stream.read(length)
            if len(payload) != length or length < 4:
                raise ValueError("incomplete compressed chunk")
            expected = struct.unpack(">I", payload[:4])[0]
            if expected > 32 * 1024 * 1024:
                raise ValueError("implausibly large decoded chunk")
            expanded = zlib.decompress(payload[4:])
            if len(expanded) != expected:
                raise ValueError("decoded chunk length mismatch")
            parts = (pending + expanded).split(b"\n")
            pending = parts.pop()
            for line in parts:
                yield line + b"\n"
        if pending:
            yield pending


def load_trace(path):
    lines = iter(trace_lines(path))
    header_line = next(lines, b"")
    header = next(csv.reader([header_line.decode()]), [])
    required = {"arrival_sequence", "frame", "rtp_timestamp", "rtp_valid",
                "presented", "submission_boundary_us", "source_period_us"}
    if not required.issubset(header):
        raise ValueError(f"missing trace columns: {sorted(required - set(header))}")
    digest = hashlib.sha256(header_line)
    rows, sequences, footer = [], set(), {}
    duplicates = 0
    for raw in lines:
        if raw.startswith(b"#vrr_trace_footer,"):
            if footer:
                raise ValueError("multiple trace footers")
            footer = dict(item.split("=", 1) for item in raw.decode().strip().split(",")[1:])
            continue
        if footer:
            raise ValueError("trace data after footer")
        digest.update(raw)
        if not raw.strip():
            continue
        values = next(csv.reader([raw.decode()]))
        if len(values) != len(header):
            raise ValueError("malformed trace row")
        data = dict(zip(header, values))
        row = {name: int(data.get(name) or 0) for name in FIELDS}
        sequence = row["arrival_sequence"]
        duplicates += sequence in sequences
        sequences.add(sequence)
        rows.append(row)
    rows.sort(key=lambda row: row["arrival_sequence"])
    complete = bool(sequences) and min(sequences) == 1 and max(sequences) == len(rows) and duplicates == 0
    valid = (complete and footer.get("clean_shutdown") == "1" and
             footer.get("decoded_sha256") == digest.hexdigest() and
             int(footer.get("arrival_sequence_allocated", -1)) == len(rows) and
             int(footer.get("rows_enqueued", -1)) == len(rows) and
             footer.get("rows_dropped") == "0" and
             footer.get("write_failed") == "0" and footer.get("size_capped") == "0")
    return rows, {"trace_sha256": digest.hexdigest(), "rows": len(rows),
                  "sequence_complete": complete, "duplicate_sequences": duplicates,
                  "footer_present": bool(footer), "capture_valid": valid}


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return {"count": 0, "mean": None, "rms": None, "p50": None,
                "p95": None, "p99": None, "max": None}
    return {"count": len(ordered), "mean": sum(ordered) / len(ordered),
            "rms": math.sqrt(sum(x*x for x in ordered) / len(ordered)),
            **{name: ordered[max(0, math.ceil(len(ordered)*q)-1)]
               for name, q in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1))}}


def band_report(samples, duration_us, join_us=100_000, thresholds=BANDS_US, unit="us"):
    """Samples are (time, absolute magnitude, epoch); episodes never cross epochs."""
    results = []
    for threshold in thresholds:
        hits = [(t, v, epoch) for t, v, epoch in samples if v > threshold]
        spans, counts, start, end, previous_epoch, count = [], [], None, None, None, 0
        for time, _, epoch in hits:
            if end is None or epoch != previous_epoch or time - end > join_us:
                if end is not None:
                    spans.append(end-start)
                    counts.append(count)
                start, count = time, 0
            count += 1
            end, previous_epoch = time, epoch
        if end is not None:
            spans.append(end-start)
            counts.append(count)
        results.append({f"threshold_{unit}": threshold, "exceeding_intervals": len(hits),
                        "eligible_intervals": len(samples),
                        "interval_fraction": len(hits) / len(samples) if samples else None,
                        "episodes": len(spans), "longest_episode_span_us": max(spans, default=0),
                        "total_episode_span_us": sum(spans),
                        "single_interval_episodes": counts.count(1),
                        "episodes_per_minute": len(spans) * 60_000_000 / duration_us if duration_us else None})
    return results


def analyze(rows, integrity, display=None):
    """Display feedback must be correlated to this exact trace and its clock."""
    feedback = None
    uncertainty = None  # Integer microseconds do not establish clock accuracy.
    basis = "cpu_submission_proxy"
    if display is not None:
        if display.get("trace_sha256") != integrity["trace_sha256"]:
            raise ValueError("display feedback belongs to a different trace")
        if display.get("basis") not in ("optical", "compositor"):
            raise ValueError("display basis must be optical or compositor")
        offset = int(display["trace_clock_offset_us"])
        uncertainty = int(display["uncertainty_us"])
        if uncertainty < 0:
            raise ValueError("negative display timestamp uncertainty")
        feedback = {}
        presented = {r["arrival_sequence"] for r in rows if r["presented"]}
        for item in display["frames"]:
            seq = int(item["arrival_sequence"])
            if seq not in presented or seq in feedback:
                raise ValueError("unknown or duplicate display feedback frame")
            feedback[seq] = int(item["display_time_us"]) + offset
        basis = display["basis"] + "_presentation"

    series = {name: [] for name in ("output_interval", "output_interval_change",
                                   "source_interval", "client_interval_distortion",
                                   "source_cadence_deviation", "output_cadence_deviation")}
    relative_series = {name: [] for name in ("client_interval_distortion", "output_cadence_deviation")}
    windows, previous, previous_interval, previous_residual = {}, None, None, None
    origin = min((r["submission_boundary_us"] for r in rows if r["presented"]), default=0)
    duration, excluded, content_skips, alternations, run, longest_run, epoch = 0, 0, 0, 0, 0, 0, 0
    observed = 0
    invalid_pairs = 0
    invalid_availability = sum(
        bool(row["presented"]) and (
            row["submission_boundary_us"] <= 0 or
            row["submission_boundary_us"] < row["decode_complete_us"] or
            row["submission_boundary_us"] < row["gpu_ready_observed_us"] or
            row["submission_boundary_us"] < row["prepare_end_us"])
        for row in rows)
    observations = []
    for row in rows:
        if not row["presented"]:
            continue
        time = feedback.get(row["arrival_sequence"]) if feedback is not None else row["submission_boundary_us"]
        if time is None or time <= 0:
            previous, previous_interval, previous_residual = None, None, None
            epoch += 1
            excluded += 1
            invalid_pairs += 1
            continue
        observed += 1
        observations.append((time, row["source_period_us"]))
        current = (row, time)
        if (previous is None or row["rebased"] or row["external_rebase_applied"]):
            excluded += previous is not None
            previous, previous_interval, previous_residual = current, None, None
            epoch += 1
            continue
        old, old_time = previous
        frame_delta = row["frame"] - old["frame"]
        ticks = (row["rtp_timestamp"] - old["rtp_timestamp"]) & 0xffffffff
        interval = time - old_time
        if (not row["rtp_valid"] or not old["rtp_valid"] or
                frame_delta <= 0 or ticks == 0 or ticks > 0x7fffffff or interval <= 0 or
                row["source_period_us"] <= 0):
            previous, previous_interval, previous_residual = current, None, None
            epoch += 1
            excluded += 1
            invalid_pairs += 1
            continue
        source_interval = ticks * 1_000_000 / 90_000
        expected = row["source_period_us"] * frame_delta
        residual = interval - expected
        distortion = interval - source_interval
        duration += interval
        content_skips += frame_delta - 1
        vals = {"output_interval": interval, "source_interval": source_interval,
                "client_interval_distortion": abs(distortion),
                "source_cadence_deviation": abs(source_interval - expected),
                "output_cadence_deviation": abs(residual)}
        if previous_interval is not None:
            vals["output_interval_change"] = abs(interval - previous_interval)
        for name, value in vals.items():
            series[name].append((time, value, epoch))
        for name in relative_series:
            relative_series[name].append((time, vals[name]/expected, epoch))
        index = (time - origin) // WINDOW_US
        win = windows.setdefault(index, {"index": index, "start_us": origin + index*WINDOW_US,
                                        "distortion_relative": [], "interval_change_us": [],
                                        "output_relative": [], "source_relative": [],
                                        "gpu_observation_lag_us": [], "preparation_us": [],
                                        "preparation_lateness_us": [],
                                        "alternations": 0, "skipped_source_frames": 0,
                                        "epochs": set(), "covered_us": 0})
        win["epochs"].add(epoch)
        win["covered_us"] += min(interval, time - win["start_us"])
        win["distortion_relative"].append(abs(distortion) / expected)
        win["output_relative"].append(abs(residual) / expected)
        win["source_relative"].append(abs(source_interval - expected) / expected)
        win["interval_change_us"].append(vals.get("output_interval_change", 0))
        if row["gpu_ready_observed_us"] >= row["decode_complete_us"] > 0:
            win["gpu_observation_lag_us"].append(row["gpu_ready_observed_us"] - row["decode_complete_us"])
        if row["prepare_us"] > 0:
            win["preparation_us"].append(row["prepare_us"])
        if row["prepare_end_us"] > 0 and row["target_us"] > 0:
            win["preparation_lateness_us"].append(max(0, row["prepare_end_us"] - row["target_us"]))
        win["skipped_source_frames"] += frame_delta - 1
        # No visibility cutoff: record alternating residuals down to clock
        # resolution. Magnitudes remain separately available for calibration.
        alternating = (previous_residual is not None and residual * previous_residual < 0)
        run = run + 1 if alternating else 0
        longest_run = max(longest_run, run)
        alternations += alternating
        win["alternations"] += alternating
        previous, previous_interval, previous_residual = current, interval, residual

    # Retain seconds with no updates. Otherwise labeling only windows with
    # sufficient *future* frames would systematically exclude severe stalls.
    ordered_observations = sorted(observations)
    observation_times = [time for time, _ in ordered_observations]
    last_time = max(observation_times, default=origin)
    for index in range(max(0, (last_time-origin)//WINDOW_US) + 1):
        windows.setdefault(index, {"index": index, "start_us": origin+index*WINDOW_US,
                                  "distortion_relative": [], "output_relative": [],
                                  "source_relative": [], "interval_change_us": [],
                                  "gpu_observation_lag_us": [], "preparation_us": [],
                                  "preparation_lateness_us": [], "epochs": set(),
                                  "covered_us": 0, "alternations": 0, "skipped_source_frames": 0})
    exported = []
    for index, win in sorted(windows.items()):
        count = len(win["output_relative"])
        end = win["start_us"] + WINDOW_US
        previous_index = bisect.bisect_right(observation_times, end) - 1
        no_update_excess = None
        if previous_index >= 0:
            last_update, period = ordered_observations[previous_index]
            if period > 0:
                no_update_excess = max(0, (end-last_update)/period - 1)
        exported.append({"index": index, "start_us": win["start_us"],
                         "end_us": end, "intervals": count,
                         "observation_us": max(0, min(end, last_time)-win["start_us"]),
                         "end_of_window_no_update_excess_periods": no_update_excess,
                         "coverage_us": win["covered_us"], "epoch_count": len(win["epochs"]),
                         "epoch": next(iter(win["epochs"])) if len(win["epochs"]) == 1 else None,
                         "client_distortion_relative": distribution(win["distortion_relative"]),
                         "output_deviation_relative": distribution(win["output_relative"]),
                         "source_deviation_relative": distribution(win["source_relative"]),
                         "interval_change_us": distribution(win["interval_change_us"]),
                         "gpu_observation_lag_us": distribution(win["gpu_observation_lag_us"]),
                         "preparation_us": distribution(win["preparation_us"]),
                         "preparation_lateness_us": distribution(win["preparation_lateness_us"]),
                         "alternating_pairs": win["alternations"],
                         "skipped_source_frames": win["skipped_source_frames"]})
    return {"schema": VERSION, "integrity": integrity, "timing_basis": basis,
            "timestamp_uncertainty_us": uncertainty,
            "thresholds_below_difference_uncertainty_us": 2 * uncertainty if uncertainty is not None else None,
            "visible_stutter_probability": None,
            "probability_status": "requires independent labels and held-out validation",
            "eligible_duration_us": duration, "observed_presentations": observed,
            "submitted_presentations": sum(bool(r["presented"]) for r in rows),
            "presentation_feedback_complete": feedback is None or observed == sum(bool(r["presented"]) for r in rows),
            "timing_valid": invalid_pairs == 0 and invalid_availability == 0,
            "invalid_interval_pairs_or_missing_feedback": invalid_pairs,
            "submissions_before_recorded_availability": invalid_availability,
            "excluded_boundaries_or_missing_feedback": excluded,
            "local_dropped_rows": sum(bool(r["dropped"]) for r in rows),
            "skipped_source_frame_numbers": content_skips,
            "gap_fills_reported_on_presented_rows": sum(r["gap_fills_before"] for r in rows if r["presented"]),
            "alternating_residual_pairs": alternations, "longest_alternating_run_pairs": longest_run,
            "metrics_us": {name: distribution([v for _, v, _ in values]) for name, values in series.items()},
            "diagnostic_bands": {name: band_report(series[name], duration)
                                 for name in ("output_interval_change", "client_interval_distortion", "output_cadence_deviation")},
            "relative_bands": {name: band_report(values, duration,
                                                thresholds=(.005, .01, .02, .05, .1, .25, .5),
                                                unit="fraction_of_source_period")
                               for name, values in relative_series.items()},
            "windows": exported}


def review_template(report, report_path):
    windows = {w["index"]: w for w in report["windows"]}
    eligible = [i for i, w in windows.items() if w["observation_us"] == WINDOW_US and
                i-1 in windows and eligible_window(windows[i-1])]
    # Uniform review sampling avoids calibrating only on cherry-picked bad
    # windows. Deliberately selected examples are useful debugging evidence,
    # but they cannot establish a natural occurrence probability.
    selected = random.Random(report["integrity"]["trace_sha256"]).sample(eligible, min(60, len(eligible)))
    return {"report": str(Path(report_path).resolve()),
            "trace_sha256": report["integrity"]["trace_sha256"],
            "method": "observer_review", "context_id": "",
            "sampling": "uniform_windows",
            "reviewed_windows": [], "suggested_window_indices": sorted(selected),
            "instructions": "Set context_id to hardware/stream/pacing settings. Review synchronized footage; add only reviewed {index, noticeable: true/false} entries. Unreviewed windows remain absent. Use different complete captures for training and validation."}


def risk_bucket(window):
    # This is a candidate predictor, not a perceptual law. A relative scale
    # retains sub-millisecond variation across different source frame rates.
    score = max(window["output_deviation_relative"]["rms"] or 0,
                window["client_distortion_relative"]["rms"] or 0,
                window["end_of_window_no_update_excess_periods"] or 0)
    return next(i for i, edge in enumerate(RISK_BINS) if score <= edge)


def eligible_window(window):
    return (window["observation_us"] == WINDOW_US and window["epoch_count"] <= 1 and
            window["end_of_window_no_update_excess_periods"] is not None)


def reviewed_examples(paths):
    examples, hashes, contexts, bases = [], set(), set(), set()
    for path in paths:
        path = Path(path)
        labels = json.loads(path.read_text())
        if labels.get("method") not in ("observer_review", "optical_review"):
            raise ValueError("labels require independent observer or optical review")
        if labels.get("sampling") not in ("uniform_windows", "all_windows"):
            raise ValueError("probability calibration requires uniform or complete review, not selected bad moments")
        context = labels.get("context_id")
        if not isinstance(context, str) or not context.strip():
            raise ValueError("labels require a context_id identifying hardware and stream/pacing settings")
        report = json.loads((path.parent / labels["report"]).read_text())
        if (report.get("schema") != VERSION or not report["integrity"]["capture_valid"] or
                not report["presentation_feedback_complete"] or not report["timing_valid"]):
            raise ValueError("calibration requires a validated complete capture")
        digest = report["integrity"]["trace_sha256"]
        if labels.get("trace_sha256") != digest or digest in hashes:
            raise ValueError("mismatched or duplicated labeled capture")
        hashes.add(digest)
        contexts.add(context)
        bases.add(report["timing_basis"])
        windows = {w["index"]: w for w in report["windows"]}
        seen = set()
        for label in labels["reviewed_windows"]:
            index = label["index"]
            if type(index) is not int or index in seen or index not in windows:
                raise ValueError("duplicate or unknown reviewed window")
            seen.add(index)
            if type(label["noticeable"]) is not bool:
                raise ValueError("noticeable must be an explicitly reviewed boolean")
            current, previous = windows[index], windows.get(index - 1)
            # Predict the labeled window from the PREVIOUS window only.
            # Neither the label nor the labeled window's measured distortion
            # can be a predictor. No random frame split within one capture.
            if (previous is None or not eligible_window(previous) or
                    current["observation_us"] != WINDOW_US):
                continue
            examples.append((risk_bucket(previous), int(label["noticeable"])))
    if len(contexts) != 1 or len(bases) != 1:
        raise ValueError("calibrate one hardware/pacing context and timing basis at a time")
    if not examples or len({y for _, y in examples}) != 2:
        raise ValueError("review both smooth and disturbed eligible windows")
    return examples, hashes, contexts.pop(), bases.pop()


def wilson_interval(positive, total):
    if total == 0:
        return [0.0, 1.0]
    z = 1.959963984540054
    proportion = positive / total
    center = (proportion + z*z/(2*total)) / (1 + z*z/total)
    half = z * math.sqrt(proportion*(1-proportion)/total + z*z/(4*total*total)) / (1 + z*z/total)
    return [max(0, center-half), min(1, center+half)]


def calibrate(train_paths, validation_paths):
    train, training_hashes, context, basis = reviewed_examples(train_paths)
    validation, validation_hashes, validation_context, validation_basis = reviewed_examples(validation_paths)
    if training_hashes & validation_hashes:
        raise ValueError("training and validation must use different captures")
    if context != validation_context or basis != validation_basis:
        raise ValueError("training and validation contexts/timing bases differ")
    bins = []
    for i, edge in enumerate(RISK_BINS):
        labels = [y for bucket, y in train if bucket == i]
        positives, count = sum(labels), len(labels)
        bins.append({"relative_rms_upper": edge if math.isfinite(edge) else None,
                     "reviewed_windows": count, "noticeable_windows": positives,
                     "probability": (positives + .5)/(count + 1) if count else None,
                     "wilson_95_independent_window_interval": wilson_interval(positives, count)})
    prior = sum(y for _, y in train) / len(train)
    covered = [(bins[b]["probability"], y) for b, y in validation if bins[b]["probability"] is not None]
    brier = sum((p-y)**2 for p, y in covered)/len(covered) if covered else None
    baseline = sum((prior-y)**2 for _, y in covered)/len(covered) if covered else None
    reliability = []
    for i, item in enumerate(bins):
        labels = [y for b, y in validation if b == i]
        reliability.append({"bin": i, "windows": len(labels),
                            "predicted_probability": item["probability"],
                            "observed_fraction": sum(labels)/len(labels) if labels else None})
    return {"schema": VERSION, "status": "experimental_supervised_forecast",
            "target": "any observer-noticeable smoothness disturbance in the next one-second window",
            "automatic_profile_selection_allowed": False,
            "context_id": context, "timing_basis": basis, "bins": bins,
            "training_trace_hashes": sorted(training_hashes),
            "validation_trace_hashes": sorted(validation_hashes),
            "validation": {"reviewed_windows": len(validation), "predicted_windows": len(covered),
                           "brier_score": brier, "constant_training_prevalence_brier": baseline,
                           "beats_constant_baseline": brier < baseline if brier is not None else False,
                           "reliability": reliability},
            "limits": ["confidence intervals assume independent windows; clustered events reduce effective sample size",
                       "timing-only predictors omit motion and individual perceptual sensitivity",
                       "validation scores apply only to this context; there is no universal visibility threshold",
                       "unseen risk bins have no probability estimate"]}


def forecast(report, model, context):
    if (model.get("schema") != VERSION or model.get("status") != "experimental_supervised_forecast" or
            model.get("context_id") != context or model.get("timing_basis") != report["timing_basis"]):
        raise ValueError("model schema, context, or timing basis does not match")
    if (not report["integrity"]["capture_valid"] or not report["presentation_feedback_complete"] or
            not report["timing_valid"]):
        raise ValueError("forecasting requires a complete validated trace")
    estimates = []
    for win in report["windows"]:
        if not eligible_window(win):
            continue
        item = model["bins"][risk_bucket(win)]
        estimates.append({"predictor_window": win["index"], "forecast_window": win["index"] + 1,
                          "probability": item["probability"],
                          "training_windows_in_bin": item["reviewed_windows"]})
    report["experimental_next_window_forecasts"] = estimates
    report["probability_status"] = "experimental supervised forecast; inspect held-out reliability before use"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    diagnose = commands.add_parser("analyze")
    diagnose.add_argument("trace", type=Path)
    diagnose.add_argument("--display-feedback", type=Path)
    diagnose.add_argument("--model", type=Path)
    diagnose.add_argument("--context-id")
    diagnose.add_argument("--review-template", type=Path)
    diagnose.add_argument("--output", type=Path, required=True)
    fit = commands.add_parser("calibrate")
    fit.add_argument("--train", type=Path, nargs="+", required=True)
    fit.add_argument("--validate", type=Path, nargs="+", required=True)
    fit.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "calibrate":
            result = calibrate(args.train, args.validate)
        else:
            rows, integrity = load_trace(args.trace)
            display = json.loads(args.display_feedback.read_text()) if args.display_feedback else None
            result = analyze(rows, integrity, display)
            if args.model:
                result = forecast(result, json.loads(args.model.read_text()), args.context_id)
            if args.review_template:
                args.review_template.write_text(json.dumps(review_template(result, args.output), indent=2) + "\n")
        args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    except (OSError, ValueError, KeyError, TypeError, zlib.error) as error:
        parser.exit(1, f"vrr-smoothness: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
