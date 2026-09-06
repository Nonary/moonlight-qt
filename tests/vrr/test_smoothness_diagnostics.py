#!/usr/bin/env python3
"""Deterministic observability and calibration tests, independent of app builds."""
import csv
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

SPEC = importlib.util.spec_from_file_location(
    "smoothness", Path(__file__).resolve().parents[2] / "scripts/vrr_smoothness.py")
SM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SM)


def frames(count=601, shift=None, source_shift=None):
    rows = []
    for i in range(count):
        source = i * 10000 + (source_shift(i) if source_shift else 0)
        row = dict.fromkeys(SM.FIELDS, 0)
        row.update(arrival_sequence=i+1, frame=i+1,
                   rtp_timestamp=round(source * .09) & 0xffffffff, rtp_valid=1,
                   presented=1, source_period_us=10000,
                   submission_boundary_us=1_000_000 + source + (shift(i) if shift else 0))
        rows.append(row)
    return rows


def integrity(name="train"):
    return {"capture_valid": True, "trace_sha256": name}


def encoded_trace(rows):
    text = io.StringIO(newline="")
    writer = csv.DictWriter(text, fieldnames=SM.FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    body = text.getvalue().encode()
    footer = (f"#vrr_trace_footer,format_version=2,clean_shutdown=1,"
              f"arrival_sequence_allocated={len(rows)},rows_enqueued={len(rows)},"
              f"rows_dropped=0,size_capped=0,write_failed=0,"
              f"decoded_sha256={hashlib.sha256(body).hexdigest()}\n").encode()
    return body + footer


class SmoothnessDiagnostics(unittest.TestCase):
    def test_steady_output_is_not_a_hitch(self):
        report = SM.analyze(frames(), integrity())
        self.assertEqual(report["metrics_us"]["output_interval_change"]["max"], 0)
        self.assertEqual(report["metrics_us"]["client_interval_distortion"]["max"], 0)
        self.assertIsNone(report["visible_stutter_probability"])

    def test_repeated_micro_judder_below_one_millisecond_is_retained(self):
        report = SM.analyze(frames(100, lambda i: 100 if i % 2 else -100), integrity())
        self.assertEqual(report["metrics_us"]["output_interval_change"]["p99"], 400)
        self.assertGreater(report["longest_alternating_run_pairs"], 90)
        bands = report["diagnostic_bands"]["output_interval_change"]
        self.assertGreater(bands[1]["exceeding_intervals"], 90)  # 250 us
        self.assertGreater(bands[1]["longest_episode_span_us"], 900_000)
        self.assertEqual(bands[3]["exceeding_intervals"], 0)  # 1 ms

    def test_source_judder_is_reported_without_inventing_client_distortion(self):
        report = SM.analyze(frames(100, source_shift=lambda i: 1000 if i % 2 else 0), integrity())
        self.assertGreater(report["metrics_us"]["output_interval_change"]["p99"], 1000)
        self.assertEqual(report["metrics_us"]["client_interval_distortion"]["max"], 0)

    def test_cluster_is_one_episode_not_many_independent_failures(self):
        report = SM.analyze(frames(100, lambda i: 5000 if i in (20, 22) else 0), integrity())
        band = report["diagnostic_bands"]["client_interval_distortion"][4]
        self.assertEqual(band["exceeding_intervals"], 4)
        self.assertEqual(band["episodes"], 1)

    def test_content_skip_and_rebase_are_not_silently_lost(self):
        rows = frames(100)
        del rows[30]
        rows[50]["rebased"] = 1
        report = SM.analyze(rows, integrity())
        self.assertEqual(report["skipped_source_frame_numbers"], 1)
        self.assertEqual(report["excluded_boundaries_or_missing_feedback"], 1)
        self.assertEqual(report["metrics_us"]["client_interval_distortion"]["max"], 0)

    def test_seconds_without_updates_are_retained_as_forecast_inputs(self):
        rows = frames()
        rows = rows[:190] + rows[410:]
        report = SM.analyze(rows, integrity())
        stalled = next(w for w in report["windows"] if w["index"] == 2)
        self.assertEqual(stalled["intervals"], 0)
        self.assertGreater(stalled["end_of_window_no_update_excess_periods"], 100)
        self.assertTrue(SM.eligible_window(stalled))
        self.assertEqual(SM.risk_bucket(stalled), len(SM.RISK_BINS)-1)

    def test_rtp_wrap_is_a_valid_interval(self):
        rows = frames(30)
        for row in rows:
            row["rtp_timestamp"] = (row["rtp_timestamp"] + 0xfffff000) & 0xffffffff
        report = SM.analyze(rows, integrity())
        self.assertEqual(report["metrics_us"]["source_interval"]["count"], 29)
        self.assertEqual(report["metrics_us"]["client_interval_distortion"]["max"], 0)

    def test_causal_availability_errors_block_model_use(self):
        rows = frames()
        rows[10]["gpu_ready_observed_us"] = rows[10]["submission_boundary_us"] + 10
        report = SM.analyze(rows, integrity())
        self.assertFalse(report["timing_valid"])
        self.assertEqual(report["submissions_before_recorded_availability"], 1)

    def test_display_feedback_cannot_bridge_unknown_presentations(self):
        rows = frames(6)
        display = {"trace_sha256": "train", "basis": "compositor",
                   "trace_clock_offset_us": 500, "uncertainty_us": 200,
                   "frames": [{"arrival_sequence": i+1,
                               "display_time_us": rows[i]["submission_boundary_us"]}
                              for i in (0, 1, 3, 4, 5)]}
        report = SM.analyze(rows, integrity(), display)
        self.assertEqual(report["timing_basis"], "compositor_presentation")
        self.assertEqual(report["metrics_us"]["output_interval"]["count"], 3)
        self.assertEqual(report["thresholds_below_difference_uncertainty_us"], 400)
        display["trace_sha256"] = "other"
        with self.assertRaises(ValueError):
            SM.analyze(rows, integrity(), display)

    def test_compressed_split_lines_and_corrupt_capture_gate(self):
        raw = encoded_trace(frames(20))
        compressed = bytearray(b"MLVRR1\n")
        for offset in range(0, len(raw), 77):
            chunk = raw[offset:offset+77]
            payload = struct.pack(">I", len(chunk)) + zlib.compress(chunk)
            compressed += struct.pack("<I", len(payload)) + payload
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"trace.vrrtrace"
            path.write_bytes(compressed)
            rows, valid = SM.load_trace(path)
            self.assertTrue(valid["capture_valid"])
            self.assertEqual(len(rows), 20)
            path.write_bytes(raw.replace(b"clean_shutdown=1", b"clean_shutdown=0"))
            self.assertFalse(SM.load_trace(path)[1]["capture_valid"])
            path.write_bytes(compressed[:-5])
            with self.assertRaises(ValueError):
                SM.load_trace(path)

    def test_future_window_labels_cannot_leak_into_predictors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = []
            for name in ("training", "validation"):
                report = SM.analyze(frames(), integrity(name))
                report["windows"][0]["output_deviation_relative"]["rms"] = .001
                report["windows"][1]["output_deviation_relative"]["rms"] = .1
                (root/f"{name}.json").write_text(json.dumps(report))
                labels = {"report": f"{name}.json", "trace_sha256": name,
                          "method": "observer_review", "context_id": "synthetic-100fps",
                          "sampling": "uniform_windows",
                          "reviewed_windows": [{"index": 1, "noticeable": False},
                                               {"index": 2, "noticeable": True}]}
                path = root/f"{name}-labels.json"
                path.write_text(json.dumps(labels))
                paths.append(path)
            examples = SM.reviewed_examples([paths[0]])[0]
            self.assertEqual(examples, [(0, 0), (5, 1)])
            report_path = root/"training.json"
            changed = json.loads(report_path.read_text())
            changed["windows"][2]["output_deviation_relative"]["rms"] = 10
            report_path.write_text(json.dumps(changed))
            self.assertEqual(SM.reviewed_examples([paths[0]])[0], examples)
            model = SM.calibrate([paths[0]], [paths[1]])
            self.assertTrue(model["validation"]["beats_constant_baseline"])
            self.assertFalse(model["automatic_profile_selection_allowed"])
            with self.assertRaises(ValueError):
                SM.calibrate([paths[0]], [paths[0]])
            with self.assertRaises(ValueError):
                SM.forecast(changed, model, "different-hardware")

    def test_unreviewed_or_cherry_picked_windows_cannot_train_probabilities(self):
        report = SM.analyze(frames(), integrity())
        template = SM.review_template(report, "report.json")
        self.assertEqual(template["reviewed_windows"], [])
        self.assertTrue(template["suggested_window_indices"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/"report.json").write_text(json.dumps(report))
            template.update(report="report.json", context_id="synthetic",
                            reviewed_windows=[{"index": 1, "noticeable": None}])
            path = root/"labels.json"
            path.write_text(json.dumps(template))
            with self.assertRaisesRegex(ValueError, "explicitly reviewed boolean"):
                SM.reviewed_examples([path])
            template["sampling"] = "selected_bad_moments"
            path.write_text(json.dumps(template))
            with self.assertRaisesRegex(ValueError, "uniform or complete review"):
                SM.reviewed_examples([path])


if __name__ == "__main__":
    unittest.main()
